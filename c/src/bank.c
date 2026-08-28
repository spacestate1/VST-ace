#include "bank.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FXB_HEADER_BYTES 160   /* 28-byte header + 128 reserved + u32 chunkSize */

/* Recovered from the InitParam call sites in FUN_18052f240 @ 0x18052f240.
 * Ghidra reorders the statements, so the registration order in the pseudo-C is
 * not the parameter order; the real index is the immediate in each call's
 * bounds check (cmp $imm,%rax where imm = index * 8).
 *
 * Every column of the factory banks falls inside the range its name predicts,
 * which is what confirms the mapping. */
static const char *const param_names[BANK_NPARAM] = {
    "OSC1 Octave",          "OSC1 Waveform",        "OSC1 Level",
    "Auto Bend Select",     "Auto Bend Mode",       "Auto Bend Time",
    "Auto Bend Intensity",  "OSC2 Octave",          "OSC2 Waveform",
    "OSC2 Level",           "OSC2 Interval",        "OSC2 Detune",
    "Noise Level",          "Mode",                 "Edit Parameter",
    "VCF Cutoff",           "VCF Resonance",        "VCF KBD Track",
    "VCF EG Polarity",      "VCF EG Intensity",     "VCF EG Attack",
    "VCF EG Decay",         "VCF EG Break Point",   "VCF EG Slope",
    "VCF EG Sustain",       "VCF EG Release",       "VCF EG Velocity",
    "VCA EG Attack",        "VCA EG Decay",         "VCA EG Break Point",
    "VCA EG Slope",         "VCA EG Sustain",       "VCA EG Release",
    "VCA EG Velocity",      "MG Waveform",          "MG Frequency",
    "MG Delay",             "MG Osc",               "MG VCF",
    "Bend OSC",             "Bend VCF",             "Delay Time",
    "Delay Factor",         "Delay Feedback",       "Delay Mod. Frequency",
    "Delay Mod. Intensity", "Delay Level",          "Portamento",
    "After Touch OSC MG",   "After Touch VCF",      "After Touch VCA",
    "Mod.Wheel Osc MG",     "Mod.Wheel VCF MG",     "\"Pseudo\" Stereo",
    "Voices",               "Volume",               "Tune",
    "DW Mode",              "Wavetable Set",        "VCF MG Mod. Source",
    "reserved",             "reserved",             "reserved",
    "reserved",             "reserved",             "reserved",
    "reserved",             "reserved",             "reserved"
};

const char *bank_param_name(int index)
{
    if (index < 0 || index >= BANK_NPARAM) return NULL;
    return param_names[index];
}

static uint32_t rd32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static double rdf64le(const unsigned char *p)
{
    uint64_t u = 0;
    double   d;
    int      i;
    for (i = 7; i >= 0; i--) u = (u << 8) | p[i];
    memcpy(&d, &u, sizeof d);
    return d;
}

int bank_parse(bank *b, const unsigned char *data, size_t size)
{
    const unsigned char *c;
    size_t               clen, off;
    int                  cap, n = 0;

    memset(b, 0, sizeof *b);

    if (size < FXB_HEADER_BYTES) { fprintf(stderr, "bank: too short\n"); return -1; }
    if (memcmp(data, "CcnK", 4)) { fprintf(stderr, "bank: bad chunkMagic\n"); return -1; }
    if (memcmp(data + 8, "FBCh", 4)) {
        fprintf(stderr, "bank: fxMagic is not FBCh (not an opaque-chunk bank)\n");
        return -1;
    }

    b->fx_id        = rd32be(data + 16);
    b->fx_version   = rd32be(data + 20);
    b->num_programs = rd32be(data + 24);
    b->chunk_size   = rd32be(data + 156);

    clen = b->chunk_size;
    if (FXB_HEADER_BYTES + clen > size) clen = size - FXB_HEADER_BYTES;
    c = data + FXB_HEADER_BYTES;

    if (clen < 8 || memcmp(c, "tffp", 4)) {
        fprintf(stderr, "bank: chunk does not start with 'tffp'\n");
        return -1;
    }

    cap = (int)(b->num_programs ? b->num_programs : 64);
    if (!(b->prog = calloc((size_t)cap, sizeof *b->prog))) return -1;

    /* Detect the per-program body size. Taking the stride between the first
     * two records is not enough -- a plausible-looking length prefix turns up
     * inside the body itself and yields a false, far-too-small answer. So try
     * each candidate size and accept only one that walks the entire chunk:
     * every name printable, exactly num_programs records, and the last one
     * ending exactly at chunkSize. That triple constraint is what makes the
     * detection trustworthy. */
    {
        int cand;
        b->body_bytes = 0;
        for (cand = 1; cand <= BANK_MAXBODY; cand++) {
            size_t off2 = 8;
            int    n2 = 0, ok = 1;
            while (off2 + 4 <= clen && n2 < cap) {
                uint32_t nl = rd32le(c + off2);
                size_t   k;
                if (nl >= BANK_NAME_MAX || off2 + 4 + nl + (size_t)cand > clen) { ok = 0; break; }
                /* Names are not restricted to ASCII: 0x7f and high-bit bytes
                 * both appear in the factory sets. Only C0 control codes are
                 * implausible in a name. */
                for (k = 0; k < nl; k++)
                    if (c[off2 + 4 + k] < 32) { ok = 0; break; }
                if (!ok) break;
                off2 += 4 + nl + (size_t)cand;
                n2++;
            }
            if (ok && n2 == cap && off2 == clen) { b->body_bytes = cand; break; }
        }
        if (b->body_bytes <= 0) {
            /* No single stride walks the chunk, which means this plugin's
             * programs are variable-length -- the big ones (modulair,
             * sequencair, bucketpops) carry per-program patch graphs or
             * samples. Decoding those needs that plugin's own serialisation,
             * not a stride. */
            fprintf(stderr, "bank: variable-length program bodies; no fixed "
                            "stride fits (%u programs, %zu chunk bytes)\n",
                    b->num_programs, clen);
            free(b->prog); b->prog = NULL;
            return -1;
        }

        /* Does the body have FB-7999's shape? u8 version, N doubles, 2 u32s. */
        if (b->body_bytes > 9 && (b->body_bytes - 9) % 8 == 0 &&
            (b->body_bytes - 9) / 8 <= BANK_MAXPARAM)
            b->nparam = (b->body_bytes - 9) / 8;
        else
            b->nparam = 0;
    }

    off = 8;   /* skip 'tffp' + u32 version word */
    while (off + 4 <= clen && n < cap) {
        uint32_t nlen = rd32le(c + off);
        size_t   need;
        int      i;

        off += 4;
        need = (size_t)nlen + (size_t)b->body_bytes;
        if (nlen >= BANK_NAME_MAX || off + need > clen) {
            fprintf(stderr, "bank: truncated record %d (nameLen=%u)\n", n, nlen);
            break;
        }

        memcpy(b->prog[n].name, c + off, nlen);
        b->prog[n].name[nlen] = '\0';
        off += nlen;

        b->prog[n].version = c[off];
        for (i = 0; i < b->nparam; i++)
            b->prog[n].param[i] = rdf64le(c + off + 1 + (size_t)i * 8);
        if (b->nparam) {
            b->prog[n].five     = rd32le(c + off + 1 + (size_t)b->nparam * 8);
            b->prog[n].revision = rd32le(c + off + 5 + (size_t)b->nparam * 8);
        }
        off += (size_t)b->body_bytes;

        n++;
    }

    b->count = n;

    /* A body size divisible by 8 does not prove the body *is* doubles at that
     * offset. tricent's is, yet its first four "parameters" decode as 1e+151
     * and 1e-312 -- its header is longer than FB-7999's single byte. So check
     * the values: real parameters are finite and modest. If most are not,
     * report the layout as unknown rather than print noise. */
    if (b->nparam && n > 0) {
        int i, j, bad = 0;
        int lead = b->nparam < 8 ? b->nparam : 8;
        /* A wrong offset corrupts the *first* values, so test those rather
         * than an overall proportion: tricent's body is divisible by 8 and
         * only 4 of its 96 slots decode as nonsense, which any percentage
         * threshold would wave through. */
        for (j = 0; j < n && j < 4; j++)
            for (i = 0; i < lead; i++) {
                double v = b->prog[j].param[i];
                if (!(v == v && v > -1e7 && v < 1e7)) bad++;
            }
        if (bad) b->nparam = 0;
    }

    if (off != clen)
        fprintf(stderr, "bank: note - %zu of %zu chunk bytes consumed\n", off, clen);
    return n > 0 ? 0 : -1;
}

int bank_find(const bank *b, const char *sel)
{
    char *end;
    long  n;
    int   i;

    if (!b || !sel) return -1;
    n = strtol(sel, &end, 10);
    if (*sel && !*end) return (n >= 0 && n < b->count) ? (int)n : -1;

    for (i = 0; i < b->count; i++) {          /* case-insensitive substring */
        const char *p;
        for (p = b->prog[i].name; *p; p++) {
            const char *a = p, *q = sel;
            while (*q && *a && ((*a | 32) == (*q | 32))) { a++; q++; }
            if (!*q) return i;
        }
    }
    return -1;
}

void bank_free(bank *b)
{
    free(b->prog);
    memset(b, 0, sizeof *b);
}

int bank_names_load(bank_names *n, const char *path)
{
    FILE  *f;
    char   line[256];
    int    cap = 16;

    memset(n, 0, sizeof *n);
    if (!(f = fopen(path, "r"))) { perror(path); return -1; }
    if (!(n->name = malloc((size_t)cap * sizeof *n->name))) { fclose(f); return -1; }

    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (!l) continue;
        if (n->count == cap) {
            char **grown = realloc(n->name, (size_t)(cap * 2) * sizeof *n->name);
            if (!grown) break;
            n->name = grown; cap *= 2;
        }
        n->name[n->count] = malloc(l + 1);
        if (!n->name[n->count]) break;
        memcpy(n->name[n->count], line, l + 1);
        n->count++;
    }
    fclose(f);
    return n->count;
}

void bank_names_free(bank_names *n)
{
    int i;
    for (i = 0; i < n->count; i++) free(n->name[i]);
    free(n->name);
    memset(n, 0, sizeof *n);
}

/* An explicit -n file wins; otherwise fall back to the recovered names. */
static const char *plabel(const bank_names *names, int i, char *buf, size_t cap)
{
    const char *n;
    if (names && i < names->count) return names->name[i];
    if ((n = bank_param_name(i))) return n;
    snprintf(buf, cap, "p%02d", i);
    return buf;
}

void bank_print(const bank *b, const bank_names *names, int verbose)
{
    char idbuf[5];
    int  p, i;

    idbuf[0] = (char)(b->fx_id >> 24); idbuf[1] = (char)(b->fx_id >> 16);
    idbuf[2] = (char)(b->fx_id >> 8);  idbuf[3] = (char)(b->fx_id);
    idbuf[4] = '\0';

    printf("fxID='%s' fxVersion=%u numPrograms=%u chunkSize=%u parsed=%d\n"
           "body=%d bytes -> %s\n\n",
           idbuf, b->fx_version, b->num_programs, b->chunk_size, b->count,
           b->body_bytes,
           b->nparam ? "decoded as doubles" : "opaque (not a doubles layout)");

    for (p = 0; p < b->count; p++) {
        const bank_program *g = &b->prog[p];
        printf("%2d  %-20s", p, g->name);
        if (!verbose) {
            for (i = 0; i < 10 && i < b->nparam; i++) printf(" %g", g->param[i]);
            printf(b->nparam ? " ...\n" : "\n");
        } else {
            char buf[16];
            printf("\n");
            for (i = 0; i < b->nparam; i++)
                printf("      %-22s %g\n", plabel(names, i, buf, sizeof buf), g->param[i]);
        }
    }

    /* Column profile -- the cheapest route to identifying what each slot is. */
    printf("\n--- column profile ---\n");
    for (i = 0; i < b->nparam; i++) {
        double lo = 1e300, hi = -1e300;
        int    frac = 0;
        char   buf[16];
        for (p = 0; p < b->count; p++) {
            double v = b->prog[p].param[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (v != floor(v)) frac++;
        }
        printf("  %-22s %7.2f .. %7.2f%s\n",
               plabel(names, i, buf, sizeof buf), lo, hi,
               frac ? "   (continuous)" : "");
    }
}

int bank_write_csv(const bank *b, const bank_names *names, const char *path)
{
    FILE *f;
    int   p, i;
    char  buf[16];

    if (!(f = fopen(path, "w"))) { perror(path); return -1; }

    fprintf(f, "index,name");
    for (i = 0; i < b->nparam; i++) fprintf(f, ",%s", plabel(names, i, buf, sizeof buf));
    fprintf(f, "\n");

    for (p = 0; p < b->count; p++) {
        fprintf(f, "%d,\"%s\"", p, b->prog[p].name);
        for (i = 0; i < b->nparam; i++) fprintf(f, ",%g", b->prog[p].param[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

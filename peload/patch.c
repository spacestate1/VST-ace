/* patch.c -- read and write plugin patches as JSON. See patch.h for the format.
 *
 * The parser is hand-written and deliberately small: a patch is a flat object
 * of names to numbers, so pulling in a JSON library to read one would cost more
 * than it saved. It is strict about structure and forgiving about content --
 * unknown keys are skipped rather than rejected, so a bank carrying a comment
 * field or a tag from some later version still loads. */
#define _GNU_SOURCE
#include "patch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYMAX 128

/* ------------------------------------------------------------------ scanner */

typedef struct { const char *p; } scan;

static void skip_ws(scan *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

/* Encode one Unicode scalar as UTF-8. Parameter names are ASCII in every plugin
 * seen here, but a \u escape is legal JSON and dropping it would silently
 * corrupt the name it appears in -- which then matches nothing. */
static int utf8_put(char *buf, int n, int at, unsigned cp)
{
    if (cp < 0x80) {
        if (at + 1 >= n) return at;
        buf[at++] = (char)cp;
    } else if (cp < 0x800) {
        if (at + 2 >= n) return at;
        buf[at++] = (char)(0xC0 | (cp >> 6));
        buf[at++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (at + 3 >= n) return at;
        buf[at++] = (char)(0xE0 | (cp >> 12));
        buf[at++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[at++] = (char)(0x80 | (cp & 0x3F));
    }
    return at;
}

/* Read a JSON string into `buf`. `s` must be positioned on the opening quote,
 * and is left just past the closing one. Returns 1 on success.
 *
 * Content past `n` is dropped rather than overflowing, and the scan still
 * completes -- so a caller skipping a value it does not care about can pass a
 * tiny buffer and still land in the right place. A name truncated this way
 * simply matches nothing, which is reported as a miss. */
static int scan_string(scan *s, char *buf, int n)
{
    int at = 0;

    if (*s->p != '"') return 0;
    s->p++;
    while (*s->p && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            switch (*s->p) {
            case 'n':  if (at + 1 < n) buf[at++] = '\n'; s->p++; break;
            case 't':  if (at + 1 < n) buf[at++] = '\t'; s->p++; break;
            case 'r':  if (at + 1 < n) buf[at++] = '\r'; s->p++; break;
            case 'b':  if (at + 1 < n) buf[at++] = '\b'; s->p++; break;
            case 'f':  if (at + 1 < n) buf[at++] = '\f'; s->p++; break;
            case 'u': {
                unsigned cp = 0;
                int k;
                s->p++;
                for (k = 0; k < 4 && isxdigit((unsigned char)*s->p); k++, s->p++) {
                    char c = *s->p;
                    cp = cp * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
                }
                at = utf8_put(buf, n, at, cp);
                break;
            }
            case 0:    return 0;
            default:   if (at + 1 < n) buf[at++] = *s->p; s->p++; break;
            }
            continue;
        }
        if (at + 1 < n) buf[at++] = *s->p;
        s->p++;
    }
    if (*s->p != '"') return 0;
    s->p++;
    if (n > 0) buf[at] = 0;
    return 1;
}

/* Step over one value of any type, so an unrecognised key costs nothing.
 *
 * Containers are skipped by counting delimiters, but with the scanner aware of
 * strings: a value like {"note":"}"} closes nothing, and counting naively would
 * end the object early and leave the parser reading a fragment. */
static int skip_value(scan *s)
{
    char throwaway[8];
    int  depth = 0;

    skip_ws(s);
    if (*s->p == '"') return scan_string(s, throwaway, (int)sizeof throwaway);
    if (*s->p == '{' || *s->p == '[') {
        do {
            if (*s->p == '{' || *s->p == '[') { depth++; s->p++; continue; }
            if (*s->p == '}' || *s->p == ']') { depth--; s->p++; continue; }
            if (*s->p == '"') {
                if (!scan_string(s, throwaway, (int)sizeof throwaway)) return 0;
                continue;
            }
            if (!*s->p) return 0;
            s->p++;
        } while (depth > 0);
        return 1;
    }
    /* A scalar -- number, true, false, null -- runs to the next delimiter. */
    while (*s->p && *s->p != ',' && *s->p != '}' && *s->p != ']' &&
           *s->p != ' ' && *s->p != '\t' && *s->p != '\n' && *s->p != '\r')
        s->p++;
    return 1;
}

/* ----------------------------------------------------------------- the bank */

typedef struct { char key[KEYMAX]; double val; } entry;

typedef struct {
    char   name[64];
    /* Set only when the patch names its own plugin, which is what makes a bank
     * able to span machines. Empty means "the bank's". */
    char   path[1024], id[32];
    int    program;        /* -1: leave whatever the plugin is on */
    entry *v;
    int    n, cap;
} patch;

struct patch_bank {
    char   plugin[128], id[32], path[1024];
    patch *p;
    int    n, cap;
};

static int patch_push(patch *p, const char *key, double val)
{
    if (p->n == p->cap) {
        int   cap = p->cap ? p->cap * 2 : 64;
        entry *v  = realloc(p->v, (size_t)cap * sizeof *v);
        if (!v) return 0;
        p->v = v; p->cap = cap;
    }
    snprintf(p->v[p->n].key, sizeof p->v[p->n].key, "%s", key);
    p->v[p->n].val = val;
    p->n++;
    return 1;
}

static patch *bank_add(patch_bank *b)
{
    patch *p;
    if (b->n == b->cap) {
        int    cap = b->cap ? b->cap * 2 : 8;
        patch *v   = realloc(b->p, (size_t)cap * sizeof *v);
        if (!v) return NULL;
        b->p = v; b->cap = cap;
    }
    p = &b->p[b->n++];
    memset(p, 0, sizeof *p);
    p->program = -1;
    return p;
}

/* ---------------------------------------------------------- name resolution */

/* Compare ignoring case and surrounding whitespace. A patch edited by hand
 * picks up stray spaces, and plugins do not always trim their own parameter
 * names either. */
static int name_eq(const char *a, const char *b)
{
    const char *ae, *be;

    while (*a == ' ' || *a == '\t') a++;
    while (*b == ' ' || *b == '\t') b++;
    ae = a + strlen(a);
    be = b + strlen(b);
    while (ae > a && (ae[-1] == ' ' || ae[-1] == '\t')) ae--;
    while (be > b && (be[-1] == ' ' || be[-1] == '\t')) be--;
    if (ae - a != be - b) return 0;
    while (a < ae) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return 1;
}

static int all_digits(const char *s)
{
    if (!*s) return 0;
    while (*s) { if (!isdigit((unsigned char)*s)) return 0; s++; }
    return 1;
}

/* Resolve a patch key to a parameter index, or -1.
 *
 * Names win over indices, so a plugin that genuinely calls a parameter "12" is
 * still addressable and only a key matching no name at all is read as one.
 * `used` makes repeated names map in the order they appear -- patch_save
 * prefers to emit an index for those, so this is the fallback that keeps a
 * hand-written file working. */
static int resolve(const char *key, char **names, int nparams, char *used)
{
    int i;

    for (i = 0; i < nparams; i++)
        if (!used[i] && name_eq(key, names[i])) return i;
    if (all_digits(key)) {
        i = atoi(key);
        if (i >= 0 && i < nparams) return i;
    }
    return -1;
}

static void free_names(char **names, int nparams)
{
    int i;
    if (!names) return;
    for (i = 0; i < nparams; i++) free(names[i]);
    free(names);
}

static char **read_names(pehost *h, int nparams)
{
    char **names;
    int i;

    if (!(names = calloc((size_t)(nparams > 0 ? nparams : 1), sizeof *names)))
        return NULL;
    for (i = 0; i < nparams; i++) {
        char nm[128];
        pehost_param_name(h, i, nm, sizeof nm);
        if (!(names[i] = strdup(nm))) { free_names(names, i); return NULL; }
    }
    return names;
}

/* ------------------------------------------------------------------ parsing */

static char *read_file(const char *path, char *err, int errn)
{
    FILE *f;
    char *buf;
    long  len;

    if (!(f = fopen(path, "rb"))) {
        snprintf(err, (size_t)errn, "%s: cannot open", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0) {
        snprintf(err, (size_t)errn, "%s: cannot size", path);
        fclose(f);
        return NULL;
    }
    /* A bank is kilobytes. Anything past this is not one, and refusing early
     * beats allocating whatever a mistyped path happened to point at. */
    if (len > 64L * 1024 * 1024) {
        snprintf(err, (size_t)errn, "%s: too large to be a patch (%ld bytes)", path, len);
        fclose(f);
        return NULL;
    }
    rewind(f);
    if (!(buf = malloc((size_t)len + 1))) {
        snprintf(err, (size_t)errn, "out of memory");
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        snprintf(err, (size_t)errn, "%s: short read", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = 0;
    fclose(f);
    return buf;
}

/* Read the { "name": number, ... } body of a "params" object into `p`. */
static int parse_params(scan *s, patch *p, const char *path, char *err, int errn)
{
    if (*s->p != '{') {
        snprintf(err, (size_t)errn, "%s: \"params\" is not an object", path);
        return 0;
    }
    s->p++;
    for (;;) {
        char   pk[KEYMAX];
        char  *endp;
        double v;

        skip_ws(s);
        if (*s->p == '}') { s->p++; return 1; }
        if (*s->p == ',') { s->p++; continue; }
        if (!*s->p) {
            snprintf(err, (size_t)errn, "%s: unterminated \"params\"", path);
            return 0;
        }
        if (!scan_string(s, pk, sizeof pk)) {
            snprintf(err, (size_t)errn, "%s: expected a parameter name", path);
            return 0;
        }
        skip_ws(s);
        if (*s->p != ':') {
            snprintf(err, (size_t)errn, "%s: expected ':' after \"%s\"", path, pk);
            return 0;
        }
        s->p++;
        skip_ws(s);
        v = strtod(s->p, &endp);
        if (endp == s->p) {
            snprintf(err, (size_t)errn, "%s: \"%s\" is not a number", path, pk);
            return 0;
        }
        s->p = endp;
        if (!patch_push(p, pk, v)) {
            snprintf(err, (size_t)errn, "out of memory");
            return 0;
        }
    }
}

/* Parse one object. `b` is non-NULL only for the top-level object, which is
 * where the bank-level keys and the "patches" array are recognised; a member of
 * that array is parsed with b == NULL, so a nested "patches" is simply skipped
 * rather than building a tree nobody would read. */
static int parse_obj(scan *s, patch *p, patch_bank *b,
                     const char *path, char *err, int errn)
{
    skip_ws(s);
    if (*s->p != '{') {
        snprintf(err, (size_t)errn, "%s: not a JSON object", path);
        return 0;
    }
    s->p++;

    for (;;) {
        char key[KEYMAX];

        skip_ws(s);
        if (*s->p == '}') { s->p++; return 1; }
        if (*s->p == ',') { s->p++; continue; }
        if (!*s->p) {
            snprintf(err, (size_t)errn, "%s: unterminated object", path);
            return 0;
        }
        if (!scan_string(s, key, sizeof key)) {
            snprintf(err, (size_t)errn, "%s: expected a key", path);
            return 0;
        }
        skip_ws(s);
        if (*s->p != ':') {
            snprintf(err, (size_t)errn, "%s: expected ':' after \"%s\"", path, key);
            return 0;
        }
        s->p++;
        skip_ws(s);

        if (name_eq(key, "params")) {
            if (!parse_params(s, p, path, err, errn)) return 0;
            continue;
        }
        if (b && name_eq(key, "patches")) {
            if (*s->p != '[') {
                snprintf(err, (size_t)errn, "%s: \"patches\" is not an array", path);
                return 0;
            }
            s->p++;
            for (;;) {
                patch *q;
                skip_ws(s);
                if (*s->p == ']') { s->p++; break; }
                if (*s->p == ',') { s->p++; continue; }
                if (!*s->p) {
                    snprintf(err, (size_t)errn, "%s: unterminated \"patches\"", path);
                    return 0;
                }
                if (!(q = bank_add(b))) {
                    snprintf(err, (size_t)errn, "out of memory");
                    return 0;
                }
                if (!parse_obj(s, q, NULL, path, err, errn)) return 0;
            }
            continue;
        }
        if (name_eq(key, "program")) {
            char *endp;
            long  v = strtol(s->p, &endp, 10);
            if (endp != s->p) { p->program = (int)v; s->p = endp; continue; }
            if (!skip_value(s)) goto badval;
            continue;
        }
        if (name_eq(key, "name")) {
            if (*s->p == '"') {
                if (!scan_string(s, p->name, (int)sizeof p->name)) goto badval;
                continue;
            }
            if (!skip_value(s)) goto badval;
            continue;
        }
        /* "pluginPath" is read at both levels: on the bank it is the plugin
         * every patch belongs to, on a patch it overrides that for this one. */
        if (name_eq(key, "pluginPath")) {
            char *dst = b ? b->path : p->path;
            int   cap = b ? (int)sizeof b->path : (int)sizeof p->path;
            if (*s->p == '"') {
                if (!scan_string(s, dst, cap)) goto badval;
                continue;
            }
            if (!skip_value(s)) goto badval;
            continue;
        }
        if (b && name_eq(key, "plugin")) {
            if (*s->p == '"') {
                if (!scan_string(s, b->plugin, (int)sizeof b->plugin)) goto badval;
                continue;
            }
            if (!skip_value(s)) goto badval;
            continue;
        }
        if (name_eq(key, "uniqueID")) {
            char *dst = b ? b->id : p->id;
            int   cap = b ? (int)sizeof b->id : (int)sizeof p->id;
            if (*s->p == '"') {
                if (!scan_string(s, dst, cap)) goto badval;
                continue;
            }
            /* A uniqueID written as a bare number is normalised to the same hex
             * form the plugin reports, so comparing is one shape rather than
             * two. */
            if (isdigit((unsigned char)*s->p) || *s->p == '-') {
                char *endp;
                long  v = strtol(s->p, &endp, 0);
                if (endp != s->p) {
                    s->p = endp;
                    snprintf(dst, (size_t)cap, "0x%08x", (unsigned)v);
                    continue;
                }
            }
            if (!skip_value(s)) goto badval;
            continue;
        }
        if (!skip_value(s)) goto badval;
        continue;

badval:
        snprintf(err, (size_t)errn, "%s: bad value for \"%s\"", path, key);
        return 0;
    }
}

/* Resolve one "pluginPath" against the directory the bank was read from, so a
 * bank kept beside its plugin travels with it. Absolute paths are left alone. */
static void resolve_one(char *path, int cap, const char *bank_path)
{
    char  dir[1024], joined[2048];
    char *slash;

    if (!path[0] || path[0] == '/') return;
    snprintf(dir, sizeof dir, "%s", bank_path);
    if (!(slash = strrchr(dir, '/'))) return;      /* bank is in the cwd */
    *slash = 0;
    snprintf(joined, sizeof joined, "%s/%s", dir, path);
    snprintf(path, (size_t)cap, "%s", joined);
}

static void resolve_path(patch_bank *b, const char *bank_path)
{
    int i;
    resolve_one(b->path, (int)sizeof b->path, bank_path);
    /* Each patch's own path too, so a bank spanning machines resolves every one
     * of them against the same directory rather than only the bank default. */
    for (i = 0; i < b->n; i++)
        resolve_one(b->p[i].path, (int)sizeof b->p[i].path, bank_path);
}

patch_bank *patch_bank_read(const char *path, char *err, int errn)
{
    patch_bank *b;
    char       *text;
    scan        s;
    patch       single;

    if (errn > 0) err[0] = 0;
    if (!(text = read_file(path, err, errn))) return NULL;
    if (!(b = calloc(1, sizeof *b))) {
        snprintf(err, (size_t)errn, "out of memory");
        free(text);
        return NULL;
    }

    memset(&single, 0, sizeof single);
    single.program = -1;
    s.p = text;
    if (!parse_obj(&s, &single, b, path, err, errn)) {
        free(single.v);
        patch_bank_free(b);
        free(text);
        return NULL;
    }
    free(text);

    if (b->n == 0) {
        /* No "patches" array, so the top level was itself one patch. */
        patch *q;
        if (single.n == 0 && single.program < 0) {
            snprintf(err, (size_t)errn,
                     "%s: no \"patches\", no \"params\" and no \"program\"", path);
            free(single.v);
            patch_bank_free(b);
            return NULL;
        }
        if (!(q = bank_add(b))) {
            snprintf(err, (size_t)errn, "out of memory");
            free(single.v);
            patch_bank_free(b);
            return NULL;
        }
        *q = single;
        if (!q->name[0]) {
            /* Named after the file, so a single patch still shows something
             * meaningful in a list that expects names. */
            const char *base = strrchr(path, '/');
            char       *dot;
            snprintf(q->name, sizeof q->name, "%s", base ? base + 1 : path);
            if ((dot = strrchr(q->name, '.'))) *dot = 0;
        }
    } else {
        free(single.v);    /* a top-level "params" beside "patches": not ours */
    }
    resolve_path(b, path);
    return b;
}

void patch_bank_free(patch_bank *b)
{
    int i;
    if (!b) return;
    for (i = 0; i < b->n; i++) free(b->p[i].v);
    free(b->p);
    free(b);
}

int         patch_bank_count(const patch_bank *b)       { return b ? b->n : 0; }
const char *patch_bank_plugin_name(const patch_bank *b) { return b ? b->plugin : ""; }
const char *patch_bank_plugin_id(const patch_bank *b)   { return b ? b->id : ""; }
const char *patch_bank_plugin_path(const patch_bank *b) { return b ? b->path : ""; }

const char *patch_bank_patch_plugin_path(const patch_bank *b, int i)
{
    if (!b || i < 0 || i >= b->n) return "";
    return b->p[i].path[0] ? b->p[i].path : b->path;
}

int patch_bank_is_multi_plugin(const patch_bank *b)
{
    int i;
    if (!b) return 0;
    for (i = 0; i < b->n; i++)
        if (b->p[i].path[0] &&
            strcmp(b->p[i].path, patch_bank_patch_plugin_path(b, 0)) != 0)
            return 1;
    return 0;
}

const char *patch_bank_patch_name(const patch_bank *b, int i)
{
    static char numbered[32];
    if (!b || i < 0 || i >= b->n) return "";
    if (b->p[i].name[0]) return b->p[i].name;
    snprintf(numbered, sizeof numbered, "patch %d", i + 1);
    return numbered;
}

/* ------------------------------------------------------------------- apply */

static int apply_one(const patch_bank *b, int ix, pehost *h, int use_program,
                     char *err, int errn, int *applied, int *missed)
{
    const patch *p;
    char       **names = NULL;
    char        *used  = NULL;
    int          nparams, i, nok = 0, nmiss = 0;

    if (errn > 0) err[0] = 0;
    if (applied) *applied = 0;
    if (missed)  *missed  = 0;
    if (!h) { snprintf(err, (size_t)errn, "no plugin loaded"); return -1; }
    if (!b || ix < 0 || ix >= b->n) {
        snprintf(err, (size_t)errn, "no such patch");
        return -1;
    }
    p = &b->p[ix];

    /* Applied before the parameters, and never after: selecting a program makes
     * the plugin overwrite every parameter with that program's values, so the
     * other order would discard the whole patch. Skipped entirely when the
     * caller is re-asserting a patch over a program the user just chose. */
    if (use_program && p->program >= 0 && p->program < pehost_num_programs(h))
        pehost_set_program(h, p->program);

    nparams = pehost_num_params(h);
    if (nparams > 0 && p->n > 0) {
        if (!(names = read_names(h, nparams)) ||
            !(used = calloc((size_t)nparams, 1))) {
            snprintf(err, (size_t)errn, "out of memory");
            free_names(names, nparams);
            free(used);
            return -1;
        }
        for (i = 0; i < p->n; i++) {
            int px = resolve(p->v[i].key, names, nparams, used);
            if (px < 0) { nmiss++; continue; }
            used[px] = 1;
            pehost_set_param(h, px, (float)p->v[i].val);
            nok++;
        }
        free_names(names, nparams);
        free(used);
    } else {
        nmiss = p->n;
    }
    /* Parameter writes are queued for the audio thread on the in-process VST2
     * path, so without this a caller reading the values straight back -- or
     * saving them again -- would see the state from before the patch. */
    pehost_flush_params(h);

    /* Reported rather than refused. A patch is a plain text file, and moving one
     * between two builds of a plugin, or into a sibling, is a reasonable thing
     * to want; names that do not match are skipped. What is never reasonable is
     * not being told. */
    {
        /* The patch's own id when it has one -- in a bank spanning machines the
         * bank-level id means nothing, and comparing against it would warn on
         * every patch that is working exactly as intended. */
        const char *want = p->id[0] ? p->id : b->id;
        if (want[0]) {
            char have[32];
            snprintf(have, sizeof have, "0x%08x", (unsigned)pehost_unique_id(h));
            if (!name_eq(want, have))
                snprintf(err, (size_t)errn, "patch is for uniqueID %s%s%s, "
                         "this plugin is %s (%s)", want,
                         b->plugin[0] ? " -- " : "", b->plugin[0] ? b->plugin : "",
                         have, pehost_name(h));
        }
    }
    if (!err[0] && nok == 0 && nmiss > 0)
        snprintf(err, (size_t)errn,
                 "none of the %d parameter(s) in the patch matched this plugin",
                 nmiss);

    if (applied) *applied = nok;
    if (missed)  *missed  = nmiss;
    return 0;
}

int patch_bank_apply(const patch_bank *b, int ix, pehost *h,
                     char *err, int errn, int *applied, int *missed)
{
    return apply_one(b, ix, h, 1, err, errn, applied, missed);
}

int patch_bank_apply_params(const patch_bank *b, int ix, pehost *h,
                            char *err, int errn, int *applied, int *missed)
{
    return apply_one(b, ix, h, 0, err, errn, applied, missed);
}

int patch_load(pehost *h, const char *path, char *err, int errn,
               int *applied, int *missed)
{
    patch_bank *b;
    int         rc;

    if (!(b = patch_bank_read(path, err, errn))) return -1;
    rc = patch_bank_apply(b, 0, h, err, errn, applied, missed);
    patch_bank_free(b);
    return rc;
}

/* ------------------------------------------------------------------- save */

static void json_puts(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc((char)c, f); }
        else if (c == '\n')        fputs("\\n", f);
        else if (c == '\t')        fputs("\\t", f);
        else if (c == '\r')        fputs("\\r", f);
        else if (c < 0x20)         fprintf(f, "\\u%04x", c);
        else                       fputc((char)c, f);
    }
    fputc('"', f);
}

int patch_save(pehost *h, const char *path, const char *plugin_path,
               char *err, int errn)
{
    FILE  *f;
    char **names;
    int    nparams, i, j;

    if (errn > 0) err[0] = 0;
    if (!h) { snprintf(err, (size_t)errn, "no plugin loaded"); return -1; }

    nparams = pehost_num_params(h);
    if (!(names = read_names(h, nparams))) {
        snprintf(err, (size_t)errn, "out of memory");
        return -1;
    }
    if (!(f = fopen(path, "wb"))) {
        snprintf(err, (size_t)errn, "%s: cannot write", path);
        free_names(names, nparams);
        return -1;
    }

    fputs("{\n  \"plugin\":     ", f);
    json_puts(f, pehost_name(h));
    fprintf(f, ",\n  \"uniqueID\":   \"0x%08x\",\n", (unsigned)pehost_unique_id(h));
    if (plugin_path && *plugin_path) {
        /* Absolute, always. A hand-written bank may use a relative path and
         * have it resolved against the bank file, which is what lets a bank sit
         * beside its plugin and travel with it -- but what arrives here is
         * relative to the *caller's* working directory, and writing that
         * verbatim would have it read back against a different base. Saving a
         * patch into another directory then produced a path pointing nowhere. */
        char *abs = realpath(plugin_path, NULL);
        fputs("  \"pluginPath\": ", f);
        json_puts(f, abs ? abs : plugin_path);
        fputs(",\n", f);
        free(abs);
    }
    fprintf(f, "  \"program\":    %d,\n  \"params\": {\n", pehost_get_program(h));
    for (i = 0; i < nparams; i++) {
        /* A name is usable as a key only if it identifies exactly one
         * parameter. Plugins do repeat them, and leave them empty, so those are
         * written as the index instead -- which the reader takes back
         * unambiguously. Everything else keeps its name. */
        int dup = !names[i][0];
        for (j = 0; !dup && j < nparams; j++)
            if (j != i && name_eq(names[i], names[j])) dup = 1;
        fputs("    ", f);
        if (dup) fprintf(f, "\"%d\"", i);
        else     json_puts(f, names[i]);
        fprintf(f, ": %.6f%s\n", (double)pehost_get_param(h, i),
                i + 1 < nparams ? "," : "");
    }
    fputs("  }\n}\n", f);

    if (ferror(f)) {
        snprintf(err, (size_t)errn, "%s: write failed", path);
        fclose(f);
        free_names(names, nparams);
        return -1;
    }
    fclose(f);
    free_names(names, nparams);
    return 0;
}

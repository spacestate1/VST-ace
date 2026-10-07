/* sessfile.c -- read and write a studio session as one JSON file. See
 * sessfile.h for the format.
 *
 * The parser is hand-written and deliberately small, in the idiom of
 * patch.c's -- a session is two short arrays of flat objects, so pulling in a
 * JSON library would cost more than it saved. Strict about structure,
 * forgiving about content: unknown keys are skipped rather than rejected, so
 * a file from a newer version still loads. A tab's patch is lifted out as
 * raw text rather than parsed -- patch.c owns that format, and this file only
 * has to carry it whole. */
#define _GNU_SOURCE
#include "sessfile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYMAX 64
#define STRMAX 4096

/* ------------------------------------------------------------------ writing */

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

/* What gets embedded as a tab's patch must be one JSON object -- anything
 * else is left out rather than written into the file to corrupt it. */
static int looks_like_object(const char *text)
{
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    return *text == '{';
}

int sess_write(const char *path, const sess_file *s, char *err, int errn)
{
    FILE *f;
    char *tmp;
    int   i, bad;

    if (errn > 0) err[0] = 0;
    if (!s) { snprintf(err, (size_t)errn, "no session"); return -1; }
    /* Written beside the file and renamed over it, so a write that fails
     * part-way leaves the session that was there rather than half of one. */
    if (asprintf(&tmp, "%s.tmp", path) < 0) {
        snprintf(err, (size_t)errn, "out of memory");
        return -1;
    }
    if (!(f = fopen(tmp, "wb"))) {
        snprintf(err, (size_t)errn, "%s: cannot write", path);
        free(tmp);
        return -1;
    }

    fputs("{\n  \"session\": \"vst-ace\",\n  \"version\": 1,\n  \"synths\": [", f);
    for (i = 0; i < s->nsynths; i++) {
        const sess_synth *sy = &s->synths[i];
        fputs(i ? ",\n    {" : "\n    {", f);
        fputs("\"plugin\": ", f);
        json_puts(f, sy->plugin ? sy->plugin : "");
        if (looks_like_object(sy->patch)) {
            /* Carried verbatim: it is patch.c's own output, and parsing it
             * here to re-emit it could only lose something. */
            fputs(", \"patch\": ", f);
            fputs(sy->patch, f);
        }
        fputc('}', f);
    }
    fputs(s->nsynths ? "\n  ]," : "],", f);

    fputs("\n  \"song\": ", f);
    json_puts(f, s->song ? s->song : "");
    fputs(",\n  \"routes\": [", f);
    for (i = 0; i < s->nroutes; i++) {
        fprintf(f, "%s{ \"track\": %d, ", i ? ", " : "", s->routes[i].track);
        if (s->routes[i].synth >= 0)
            fprintf(f, "\"synth\": %d, ", s->routes[i].synth);
        fputs("\"sink\": ", f);
        json_puts(f, s->routes[i].sink ? s->routes[i].sink : "");
        fputs(" }", f);
    }
    fputs(" ]\n}\n", f);

    bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (!bad && rename(tmp, path) != 0) bad = 1;
    if (bad) {
        snprintf(err, (size_t)errn, "%s: write failed", path);
        remove(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* ----------------------------------------------------------------- scanning */

typedef struct { const char *p; } scan;

static void skip_ws(scan *s)
{ while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++; }

static int utf8_put(char *buf, int n, int at, unsigned cp)
{
    if (cp < 0x80) {
        if (at + 1 >= n) return -1;
        buf[at++] = (char)cp;
    } else if (cp < 0x800) {
        if (at + 2 >= n) return -1;
        buf[at++] = (char)(0xC0 | (cp >> 6));
        buf[at++] = (char)(0x80 | (cp & 63));
    } else {
        if (at + 3 >= n) return -1;
        buf[at++] = (char)(0xE0 | (cp >> 12));
        buf[at++] = (char)(0x80 | ((cp >> 6) & 63));
        buf[at++] = (char)(0x80 | (cp & 63));
    }
    return at;
}

static int scan_string(scan *s, char *buf, int n)
{
    int at = 0;
    if (*s->p != '"') return 0;
    s->p++;
    while (*s->p && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p;
        if (c == '\\') {
            unsigned cp;
            s->p++;
            switch (*s->p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': case '\\': case '"': c = (unsigned char)*s->p; break;
            case 'u':
                cp = 0;
                for (int i = 0; i < 4; i++) {
                    int d;
                    s->p++;
                    if (*s->p >= '0' && *s->p <= '9') d = *s->p - '0';
                    else if (*s->p >= 'a' && *s->p <= 'f') d = *s->p - 'a' + 10;
                    else if (*s->p >= 'A' && *s->p <= 'F') d = *s->p - 'A' + 10;
                    else return 0;
                    cp = cp * 16 + (unsigned)d;
                }
                if ((at = utf8_put(buf, n, at, cp)) < 0) return 0;
                s->p++;
                continue;
            default: return 0;
            }
            if (at + 1 >= n) return 0;
            buf[at++] = (char)c;
            s->p++;
        } else {
            if (at + 1 >= n) return 0;
            buf[at++] = (char)c;
            s->p++;
        }
    }
    if (*s->p != '"') return 0;
    s->p++;
    buf[at] = 0;
    return 1;
}

/* Past a string, kept nowhere -- so, unlike scan_string, of any length: a
 * patch can hold strings far longer than any key or path. */
static int skip_string(scan *s)
{
    if (*s->p != '"') return 0;
    s->p++;
    while (*s->p && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            if (!*s->p) return 0;
        }
        s->p++;
    }
    if (*s->p != '"') return 0;
    s->p++;
    return 1;
}

/* Past a value nobody asked about: string, number, true/false/null, object
 * or array. */
static int skip_value(scan *s)
{
    int depth = 0;
    skip_ws(s);
    if (*s->p == '"') return skip_string(s);
    if (*s->p == '{' || *s->p == '[') {
        do {
            if (*s->p == '{' || *s->p == '[') depth++;
            else if (*s->p == '}' || *s->p == ']') depth--;
            else if (*s->p == '"') {
                if (!skip_string(s)) return 0;
                continue;
            } else if (!*s->p) return 0;
            s->p++;
        } while (depth > 0);
        return 1;
    }
    while (isalnum((unsigned char)*s->p) || *s->p == '.' || *s->p == '-' ||
           *s->p == '+' || *s->p == 'e' || *s->p == 'E')
        s->p++;
    return 1;
}

/* The "patch" value, lifted whole: from the opening brace to the one that
 * closes it, strings respected, as a malloc'd copy. */
static char *raw_object(scan *s)
{
    const char *start;
    int depth = 0;
    char *out;
    size_t n;

    skip_ws(s);
    if (*s->p != '{') return NULL;
    start = s->p;
    do {
        if (*s->p == '{') depth++;
        else if (*s->p == '}') depth--;
        else if (*s->p == '"') {
            if (!skip_string(s)) return NULL;
            continue;
        } else if (!*s->p) return NULL;
        s->p++;
    } while (depth > 0);
    n = (size_t)(s->p - start);
    if (!(out = malloc(n + 1))) return NULL;
    memcpy(out, start, n);
    out[n] = 0;
    return out;
}

/* ------------------------------------------------------------------ parsing */

static char *dup_str(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

/* One { "plugin": ..., "patch": {...} } inside "synths". */
static int parse_synth(scan *s, sess_synth *sy, char *err, int errn)
{
    skip_ws(s);
    if (*s->p != '{') {
        snprintf(err, (size_t)errn, "a \"synths\" entry is not an object");
        return 0;
    }
    s->p++;
    for (;;) {
        char key[KEYMAX], val[STRMAX];
        skip_ws(s);
        if (*s->p == '}') { s->p++; return 1; }
        if (*s->p == ',') { s->p++; continue; }
        if (!*s->p || !scan_string(s, key, sizeof key)) {
            snprintf(err, (size_t)errn, "bad key in a \"synths\" entry");
            return 0;
        }
        skip_ws(s);
        if (*s->p != ':') {
            snprintf(err, (size_t)errn, "expected ':' after \"%s\"", key);
            return 0;
        }
        s->p++;
        skip_ws(s);
        if (!strcmp(key, "plugin")) {
            if (!scan_string(s, val, sizeof val)) {
                snprintf(err, (size_t)errn, "bad \"plugin\" value");
                return 0;
            }
            free(sy->plugin);
            if (!(sy->plugin = dup_str(val))) goto oom;
        } else if (!strcmp(key, "patch")) {
            free(sy->patch);
            sy->patch = NULL;
            /* An object is taken whole; anything else (null, say) is passed
             * over. A broken object is an error, not a fall-through to
             * skip_value from wherever raw_object gave up. */
            if (*s->p == '{' ? !(sy->patch = raw_object(s)) : !skip_value(s)) {
                snprintf(err, (size_t)errn, "bad \"patch\" value");
                return 0;
            }
        } else if (!skip_value(s)) {
            snprintf(err, (size_t)errn, "bad value for \"%s\"", key);
            return 0;
        }
    }
oom:
    snprintf(err, (size_t)errn, "out of memory");
    return 0;
}

/* One { "track": n, "sink": name } inside "routes". */
static int parse_route(scan *s, sess_route *r, char *err, int errn)
{
    skip_ws(s);
    if (*s->p != '{') {
        snprintf(err, (size_t)errn, "a \"routes\" entry is not an object");
        return 0;
    }
    s->p++;
    for (;;) {
        char key[KEYMAX], val[STRMAX], *endp;
        long v;
        skip_ws(s);
        if (*s->p == '}') { s->p++; return 1; }
        if (*s->p == ',') { s->p++; continue; }
        if (!*s->p || !scan_string(s, key, sizeof key)) {
            snprintf(err, (size_t)errn, "bad key in a \"routes\" entry");
            return 0;
        }
        skip_ws(s);
        if (*s->p != ':') {
            snprintf(err, (size_t)errn, "expected ':' after \"%s\"", key);
            return 0;
        }
        s->p++;
        skip_ws(s);
        if (!strcmp(key, "track")) {
            v = strtol(s->p, &endp, 10);
            if (endp == s->p) {
                snprintf(err, (size_t)errn, "\"track\" is not a number");
                return 0;
            }
            s->p = endp;
            r->track = (int)v;
        } else if (!strcmp(key, "synth")) {
            v = strtol(s->p, &endp, 10);
            if (endp == s->p) {
                snprintf(err, (size_t)errn, "\"synth\" is not a number");
                return 0;
            }
            s->p = endp;
            r->synth = (int)v;
        } else if (!strcmp(key, "sink")) {
            if (!scan_string(s, val, sizeof val)) {
                snprintf(err, (size_t)errn, "bad \"sink\" value");
                return 0;
            }
            free(r->sink);
            if (!(r->sink = dup_str(val))) {
                snprintf(err, (size_t)errn, "out of memory");
                return 0;
            }
        } else if (!skip_value(s)) {
            snprintf(err, (size_t)errn, "bad value for \"%s\"", key);
            return 0;
        }
    }
}

static char *read_whole(const char *path, char *err, int errn)
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
    /* Patches embed whole parameter sets, so a session can be a few hundred
     * kilobytes; anything past this is not one. */
    if (len > 64L * 1024 * 1024) {
        snprintf(err, (size_t)errn, "%s: too large to be a session (%ld bytes)",
                 path, len);
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

void sess_free(sess_file *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->nsynths; i++) {
        free(s->synths[i].plugin);
        free(s->synths[i].patch);
    }
    for (i = 0; i < s->nroutes; i++) free(s->routes[i].sink);
    free(s->synths);
    free(s->routes);
    free(s->song);
    free(s);
}

sess_file *sess_read(const char *path, char *err, int errn)
{
    sess_file *s = NULL;
    char      *text;
    scan       sc;

    if (errn > 0) err[0] = 0;
    if (!(text = read_whole(path, err, errn))) return NULL;
    if (!(s = calloc(1, sizeof *s))) {
        snprintf(err, (size_t)errn, "out of memory");
        goto fail;
    }
    if (!(s->song = dup_str(""))) goto oom;

    sc.p = text;
    skip_ws(&sc);
    if (*sc.p != '{') {
        snprintf(err, (size_t)errn, "%s: not a JSON object", path);
        goto fail;
    }
    sc.p++;
    for (;;) {
        char key[KEYMAX], val[STRMAX];
        skip_ws(&sc);
        if (*sc.p == '}') { sc.p++; break; }
        if (*sc.p == ',') { sc.p++; continue; }
        if (!*sc.p || !scan_string(&sc, key, sizeof key)) {
            snprintf(err, (size_t)errn, "%s: expected a key", path);
            goto fail;
        }
        skip_ws(&sc);
        if (*sc.p != ':') {
            snprintf(err, (size_t)errn, "%s: expected ':' after \"%s\"", path, key);
            goto fail;
        }
        sc.p++;
        skip_ws(&sc);

        if (!strcmp(key, "synths")) {
            if (*sc.p != '[') {
                snprintf(err, (size_t)errn, "%s: \"synths\" is not an array", path);
                goto fail;
            }
            sc.p++;
            for (;;) {
                sess_synth *sy;
                skip_ws(&sc);
                if (*sc.p == ']') { sc.p++; break; }
                if (*sc.p == ',') { sc.p++; continue; }
                if (!*sc.p) {
                    snprintf(err, (size_t)errn, "%s: unterminated \"synths\"", path);
                    goto fail;
                }
                if (!(sy = realloc(s->synths,
                                   (size_t)(s->nsynths + 1) * sizeof *sy))) goto oom;
                s->synths = (sess_synth *)sy;
                sy = &s->synths[s->nsynths];
                sy->plugin = NULL;
                sy->patch = NULL;
                if (!parse_synth(&sc, sy, err, errn)) goto fail;
                s->nsynths++;
            }
        } else if (!strcmp(key, "routes")) {
            if (*sc.p != '[') {
                snprintf(err, (size_t)errn, "%s: \"routes\" is not an array", path);
                goto fail;
            }
            sc.p++;
            for (;;) {
                sess_route *r;
                skip_ws(&sc);
                if (*sc.p == ']') { sc.p++; break; }
                if (*sc.p == ',') { sc.p++; continue; }
                if (!*sc.p) {
                    snprintf(err, (size_t)errn, "%s: unterminated \"routes\"", path);
                    goto fail;
                }
                if (!(r = realloc(s->routes,
                                  (size_t)(s->nroutes + 1) * sizeof *r))) goto oom;
                s->routes = (sess_route *)r;
                r = &s->routes[s->nroutes];
                r->track = -1;
                r->synth = -1;
                r->sink = NULL;
                if (!parse_route(&sc, r, err, errn)) goto fail;
                s->nroutes++;
            }
        } else if (!strcmp(key, "song")) {
            if (!scan_string(&sc, val, sizeof val)) {
                snprintf(err, (size_t)errn, "%s: bad \"song\" value", path);
                goto fail;
            }
            free(s->song);
            if (!(s->song = dup_str(val))) goto oom;
        } else if (!skip_value(&sc)) {
            /* "session" and "version" land here too: checked by being read,
             * not by name. */
            snprintf(err, (size_t)errn, "%s: bad value for \"%s\"", path, key);
            goto fail;
        }
    }
    free(text);
    return s;

oom:
    snprintf(err, (size_t)errn, "out of memory");
fail:
    sess_free(s);
    free(text);
    return NULL;
}

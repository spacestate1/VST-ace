/* See vstdirs.h. */
#include "vstdirs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* $XDG_CONFIG_HOME if the desktop set one, else ~/.config, which is what the
 * spec says to assume. Under neither -- no HOME at all, which happens in a
 * build sandbox -- the list is simply unavailable rather than written
 * somewhere arbitrary. */
static int config_dir(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home;

    if (xdg && *xdg) { snprintf(out, n, "%s/vst-ace", xdg); return 0; }
    home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, n, "%s/.config/vst-ace", home);
    return 0;
}

const char *vstdirs_file(void)
{
    static char path[VSTDIRS_PATHLEN];
    char dir[VSTDIRS_PATHLEN - 32];

    if (path[0]) return path;
    if (config_dir(dir, sizeof dir)) return path;   /* stays "" -- nowhere to keep it */
    snprintf(path, sizeof path, "%s/plugin-folders", dir);
    return path;
}

const char *vstdirs_os_label(const char *os)
{
    if (!os || !*os)                     return "Any platform";
    if (!strcmp(os, VSTDIRS_WINDOWS))    return "Windows";
    if (!strcmp(os, VSTDIRS_LINUX))      return "Linux";
    if (!strcmp(os, VSTDIRS_MACOS))      return "macOS";
    if (!strcmp(os, VSTDIRS_CLASSIC))    return "Mac OS 9";
    return "Any platform";
}

/* Only the four tags are accepted. Anything else in a hand-edited file becomes
 * "any" rather than a platform nothing will ever match, which would leave the
 * folder scanned but filed under a heading the dialog cannot show. */
static const char *os_norm(const char *os)
{
    if (!os || !*os) return VSTDIRS_ANY;
    if (!strcmp(os, VSTDIRS_WINDOWS)) return VSTDIRS_WINDOWS;
    if (!strcmp(os, VSTDIRS_LINUX))   return VSTDIRS_LINUX;
    if (!strcmp(os, VSTDIRS_MACOS))   return VSTDIRS_MACOS;
    if (!strcmp(os, VSTDIRS_CLASSIC)) return VSTDIRS_CLASSIC;
    return VSTDIRS_ANY;
}

/* Trailing newline off, trailing slash off, trailing spaces off. A path typed
 * or pasted into the file arrives with any of the three, and "/usr/lib/vst/"
 * and "/usr/lib/vst" being two entries would show the same plug-ins twice. */
static void tidy(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' ||
                 s[l - 1] == ' '  || s[l - 1] == '\t')) s[--l] = 0;
    while (l > 1 && s[l - 1] == '/') s[--l] = 0;
}

int vstdirs_load(vstdir *out, int max)
{
    char  line[VSTDIRS_PATHLEN + 32];
    FILE *f;
    int   n = 0, i;

    if (!out || max <= 0) return 0;
    if (!*vstdirs_file() || !(f = fopen(vstdirs_file(), "r"))) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *p = line, *path, *tab;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;

        /* "<platform>\t<path>". No tab means the whole line is the path, which
         * is what the first version of this file wrote -- those entries read
         * back as "any" rather than being thrown away. */
        if ((tab = strchr(p, '\t'))) { *tab = 0; path = tab + 1; }
        else                         { path = p; p = NULL; }
        while (*path == ' ' || *path == '\t') path++;
        tidy(path);
        if (!*path) continue;
        /* Deduped on the way in: two runs of an older build, or a hand edit,
         * could have left the same folder listed twice, and every caller would
         * then scan it twice and list its plug-ins twice. */
        for (i = 0; i < n; i++) if (!strcmp(out[i].path, path)) break;
        if (i < n) continue;
        snprintf(out[n].os,   sizeof out[0].os,   "%s", os_norm(p));
        snprintf(out[n].path, sizeof out[0].path, "%s", path);
        n++;
    }
    fclose(f);
    return n;
}

/* mkdir -p for the one directory we need. */
static int make_config_dir(void)
{
    char dir[VSTDIRS_PATHLEN], *slash;

    if (config_dir(dir, sizeof dir)) return -1;
    if (!(slash = strrchr(dir, '/'))) return -1;
    *slash = 0;
    mkdir(dir, 0755);          /* ~/.config, which normally exists already */
    *slash = '/';
    if (mkdir(dir, 0755) && errno != EEXIST) return -1;
    return 0;
}

int vstdirs_save(const vstdir *dirs, int n)
{
    char  tmp[VSTDIRS_PATHLEN + 8];
    FILE *f;
    int   i;

    if (!*vstdirs_file()) { errno = ENOENT; return -1; }
    if (make_config_dir()) return -1;
    snprintf(tmp, sizeof tmp, "%s.new", vstdirs_file());
    if (!(f = fopen(tmp, "w"))) return -1;
    fprintf(f, "# Folders vst-ace searches for plug-ins: <platform><TAB><path>.\n"
               "# Set in either window under Settings > Plug-in Folders.\n"
               "# The platform says what the folder is for; what each plug-in\n"
               "# actually is comes from its own binary, every time it is scanned.\n"
               "# The corpora beside the binary and the system VST directories\n"
               "# are found automatically and are not listed here.\n");
    for (i = 0; i < n; i++)
        if (dirs[i].path[0])
            fprintf(f, "%s\t%s\n", os_norm(dirs[i].os), dirs[i].path);
    if (fflush(f) || ferror(f)) { fclose(f); unlink(tmp); return -1; }
    fclose(f);
    /* Renamed over the old one rather than written in place: a full disk or a
     * kill halfway through then costs the new entry, not the whole list. */
    if (rename(tmp, vstdirs_file())) { unlink(tmp); return -1; }
    return 0;
}

/* Resolved, so ~/vst and /home/me/vst are one entry and a symlinked corpus is
 * not scanned twice. A path that does not resolve is kept as given -- it may
 * be on a drive that is not mounted right now, and refusing to remember it
 * would be worse than remembering something currently unreachable. */
static void canon(const char *in, char *out, size_t n)
{
    char real[PATH_MAX];

    if (realpath(in, real)) snprintf(out, n, "%s", real);
    else                    snprintf(out, n, "%s", in);
    tidy(out);
}

int vstdirs_add(const char *os, const char *dir)
{
    vstdir dirs[VSTDIRS_MAX];
    char   want[VSTDIRS_PATHLEN];
    int    n, i;

    if (!dir || !*dir) return -1;
    canon(dir, want, sizeof want);
    if (!*want) return -1;
    os = os_norm(os);
    n = vstdirs_load(dirs, VSTDIRS_MAX);
    for (i = 0; i < n; i++) {
        if (strcmp(dirs[i].path, want)) continue;
        /* Listed already. Under another platform it is a re-tag, not a second
         * entry -- one folder cannot be in two groups without being scanned
         * twice and listed twice. */
        if (!strcmp(dirs[i].os, os)) return 0;
        snprintf(dirs[i].os, sizeof dirs[i].os, "%s", os);
        return vstdirs_save(dirs, n) ? -1 : 1;
    }
    if (n >= VSTDIRS_MAX) { errno = ENOSPC; return -1; }
    snprintf(dirs[n].os,   sizeof dirs[0].os,   "%s", os);
    snprintf(dirs[n].path, sizeof dirs[0].path, "%s", want);
    n++;
    return vstdirs_save(dirs, n) ? -1 : 1;
}

/* A shared library by extension. */
static int module_ext(const char *name)
{
    size_t l = strlen(name);

    if (l > 3 && !strcasecmp(name + l - 3, ".so"))    return 1;
    if (l > 4 && !strcasecmp(name + l - 4, ".dll"))   return 1;
    if (l > 5 && !strcasecmp(name + l - 5, ".vst3"))  return 1;
    if (l > 6 && !strcasecmp(name + l - 6, ".dylib")) return 1;
    return 0;
}

/* The module in one directory of a bundle: the first library, or a file
 * named after the bundle (a macOS module carries no extension), or -- an
 * architecture directory holds essentially just the module -- the one
 * regular file there. */
static int first_module(const char *dir, const char *stem, char *out, size_t n)
{
    DIR           *d;
    struct dirent *de;
    char           fallback[VSTDIRS_PATHLEN];

    fallback[0] = 0;
    if (!(d = opendir(dir))) return -1;
    while ((de = readdir(d))) {
        char        p[VSTDIRS_PATHLEN];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        if (module_ext(de->d_name) ||
            (stem && !strncmp(de->d_name, stem, strlen(stem)))) {
            snprintf(out, n, "%s", p);
            closedir(d);
            return 0;
        }
        if (!fallback[0]) snprintf(fallback, sizeof fallback, "%s", p);
    }
    closedir(d);
    if (fallback[0]) { snprintf(out, n, "%s", fallback); return 0; }
    return -1;
}

/* The module a bundle directory runs, for identifying it. The standard
 * layout is Contents/<arch>-<os>/<module>; a flat bundle keeps the library
 * directly inside. -1 when this is no bundle, or none is found -- the id
 * then describes the directory itself, which is the same only as itself. */
static int bundle_module(const char *path, const struct stat *st,
                         char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');
    size_t      l     = strlen(base);
    char        stem[256], contents[VSTDIRS_PATHLEN];
    DIR           *d;
    struct dirent *de;

    if (!S_ISDIR(st->st_mode) || !dot) return -1;
    if (!(l > 5  && !strcasecmp(base + l - 5,  ".vst3")) &&
        !(l > 4  && !strcasecmp(base + l - 4,  ".vst")) &&
        !(l > 10 && !strcasecmp(base + l - 10, ".component")))
        return -1;
    snprintf(stem, sizeof stem, "%.*s", (int)(dot - base), base);
    snprintf(contents, sizeof contents, "%s/Contents", path);
    if ((d = opendir(contents))) {
        while ((de = readdir(d))) {
            char        sub[VSTDIRS_PATHLEN];
            struct stat sst;
            if (de->d_name[0] == '.') continue;
            snprintf(sub, sizeof sub, "%s/%s", contents, de->d_name);
            if (stat(sub, &sst) || !S_ISDIR(sst.st_mode)) continue;
            if (!first_module(sub, stem, out, n)) { closedir(d); return 0; }
        }
        closedir(d);
    }
    /* Flat: the library beside the bundle's resources. Only a library
     * counts here -- the top level also holds everything else. */
    if (first_module(path, NULL, out, n) == 0 && module_ext(strrchr(out, '/') + 1))
        return 0;
    return -1;
}

int vstdirs_identify(const char *path, vstdirs_id *id)
{
    struct stat st;
    char        mod[VSTDIRS_PATHLEN];

    memset(id, 0, sizeof *id);
    if (!path || stat(path, &st)) return -1;
    /* A bundle directory has no bytes of its own worth comparing -- a copy
     * has a different inode and that is all. Identify it by the module
     * inside instead, so a copied plug-in is recognised the way a copied
     * file already is. */
    if (!bundle_module(path, &st, mod, sizeof mod) && !stat(mod, &st))
        snprintf(id->mod, sizeof id->mod, "%s", mod);
    id->dev     = (unsigned long long)st.st_dev;
    id->ino     = (unsigned long long)st.st_ino;
    id->size    = (unsigned long long)st.st_size;
    id->regular = S_ISREG(st.st_mode);
    return 0;
}

int vstdirs_same_plugin(const char *a, const vstdirs_id *ia,
                        const char *b, const vstdirs_id *ib)
{
    /* What the ids describe: the module for a bundle, the candidate itself
     * otherwise. */
    const char *pa = ia->mod[0] ? ia->mod : a;
    const char *pb = ib->mod[0] ? ib->mod : b;
    FILE *fa, *fb;
    char  ba[65536], bb[65536];
    int   same = 1;

    if (!ia->ino && !ia->dev) return 0;              /* could not be stat'ed */
    if (ia->dev == ib->dev && ia->ino == ib->ino) return 1;
    if (!ia->regular || !ib->regular || ia->size != ib->size || !ia->size) return 0;
    /* A copy keeps its name. Asking for that as well is what keeps this cheap
     * on a collection built from one template -- hundreds of plug-ins of the
     * same size -- where comparing contents pairwise would read the lot. */
    {
        const char *na = strrchr(pa, '/'), *nb = strrchr(pb, '/');
        if (strcmp(na ? na + 1 : pa, nb ? nb + 1 : pb)) return 0;
    }
    if (!(fa = fopen(pa, "rb"))) return 0;
    if (!(fb = fopen(pb, "rb"))) { fclose(fa); return 0; }
    for (;;) {
        size_t na = fread(ba, 1, sizeof ba, fa);
        size_t nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na)) { same = 0; break; }
        if (na < sizeof ba) break;
    }
    fclose(fa);
    fclose(fb);
    return same;
}

int vstdirs_remove(const char *dir)
{
    vstdir dirs[VSTDIRS_MAX];
    char   want[VSTDIRS_PATHLEN];
    int    n, i, w = 0, hit = 0;

    if (!dir || !*dir) return -1;
    canon(dir, want, sizeof want);
    n = vstdirs_load(dirs, VSTDIRS_MAX);
    for (i = 0; i < n; i++) {
        /* Matched as written as well as resolved: an entry pointing at a drive
         * that is not mounted cannot be resolved, and has to stay removable. */
        if (!strcmp(dirs[i].path, want) || !strcmp(dirs[i].path, dir)) { hit = 1; continue; }
        if (w != i) dirs[w] = dirs[i];
        w++;
    }
    if (!hit) return 0;
    return vstdirs_save(dirs, w) ? -1 : 1;
}

int vstdirs_contains(const char *path)
{
    vstdir dirs[VSTDIRS_MAX];
    char want[PATH_MAX];
    int n, i;
    if (!path || !*path || !realpath(path, want)) return 0;
    n = vstdirs_load(dirs, VSTDIRS_MAX);
    for (i = 0; i < n; i++) {
        char root[PATH_MAX];
        size_t l;
        if (!realpath(dirs[i].path, root)) continue;
        l = strlen(root);
        if (!strncmp(want, root, l) && (want[l] == '/' || l == 1)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- hidden ---- */

const char *vstdirs_hidden_file(void)
{
    static char path[VSTDIRS_PATHLEN + 16];
    char *slash;
    snprintf(path, sizeof path, "%s", vstdirs_file());
    if ((slash = strrchr(path, '/'))) *slash = 0;
    snprintf(path + strlen(path), sizeof path - strlen(path), "/hidden-plugins");
    return path;
}

/* Read whole, a line a path. Scans ask once per candidate, so the list is
 * kept and only read again when the file's time changes. */
static int hidden_read(char (*out)[VSTDIRS_PATHLEN], int max)
{
    FILE *f = fopen(vstdirs_hidden_file(), "r");
    char line[VSTDIRS_PATHLEN + 8];
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        snprintf(out[n++], VSTDIRS_PATHLEN, "%s", line);
    }
    fclose(f);
    return n;
}

#define HIDDEN_MAX 4096
int vstdirs_hidden_list(char (*out)[VSTDIRS_PATHLEN], int max) { return hidden_read(out, max); }

int vstdirs_is_hidden(const char *path)
{
    static char (*cache)[VSTDIRS_PATHLEN];
    static int n;
    static time_t stamp;
    static long size = -1;
    struct stat st;
    int i;
    if (!path || !*path) return 0;
    if (stat(vstdirs_hidden_file(), &st) != 0) { n = 0; size = -1; return 0; }
    if (!cache && !(cache = malloc((size_t)HIDDEN_MAX * VSTDIRS_PATHLEN))) return 0;
    if (st.st_mtime != stamp || (long)st.st_size != size) {
        n = hidden_read(cache, HIDDEN_MAX);
        stamp = st.st_mtime;
        size = (long)st.st_size;
    }
    for (i = 0; i < n; i++) if (!strcmp(cache[i], path)) return 1;
    return 0;
}

static int hidden_write(char (*list)[VSTDIRS_PATHLEN], int n)
{
    char tmp[VSTDIRS_PATHLEN + 32], dir[VSTDIRS_PATHLEN + 16], *slash;
    FILE *f;
    int i, ok;
    snprintf(dir, sizeof dir, "%s", vstdirs_hidden_file());
    if ((slash = strrchr(dir, '/'))) {
        char *q;
        *slash = 0;
        /* The folder and its parents, as `mkdir -p`. */
        for (q = dir + 1; *q; q++)
            if (*q == '/') { *q = 0; mkdir(dir, 0700); *q = '/'; }
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof tmp, "%s.new", vstdirs_hidden_file());
    if (!(f = fopen(tmp, "w"))) return -1;
    for (i = 0; i < n; i++) fprintf(f, "%s\n", list[i]);
    ok = fclose(f) == 0;
    if (!ok || rename(tmp, vstdirs_hidden_file()) != 0) { unlink(tmp); return -1; }
    return 0;
}

int vstdirs_hide(const char *path)
{
    char (*list)[VSTDIRS_PATHLEN];
    int n, i, r = 1;
    if (!path || !*path || strchr(path, '\n')) return -1;
    if (!(list = malloc((size_t)(HIDDEN_MAX + 1) * VSTDIRS_PATHLEN))) return -1;
    n = hidden_read(list, HIDDEN_MAX);
    for (i = 0; i < n; i++) if (!strcmp(list[i], path)) r = 0;
    if (r) {
        if (n >= HIDDEN_MAX) r = -1;
        else {
            snprintf(list[n++], VSTDIRS_PATHLEN, "%s", path);
            if (hidden_write(list, n)) r = -1;
        }
    }
    free(list);
    return r;
}

int vstdirs_unhide(const char *path)
{
    char (*list)[VSTDIRS_PATHLEN];
    int n, i, j, r = 0;
    if (!path || !*path) return -1;
    if (!(list = malloc((size_t)HIDDEN_MAX * VSTDIRS_PATHLEN))) return -1;
    n = hidden_read(list, HIDDEN_MAX);
    for (i = j = 0; i < n; i++) {
        if (!strcmp(list[i], path)) { r = 1; continue; }
        if (i != j) memcpy(list[j], list[i], VSTDIRS_PATHLEN);
        j++;
    }
    if (r && hidden_write(list, j)) r = -1;
    free(list);
    return r;
}

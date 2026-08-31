/* See vstdirs.h. */
#include "vstdirs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

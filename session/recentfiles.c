#define _GNU_SOURCE
#include "recentfiles.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEEP 30                         /* what the file holds; menus show fewer */

static int conf_path(char *out, size_t n, int make_dir)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    char dir[1024];
    if (x && *x) snprintf(dir, sizeof dir, "%s/vst-ace", x);
    else if (h && *h) snprintf(dir, sizeof dir, "%s/.config/vst-ace", h);
    else return 0;
    if (make_dir) {
        char parent[1024], *slash;
        snprintf(parent, sizeof parent, "%s", dir);
        if ((slash = strrchr(parent, '/'))) { *slash = 0; mkdir(parent, 0700); }
        mkdir(dir, 0700);
    }
    snprintf(out, n, "%s/recent", dir);
    return 1;
}

static int read_all(recent_item *v, int max)
{
    char path[1100], line[sizeof v->kind + sizeof v->path + 4];
    FILE *f;
    int n = 0;
    if (!conf_path(path, sizeof path, 0) || !(f = fopen(path, "r"))) return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!tab) continue;
        *tab++ = 0;
        if (strcmp(line, "song") && strcmp(line, "session")) continue;
        if (tab[0] != '/') continue;
        snprintf(v[n].kind, sizeof v[n].kind, "%s", line);
        snprintf(v[n].path, sizeof v[n].path, "%s", tab);
        n++;
    }
    fclose(f);
    return n;
}

static void write_all(const recent_item *v, int n)
{
    char path[1100], tmp[1120];
    FILE *f;
    int i, bad;
    if (!conf_path(path, sizeof path, 1)) return;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return;
    if (fchmod(fileno(f), 0600) != 0) { /* the file holds paths; private where the system allows */ }
    for (i = 0; i < n; i++) fprintf(f, "%s\t%s\n", v[i].kind, v[i].path);
    bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, path) != 0) unlink(tmp);
}

int recent_list(recent_item *out, int max)
{
    recent_item *all = malloc(sizeof *all * KEEP);
    int n, i, k = 0;
    if (!all) return 0;
    n = read_all(all, KEEP);
    for (i = 0; i < n && k < max; i++)
        if (access(all[i].path, R_OK) == 0) out[k++] = all[i];
    free(all);
    return k;
}

void recent_add(const char *kind, const char *path)
{
    recent_item *all = malloc(sizeof *all * (KEEP + 1));
    char real[PATH_MAX];
    int n, i, k = 1;
    if (!all || !path || !*path || (strcmp(kind, "song") && strcmp(kind, "session"))) { free(all); return; }
    if (!realpath(path, real)) { free(all); return; }        /* a file that is not there is not recent */
    n = read_all(all + 1, KEEP);
    snprintf(all[0].kind, sizeof all[0].kind, "%s", kind);
    snprintf(all[0].path, sizeof all[0].path, "%s", real);
    for (i = 1; i <= n && k < KEEP; i++)
        if (strcmp(all[i].path, real)) all[k++] = all[i];
    write_all(all, k);
    free(all);
}

void recent_clear(void)
{
    write_all(NULL, 0);
}

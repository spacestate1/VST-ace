/* The files recently opened or saved -- songs and sessions -- kept in one list
 * shared by both studio shells (and between runs), in
 * ~/.config/vst-ace/recent: one "kind<TAB>path" per line, newest first. */
#ifndef RECENTFILES_H
#define RECENTFILES_H

#ifdef __cplusplus
extern "C" {
#endif

#define RECENT_SHOWN 12                 /* how many a menu offers */

typedef struct {
    char kind[8];                       /* "song" or "session" */
    char path[4096];                    /* absolute */
} recent_item;

/* The list, newest first, at most `max`. Files that are gone are left out of
 * what is returned (and stay in the file, in case a drive comes back). */
int  recent_list(recent_item *out, int max);

/* Put `path` at the front: made absolute, any earlier entry for it dropped. */
void recent_add(const char *kind, const char *path);

void recent_clear(void);

#ifdef __cplusplus
}
#endif
#endif

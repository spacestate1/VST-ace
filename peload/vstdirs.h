/* vstdirs -- the plug-in folders the user has set, kept where both windows
 * can see them.
 *
 * pestudio and dwstudio browse the same machine, so "where are my plug-ins"
 * has to have one answer: a folder added in the Qt window is one the GTK
 * window searches too, and neither of them is the authoritative copy. That is
 * what this file is -- one list, on disk, that both read at startup and both
 * write when the user changes it.
 *
 * Each folder carries the platform it holds, so a Windows corpus, a Linux one
 * and a macOS one are three separate entries rather than one undifferentiated
 * pile -- which is how people keep them, and how the tree here is laid out
 * (windows/VST2-64, linux/extracted, macos/VST2).
 *
 * That tag says what the folder is *for*. It does not decide what anything in
 * it is: every plug-in is sniffed when it is scanned, and its own binary
 * settles which loader runs it and which platform the browser files it under.
 * A Windows .dll dropped in the Linux folder is still listed as Windows. The
 * tag is there so the settings list is legible and so a folder can be added or
 * dropped a platform at a time; VSTDIRS_ANY is for a folder holding a mix, or
 * one the user did not care to label.
 *
 * The corpora found by walking up from the binary, and the system VST
 * directories, are *not* in here. Those are discovered every run and would
 * only go stale if written down; this is the list of places the user has
 * pointed us at, which is exactly the part a program cannot work out for
 * itself.
 *
 * Format: one folder per line, "<platform>\t<path>", '#' starting a comment,
 * blanks ignored. A line with no tab is read as VSTDIRS_ANY, which is what an
 * older version of this file wrote. Plain text rather than a settings database
 * because the thing it holds is a handful of paths, and being able to fix it
 * in an editor -- or see at a glance what a broken install is searching -- is
 * worth more here than any structure. */
#ifndef VSTDIRS_H
#define VSTDIRS_H

#ifdef __cplusplus
extern "C" {
#endif

#define VSTDIRS_MAX      64
#define VSTDIRS_PATHLEN  1024

/* The platform tags, as written in the file. Spelled the way pehost_info::os
 * spells them, so a folder's tag and a plug-in's detected platform compare
 * directly. */
#define VSTDIRS_WINDOWS  "windows"
#define VSTDIRS_LINUX    "linux"
#define VSTDIRS_MACOS    "macos"
#define VSTDIRS_ANY      "any"

typedef struct {
    char os[16];                   /* one of the four above */
    char path[VSTDIRS_PATHLEN];
} vstdir;

/* The config file's own path, e.g. ~/.config/vst-ace/plugin-folders. Always
 * returns something so a dialog can show it whether or not it exists yet. */
const char *vstdirs_file(void);

/* "windows" -> "Windows". For the settings dialogs, so both spell it alike. */
const char *vstdirs_os_label(const char *os);

/* Read the list into `out`, at most `max` entries, and return how many. Never
 * fails: a missing or unreadable file is an empty list, which is the correct
 * starting state. Entries are returned as written, including any that no
 * longer exist -- a folder on a disconnected drive is still the user's
 * setting, and silently dropping it would lose it on the next save. */
int vstdirs_load(vstdir *out, int max);

/* Write the list back. Returns 0, or -1 with errno set. Written to a temporary
 * file and renamed, so an interrupted save cannot leave a half-written list
 * where a good one was. */
int vstdirs_save(const vstdir *dirs, int n);

/* Add one folder under a platform, or remove one whatever its platform.
 * `os` may be NULL, meaning VSTDIRS_ANY. `dir` is stored resolved, so the same
 * folder reached by two paths is one entry. Adding a folder that is already
 * listed under a different platform re-tags it rather than duplicating it.
 *
 * add returns 1 when the list changed, 0 when it already said exactly this,
 * -1 on error; remove returns 1 when it was there, 0 when it was not, -1 on
 * error. */
int vstdirs_add(const char *os, const char *dir);
int vstdirs_remove(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* VSTDIRS_H */

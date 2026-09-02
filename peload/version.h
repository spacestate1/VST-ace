/* What this build is, for the About boxes and anything else that has to say so.
 *
 * The three values are supplied by CMake. The defaults below are what a
 * compile outside the build system gets, so nothing fails to build without
 * them -- it just cannot claim to know.
 *
 * The date comes from SOURCE_DATE_EPOCH when the environment sets one, which
 * is what Debian's and Fedora's build tooling does to keep packages
 * reproducible: two builds of the same source then produce byte-identical
 * binaries rather than differing in a string nobody reads. __DATE__ would have
 * been simpler and would have broken exactly that. */
#ifndef PELOAD_VERSION_H
#define PELOAD_VERSION_H

#include <stdio.h>

#ifndef VSTACE_VERSION
#define VSTACE_VERSION "0.2.0"
#endif

#ifndef VSTACE_BUILD_DATE
#define VSTACE_BUILD_DATE "unknown"
#endif

/* The commit this was built from, short form, or empty when the source is not
 * a git checkout -- an unpacked release tarball, which is how a distribution
 * builds it. */
#ifndef VSTACE_GIT
#define VSTACE_GIT ""
#endif

/* "vst-ace 0.1.0 (06a9e5e), built 2026-08-29" -- the commit part is dropped
 * when there is none to name. */
static inline const char *vstace_version_line(void)
{
    static char line[160];
    if (!line[0]) {
        if (VSTACE_GIT[0])
            snprintf(line, sizeof line, "vst-ace %s (%s), built %s",
                     VSTACE_VERSION, VSTACE_GIT, VSTACE_BUILD_DATE);
        else
            snprintf(line, sizeof line, "vst-ace %s, built %s",
                     VSTACE_VERSION, VSTACE_BUILD_DATE);
    }
    return line;
}

#endif /* PELOAD_VERSION_H */

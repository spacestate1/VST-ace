/* dwplay's command line.
 *
 * Everything this does is turn argv into a dwplay_opts and read the two blobs
 * off disk; the program itself is dwplay.c. The split exists so `dw` can run
 * the same live loop without starting a second process for it. */

#include "dwplay.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: dwplay -w <WAVEDST|plugin.dll> [-b <PROGINIT>]"
                    " [-p <n>] [-o <capture.wav>] [-c <1-16>]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *wpath = NULL, *bpath = NULL;
    dwplay_opts o;
    unsigned char *wraw = NULL, *braw = NULL;
    size_t wsize = 0, bsize = 0;
    int i, rc;

    memset(&o, 0, sizeof o);
    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-w") && i + 1 < argc) wpath = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bpath = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) o.program      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) o.record_path  = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) o.midi_channel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") || !strcmp(argv[i], "-k"))
            return dwplay_keyboard_info();
        else return usage();
    }
    if (!wpath) return usage();

    if (!(wraw = rom_wavedst(wpath, &wsize))) return 1;
    if (bpath && !(braw = rom_slurp(bpath, &bsize))) { free(wraw); return 1; }

    o.wavedst       = wraw;
    o.wavedst_size  = wsize;
    o.proginit      = braw;
    o.proginit_size = bsize;
    o.proginit_name = bpath;

    rc = dwplay_run(&o);
    free(wraw);
    free(braw);
    return rc;
}

/* Inspect and load a macOS plugin bundle. The first thing to point at a new
 * Mach-O image: it says what mapped, what bound, what is still missing, and
 * what the bundle exports -- which is how you find an entry point whose name
 * you do not know in advance. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "machoload.h"

static int g_show_all;

static void show_stub(const char *name, unsigned long calls, void *ud)
{
    int *n = ud;
    if (!g_show_all && *n >= 20) { (*n)++; return; }
    printf("    %-56s %s\n", name, calls ? "CALLED" : "");
    (*n)++;
}

static void show_export(const char *name, void *addr, void *ud)
{
    int *n = ud;
    if (!g_show_all && *n >= 20) { (*n)++; return; }
    printf("    %-56s %p\n", name, addr);
    (*n)++;
}

/* An AU's entry point is <Name>Factory and a VST2's is VSTPluginMain, so guess
 * from the export list rather than requiring the caller to know. */
static void find_entry(const char *name, void *addr, void *ud)
{
    struct { const char *name; void *addr; } *best = ud;
    size_t l = strlen(name);
    /* Three conventions in the wild: VST2 exports VSTPluginMain, a modern AU
     * exports <Name>Factory, and an older Component Manager AU exports
     * <Name>_Entry. */
    if (!strcmp(name, "_VSTPluginMain") || !strcmp(name, "_main_macho") ||
        (l > 7 && !strcmp(name + l - 7, "Factory")) ||
        (l > 6 && !strcmp(name + l - 6, "_Entry")))
        if (!best->addr) { best->name = name; best->addr = addr; }
}

int main(int argc, char **argv)
{
    /* Unbuffered: a plugin that faults takes the process down before an
     * exit flush, and the lost lines are exactly the ones that say how far
     * it got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = NULL;
    int i, run_init = 0, want_exports = 0, want_stubs = 0, want_factory = 0;
    const char *entry_name = NULL;
    void *entry_addr = NULL;
    macho *m;
    int resolved, stubbed, reached, n;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--init"))         run_init = 1;
        else if (!strcmp(argv[i], "--exports")) want_exports = 1;
        else if (!strcmp(argv[i], "--stubs"))   want_stubs = 1;
        else if (!strcmp(argv[i], "--all"))     g_show_all = 1;
        else if (!strcmp(argv[i], "--factory")) { want_factory = 1; run_init = 1; }
        else if (argv[i][0] != '-')             path = argv[i];
    }
    if (!path) {
        fprintf(stderr,
            "usage: macload <Plugin.vst|Plugin.component|macho-file> [options]\n"
            "  --exports   list exported symbols\n"
            "  --stubs     list imports with no implementation\n"
            "  --init      run the image's initialisers (executes plugin code)\n"
            "  --all       do not truncate the listings\n"
            "\n"
            "MACHO_VERBOSE=1 traces mapping and binding.\n");
        return 2;
    }

    if (!(m = macho_open(path))) {
        fprintf(stderr, "%s\n", macho_last_error());
        return 1;
    }
    printf("%s\n", path);
    if (macho_bundle_name(m)[0] || macho_bundle_id(m)[0])
        printf("  bundle: %s  (%s)\n", macho_bundle_name(m), macho_bundle_id(m));

    macho_import_stats(m, &resolved, &stubbed, &reached);
    printf("  imports: %d resolved against host libraries, %d unimplemented\n",
           resolved, stubbed);

    {   struct { const char *name; void *addr; } e = { NULL, NULL };
        macho_each_export(m, find_entry, &e);
        entry_name = e.name; entry_addr = e.addr;
        if (e.addr) printf("  entry point: %s at %p\n", e.name, e.addr);
        else        printf("  entry point: none recognised among %s\n",
                           "VSTPluginMain / *Factory");
    }

    if (want_exports) {
        printf("  exports:\n");
        n = 0; macho_each_export(m, show_export, &n);
        if (!g_show_all && n > 20) printf("    ... %d more (--all)\n", n - 20);
    }
    if (want_stubs) {
        printf("  unimplemented imports:\n");
        n = 0; macho_each_stub(m, show_stub, &n);
        if (!g_show_all && n > 20) printf("    ... %d more (--all)\n", n - 20);
    }

    if (run_init) {
        printf("  running initialisers...\n");
        fflush(stdout);
        macho_run_init(m);
    }

    if (want_factory && entry_addr) {
        /* An AU's factory takes an AudioComponentDescription and returns an
         * AudioComponentPlugInInterface: three function pointers (Open, Close,
         * Lookup) plus a reserved word. Calling it is the first thing that
         * executes real plugin logic rather than just its static setup. */
        struct { uint32_t type, subtype, manufacturer, flags, flagsmask; } desc;
        struct plugin_iface {
            int32_t (*Open)(void *self, void *inst);
            int32_t (*Close)(void *self);
            void *(*Lookup)(int16_t selector);
            void *reserved;
        } *iface;
        void *(*factory)(const void *) = entry_addr;

        memset(&desc, 0, sizeof desc);
        desc.type = 0x61756678;                 /* 'aufx' -- an effect */
        printf("  calling %s...\n", entry_name);
        fflush(stdout);
        iface = factory(&desc);
        if (!iface) {
            printf("  factory returned NULL\n");
        } else {
            printf("  interface at %p: Open=%p Close=%p Lookup=%p\n",
                   (void *)iface, (void *)iface->Open, (void *)iface->Close,
                   (void *)iface->Lookup);
            if (iface->Lookup) {
                int s, found = 0;
                printf("  selectors the plugin answers:");
                for (s = 0; s < 0x20; s++)
                    if (iface->Lookup((int16_t)s)) { printf(" %d", s); found++; }
                printf("   (%d)\n", found);
            }
        }
    }

    if (run_init) {
        macho_import_stats(m, &resolved, &stubbed, &reached);
        printf("  %d stub(s) actually called\n", reached);
    }

    macho_close(m);
    return 0;
}

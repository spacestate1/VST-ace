/* tracker-gtk -- the standalone GTK 4 window: a trackerview in a window of
 * its own. Everything else lives in trackerview.c; a shell embeds the same
 * view without this wrapper. */
#include "trackerview.h"

#include <stdio.h>

static void activate(GtkApplication *app, gpointer u)
{
    trk_view *v = u;
    const char *song = g_object_get_data(G_OBJECT(app), "song");
    trk_view_standalone(v, app, song);
#ifdef TRACKER_UITEST
    trk_view_uitest(v, g_object_get_data(G_OBJECT(app), "outdir"));
#endif
}

int main(int argc, char **argv)
{
    char err[256];
    int rc;
    char *args[] = { argv[0], NULL };
    GtkApplication *app;
    trk_engine *e;
    trk_view *v;

    if (!(e = trk_open(err, sizeof err))) {
        fprintf(stderr, "tracker: %s\n", err);
        return 1;
    }
    v = trk_view_new(e);
    /* Not unique: two trackers driving different windows is a reasonable
     * thing to want, and GApplication would otherwise hand the second
     * launch to the first and exit. */
    app = gtk_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_object_set_data(G_OBJECT(app), "song", argc > 1 ? argv[1] : NULL);
#ifdef TRACKER_UITEST
    g_object_set_data(G_OBJECT(app), "outdir", argc > 2 ? argv[2] : ".");
#endif
    g_signal_connect(app, "activate", G_CALLBACK(activate), v);
    rc = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    trk_close(e);            /* releases anything still sounding */
    trk_view_free(v);
#ifdef TRACKER_UITEST
    return trk_view_uitest_failures();
#endif
    return rc;
}

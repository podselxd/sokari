/* La captura en Linux (GNOME): la toma la extensión, sin las ventanas de
   Sokari, y aquí se lee y se borra. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux/gnome.h"
#include "pantalla.h"
#include "util.h"

uint8_t *screen_grab(int *w, int *h, char **why)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "sokari", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "pantalla.png", NULL);
    g_free(dir);
    bool ok = false;
    GnomeStatus st = gnome_screenshot(path, &ok);
    uint8_t *rgb = NULL;
    if (st != GN_OK || !ok) {
        if (why) *why = xstrdup(st == GN_OK ? "GNOME no la dejó tomar" : gnome_status_message(st));
    } else {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
        if (pb) {
            int W = gdk_pixbuf_get_width(pb), H = gdk_pixbuf_get_height(pb), n = gdk_pixbuf_get_n_channels(pb);
            int stride = gdk_pixbuf_get_rowstride(pb);
            const guchar *px = gdk_pixbuf_get_pixels(pb);
            rgb = xmalloc((size_t)W * H * 3);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) memcpy(rgb + ((size_t)y * W + x) * 3, px + (size_t)y * stride + x * n, 3);
            *w = W;
            *h = H;
            g_object_unref(pb);
        } else if (why) {
            *why = xstrdup("no pude leer la captura");
        }
    }
    g_unlink(path); /* no se queda guardada */
    g_free(path);
    return rgb;
}

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

/* PDF en Linux: pdftotext (poppler-utils, viene en Ubuntu). */
#include "linux/proc.h"
#include "lector.h"
char *pdf_text(const wchar_t *path, char **why)
{
    char *p = wide_to_utf8(path), *bin = proc_which("pdftotext");
    char *out = NULL;
    if (!bin) {
        if (why) *why = xstrdup("Para leer PDF necesito pdftotext: sudo apt install poppler-utils");
    } else {
        const char *argv[] = {bin, "-q", "-enc", "UTF-8", p, "-", NULL};
        int code = -1;
        out = proc_run(argv, NULL, 0, 20000, 2 * 1024 * 1024, NULL, &code);
        if (code != 0) {
            free(out);
            out = NULL;
            if (why) *why = xstrdup("No pude leer ese PDF.");
        }
    }
    free(bin);
    free(p);
    return out;
}

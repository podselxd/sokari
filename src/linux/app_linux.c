/* app.h en Linux. Con la ventana abierta (sokari sin opciones), lo que pasa
   va a la esfera, a los subtítulos y a los avisos de GNOME; en modo voz o
   texto (sokari --voz, --texto) los avisos salen en la terminal. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "linux/gnome.h"
#include "linux/linux.h"
#include "log.h"

static volatile LONG g_state;
static volatile LONG g_quit;

void app_set_state(JvState s)
{
    InterlockedExchange(&g_state, (LONG)s);
    if (ui_active()) ui_post_state((int)s);
}

JvState app_get_state(void)
{
    return (JvState)InterlockedCompareExchange(&g_state, 0, 0);
}

void app_set_level(float level)
{
    if (ui_active()) ui_post_level(level);
}

void app_subtitle(bool from_user, const char *text)
{
    if (ui_active()) ui_post_subtitle(from_user, text);
}

void app_status(const char *text)
{
    if (ui_active()) ui_post_status(text);
}

void app_notify(const char *title, const char *text)
{
    if (ui_active()) {
        ui_post_notify(title, text);
        return;
    }
    printf("\n[%s] %s\n", title ? title : "Sokari", text ? text : "");
    fflush(stdout);
}

bool app_is_own_window(HWND h)
{
    (void)h;
    return false;
}

/* Si la ventana de Sokari está enfrente (le diste a Hablar), la que usabas
   antes vuelve al frente: ahí es donde escribe, oprime o cierra lo que le
   pides. Las ventanas de Sokari nunca salen en la lista de GNOME. */
void app_yield_focus(void)
{
    if (ui_active()) ui_post_yield();
    if (!ui_active() || !ui_own_window_active()) return;
    GnomeWindow *w;
    int n;
    if (gnome_list_windows(&w, &n) != GN_OK) return;
    for (int i = 0; i < n; i++) {
        if (w[i].minimized) continue;
        bool ok = false;
        gnome_activate(w[i].id, &ok);
        break;
    }
    gnome_windows_free(w, n);
    for (int i = 0; i < 10 && ui_own_window_active(); i++) Sleep(30);
}

void app_request_quit(void)
{
    InterlockedExchange(&g_quit, 1);
    if (ui_active()) ui_post_quit();
}

bool app_quit_requested(void)
{
    return InterlockedCompareExchange(&g_quit, 1, 1) != 0;
}

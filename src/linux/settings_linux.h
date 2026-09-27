#ifndef SOKARI_SETTINGS_LINUX_H
#define SOKARI_SETTINGS_LINUX_H

/* La Configuración de Sokari en Linux (settings_linux.c). */

#include <gtk/gtk.h>
#include <stdbool.h>

/* Las secciones, las mismas que en Windows. */
typedef enum {
    SET_ACCOUNT,
    SET_DISPLAY,
    SET_AUDIO,
    SET_GENERAL,
    SET_SKILLS,
    SET_DEVICES,
    SET_AI,
    SET_PAGES
} SettingsPage;

/* La abre en esa sección (o, si ya está abierta, la trae al frente ahí).
   first_run: la primera vez, sin API key; arriba dice qué falta. */
void settings_linux_open(GtkWindow *parent, bool first_run, SettingsPage page);
/* ¿Está abierta? (mientras, la ventana de Sokari no se esconde sola) */
bool settings_linux_is_open(void);

/* Lo que la Configuración le pide a la ventana de Sokari (ui_linux.c):
   Probar la animación de entrar y salir, aplicar lo que se guardó, y avisar
   si la primera vez se cerró sin API key. */
void ui_linux_preview_appear(int anim);
void ui_linux_settings_saved(bool first_run);
void ui_linux_settings_cancelled(bool first_run);

#endif

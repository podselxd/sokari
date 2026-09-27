#ifndef SOKARI_UI_H
#define SOKARI_UI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

#define WM_APP_TRAY (WM_APP + 1)
#define WM_APP_NOTIFY (WM_APP + 2)
#define WM_APP_YIELD (WM_APP + 3)
#define WM_APP_QUIT (WM_APP + 4)
#define WM_APP_SHOWHUD (WM_APP + 5)
#define WM_APP_SETTINGS (WM_APP + 6)
#define WM_APP_CONFIG (WM_APP + 7)
#define WM_APP_HOME (WM_APP + 8)

/* Abrir el exe otra vez la busca para mostrar la ventana de Inicio. */
#define SOKARI_MSG_CLASS L"SokariMessageWindow"

enum {
    IDM_TALK = 1001,
    IDM_TOGGLE_HUD,
    IDM_MUTE,
    IDM_SETTINGS,
    IDM_OPEN_DATA,
    IDM_QUIT,
    IDM_HOME,
    IDM_FULL_ACCESS,
    IDM_MODE_BASE = 1100, /* + modo de pantalla (DisplayMode) */
};

typedef void (*SettingsSavedFn)(bool first_run);

/* show_hud = false: la esfera queda oculta hasta ui_show_hud() (cuando se
   abre la ventana de Inicio o la primera configuración). */
bool ui_init(HINSTANCE inst, SettingsSavedFn on_saved, bool show_hud);
int ui_run(void);
void ui_config_changed(void);
/* QUIET: al arrancar con Windows o al cambiar de modo, sin quitarle el foco a
   nadie. FRONT: la pediste (bandeja, Mostrar): se restaura y pasa al frente.
   STARTED: le diste a Iniciar: en modo Ventana toma el foco (así F11 funciona
   de una vez); en Minimizado se queda minimizada. */
typedef enum { HUD_SHOW_QUIET = 0, HUD_SHOW_FRONT = 1, HUD_SHOW_STARTED = 3 } HudShow;
void ui_show_hud(HudShow how);
void ui_set_display_mode(int mode);
/* El botón Probar de Configuración: la esfera sale y vuelve a entrar (o, si
   está oculta, se asoma y se va) con esa animación (SphereAnim). false: no
   hay esfera que mostrar (Minimizado, o Sokari sin iniciar). */
bool ui_preview_appear(int anim);
/* Clic en la esfera: ¿cae en la esfera? (en pantalla completa o ventana, el
   círculo del centro; la esfera flotante es toda la ventana). Y ¿del botón
   abajo al botón arriba se movió tan poco que fue un clic y no arrastrar? */
bool ui_click_on_sphere(int mode, int x, int y, int cw, int ch);
bool ui_is_click(int dx, int dy);
/* ¿Un clic en la esfera la calla en este estado? Solo si habla o piensa. */
bool ui_click_silences(int state);
HWND ui_message_window(void);
HICON ui_app_icon(int size);

/* tray.c */
bool tray_init(HWND owner, HICON icon);
void tray_readd(void);
void tray_set_tooltip(const wchar_t *text);
void tray_notify(const wchar_t *title, const wchar_t *text);
void tray_show_menu(HWND owner, bool hud_visible, bool muted, int display_mode);
void tray_remove(void);

/* ui_settings.c */
void settings_open(HINSTANCE inst, bool first_run, SettingsSavedFn on_saved);
/* Ventana de Inicio (la misma ventana, en la sección Inicio). starting: Sokari
   todavía no arranca; si la cierras sin darle a Iniciar, Sokari se cierra.
   on_start se llama al darle a Iniciar (como después de guardar). */
void home_open(HINSTANCE inst, bool starting, SettingsSavedFn on_start);
HWND settings_window(void);
/* Pruebas: cuántos píxeles se mete la sección abierta en Guardar/Cancelar (0: cabe). */
int settings_overflow(void);
/* Si la ventana está abierta, vuelve a leer el modo de pantalla y el estado
   del micrófono (cambiados desde la bandeja). */
void settings_sync(void);
/* Nombres de los modos de pantalla, para la bandeja y la ventana. */
extern const wchar_t *const DISPLAY_MODE_LABELS[];

#endif

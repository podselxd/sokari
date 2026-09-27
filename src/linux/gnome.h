/* Lo que Sokari le pide a su extensión de GNOME (linux/extension/): en
   Wayland un programa no puede ver las ventanas de los demás, oprimirles
   teclas ni usar el portapapeles por su cuenta. Todo va por D-Bus a
   org.gnome.Shell, así que ningún otro programa puede hacerse pasar por ella. */
#ifndef SOKARI_LINUX_GNOME_H
#define SOKARI_LINUX_GNOME_H

#include <stdbool.h>
#include <stdint.h>

#define SOKARI_EXTENSION_UUID "sokari@podselxd.github.io"

typedef enum {
    GN_OK = 0,
    GN_NO_GNOME,     /* no hay GNOME (otro escritorio, o sin sesión gráfica) */
    GN_NO_EXTENSION, /* GNOME sí, pero la extensión no está prendida */
    GN_LOCKED,       /* la pantalla está bloqueada */
    GN_DENIED,       /* la extensión no reconoce a este programa */
    GN_ERROR,
} GnomeStatus;

typedef struct {
    uint64_t id; /* 0: ninguna */
    char *app;   /* "Firefox", "Archivos"; "" si GNOME no sabe */
    char *app_id; /* "firefox_firefox.desktop" */
    char *wm_class;
    char *title; /* lo pone cada app o página: nunca se le repite al modelo en un resultado */
    char *categories; /* las del .desktop: "GNOME;GTK;Utility;TerminalEmulator;" */
    int pid;
    bool focused, minimized, above, terminal;
} GnomeWindow;

/* Qué decir cuando no se pudo (estático). */
const char *gnome_status_message(GnomeStatus s);

/* Tus ventanas de todos los escritorios, la que usaste al último primero.
   Las de Sokari no vienen. */
GnomeStatus gnome_list_windows(GnomeWindow **out, int *n);
void gnome_windows_free(GnomeWindow *w, int n);
void gnome_window_clear(GnomeWindow *w);
/* La ventana con el foco (id 0 si ninguna) y si lo tiene el propio GNOME:
   la vista de actividades, «Ejecutar» (Alt+F2), un menú o un diálogo. */
GnomeStatus gnome_focused(GnomeWindow *out, bool *shell_ui);
GnomeStatus gnome_activate(uint64_t id, bool *ok);
/* "minimize", "maximize" o "close". */
GnomeStatus gnome_window_action(uint64_t id, const char *action, bool *ok);
GnomeStatus gnome_minimize_all(int *count);
/* Oprime las teclas (keysyms de X, primero los modificadores) times veces.
   expect: la ventana que tiene que seguir enfrente (0: teclas del sistema,
   como Alt+Tab). status (heap): "ok", "focus" o "focus:N" (cambiaste de
   ventana después de N veces), "terminal", "shell", "locked". */
GnomeStatus gnome_press_keys(uint64_t expect, const uint32_t *keyvals, int n, int times, char **status);
/* Escribe pegando (así salen acentos y ñ con cualquier teclado) y regresa lo
   que tenías copiado. status como gnome_press_keys. */
GnomeStatus gnome_paste(uint64_t expect, const char *text, char **status);
GnomeStatus gnome_clipboard_get(char **text);
GnomeStatus gnome_clipboard_set(const char *text);
/* Copia el archivo como lo copia Archivos; kind (heap): "image" o "uri-list". */
GnomeStatus gnome_clipboard_set_file(const char *path, char **kind);
/* Abre la app de ese .desktop (con esas direcciones) o, si ya estaba
   abierta y no le pasas ninguna, trae su ventana. status (heap): "launched",
   "focused" o "missing". */
GnomeStatus gnome_launch_app(const char *desktop_id, const char *const *uris, char **status);

/* Una ventana del propio Sokari (por su título): "above" (encima de todo y
   en todos los escritorios), "normal", "move" (a x, y) o "where". *x, *y:
   dónde quedó (pueden ser NULL). ok false si no la encontró. */
GnomeStatus gnome_own_window(const char *title, const char *action, int x, int y, int *out_x, int *out_y, bool *ok);

/* Al arrancar: si hay GNOME y la extensión no está prendida, la prende (si
   GNOME todavía no la conoce, queda para el siguiente inicio de sesión). No
   la toca si tú la apagaste. */
void gnome_extension_enable(void);
/* ¿Es una sesión de GNOME (Ubuntu, Fedora)? */
bool gnome_desktop(void);
/* Bloquea la pantalla: GNOME o, si no, logind. */
bool session_lock(void);

#endif

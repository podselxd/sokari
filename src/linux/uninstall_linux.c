/* Desinstalar Sokari en Linux (ver uninstall.h): el clic derecho en su ícono
   («Desinstalar Sokari») corre «sokari --desinstalar». El paquete se quita
   con apt o dnf y pide tu contraseña. */
#include <windows.h>

#include <ftw.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autostart.h"
#include "config.h"
#include "linux/linux.h"
#include "linux/proc.h"
#include "log.h"
#include "uninstall.h"
#include "util.h"

void uninstall_register(void)
{
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    remove(path);
    return 0;
}

/* Solo carpetas de Sokari: con ruta completa, que terminen en /sokari y que
   no sean tu carpeta personal. */
static void remove_tree(const wchar_t *wdir)
{
    char *dir = wdir ? wide_to_utf8(wdir) : NULL;
    const char *home = getenv("HOME");
    size_t n = dir ? strlen(dir) : 0;
    if (dir && dir[0] == '/' && n > 7 && !strcmp(dir + n - 7, "/sokari") && (!home || strcmp(dir, home)))
        nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    free(dir);
}

/* La que está abierta se cierra sola (su acción «salir»). */
static void quit_running(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) return;
    GVariant *r = g_dbus_connection_call_sync(bus, "io.github.podselxd.Sokari", "/io/github/podselxd/Sokari",
                                              "org.gtk.Actions", "Activate",
                                              g_variant_new("(sava{sv})", "salir", NULL, NULL), NULL,
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL);
    if (r) g_variant_unref(r);
    for (int i = 0; i < 50; i++) {
        GVariant *o = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "NameHasOwner",
                                                  g_variant_new("(s)", "io.github.podselxd.Sokari"),
                                                  G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        gboolean has = FALSE;
        if (o) {
            g_variant_get(o, "(b)", &has);
            g_variant_unref(o);
        }
        if (!has) break;
        g_usleep(100 * 1000);
    }
    g_object_unref(bus);
}

static void tell(GtkMessageType type, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, type, GTK_BUTTONS_OK, "%s", text);
    gtk_window_set_title(GTK_WINDOW(d), "Sokari");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

int uninstall_run(void)
{
    const char *kind = linux_package_kind();
    if (!gtk_init_check(NULL, NULL)) {
        printf("Para desinstalar Sokari: %s\n", kind && !strcmp(kind, "rpm") ? "sudo dnf remove sokari"
                                                                            : "sudo apt remove sokari");
        return 1;
    }
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "¿Desinstalar Sokari?");
    gtk_window_set_title(GTK_WINDOW(d), "Desinstalar Sokari");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
                                             "Se cierra, deja de abrirse al iniciar tu sesión y se quita el "
                                             "programa (pide tu contraseña).");
    GtkWidget *data = gtk_check_button_new_with_label("Borrar también mis datos (configuración, memoria, notas y "
                                                      "skills)");
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d))), data, FALSE, FALSE, 6);
    gtk_widget_show(data);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancelar", GTK_RESPONSE_CANCEL);
    GtkWidget *go = gtk_dialog_add_button(GTK_DIALOG(d), "Desinstalar", GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(go), "destructive-action");
    int resp = gtk_dialog_run(GTK_DIALOG(d));
    bool wipe = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data));
    gtk_widget_destroy(d);
    if (resp != GTK_RESPONSE_ACCEPT) return 0;

    quit_running();
    autostart_set(false);
    int code = -1;
    if (kind) {
        const char *deb[] = {"pkexec", "apt-get", "remove", "-y", "sokari", NULL};
        const char *rpm[] = {"pkexec", "dnf", "remove", "-y", "sokari", NULL};
        free(proc_run(!strcmp(kind, "deb") ? deb : rpm, NULL, 0, 10 * 60 * 1000, 1 << 20, NULL, &code));
    }
    if (code != 0) {
        tell(GTK_MESSAGE_WARNING, kind ? "No se desinstaló (¿cancelaste la contraseña?). Sokari sigue instalado."
                                       : "Este Sokari no se instaló con un paquete: bórralo a mano de donde lo "
                                         "compilaste o copiaste.");
        return 1;
    }
    if (wipe) {
        remove_tree(g_paths.local_dir);
        remove_tree(g_paths.memory_dir);
        char *cache = g_build_filename(g_get_user_cache_dir(), "sokari", NULL);
        wchar_t *wc = utf8_to_wide(cache);
        remove_tree(wc);
        free(wc);
        g_free(cache);
    }
    tell(GTK_MESSAGE_INFO, wipe ? "Listo: Sokari se desinstaló y se borraron tus datos."
                                : "Listo: Sokari se desinstaló. Tus datos se quedaron por si lo vuelves a instalar.");
    return 0;
}

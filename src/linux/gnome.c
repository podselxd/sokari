/* La mitad de Sokari de la extensión de GNOME: llamadas por D-Bus a
   org.gnome.Shell, con la respuesta ya en C. Ver gnome.h. */
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linux/gnome.h"
#include "linux/proc.h"
#include "log.h"
#include "third_party/cJSON.h"
#include "util.h"

#define SHELL_NAME "org.gnome.Shell"
#define OBJECT_PATH "/org/gnome/Shell/Extensions/Sokari"
#define IFACE "io.github.podselxd.SokariShell1"

const char *gnome_status_message(GnomeStatus s)
{
    switch (s) {
    case GN_OK: return "";
    case GN_NO_GNOME:
        return "No encontré GNOME: en Linux, ver y manejar ventanas, oprimir teclas y usar el portapapeles por ahora "
               "solo funciona en el escritorio GNOME (el de Ubuntu y Fedora).";
    case GN_NO_EXTENSION:
        return "Para eso necesito la extensión de Sokari en GNOME y no está prendida. Si acabas de instalar Sokari, "
               "cierra sesión y vuelve a entrar una vez; si sigue igual, préndela en la app Extensiones.";
    case GN_LOCKED: return "La pantalla está bloqueada: desbloquéala y vuelve a pedírmelo.";
    case GN_DENIED:
        return "La extensión de Sokari en GNOME no me dejó: solo atiende al Sokari instalado en el sistema "
               "(/usr/bin/sokari).";
    case GN_ERROR: break;
    }
    return "No pude hablar con GNOME para hacerlo.";
}

bool gnome_desktop(void)
{
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    return d && str_contains_ci(d, "gnome");
}

static GDBusConnection *session_bus(void)
{
    GError *err = NULL;
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!c) {
        log_msg("GNOME: sin bus de sesión (%s).", err ? err->message : "?");
        g_clear_error(&err);
    }
    return c;
}

static bool screen_locked(GDBusConnection *c)
{
    GVariant *r = g_dbus_connection_call_sync(c, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
                                              "org.gnome.ScreenSaver", "GetActive", NULL, G_VARIANT_TYPE("(b)"),
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    gboolean on = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &on);
        g_variant_unref(r);
    }
    return on;
}

/* params se consume (flotante). *out: la respuesta (unref) o NULL. */
static GnomeStatus call(const char *method, GVariant *params, const char *reply_type, int timeout_ms, GVariant **out)
{
    *out = NULL;
    GDBusConnection *c = session_bus();
    if (!c) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        return GN_NO_GNOME;
    }
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(c, SHELL_NAME, OBJECT_PATH, IFACE, method, params,
                                              G_VARIANT_TYPE(reply_type), G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout_ms,
                                              NULL, &err);
    GnomeStatus st = GN_OK;
    if (!r) {
        char *remote = err ? g_dbus_error_get_remote_error(err) : NULL;
        const char *e = remote ? remote : "";
        if (!strcmp(e, "org.freedesktop.DBus.Error.ServiceUnknown") ||
            !strcmp(e, "org.freedesktop.DBus.Error.NameHasNoOwner"))
            st = GN_NO_GNOME;
        else if (!strcmp(e, "org.freedesktop.DBus.Error.UnknownMethod") ||
                 !strcmp(e, "org.freedesktop.DBus.Error.UnknownObject") ||
                 !strcmp(e, "org.freedesktop.DBus.Error.UnknownInterface"))
            /* Con la pantalla bloqueada GNOME apaga las extensiones. */
            st = screen_locked(c) ? GN_LOCKED : GN_NO_EXTENSION;
        else if (!strcmp(e, "org.freedesktop.DBus.Error.AccessDenied"))
            st = GN_DENIED;
        else
            st = GN_ERROR;
        if (st == GN_ERROR || st == GN_DENIED) log_msg("GNOME: %s falló: %s", method, err ? err->message : "?");
        g_free(remote);
        g_clear_error(&err);
    }
    g_object_unref(c);
    *out = r;
    return st;
}

static GnomeStatus call_string(const char *method, GVariant *params, int timeout_ms, char **text)
{
    GVariant *r;
    GnomeStatus st = call(method, params, "(s)", timeout_ms, &r);
    *text = NULL;
    if (r) {
        const char *s = NULL;
        g_variant_get(r, "(&s)", &s);
        *text = xstrdup(s ? s : "");
        g_variant_unref(r);
    }
    return st;
}

static GnomeStatus call_bool(const char *method, GVariant *params, bool *ok)
{
    GVariant *r;
    GnomeStatus st = call(method, params, "(b)", 3000, &r);
    gboolean b = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &b);
        g_variant_unref(r);
    }
    if (ok) *ok = b;
    return st;
}

/* ------------------------------------------------------------ ventanas --- */

static char *jstr(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
    return xstrdup(s ? s : "");
}

static bool window_from_json(const cJSON *o, GnomeWindow *w)
{
    memset(w, 0, sizeof *w);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
    if (!cJSON_IsNumber(id) || id->valuedouble <= 0) return false;
    w->id = (uint64_t)id->valuedouble;
    w->app = jstr(o, "app");
    w->app_id = jstr(o, "app_id");
    w->wm_class = jstr(o, "wm_class");
    w->title = jstr(o, "title");
    w->categories = jstr(o, "categories");
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(o, "pid");
    w->pid = cJSON_IsNumber(pid) ? (int)pid->valuedouble : 0;
    w->focused = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "focused"));
    w->minimized = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "minimized"));
    w->above = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "above"));
    w->terminal = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "terminal"));
    return true;
}

void gnome_window_clear(GnomeWindow *w)
{
    free(w->app);
    free(w->app_id);
    free(w->wm_class);
    free(w->title);
    free(w->categories);
    memset(w, 0, sizeof *w);
}

void gnome_windows_free(GnomeWindow *w, int n)
{
    for (int i = 0; w && i < n; i++) gnome_window_clear(&w[i]);
    free(w);
}

GnomeStatus gnome_list_windows(GnomeWindow **out, int *n)
{
    *out = NULL;
    *n = 0;
    char *json;
    GnomeStatus st = call_string("ListWindows", NULL, 3000, &json);
    if (st) return st;
    cJSON *arr = cJSON_Parse(json);
    free(json);
    int cap = cJSON_GetArraySize(arr);
    GnomeWindow *w = xcalloc((size_t)(cap > 0 ? cap : 1), sizeof *w);
    int k = 0;
    const cJSON *o;
    int me = (int)getpid();
    cJSON_ArrayForEach(o, arr)
    {
        if (window_from_json(o, &w[k])) {
            if (w[k].pid == me) gnome_window_clear(&w[k]); /* las de Sokari no cuentan */
            else k++;
        }
    }
    cJSON_Delete(arr);
    *out = w;
    *n = k;
    return GN_OK;
}

GnomeStatus gnome_focused(GnomeWindow *out, bool *shell_ui)
{
    memset(out, 0, sizeof *out);
    *shell_ui = false;
    char *json;
    GnomeStatus st = call_string("Focused", NULL, 3000, &json);
    if (st) return st;
    cJSON *o = cJSON_Parse(json);
    free(json);
    *shell_ui = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "shell_ui"));
    bool locked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "locked"));
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "window");
    if (cJSON_IsObject(w) && window_from_json(w, out) && out->pid == (int)getpid()) gnome_window_clear(out);
    cJSON_Delete(o);
    return locked ? GN_LOCKED : GN_OK;
}

GnomeStatus gnome_activate(uint64_t id, bool *ok)
{
    return call_bool("Activate", g_variant_new("(t)", (guint64)id), ok);
}

GnomeStatus gnome_window_action(uint64_t id, const char *action, bool *ok)
{
    return call_bool("WindowAction", g_variant_new("(ts)", (guint64)id, action), ok);
}

GnomeStatus gnome_own_window(const char *title, const char *action, int x, int y, int *out_x, int *out_y, bool *ok)
{
    GVariant *r;
    GnomeStatus st = call("OwnWindow", g_variant_new("(ssii)", title, action, x, y), "(bii)", 3000, &r);
    gboolean b = FALSE;
    gint32 rx = 0, ry = 0;
    if (r) {
        g_variant_get(r, "(bii)", &b, &rx, &ry);
        g_variant_unref(r);
    }
    if (ok) *ok = b;
    if (out_x) *out_x = rx;
    if (out_y) *out_y = ry;
    return st;
}

GnomeStatus gnome_minimize_all(int *count)
{
    GVariant *r;
    GnomeStatus st = call("MinimizeAll", NULL, "(u)", 3000, &r);
    guint32 n = 0;
    if (r) {
        g_variant_get(r, "(u)", &n);
        g_variant_unref(r);
    }
    if (count) *count = (int)n;
    return st;
}

/* ---------------------------------------------- teclas y portapapeles --- */

GnomeStatus gnome_press_keys(uint64_t expect, const uint32_t *keyvals, int n, int times, char **status)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
    for (int i = 0; i < n; i++) g_variant_builder_add(&b, "u", (guint32)keyvals[i]);
    /* Hasta 20 veces con 60 ms entre una y otra. */
    return call_string("PressKeys", g_variant_new("(tauu)", (guint64)expect, &b, (guint32)times), 8000, status);
}

GnomeStatus gnome_paste(uint64_t expect, const char *text, char **status)
{
    return call_string("Paste", g_variant_new("(ts)", (guint64)expect, text), 8000, status);
}

GnomeStatus gnome_clipboard_get(char **text)
{
    return call_string("GetClipboard", NULL, 5000, text);
}

GnomeStatus gnome_clipboard_set(const char *text)
{
    bool ok = false;
    GnomeStatus st = call_bool("SetClipboard", g_variant_new("(s)", text), &ok);
    return st ? st : ok ? GN_OK : GN_ERROR;
}

GnomeStatus gnome_clipboard_set_file(const char *path, char **kind)
{
    return call_string("SetClipboardFile", g_variant_new("(s)", path), 8000, kind);
}

GnomeStatus gnome_launch_app(const char *desktop_id, const char *const *uris, char **status)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    for (int i = 0; uris && uris[i]; i++) g_variant_builder_add(&b, "s", uris[i]);
    return call_string("LaunchApp", g_variant_new("(sas)", desktop_id, &b), 8000, status);
}

/* ------------------------------------------------- prender y bloquear --- */

void gnome_extension_enable(void)
{
    if (!gnome_desktop()) return;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.shell", TRUE) : NULL;
    if (!schema) return;
    bool keys = g_settings_schema_has_key(schema, "enabled-extensions") &&
                g_settings_schema_has_key(schema, "disabled-extensions");
    g_settings_schema_unref(schema);
    if (!keys) return;
    GSettings *s = g_settings_new("org.gnome.shell");
    gchar **on = g_settings_get_strv(s, "enabled-extensions");
    gchar **off = g_settings_get_strv(s, "disabled-extensions");
    if (g_strv_contains((const gchar *const *)off, SOKARI_EXTENSION_UUID)) {
        log_msg("GNOME: apagaste la extensión de Sokari; no la prendo (sin ella no puedo ver ventanas ni oprimir "
                "teclas).");
    } else if (!g_strv_contains((const gchar *const *)on, SOKARI_EXTENSION_UUID)) {
        guint n = g_strv_length(on);
        gchar **more = g_new0(gchar *, n + 2);
        for (guint i = 0; i < n; i++) more[i] = on[i];
        more[n] = (gchar *)SOKARI_EXTENSION_UUID;
        bool ok = g_settings_set_strv(s, "enabled-extensions", (const gchar *const *)more);
        g_settings_sync();
        g_free(more);
        log_msg(ok ? "GNOME: prendí la extensión de Sokari (si GNOME todavía no la conoce, queda para el siguiente "
                     "inicio de sesión)."
                   : "GNOME: no pude prender la extensión de Sokari.");
    }
    g_strfreev(on);
    g_strfreev(off);
    g_object_unref(s);
}

bool session_lock(void)
{
    GDBusConnection *c = session_bus();
    if (c) {
        GVariant *r = g_dbus_connection_call_sync(c, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver",
                                                  "org.gnome.ScreenSaver", "Lock", NULL, NULL,
                                                  G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, NULL);
        g_object_unref(c);
        if (r) {
            g_variant_unref(r);
            return true;
        }
    }
    const char *argv[] = {"loginctl", "lock-session", NULL};
    int code = -1;
    char *out = proc_run(argv, NULL, 0, 5000, 4096, NULL, &code);
    free(out);
    return code == 0;
}

/* ------------------------------------------------------- el escritorio --- */

static bool rect_from(const cJSON *a, DeskRect *r)
{
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 4) return false;
    int v[4];
    for (int i = 0; i < 4; i++) {
        const cJSON *n = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(n)) return false;
        v[i] = n->valueint;
    }
    *r = (DeskRect){v[0], v[1], v[2], v[3]};
    return true;
}

GnomeStatus gnome_desk(DeskView *v)
{
    memset(v, 0, sizeof *v);
    GVariant *r;
    GnomeStatus st = call("Desk", NULL, "(s)", 1500, &r);
    if (!r) return st;
    const char *json = NULL;
    g_variant_get(r, "(&s)", &json);
    cJSON *j = cJSON_Parse(json);
    g_variant_unref(r);
    if (!j) return GN_ERROR;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(j, "monitors")) {
        if (v->nmon >= DESK_MAX_MONITORS) break;
        DeskMonitor *dm = &v->mon[v->nmon];
        if (rect_from(cJSON_GetObjectItem(m, "area"), &dm->area) && rect_from(cJSON_GetObjectItem(m, "work"), &dm->work))
            v->nmon++;
    }
    const cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItem(j, "windows")) {
        if (v->nwin >= DESK_MAX_WINDOWS) break;
        if (!rect_from(cJSON_GetObjectItem(w, "r"), &v->win[v->nwin].r)) continue;
        v->win[v->nwin].active = cJSON_IsTrue(cJSON_GetObjectItem(w, "active"));
        v->nwin++;
    }
    const cJSON *p = cJSON_GetObjectItem(j, "pointer");
    if (cJSON_IsArray(p) && cJSON_GetArraySize(p) == 2) {
        v->cursor_x = cJSON_GetArrayItem(p, 0)->valueint;
        v->cursor_y = cJSON_GetArrayItem(p, 1)->valueint;
    }
    v->fullscreen = cJSON_IsTrue(cJSON_GetObjectItem(j, "fullscreen"));
    cJSON_Delete(j);
    return v->nmon ? GN_OK : GN_ERROR;
}

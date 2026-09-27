/* Las herramientas del sistema en Linux, con los mismos mensajes y las mismas
   reglas que en Windows: abrir apps, páginas, carpetas y archivos (con GIO,
   las mismas apps que ves en GNOME), la música y el volumen (MPRIS y
   PulseAudio/PipeWire), las ventanas, escribir y el portapapeles (con la
   extensión de GNOME) y el estado de la PC. */
#include <windows.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "app.h"
#include "intents.h"
#include "linux/acciones.h"
#include "linux/gnome.h"
#include "linux/linux.h"
#include "log.h"
#include "tools.h"
#include "util.h"

/* ----------------------------------------------------- lo compartido --- */

const char *lx_app_name(const GnomeWindow *w, const char *fallback)
{
    return w && w->app && *w->app ? w->app : fallback;
}

bool lx_runs_commands(const GnomeWindow *w, bool shell_ui)
{
    static const char *const FILE_MANAGERS[] = {"nautilus", "org.gnome.files", "nemo", "thunar", "dolphin",
                                                "caja", "pcmanfm", "spacefm", "doublecmd", "krusader"};
    if (shell_ui) return true;
    if (!w || !w->id) return false;
    if (w->terminal) return true;
    if (w->categories && strstr(w->categories, "FileManager")) return true;
    for (size_t i = 0; i < sizeof FILE_MANAGERS / sizeof *FILE_MANAGERS; i++)
        if ((w->app_id && str_contains_ci(w->app_id, FILE_MANAGERS[i])) ||
            (w->wm_class && str_contains_ci(w->wm_class, FILE_MANAGERS[i])))
            return true;
    return false;
}

char *lx_refusal(const char *status, const char *what)
{
    if (!status) status = "";
    if (str_starts_with(status, "focus"))
        return str_printf("%s: cambiaste de ventana (o se abrió otra) antes de que terminara.", what);
    if (!strcmp(status, "terminal"))
        return str_printf("%s: por seguridad no le doy Enter ni escribo en terminales (Terminal, Consola, Ptyxis y "
                          "parecidas), ahí se ejecutaría un comando.",
                          what);
    if (!strcmp(status, "shell"))
        return str_printf("%s: quedó enfrente la vista de actividades o «Ejecutar» de GNOME, donde Enter abre o "
                          "ejecuta cosas.",
                          what);
    if (!strcmp(status, "locked")) return str_printf("%s: la pantalla está bloqueada.", what);
    return str_printf("%s: GNOME no me dejó.", what);
}

/* --------------------------------------------------------------- apps --- */

typedef struct {
    const char *alias;
    const char *target; /* una página, "@archivos" o varios .desktop (el primero que esté) */
} Alias;

#define TEXT_EDITORS "org.gnome.TextEditor.desktop org.gnome.gedit.desktop gedit.desktop org.kde.kwrite.desktop"
static const Alias APP_ALIASES[] = {
    {"google", "google.com"},
    {"youtube", "youtube.com"},
    {"bloc de notas", TEXT_EDITORS},
    {"notas", TEXT_EDITORS},
    {"notepad", TEXT_EDITORS},
    {"editor de texto", TEXT_EDITORS},
    {"calculadora", "org.gnome.Calculator.desktop gnome-calculator.desktop org.kde.kcalc.desktop"},
    {"explorador", "@archivos"},
    {"archivos", "@archivos"},
    {"explorer", "@archivos"},
    {"file explorer", "@archivos"},
    {"explorador de archivos", "@archivos"},
    {"este equipo", "@archivos"},
    {"mi pc", "@archivos"},
    {"mis archivos", "@archivos"},
    {"word", "libreoffice-writer.desktop org.libreoffice.LibreOffice.writer.desktop"},
    {"excel", "libreoffice-calc.desktop org.libreoffice.LibreOffice.calc.desktop"},
    {"powerpoint", "libreoffice-impress.desktop org.libreoffice.LibreOffice.impress.desktop"},
    {"configuracion", "org.gnome.Settings.desktop gnome-control-center.desktop"},
    {"configuración", "org.gnome.Settings.desktop gnome-control-center.desktop"},
};

/* Minúsculas, sin acentos ni símbolos: "Spotify Premium™" -> "spotifypremium". */
static char *normalize_name(const char *s)
{
    char *n = intents_normalize(s);
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = n; *p; p++)
        if (*p != ' ') sb_append_char(&sb, *p);
    free(n);
    char *r = sb_steal(&sb);
    return r ? r : xstrdup("");
}

/* "org.gnome.Calculator.desktop" -> "calculator"; "spotify_spotify.desktop" -> "spotify". */
static char *id_tail(const char *id)
{
    char *t = xstrdup(id ? id : "");
    char *d = strstr(t, ".desktop");
    if (d) *d = 0;
    char *u = strchr(t, '_'); /* las de Snap: app_app */
    if (u) *u = 0;
    char *dot = strrchr(t, '.');
    char *r = normalize_name(dot ? dot + 1 : t);
    free(t);
    return r;
}

typedef struct {
    GAppInfo *info;
    char *display;
    int score;
    int distance;
} AppMatch;

static int app_score(GAppInfo *info, const char *q, const char *nd)
{
    int score = 0;
    if (!strcmp(nd, q)) score = 100;
    else if (str_starts_with(nd, q)) score = 80;
    else if (strstr(nd, q)) score = 60;
    if (score < 90) {
        /* Por su programa o su .desktop: "gnome-calculator", "nautilus". */
        const char *exe = g_app_info_get_executable(info);
        const char *slash = exe ? strrchr(exe, '/') : NULL;
        char *ne = normalize_name(slash ? slash + 1 : exe ? exe : "");
        char *nt = id_tail(g_app_info_get_id(info));
        if (!strcmp(ne, q) || !strcmp(nt, q)) score = 90;
        free(ne);
        free(nt);
    }
    if (score < 70 && G_IS_DESKTOP_APP_INFO(info)) {
        /* "Navegador web", "Editor de texto"… */
        const char *gen = g_desktop_app_info_get_generic_name(G_DESKTOP_APP_INFO(info));
        char *ng = normalize_name(gen ? gen : "");
        if (*ng && !strcmp(ng, q)) score = 70;
        free(ng);
    }
    if (score < 40 && G_IS_DESKTOP_APP_INFO(info)) {
        const char *const *kw = g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(info));
        for (int i = 0; kw && kw[i] && score < 40; i++) {
            char *nk = normalize_name(kw[i]);
            if (*nk && !strcmp(nk, q)) score = 40;
            free(nk);
        }
    }
    return score;
}

/* Todas las apps que ves en GNOME (las de Snap y Flatpak también): la que
   mejor se llama así y, si ninguna, hasta max_similar que se le parecen. */
static int find_installed_apps(const char *query, AppMatch *best, AppMatch *similar, int max_similar)
{
    char *q = normalize_name(query);
    best->score = 0;
    if (strlen(q) < 2) {
        free(q);
        return 0;
    }
    int n_similar = 0;
    GList *all = g_app_info_get_all();
    for (GList *l = all; l; l = l->next) {
        GAppInfo *info = l->data;
        if (!g_app_info_should_show(info)) continue;
        const char *d = g_app_info_get_display_name(info);
        if (!d || !*d) continue;
        char *nd = normalize_name(d);
        int score = app_score(info, q, nd);
        if (score > best->score || (score && score == best->score && strlen(d) < strlen(best->display))) {
            if (best->info) g_object_unref(best->info);
            free(best->display);
            best->info = g_object_ref(info);
            best->display = xstrdup(d);
            best->score = score;
        }
        int dist = name_edit_distance(nd, q);
        if (!score && dist <= 3 + (int)strlen(q) / 4) {
            int slot = n_similar < max_similar ? n_similar++ : -1;
            if (slot < 0) {
                for (int k = 0; k < max_similar; k++)
                    if (similar[k].distance > dist) slot = k;
            }
            if (slot >= 0 && (slot >= n_similar - 1 || similar[slot].distance > dist)) {
                free(similar[slot].display);
                similar[slot].display = xstrdup(d);
                similar[slot].distance = dist;
            }
        }
        free(nd);
    }
    g_list_free_full(all, g_object_unref);
    free(q);
    return n_similar;
}

/* Abre la app (con esa dirección) por la extensión, para que su ventana salga
   enfrente como si la hubieras abierto tú; si no hay extensión, directo.
   Devuelve "launched", "focused" o NULL si no se pudo. */
static const char *launch_app(GAppInfo *info, const char *uri)
{
    app_yield_focus();
    const char *id = g_app_info_get_id(info);
    if (id) {
        const char *uris[] = {uri, NULL};
        char *st = NULL;
        GnomeStatus gs = gnome_launch_app(id, uri ? uris : NULL, &st);
        const char *r = gs == GN_OK && st && strcmp(st, "missing") ? (!strcmp(st, "focused") ? "focused" : "launched")
                                                                    : NULL;
        free(st);
        if (r) return r;
    }
    GList *list = uri ? g_list_append(NULL, (gpointer)uri) : NULL;
    GError *err = NULL;
    bool ok;
    if (G_IS_DESKTOP_APP_INFO(info))
        /* Sin G_SPAWN_DO_NOT_REAP_CHILD: GLib la suelta y no queda colgada de Sokari. */
        ok = g_desktop_app_info_launch_uris_as_manager(G_DESKTOP_APP_INFO(info), list, NULL, G_SPAWN_SEARCH_PATH, NULL,
                                                        NULL, NULL, NULL, &err);
    else
        ok = g_app_info_launch_uris(info, list, NULL, &err);
    if (!ok) log_msg("No pude abrir %s: %s", id ? id : "?", err ? err->message : "?");
    g_clear_error(&err);
    g_list_free(list);
    return ok ? "launched" : NULL;
}

static GAppInfo *first_installed(const char *ids)
{
    if (!strcmp(ids, "@archivos")) return g_app_info_get_default_for_type("inode/directory", FALSE);
    char *copy = xstrdup(ids);
    GAppInfo *found = NULL;
    for (char *save = NULL, *id = strtok_r(copy, " ", &save); id && !found; id = strtok_r(NULL, " ", &save))
        found = (GAppInfo *)g_desktop_app_info_new(id);
    free(copy);
    return found;
}

static char *opened(const char *how, const char *name)
{
    if (!how) return NULL;
    return !strcmp(how, "focused") ? str_printf("Ya estaba abierto: traje al frente %s.", name)
                                   : str_printf("Abrí %s.", name);
}

/* La app que abre esa carpeta o ese archivo (la que tengas de predeterminada). */
static char *open_path(const char *path, const char *name)
{
    GFile *f = g_file_new_for_path(path);
    GAppInfo *h = g_file_query_default_handler(f, NULL, NULL);
    char *uri = g_file_get_uri(f);
    char *r = NULL;
    if (h) {
        const char *how = launch_app(h, uri);
        r = how ? str_printf("Abrí %s.", name) : NULL;
        g_object_unref(h);
    } else {
        r = str_printf("No tengo con qué abrir '%s': no hay ninguna app para ese tipo de archivo.", name);
    }
    g_free(uri);
    g_object_unref(f);
    return r;
}

/* Un navegador instalado por su nombre ("opera", "chrome", "edge"…). */
static GAppInfo *find_browser(const char *name)
{
    char *q = normalize_name(name);
    GAppInfo *found = NULL;
    int best = 0;
    GList *all = g_app_info_get_all_for_type("x-scheme-handler/https");
    for (GList *l = all; l && *q; l = l->next) {
        GAppInfo *info = l->data;
        char *nd = normalize_name(g_app_info_get_display_name(info));
        char *nt = id_tail(g_app_info_get_id(info));
        const char *id = g_app_info_get_id(info);
        char *ni = normalize_name(id ? id : "");
        int score = !strcmp(nd, q) || !strcmp(nt, q) ? 3 : str_starts_with(nd, q) ? 2 : strstr(nd, q) || strstr(ni, q) ? 1 : 0;
        if (score > best) {
            if (found) g_object_unref(found);
            found = g_object_ref(info);
            best = score;
        }
        free(nd);
        free(nt);
        free(ni);
    }
    g_list_free_full(all, g_object_unref);
    free(q);
    return found;
}

bool open_url(const char *url, const char *browser)
{
    /* Nada de comillas ni espacios, como en Windows. */
    if (strpbrk(url, "\" \t\r\n")) return false;
    char *full = str_starts_with(url, "http") ? xstrdup(url) : str_printf("https://%s", url);
    bool want = browser && *browser;
    GAppInfo *app = want ? find_browser(browser) : g_app_info_get_default_for_uri_scheme("https");
    bool ok = false;
    if (app) {
        ok = launch_app(app, full) != NULL;
        g_object_unref(app);
    } else if (!want) {
        ok = g_app_info_launch_default_for_uri(full, NULL, NULL);
    }
    free(full);
    return ok;
}

/* Lo que se ejecuta en vez de abrirse en un visor: los de Windows (con Wine se
   ejecutan) y los de Linux. */
static const char *const DANGEROUS_EXT[] = {
    ".exe", ".com", ".bat", ".cmd", ".scr", ".pif", ".cpl", ".msi", ".msp", ".msc", ".vbs", ".vbe", ".js", ".jse",
    ".wsf", ".wsh", ".ws", ".ps1", ".psm1", ".psd1", ".hta", ".jar", ".lnk", ".url", ".reg", ".inf", ".scf",
    ".application", ".appref-ms", ".appx", ".appxbundle", ".msix", ".msixbundle", ".appinstaller",
    ".settingcontent-ms", ".library-ms", ".search-ms", ".searchconnector-ms", ".diagcab", ".chm", ".iso", ".img",
    ".vhd", ".vhdx", ".xll", ".gadget", ".py", ".pyw", ".dll", ".sys", ".ocx",
    ".sh", ".bash", ".zsh", ".fish", ".csh", ".ksh", ".run", ".bin", ".appimage", ".desktop", ".deb", ".rpm",
    ".snap", ".flatpak", ".flatpakref", ".flatpakrepo", ".pl", ".rb", ".php", ".lua", ".elf", ".so", ".ko", ".out",
    ".x86_64", ".command", ".service", ".pyz", ".mjs", ".cjs", ".vbox-extpack",
};

/* Programas e instaladores: se ejecutan o se instalan al abrirlos. */
static const char *const DANGEROUS_TYPES[] = {
    "application/x-executable", "application/x-pie-executable", "application/x-sharedlib",
    "application/x-desktop", "application/x-ms-dos-executable", "application/x-msdownload", "application/x-msi",
    "application/x-java-archive", "application/vnd.appimage", "application/x-iso9660-appimage",
    "application/vnd.debian.binary-package", "application/x-rpm", "application/vnd.flatpak.ref",
    "application/vnd.flatpak.repo", "application/vnd.flatpak",
};

/* Scripts: sin permiso de ejecutar solo se abren para leerlos. */
static const char *const SCRIPT_TYPES[] = {
    "application/x-shellscript", "text/x-python", "text/x-python3", "application/x-perl", "application/x-ruby",
    "application/x-php",
};

bool open_target_is_dangerous(const wchar_t *path)
{
    char *p = wide_to_utf8(path);
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    /* "virus.sh." o "virus.sh " terminan igual en .sh para quien lo abre. */
    char *name = str_trim(base);
    size_t n = strlen(name);
    while (n && name[n - 1] == '.') name[--n] = 0;
    const char *ext = strrchr(name, '.');
    bool bad = false;
    for (size_t i = 0; ext && i < sizeof DANGEROUS_EXT / sizeof *DANGEROUS_EXT && !bad; i++)
        bad = !strcasecmp(ext, DANGEROUS_EXT[i]);
    free(name);
    if (!bad) {
        /* Sin extensión (o con una inocente): por lo que tiene adentro. */
        unsigned char head[4096];
        ssize_t got = 0;
        bool exec_bit = false;
        struct stat st;
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
                exec_bit = (st.st_mode & 0111) != 0;
                got = read(fd, head, sizeof head);
            }
            close(fd);
        }
        if (got >= 4 && !memcmp(head, "\x7f" "ELF", 4)) bad = true; /* un programa de Linux */
        else if (got >= 2 && !memcmp(head, "MZ", 2)) bad = true;    /* uno de Windows */
        else if (got >= 2 && !memcmp(head, "#!", 2) && exec_bit) bad = true; /* un script que se puede correr */
        if (!bad && got > 0) {
            gboolean uncertain = FALSE;
            char *type = g_content_type_guess(p, head, (gsize)got, &uncertain);
            for (size_t i = 0; type && i < sizeof DANGEROUS_TYPES / sizeof *DANGEROUS_TYPES && !bad; i++)
                bad = g_content_type_is_a(type, DANGEROUS_TYPES[i]);
            for (size_t i = 0; type && exec_bit && i < sizeof SCRIPT_TYPES / sizeof *SCRIPT_TYPES && !bad; i++)
                bad = g_content_type_is_a(type, SCRIPT_TYPES[i]);
            g_free(type);
        }
    }
    free(p);
    return bad;
}

static bool is_network_path(const char *p)
{
    if ((p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\')) return true;
    /* Las carpetas de red que monta GNOME (smb, sftp, ftp…). */
    char gvfs[64];
    snprintf(gvfs, sizeof gvfs, "/run/user/%u/gvfs", (unsigned)getuid());
    return str_starts_with(p, gvfs);
}

char *tool_open_app(const cJSON *a)
{
    char *name = str_trim(arg_str(a, "name"));
    if (!*name) {
        free(name);
        return xstrdup("No me dijiste qué abrir.");
    }
    char *low = str_lower(name);
    char *result = NULL;
    const char *target = name;
    for (size_t i = 0; i < sizeof APP_ALIASES / sizeof *APP_ALIASES; i++)
        if (!strcmp(low, APP_ALIASES[i].alias)) target = APP_ALIASES[i].target;
    bool is_alias = target != name;
    bool is_path = false;

    if (has_other_scheme(target)) {
        free(low);
        free(name);
        return xstrdup("Por seguridad solo abro apps, carpetas, archivos y páginas http o https.");
    }
    wchar_t *folder = known_folder_alias(low);
    if (folder) {
        char *f = wide_to_utf8(folder);
        result = open_path(f, name);
        free(f);
        free(folder);
    } else if (!strcmp(low, "navegador") || !strcmp(low, "el navegador") || !strcmp(low, "browser")) {
        /* El navegador que tengas de predeterminado, no uno fijo. */
        GAppInfo *b = g_app_info_get_default_for_uri_scheme("https");
        if (b && launch_app(b, NULL)) result = xstrdup("Abrí tu navegador.");
        if (b) g_object_unref(b);
    } else if (looks_like_url(target) && *arg_str(a, "navegador")) {
        /* "Abre YouTube en Opera": esa dirección, en ese navegador. */
        const char *nav = arg_str(a, "navegador");
        if (open_url(target, nav)) result = str_printf("Abrí %s en %s.", target, nav);
        else result = str_printf("No encontré el navegador «%s» instalado; dime otro o lo abro en el predeterminado.", nav);
    } else if (looks_like_url(target)) {
        char *url = str_starts_with(target, "http") ? xstrdup(target) : str_printf("https://%s", target);
        if (open_url(url, NULL)) result = str_printf("Abrí %s en el navegador.", url);
        free(url);
    } else if (is_alias) {
        GAppInfo *app = first_installed(target);
        if (app) {
            result = opened(launch_app(app, NULL), g_app_info_get_display_name(app));
            g_object_unref(app);
        }
    }
    if (!result && !is_alias) {
        wchar_t *w = utf8_to_wide(target);
        wchar_t *expanded = expand_env(w);
        char *e = wide_to_utf8(expanded);
        is_path = strchr(e, '/') || strchr(e, '\\');
        if (is_path) {
            char *full = realpath(e, NULL);
            if (is_network_path(e) || (full && is_network_path(full))) {
                result = xstrdup("Por seguridad no abro rutas de red.");
            } else if (full) {
                struct stat st;
                wchar_t *wf = utf8_to_wide(full);
                if (!stat(full, &st) && S_ISDIR(st.st_mode)) {
                    result = open_path(full, name);
                } else if (!stat(full, &st) && S_ISREG(st.st_mode)) {
                    if (open_target_is_dangerous(wf) || open_target_is_dangerous(expanded))
                        result = xstrdup("Por seguridad no abro programas ni scripts sueltos (.sh, AppImage, "
                                         ".desktop, .exe...). Dime el nombre de la app y la busco entre tus "
                                         "aplicaciones.");
                    else
                        result = open_path(full, name);
                }
                free(wf);
            }
            free(full);
        }
        free(e);
        free(expanded);
        free(w);
    }
    if (!result && !is_path) {
        AppMatch best = {0};
        AppMatch similar[3] = {{0}};
        int ns = find_installed_apps(name, &best, similar, 3);
        if (best.score && best.info) result = opened(launch_app(best.info, NULL), best.display);
        if (!result) {
            StrBuf sb;
            sb_init(&sb);
            sb_appendf(&sb, "No encontré ninguna app llamada '%s'.", name);
            if (ns) {
                sb_append(&sb, " Parecidas instaladas: ");
                for (int i = 0; i < ns; i++) sb_appendf(&sb, "%s%s", i ? ", " : "", similar[i].display);
                sb_append(&sb, ". Si era alguna de esas, vuelve a llamar a open_app con ese nombre exacto.");
            }
            result = sb_steal(&sb);
        }
        if (best.info) g_object_unref(best.info);
        free(best.display);
        for (int i = 0; i < 3; i++) free(similar[i].display);
    }
    if (!result) result = str_printf("No encontré '%s'.", name);
    free(low);
    free(name);
    return result;
}

/* ------------------------------------------------ música y volumen --- */

/* El reproductor que está sonando (o el que está en pausa): Spotify, el
   navegador con un video… cualquiera que hable MPRIS. */
static char *mpris_player(GDBusConnection *c)
{
    GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
                                              G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (!r) return NULL;
    GVariantIter *it = NULL;
    const char *name;
    char *best = NULL;
    int best_rank = -1;
    g_variant_get(r, "(as)", &it);
    while (g_variant_iter_loop(it, "&s", &name)) {
        if (!g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) continue;
        int rank = 0;
        GVariant *p = g_dbus_connection_call_sync(
            c, name, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "PlaybackStatus"), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
        if (p) {
            GVariant *v = NULL;
            g_variant_get(p, "(v)", &v);
            const char *s = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_get_string(v, NULL) : "";
            rank = !strcmp(s, "Playing") ? 2 : !strcmp(s, "Paused") ? 1 : 0;
            if (v) g_variant_unref(v);
            g_variant_unref(p);
        }
        if (rank > best_rank) {
            free(best);
            best = xstrdup(name);
            best_rank = rank;
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    return best;
}

static char *media_key(const char *method)
{
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    char *player = c ? mpris_player(c) : NULL;
    bool ok = false;
    if (player) {
        GError *err = NULL;
        GVariant *r = g_dbus_connection_call_sync(c, player, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
                                                  method, NULL, NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, NULL, &err);
        ok = r != NULL;
        if (r) g_variant_unref(r);
        else log_msg("MPRIS: %s en %s falló: %s", method, player, err ? err->message : "?");
        g_clear_error(&err);
    }
    free(player);
    if (c) g_object_unref(c);
    if (ok) return xstrdup("Listo.");
    return xstrdup(player ? "No pude controlar el reproductor."
                          : "No encontré ningún reproductor abierto que se pueda controlar (Spotify, el navegador con "
                            "un video…).");
}

char *tool_control_media(const cJSON *a)
{
    const char *action = arg_str(a, "action");
    if (!strcmp(action, "set_volume")) {
        int lvl = arg_int(a, "nivel", -1);
        if (lvl < 0 || lvl > 100) return xstrdup("Dime un nivel de volumen entre 0 y 100.");
        return system_volume_set(lvl) ? str_printf("Listo, volumen al %d%%.", lvl)
                                      : xstrdup("No pude cambiar el volumen del sistema.");
    }
    if (!strcmp(action, "volume_up") || !strcmp(action, "volume_down")) {
        int now = 0;
        bool muted = false;
        if (!system_volume_get(&now, &muted)) return xstrdup("No pude cambiar el volumen del sistema.");
        /* De 5 en 5 como las teclas de volumen de GNOME, sin pasar de 100. */
        int want = !strcmp(action, "volume_up") ? (now >= 100 ? now : now + 5 > 100 ? 100 : now + 5)
                                                : (now - 5 < 0 ? 0 : now - 5);
        if (!system_volume_set(want)) return xstrdup("No pude cambiar el volumen del sistema.");
        return str_printf("Listo, volumen al %d%%.", want);
    }
    if (!strcmp(action, "mute")) {
        /* Como la tecla de silencio: si ya estaba en silencio, vuelve a sonar. */
        bool muted = false;
        if (!system_volume_get(NULL, &muted) || !system_mute_set(!muted))
            return xstrdup("No pude cambiar el volumen del sistema.");
        return xstrdup(muted ? "Listo, ya suena otra vez." : "Listo, lo silencié.");
    }
    if (!strcmp(action, "play_pause")) return media_key("PlayPause");
    if (!strcmp(action, "next_track")) return media_key("Next");
    if (!strcmp(action, "previous_track")) return media_key("Previous");
    return xstrdup("Acción de media no reconocida.");
}

/* ------------------------------------------------------------ ventanas --- */

/* Tu ventana de enfrente: la que tiene el foco o, si lo tiene GNOME (la vista
   de actividades), la última que usaste. */
static GnomeStatus front_window(GnomeWindow *out)
{
    app_yield_focus();
    bool shell = false;
    GnomeStatus st = gnome_focused(out, &shell);
    if (st || (out->id && !shell)) return st;
    gnome_window_clear(out);
    GnomeWindow *w;
    int n;
    st = gnome_list_windows(&w, &n);
    if (st) return st;
    for (int i = 0; i < n; i++) {
        if (!w[i].minimized) {
            *out = w[i];
            memset(&w[i], 0, sizeof w[i]);
            break;
        }
    }
    gnome_windows_free(w, n);
    return GN_OK;
}

static char *window_action(const char *action)
{
    GnomeWindow w;
    GnomeStatus st = front_window(&w);
    if (st) return xstrdup(gnome_status_message(st));
    if (!w.id) return xstrdup("No encontré ninguna ventana tuya enfrente.");
    const char *t = lx_app_name(&w, "la ventana de enfrente");
    bool ok = false;
    bool min = !strcmp(action, "minimize"), max = !strcmp(action, "maximize");
    /* Cerrar es como darle a la X: si tiene algo sin guardar, la app misma pregunta. */
    st = gnome_window_action(w.id, min ? "minimize" : max ? "maximize" : "close", &ok);
    char *r;
    if (st) r = xstrdup(gnome_status_message(st));
    else if (min) r = ok ? str_printf("Minimicé %s.", t) : str_printf("No pude minimizar %s.", t);
    else if (max) r = ok ? str_printf("Maximicé %s.", t) : str_printf("No pude maximizar %s.", t);
    else r = ok ? str_printf("Cerré %s.", t) : str_printf("No pude cerrar %s.", t);
    gnome_window_clear(&w);
    return r;
}

/* Una tecla o combinación en la ventana de enfrente (expect) o del sistema. */
static char *press_simple(const uint32_t *keys, int n, bool on_window)
{
    uint64_t expect = 0;
    if (on_window) {
        GnomeWindow w;
        GnomeStatus st = front_window(&w);
        if (st) return xstrdup(gnome_status_message(st));
        expect = w.id;
        gnome_window_clear(&w);
        if (!expect) return xstrdup("No encontré ninguna ventana tuya enfrente.");
        bool ok = false;
        gnome_activate(expect, &ok);
    }
    char *status = NULL;
    GnomeStatus st = gnome_press_keys(expect, keys, n, 1, &status);
    char *r = st ? xstrdup(gnome_status_message(st)) : !strcmp(status, "ok") ? xstrdup("Listo.")
                                                                             : lx_refusal(status, "No lo hice");
    free(status);
    return r;
}

char *tool_control_desktop(const cJSON *a)
{
    const char *action = arg_str(a, "action");
    if (!strcmp(action, "lock")) return session_lock() ? xstrdup("Listo.") : xstrdup("No pude bloquear la PC.");
    if (!strcmp(action, "minimize") || !strcmp(action, "maximize") || !strcmp(action, "close_window"))
        return window_action(action);
    if (!strcmp(action, "show_desktop") || !strcmp(action, "minimize_all")) {
        app_yield_focus();
        int n = 0;
        GnomeStatus st = gnome_minimize_all(&n);
        return st ? xstrdup(gnome_status_message(st)) : xstrdup("Listo.");
    }
    if (!strcmp(action, "switch_window")) {
        const uint32_t k[] = {XK_Alt_L, XK_Tab};
        return press_simple(k, 2, false);
    }
    if (!strcmp(action, "fullscreen")) {
        const uint32_t k[] = {'f'};
        return press_simple(k, 1, true);
    }
    if (!strcmp(action, "close_tab")) {
        const uint32_t k[] = {XK_Control_L, 'w'};
        return press_simple(k, 2, true);
    }
    return xstrdup("Acción de escritorio no reconocida.");
}

char *focus_window_by_title(const char *needle, bool *ok)
{
    *ok = false;
    char *n = str_trim(needle);
    if (!*n) {
        free(n);
        return xstrdup("No dijiste qué ventana buscar.");
    }
    GnomeWindow *w;
    int count;
    GnomeStatus st = gnome_list_windows(&w, &count);
    if (st) {
        free(n);
        return xstrdup(gnome_status_message(st));
    }
    int found = -1;
    for (int i = 0; i < count && found < 0; i++)
        if (str_contains_ci(w[i].title, n) || str_contains_ci(w[i].app, n)) found = i;
    char *r;
    if (found < 0) {
        r = str_printf("No encontré ninguna ventana con '%s' abierta.", n);
    } else {
        app_yield_focus();
        const char *app = *w[found].app ? w[found].app : NULL;
        char *t = app && str_contains_ci(app, n) ? xstrdup(app)
                  : app                          ? str_printf("«%s» en %s", n, app)
                                                 : str_printf("«%s»", n);
        bool done = false;
        st = gnome_activate(w[found].id, &done);
        bool front = false;
        /* GNOME la trae al frente un momento después. */
        for (int i = 0; !st && done && i < 15 && !front; i++) {
            GnomeWindow f;
            bool shell = false;
            front = gnome_focused(&f, &shell) == GN_OK && f.id == w[found].id && !shell;
            gnome_window_clear(&f);
            if (!front) Sleep(100);
        }
        if (front) {
            *ok = true;
            r = str_printf("Enfoqué %s.", t);
        } else if (st) {
            r = xstrdup(gnome_status_message(st));
        } else {
            r = str_printf("Encontré %s, pero GNOME no me dejó traerla al frente.", t);
        }
        free(t);
    }
    gnome_windows_free(w, count);
    free(n);
    return r;
}

char *tool_focus_window(const cJSON *a)
{
    bool ok;
    return focus_window_by_title(arg_str(a, "title_contains"), &ok);
}

char *tool_list_windows(const cJSON *a)
{
    GnomeWindow *w;
    int n;
    GnomeStatus st = gnome_list_windows(&w, &n);
    if (st) return xstrdup(gnome_status_message(st));
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < n; i++) {
        char *t = str_trim(*w[i].title ? w[i].title : w[i].app);
        if (*t) sb_appendf(&sb, "%s%s", sb.len ? "\n" : "", t);
        free(t);
    }
    gnome_windows_free(w, n);
    if (!sb.len) {
        sb_free(&sb);
        return xstrdup("No encontré ninguna ventana abierta.");
    }
    return sb_steal(&sb);
}

/* ------------------------------------------ escribir y el portapapeles --- */

char *tool_type_text(const cJSON *a)
{
    const char *texto = arg_str(a, "texto");
    const char *ventana = arg_str(a, "ventana");
    bool enviar = arg_bool(a, "enviar");
    if (str_is_blank(texto)) return xstrdup("No me dijiste qué escribir.");
    if (str_contains_ci(texto, "javascript:"))
        return xstrdup("Por seguridad no escribo «javascript:»: en la barra de direcciones correría código dentro de "
                       "la página.");
    if (!str_is_blank(ventana)) {
        bool ok;
        char *r = focus_window_by_title(ventana, &ok);
        if (!ok) return r;
        free(r);
    } else {
        app_yield_focus();
    }
    GnomeWindow w;
    bool shell = false;
    GnomeStatus st = gnome_focused(&w, &shell);
    if (st) return xstrdup(gnome_status_message(st));
    char *r = NULL;
    if (w.terminal) {
        r = xstrdup("Por seguridad no escribo en terminales (Terminal, Consola, Ptyxis y parecidas): ahí el texto se "
                    "ejecuta como comando.");
    } else if (!w.id || shell) {
        r = xstrdup("No escribí nada: no hay ninguna ventana tuya enfrente (o está abierta la vista de actividades).");
    } else {
        /* Pegando, para que salgan acentos, ñ y emojis con cualquier teclado; lo
           que tenías copiado vuelve a su lugar. Los saltos de línea se pegan
           tal cual: en un chat agregan una línea, no mandan el mensaje. */
        char *status = NULL;
        st = gnome_paste(w.id, texto, &status);
        /* Sokari escribe a ciegas: dice dónde lo escribió, pero no puede saber
           si ahí había un chat abierto, así que no afirma que se envió. */
        const char *where = lx_app_name(&w, "la ventana de enfrente");
        if (st) {
            r = xstrdup(gnome_status_message(st));
        } else if (strcmp(status, "ok")) {
            r = lx_refusal(status, "No escribí nada");
        } else if (enviar && lx_runs_commands(&w, false)) {
            /* En Archivos Enter abre lo seleccionado: eso solo si tú lo pides con tu voz. */
            r = str_printf("Escribí el texto en %s, pero no le di Enter: ahí Enter abre o ejecuta cosas. Si eso "
                           "quieres, dime «dale enter».",
                           where);
        } else if (enviar) {
            /* Que la app termine de pegar antes del Enter, o mandaría el mensaje sin el texto. */
            Sleep(400);
            const uint32_t enter = XK_Return;
            char *s2 = NULL;
            GnomeStatus st2 = gnome_press_keys(w.id, &enter, 1, 1, &s2);
            if (!st2 && !strcmp(s2, "ok"))
                r = str_printf("Escribí el texto en %s y le di Enter. No veo la pantalla: si ahí no había un chat o "
                               "un cuadro de texto abierto, no se envió.",
                               where);
            else
                r = str_printf("Escribí el texto en %s, pero no le di Enter: %s", where,
                               st2 ? gnome_status_message(st2) : "cambiaste de ventana antes.");
            free(s2);
        } else {
            r = str_printf("Escribí el texto en %s, sin enviarlo. No veo la pantalla: que revise que quedó donde "
                           "quería.",
                           where);
        }
        free(status);
    }
    gnome_window_clear(&w);
    return r;
}

char *tool_leer_portapapeles(const cJSON *a)
{
    char *t = NULL;
    GnomeStatus st = gnome_clipboard_get(&t);
    if (st) {
        free(t);
        return xstrdup(gnome_status_message(st));
    }
    char *r = str_trim(t);
    free(t);
    if (!*r) {
        free(r);
        return xstrdup("No hay texto en el portapapeles (puede tener una imagen u otra cosa).");
    }
    size_t cut = utf8_truncate_len(r, 6000);
    if (cut < strlen(r)) {
        r[cut] = 0;
        char *longer = str_printf("%s\n[...se cortó aquí, el texto copiado sigue...]", r);
        free(r);
        r = longer;
    }
    return r;
}

char *tool_copiar_portapapeles(const cJSON *a)
{
    const char *texto = arg_str(a, "texto");
    if (str_is_blank(texto)) return xstrdup("No me dijiste qué copiar.");
    GnomeStatus st = gnome_clipboard_set(texto);
    if (st == GN_OK) return xstrdup("Listo, lo copié.");
    return xstrdup(st == GN_ERROR ? "No pude copiar al portapapeles." : gnome_status_message(st));
}

/* --------------------------------------------------- estado de la PC --- */

static bool cpu_times(unsigned long long *idle, unsigned long long *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7]);
    fclose(f);
    if (n < 4) return false;
    *idle = v[3] + v[4]; /* sin hacer nada y esperando al disco */
    *total = 0;
    for (int i = 0; i < 8; i++) *total += v[i];
    return true;
}

static long long meminfo_kb(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? atoll(p + strlen(key)) : -1;
}

static bool read_small(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return n > 0;
}

char *tool_info_sistema(const cJSON *a)
{
    StrBuf sb;
    sb_init(&sb);
    unsigned long long i1, t1, i2, t2;
    if (cpu_times(&i1, &t1)) {
        Sleep(300);
        if (cpu_times(&i2, &t2) && t2 > t1)
            sb_appendf(&sb, "CPU al %.0f%%", 100.0 * (double)((t2 - t1) - (i2 - i1)) / (double)(t2 - t1));
    }
    char mem[4096];
    if (read_small("/proc/meminfo", mem, sizeof mem)) {
        long long total = meminfo_kb(mem, "MemTotal:"), avail = meminfo_kb(mem, "MemAvailable:");
        if (total > 0 && avail >= 0) {
            unsigned long long gb = 1ull << 20; /* en KB */
            sb_appendf(&sb, "%sRAM al %lld%% (%lluGB de %lluGB)", sb.len ? "; " : "", (total - avail) * 100 / total,
                       (unsigned long long)(total - avail) / gb, (unsigned long long)total / gb);
        }
    }
    const char *home = getenv("HOME");
    struct statvfs vf;
    if (home && !statvfs(home, &vf) && vf.f_blocks) {
        unsigned long long totalb = (unsigned long long)vf.f_blocks * vf.f_frsize;
        unsigned long long freeb = (unsigned long long)vf.f_bavail * vf.f_frsize;
        double used = 100.0 * (double)(totalb - freeb) / (double)totalb;
        sb_appendf(&sb, "%sdisco al %.0f%% usado (%lluGB libres)", sb.len ? "; " : "", used, freeb >> 30);
    }
    /* La batería, si hay (una laptop), y si está conectada a la corriente. */
    DIR *d = opendir("/sys/class/power_supply");
    int battery = -1;
    bool plugged = false;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512], val[64];
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
        if (!read_small(path, val, sizeof val)) continue;
        if (!strcmp(val, "Battery") && battery < 0) {
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/scope", e->d_name);
            if (read_small(path, val, sizeof val) && !strcmp(val, "Device")) continue; /* la de un mouse o audífonos */
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
            if (read_small(path, val, sizeof val)) battery = atoi(val);
        } else if (!strcmp(val, "Mains") || !strcmp(val, "USB")) {
            snprintf(path, sizeof path, "/sys/class/power_supply/%s/online", e->d_name);
            if (read_small(path, val, sizeof val) && !strcmp(val, "1")) plugged = true;
        }
    }
    if (d) closedir(d);
    if (battery >= 0 && battery <= 100)
        sb_appendf(&sb, "%sbatería al %d%% (%s)", sb.len ? "; " : "", battery, plugged ? "cargando" : "sin cargador");
    if (!sb.len) {
        sb_free(&sb);
        return xstrdup("No pude leer el estado del sistema.");
    }
    return sb_steal(&sb);
}

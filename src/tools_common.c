/* Herramientas y reglas que no dependen del sistema: las usan igual Windows
   y Linux. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "keys.h"
#include "log.h"
#include "tools.h"
#include "util.h"

bool looks_like_url(const char *s)
{
    if (str_starts_with(s, "http://") || str_starts_with(s, "https://") || str_starts_with(s, "www.")) return true;
    if (strchr(s, ' ') || strchr(s, '\\')) return false;
    const char *dot = strrchr(s, '.');
    if (!dot || dot == s || strlen(dot + 1) < 2) return false;
    static const char *tlds[] = {"com", "org", "net", "io", "mx", "es", "ar", "co", "tv", "gg", "dev", "app", "ai", "edu", "gov"};
    for (size_t i = 0; i < sizeof tlds / sizeof *tlds; i++) {
        char *end = str_lower(dot + 1);
        char *slash = strchr(end, '/');
        if (slash) *slash = 0;
        bool m = !strcmp(end, tlds[i]);
        free(end);
        if (m) return true;
    }
    return false;
}

/* "esquema:" que no sea una letra de unidad (C:) ni http/https: ms-settings:,
   search-ms:, file:, shell:... abren cosas que no son apps ni páginas. */
bool has_other_scheme(const char *s)
{
    const char *p = s;
    if (!isalpha((unsigned char)*p)) return false;
    while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    if (*p != ':') return false;
    size_t n = (size_t)(p - s);
    if (n == 1) return false;
    return !(n == 4 && !strncasecmp(s, "http", 4)) && !(n == 5 && !strncasecmp(s, "https", 5));
}

/* ¿open_app apunta a una ruta (archivo o carpeta) y no a una app o página? */
bool open_app_targets_file(const cJSON *a)
{
    char *name = str_trim(arg_str(a, "name"));
    bool file = false;
    if (!looks_like_url(name)) {
        wchar_t *w = utf8_to_wide(name);
        wchar_t *e = expand_env(w);
        file = wcschr(e, L'\\') || wcschr(e, L'/');
        free(e);
        free(w);
    }
    free(name);
    return file;
}

/* Cuántas letras hay que cambiar para pasar de un nombre a otro
   (Levenshtein): "discrod" está a 2 de "discord". */
int name_edit_distance(const char *a, const char *b)
{
    size_t n = strlen(a), m = strlen(b);
    if (n > 60 || m > 60) return 99;
    int prev[61], cur[61];
    for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
            if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
            cur[j] = v;
        }
        memcpy(prev, cur, sizeof(int) * (m + 1));
    }
    return prev[m];
}

/* "Tienes permiso para todo" / "pregúntame antes". Prenderlo con texto de
   afuera en la conversación pide un sí de voz (ver tool_needs_confirmation). */
char *tool_cambiar_permisos(const cJSON *a)
{
    bool on = arg_bool(a, "acceso_completo");
    config_set_full_access(on);
    log_msg(on ? "Acceso completo prendido por voz." : "Acceso completo apagado por voz: vuelvo a pedir permiso.");
    return xstrdup(on ? "Listo: acceso completo prendido. Ya no te pregunto nada, salvo antes de borrar."
                      : "Listo: acceso completo apagado. Vuelvo a pedirte permiso antes de acciones delicadas.");
}

char *tool_atajos_de_app(const cJSON *a)
{
    const char *app = arg_str(a, "app");
    const char *s = keys_shortcuts_for(app);
    if (s) return xstrdup(s);
#ifdef _WIN32
    return str_printf("No tengo guardados atajos de «%s»; usa los que sepas de esa app. Los de Windows: %s", app,
                      keys_shortcuts_for("windows"));
#else
    return str_printf("No tengo guardados atajos de «%s»; usa los que sepas de esa app. Los de GNOME: %s", app,
                      keys_shortcuts_for("gnome"));
#endif
}

/* ------------------------------------------------------- crear archivos --- */

/* Solo texto: nada que se pueda ejecutar (lo que lee de una página podría
   querer dejar un programa en tu PC). */
static bool runnable_ext(const char *name)
{
    static const char *const EXT[] = {".exe", ".bat", ".cmd", ".com", ".ps1", ".psm1", ".vbs", ".vbe", ".js",
                                      ".jse", ".wsf", ".wsh", ".msi", ".msp", ".scr", ".pif", ".lnk", ".url",
                                      ".reg", ".dll", ".sys", ".cpl", ".hta", ".jar", ".sh",  ".desktop", ".run",
                                      ".appimage", ".deb", ".rpm", ".py", ".pyw"};
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof EXT / sizeof *EXT; i++) {
        size_t k = strlen(EXT[i]);
        if (n >= k && !strcasecmp(name + n - k, EXT[i])) return true;
    }
    return false;
}

#define MAX_CREATE_BYTES (1024 * 1024)

char *tool_crear_archivo(const cJSON *a)
{
    const char *ruta = arg_str(a, "ruta"), *contenido = arg_str(a, "contenido");
    bool agregar = arg_bool(a, "agregar");
    char *name = str_trim(ruta);
    if (!*name) {
        free(name);
        return xstrdup("Dime cómo se llama el archivo.");
    }
    if (runnable_ext(name)) {
        free(name);
        return xstrdup("Por seguridad solo creo archivos de texto (.txt, .md, .csv, .json…), nada que se pueda "
                       "ejecutar.");
    }
    /* Solo el nombre: en el Escritorio. */
    wchar_t *path;
    if (!strchr(name, '/') && !strchr(name, '\\')) {
        wchar_t *desk = resolve_path("escritorio"), *wn = utf8_to_wide(name);
        path = path_join(desk, wn);
        free(desk);
        free(wn);
    } else {
        path = resolve_path(name);
    }
    char *r;
    wchar_t *dir = path_dirname(path);
    char *shown = wide_to_utf8(path);
    size_t len = strlen(contenido);
    if (path_is_off_limits(path)) {
        r = xstrdup("Por seguridad no uso rutas de red ni las carpetas donde Sokari guarda su configuración y su "
                    "memoria.");
    } else if (len > MAX_CREATE_BYTES) {
        r = xstrdup("Es demasiado texto para un archivo (el máximo es 1 MB).");
    } else if (dir && *dir && !dir_exists(dir)) {
        char *d = wide_to_utf8(dir);
        r = str_printf("No encontré la carpeta '%s'.", d);
        free(d);
    } else if (dir_exists(path)) {
        r = str_printf("'%s' es una carpeta, no un archivo.", shown);
    } else if (file_exists(path) && !agregar) {
        r = str_printf("Ya existe '%s' y no lo piso. Si quieres, le agrego el texto al final.", shown);
    } else {
        size_t old = 0;
        char *prev = agregar && file_exists(path) ? read_file_all(path, &old) : NULL;
        if (prev && old + len + 1 > MAX_CREATE_BYTES) {
            r = xstrdup("El archivo quedaría de más de 1 MB: no le agrego más.");
        } else {
            StrBuf sb;
            sb_init(&sb);
            if (prev) {
                sb_append_n(&sb, prev, old);
                if (old && prev[old - 1] != '\n' && len) sb_append_char(&sb, '\n');
            }
            sb_append_n(&sb, contenido, len);
            bool ok = write_file_atomic(path, sb.data ? sb.data : "", sb.len);
            r = !ok ? str_printf("No pude escribir '%s'.", shown)
                : prev ? str_printf("Listo, agregué el texto a %s.", shown)
                       : str_printf("Listo, creé %s.", shown);
            sb_free(&sb);
        }
        free(prev);
    }
    free(shown);
    free(dir);
    free(path);
    free(name);
    return r;
}

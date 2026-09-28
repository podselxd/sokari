#include "lector.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "tools.h"
#include "util.h"

#define MAX_READ 5000 /* unos 4 minutos de voz */

static char *cut(char *t)
{
    size_t n = utf8_truncate_len(t, MAX_READ);
    if (n < strlen(t)) {
        t[n] = 0;
        char *r = str_printf("%s… Hasta aquí: es largo. Si quieres, sigo con otra parte.", t);
        free(t);
        return r;
    }
    return t;
}

char *tool_leer_en_voz(const cJSON *a)
{
    const char *fuente = arg_str(a, "fuente"), *ruta = arg_str(a, "ruta");
    if (strcmp(fuente, "archivo")) return cut(tool_leer_portapapeles(NULL));
    wchar_t *path = resolve_path(ruta);
    char *r = NULL;
    size_t n = strlen(ruta);
    if (path_is_off_limits(path)) {
        r = xstrdup("Por seguridad no leo las carpetas donde Sokari guarda su configuración y su memoria.");
    } else if (!file_exists(path)) {
        r = str_printf("No encontré el archivo '%s'.", ruta);
    } else if (n > 4 && !strcasecmp(ruta + n - 4, ".pdf")) {
        char *why = NULL;
        char *t = pdf_text(path, &why);
        char *trimmed = t ? str_trim(t) : NULL;
        free(t);
        if (trimmed && *trimmed) r = cut(trimmed);
        else {
            free(trimmed);
            r = why ? why : xstrdup("Ese PDF no trae texto (puede ser una imagen escaneada).");
            why = NULL;
        }
        free(why);
    } else {
        size_t len = 0;
        char *t = read_file_all(path, &len);
        if (!t) r = str_printf("No pude leer '%s'.", ruta);
        else if (memchr(t, 0, len)) {
            free(t);
            r = xstrdup("Ese archivo no es de texto: puedo leer .txt, .md, .csv y cosas así.");
        } else {
            r = cut(t);
        }
    }
    free(path);
    return r;
}

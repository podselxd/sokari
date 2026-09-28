#include "gustos.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "tools.h"
#include "util.h"

#define MAX_GUSTOS 40

static const char *const LEVELS[] = {"encanta", "gusta", "desagrada"};

static wchar_t *gustos_path(void)
{
    ensure_dir(g_paths.memory_dir);
    return path_join(g_paths.memory_dir, L"gustos.json");
}

static cJSON *load(void)
{
    wchar_t *p = gustos_path();
    char *text = read_file_all(p, NULL);
    free(p);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsArray(j)) {
        cJSON_Delete(j);
        j = cJSON_CreateArray();
    }
    return j;
}

static void save(const cJSON *j)
{
    char *text = cJSON_Print(j);
    wchar_t *p = gustos_path();
    write_file_atomic(p, text, strlen(text));
    free(p);
    free(text);
}

char *gustos_resumen(void)
{
    cJSON *j = load();
    StrBuf by[3];
    for (int i = 0; i < 3; i++) sb_init(&by[i]);
    const cJSON *g;
    cJSON_ArrayForEach(g, j) {
        const char *cosa = cJSON_GetStringValue(cJSON_GetObjectItem(g, "cosa"));
        const char *nivel = cJSON_GetStringValue(cJSON_GetObjectItem(g, "nivel"));
        for (int i = 0; i < 3 && cosa && nivel; i++)
            if (!strcmp(nivel, LEVELS[i])) {
                if (by[i].len) sb_append(&by[i], ", ");
                sb_append(&by[i], cosa);
            }
    }
    cJSON_Delete(j);
    char *r = NULL;
    if (by[0].len || by[1].len || by[2].len) {
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, "Tus gustos hasta ahora (sé constante con ellos):");
        static const char *const SAY[] = {" te encanta: ", " te gusta: ", " te desagrada: "};
        for (int i = 0; i < 3; i++)
            if (by[i].len) {
                sb_append(&sb, SAY[i]);
                sb_append_n(&sb, by[i].data, by[i].len);
                sb_append(&sb, ";");
            }
        r = sb.data;
    }
    for (int i = 0; i < 3; i++) sb_free(&by[i]);
    return r;
}

char *tool_anotar_gusto(const cJSON *a)
{
    char *cosa = str_trim(arg_str(a, "cosa"));
    const char *nivel = arg_str(a, "nivel");
    int lv = -1;
    for (int i = 0; i < 3; i++)
        if (!strcmp(nivel, LEVELS[i])) lv = i;
    if (!*cosa || lv < 0 || strlen(cosa) > 120) {
        free(cosa);
        return xstrdup("Para anotar un gusto: cosa (corta) y nivel encanta, gusta o desagrada.");
    }
    cJSON *j = load();
    /* Si ya estaba, cambia de opinión: se quita y va al final (lo más nuevo). */
    for (int i = cJSON_GetArraySize(j) - 1; i >= 0; i--) {
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(j, i), "cosa"));
        if (c && str_contains_ci(c, cosa) && strlen(c) == strlen(cosa)) cJSON_DeleteItemFromArray(j, i);
    }
    while (cJSON_GetArraySize(j) >= MAX_GUSTOS) cJSON_DeleteItemFromArray(j, 0);
    cJSON *g = cJSON_CreateObject();
    cJSON_AddStringToObject(g, "cosa", cosa);
    cJSON_AddStringToObject(g, "nivel", LEVELS[lv]);
    cJSON_AddItemToArray(j, g);
    save(j);
    cJSON_Delete(j);
    free(cosa);
    return xstrdup("Anotado (no hace falta decirlo).");
}

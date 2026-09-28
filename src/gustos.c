#include "gustos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boveda.h"
#include "config.h"
#include "log.h"
#include "tools.h"
#include "util.h"

#define MAX_GUSTOS 40 /* los de siempre; los de para siempre no cuentan */

static const char *const LEVELS[] = {"encanta", "gusta", "desagrada"};
static char g_today[16];

void gustos_set_today(const char *ymd)
{
    snprintf(g_today, sizeof g_today, "%s", ymd ? ymd : "");
}

static char *today(void)
{
    return *g_today ? xstrdup(g_today) : format_epoch_local(now_epoch(), "%Y-%m-%d");
}

static wchar_t *gustos_path(void)
{
    ensure_dir(g_paths.memory_dir);
    return path_join(g_paths.memory_dir, L"gustos.json");
}

static void save(const cJSON *j)
{
    char *text = cJSON_Print(j);
    wchar_t *p = gustos_path();
    write_file_atomic(p, text, strlen(text));
    free(p);
    free(text);
    boveda_push(BOVEDA_GUSTOS, NULL, j);
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
    /* Si editaste su nota en Obsidian, lo que diga manda. */
    if (boveda_pull(BOVEDA_GUSTOS, NULL, j)) save(j);
    return j;
}

static const char *cosa_of(const cJSON *g)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(g, "cosa"));
}

static int level_of(const cJSON *g)
{
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(g, "nivel"));
    for (int i = 0; n && i < 3; i++)
        if (!strcmp(n, LEVELS[i])) return i;
    return -1;
}

static bool forever(const cJSON *g)
{
    return cJSON_IsTrue(cJSON_GetObjectItem(g, "siempre"));
}

static int count_forever(const cJSON *j)
{
    int n = 0;
    const cJSON *g;
    cJSON_ArrayForEach(g, j) n += forever(g);
    return n;
}

static int find(const cJSON *j, const char *cosa)
{
    for (int i = 0; i < cJSON_GetArraySize(j); i++) {
        const char *c = cosa_of(cJSON_GetArrayItem(j, i));
        if (c && str_eq_ci(c, cosa)) return i;
    }
    return -1;
}

/* Anota el día (sin repetir, los últimos GUSTOS_DIAS) y dice en cuántos días
   distintos lo ha sentido. */
static int add_day(cJSON *g, const char *day)
{
    cJSON *d = cJSON_GetObjectItem(g, "dias");
    if (!cJSON_IsArray(d)) {
        cJSON_DeleteItemFromObject(g, "dias");
        d = cJSON_AddArrayToObject(g, "dias");
    }
    bool have = false;
    const cJSON *x;
    cJSON_ArrayForEach(x, d) have |= cJSON_IsString(x) && !strcmp(x->valuestring, day);
    if (!have) cJSON_AddItemToArray(d, cJSON_CreateString(day));
    while (cJSON_GetArraySize(d) > GUSTOS_DIAS) cJSON_DeleteItemFromArray(d, 0);
    return cJSON_GetArraySize(d);
}

/* Lo de cada nivel, separado por comas: solo los de para siempre, o solo los
   demás. */
static void by_level(const cJSON *j, bool want_forever, StrBuf by[3])
{
    for (int i = 0; i < 3; i++) sb_init(&by[i]);
    const cJSON *g;
    cJSON_ArrayForEach(g, j)
    {
        int lv = level_of(g);
        if (lv < 0 || !cosa_of(g) || forever(g) != want_forever) continue;
        if (by[lv].len) sb_append(&by[lv], ", ");
        sb_append(&by[lv], cosa_of(g));
    }
}

static void append_levels(StrBuf *sb, StrBuf by[3], const char *const say[3])
{
    for (int i = 0; i < 3; i++) {
        if (by[i].len) {
            sb_append(sb, say[i]);
            sb_append_n(sb, by[i].data, by[i].len);
            sb_append(sb, ";");
        }
        sb_free(&by[i]);
    }
}

static bool any_level(const StrBuf by[3])
{
    return by[0].len || by[1].len || by[2].len;
}

char *gustos_resumen(void)
{
    static const char *const SAY[] = {" te encanta: ", " te gusta: ", " te desagrada: "};
    cJSON *j = load();
    StrBuf sb, by[3];
    sb_init(&sb);
    /* Los de para siempre primero: son parte de ella. */
    by_level(j, true, by);
    bool had = any_level(by);
    if (had) sb_append(&sb, "Tus gustos para siempre (son parte de ti y no cambian aunque te insistan):");
    append_levels(&sb, by, SAY);
    by_level(j, false, by);
    if (any_level(by))
        sb_append(&sb, had ? " Tus demás gustos (sé constante con ellos):" : "Tus gustos hasta ahora (sé constante con ellos):");
    append_levels(&sb, by, SAY);
    cJSON_Delete(j);
    if (!sb.len) {
        sb_free(&sb);
        return NULL;
    }
    return sb_steal(&sb);
}

char *gustos_describe(void)
{
    static const char *const SAY[] = {" le encanta: ", " le gusta: ", " le desagrada: "};
    cJSON *j = load();
    StrBuf sb, by[3];
    sb_init(&sb);
    int n = count_forever(j);
    if (n) {
        by_level(j, true, by);
        sb_appendf(&sb, "Para siempre (%d de %d):", n, GUSTOS_MAX_SIEMPRE);
        append_levels(&sb, by, SAY);
    }
    by_level(j, false, by);
    if (any_level(by)) sb_append(&sb, n ? " Además," : "Hasta ahora,");
    append_levels(&sb, by, SAY);
    cJSON_Delete(j);
    if (!sb.len) {
        sb_free(&sb);
        return NULL;
    }
    if (sb.data[sb.len - 1] == ';') sb.data[sb.len - 1] = '.';
    return sb_steal(&sb);
}

int gustos_count_para_siempre(void)
{
    cJSON *j = load();
    int n = count_forever(j);
    cJSON_Delete(j);
    return n;
}

int gustos_soltar_para_siempre(void)
{
    cJSON *j = load();
    int n = 0;
    cJSON *g;
    cJSON_ArrayForEach(g, j)
    {
        if (!forever(g)) continue;
        cJSON_DeleteItemFromObject(g, "siempre");
        n++;
    }
    if (n) {
        save(j);
        log_msg("Gustos: %d gusto(s) para siempre vuelven a ser normales (desde Configuración).", n);
    }
    cJSON_Delete(j);
    return n;
}

void gustos_sync(void)
{
    cJSON *j = load();
    save(j);
    cJSON_Delete(j);
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
    bool want_forever = arg_bool(a, "para_siempre");
    char *soltar = str_trim(arg_str(a, "soltar"));
    cJSON *j = load();
    char *r = NULL;
    /* Soltar uno de para siempre (para hacerle lugar a otro): vuelve a ser normal. */
    int s = *soltar ? find(j, soltar) : -1;
    if (s >= 0 && forever(cJSON_GetArrayItem(j, s))) {
        cJSON_DeleteItemFromObject(cJSON_GetArrayItem(j, s), "siempre");
        log_msg("Gustos: soltó «%s» (ya no es para siempre).", soltar);
    }
    int i = find(j, cosa);
    cJSON *g = i >= 0 ? cJSON_GetArrayItem(j, i) : NULL;
    if (g && forever(g) && level_of(g) != lv) {
        /* Es parte de ella: no cambia aunque se lo pidan. */
        r = str_printf("No cambió: «%s» es de tus gustos para siempre (te %s) y no cambia aunque te insistan. "
                       "Dilo con tu personalidad, sin sermones.",
                       cosa_of(g), LEVELS[level_of(g)]);
    } else {
        if (g && level_of(g) == lv) {
            g = cJSON_DetachItemFromArray(j, i); /* lo sintió otra vez: va al final (lo más nuevo) */
        } else {
            /* Nuevo, o cambió de opinión: empieza de cero. */
            if (g) cJSON_DeleteItemFromArray(j, i);
            g = cJSON_CreateObject();
            cJSON_AddStringToObject(g, "cosa", cosa);
            cJSON_AddStringToObject(g, "nivel", LEVELS[lv]);
        }
        char *day = today();
        int days = add_day(g, day);
        free(day);
        /* Tope para los normales: sale el más viejo (nunca uno de para siempre). */
        if (!forever(g)) {
            int normal = cJSON_GetArraySize(j) - count_forever(j);
            for (int k = 0; normal >= MAX_GUSTOS && k < cJSON_GetArraySize(j);) {
                if (forever(cJSON_GetArrayItem(j, k))) {
                    k++;
                    continue;
                }
                cJSON_DeleteItemFromArray(j, k);
                normal--;
            }
        }
        cJSON_AddItemToArray(j, g);
        /* Para siempre: si ella lo decide, o si lo ha sentido en GUSTOS_DIAS días distintos. */
        if (!forever(g) && (want_forever || days >= GUSTOS_DIAS)) {
            if (count_forever(j) < GUSTOS_MAX_SIEMPRE) {
                cJSON_AddTrueToObject(g, "siempre");
                log_msg("Gustos: «%s» (le %s) es para siempre%s.", cosa, LEVELS[lv],
                        want_forever ? "" : ": lo ha sentido en varios días");
            } else if (want_forever) {
                r = str_printf("Anotado, pero ya tienes %d gustos para siempre: para que este lo sea, suelta uno "
                               "(soltar). No hace falta decirlo.",
                               GUSTOS_MAX_SIEMPRE);
            }
        }
    }
    save(j);
    cJSON_Delete(j);
    free(soltar);
    free(cosa);
    return r ? r : xstrdup("Anotado (no hace falta decirlo).");
}

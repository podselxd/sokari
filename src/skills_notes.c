/* Skill local de notas y pendientes: "anota comprar leche", "¿qué tengo
   pendiente?", "tacha comprar leche". Se guardan por perfil en notas.json,
   junto a tu memoria. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boveda.h"
#include "intents.h"
#include "memory.h"
#include "skills.h"
#include "skills_internal.h"
#include "tools.h"
#include "util.h"

/* Guarda y reescribe su nota de Obsidian (si elegiste bóveda). */
static bool save(const wchar_t *path, const cJSON *data, const cJSON *list)
{
    bool ok = json_save(path, data);
    char *who = profile_display_name(current_speaker());
    boveda_push(BOVEDA_PENDIENTES, who, list);
    free(who);
    return ok;
}

static cJSON *load(wchar_t **path, cJSON **list)
{
    *path = memory_file(L"notas.json");
    cJSON *data = json_load_object(*path);
    const char *key = current_speaker();
    *list = cJSON_GetObjectItem(data, key);
    if (!cJSON_IsArray(*list)) {
        cJSON_DeleteItemFromObject(data, key);
        *list = cJSON_AddArrayToObject(data, key);
    }
    /* Lo que marcaste o agregaste en su nota de Obsidian manda. */
    char *who = profile_display_name(key);
    if (boveda_pull(BOVEDA_PENDIENTES, who, *list)) save(*path, data, *list);
    free(who);
    return data;
}

void skills_notes_sync(void)
{
    if (!state_try_lock(2000)) return;
    wchar_t *path;
    cJSON *list, *data = load(&path, &list);
    save(path, data, list);
    cJSON_Delete(data);
    free(path);
    state_unlock();
}

/* Lo que queda sin usar, sin relleno en las orillas (heap). */
static char *content(const Heard *h)
{
    static const char *const EDGE = " que de a y me te por favor porfa oye sokari la el en mis mi esto ";
    int a = -1, b = -1;
    for (int i = 0; i < h->n; i++) {
        if (h->used[i]) continue;
        if (a < 0 && sk_is(h, i, EDGE)) continue;
        if (a < 0) a = i;
        b = i;
    }
    while (a >= 0 && b >= a && (h->used[b] || sk_is(h, b, EDGE))) b--;
    StrBuf sb;
    sb_init(&sb);
    for (int i = a; a >= 0 && i <= b; i++)
        if (!h->used[i]) sb_appendf(&sb, "%s%s", sb.len ? " " : "", h->orig[i]);
    char *r = sb_steal(&sb);
    return r ? r : xstrdup("");
}

/* La nota que más se parece a lo que dijiste (-1 si ninguna). */
static int best_match(const cJSON *list, const char *said)
{
    char *want = intents_normalize(said);
    int best = -1, best_d = 99;
    for (int i = 0; i < cJSON_GetArraySize(list); i++) {
        const cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(list, i), "texto");
        if (!cJSON_IsString(t)) continue;
        char *have = intents_normalize(t->valuestring);
        int d = strstr(have, want) || strstr(want, have) ? 0 : name_edit_distance(have, want);
        if (d < best_d) best_d = d, best = i;
        free(have);
    }
    int limit = (int)strlen(want) / 4;
    free(want);
    return best_d <= limit ? best : -1;
}

/* Lo que es para una app o un archivo, no para tus pendientes. */
static const char *const ELSEWHERE = " archivo word documento obsidian correo mail email whatsapp discord mensaje "
                                     "excel bloc notepad block spotify youtube playlist cancion canciones musica "
                                     "telegram calendario agenda ";

char *sk_notes(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    for (int i = 0; i < h->n; i++)
        if (sk_is(h, i, ELSEWHERE)) return NULL;
    static const char *const LIST_Q[] = {"que tengo pendiente", "que pendientes tengo", "cuales son mis pendientes",
                                         "lee mis notas",       "leeme mis notas",      "dime mis pendientes",
                                         "que anote",           "que hay en mi lista",  "mis pendientes",
                                         "lee mis pendientes",  "leeme mis pendientes", "mis notas",
                                         "que tengo que hacer", "tengo pendientes"};
    for (size_t i = 0; i < sizeof LIST_Q / sizeof *LIST_Q; i++) {
        bool before[SK_MAX];
        memcpy(before, h->used, sizeof before);
        if (sk_take(h, LIST_Q[i]) < 0) continue;
        sk_take_any(h, " hoy todos todas cuales ");
        if (!sk_rest_is_filler(h)) {
            memcpy(h->used, before, sizeof before); /* "agrega X a mis pendientes": no es una pregunta */
            break;
        }
        wchar_t *path;
        cJSON *list, *data = load(&path, &list);
        int n = cJSON_GetArraySize(list);
        StrBuf sb;
        sb_init(&sb);
        for (int k = 0; k < n; k++) {
            const cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(list, k), "texto");
            if (cJSON_IsString(t)) sb_appendf(&sb, "%s%s", k == 0 ? "" : k == n - 1 ? " y " : "; ", t->valuestring);
        }
        char *items = sb_steal(&sb), *r;
        r = !n ? xstrdup("No tienes pendientes anotados.")
               : n == 1 ? str_printf("Tienes un pendiente: %s.", items)
                        : str_printf("Tienes %d pendientes: %s.", n, items);
        free(items);
        cJSON_Delete(data);
        free(path);
        return r;
    }

    /* Tachar: "tacha comprar leche", "borra comprar leche de mis pendientes". */
    bool list_named = sk_find(h, "pendientes") >= 0 || sk_find(h, "lista") >= 0 || sk_find(h, "notas") >= 0;
    int del = sk_take(h, "tacha");
    bool only_if_there = false;
    if (del < 0 && (del = sk_take(h, "ya hice")) >= 0) only_if_there = true;
    if (del < 0 && list_named) {
        static const char *const RM[] = {"borra", "quita", "elimina", "saca"};
        for (size_t i = 0; i < sizeof RM / sizeof *RM && del < 0; i++) del = sk_take(h, RM[i]);
    }
    if (del >= 0) {
        sk_take_any(h, " pendientes lista notas nota ");
        char *what = content(h);
        if (!*what) {
            free(what);
            return NULL;
        }
        wchar_t *path;
        cJSON *list, *data = load(&path, &list);
        int i = best_match(list, what);
        char *r;
        if (i < 0 && only_if_there) {
            r = NULL; /* "ya hice la tarea" sin ese pendiente: es plática */
        } else if (i < 0) {
            r = str_printf("No encontré «%s» en tus pendientes.", what);
        } else {
            const cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(list, i), "texto");
            r = str_printf("Listo, taché «%s».", cJSON_IsString(t) ? t->valuestring : what);
            cJSON_DeleteItemFromArray(list, i);
            save(path, data, list);
        }
        cJSON_Delete(data);
        free(path);
        free(what);
        return r;
    }

    /* Anotar: "anota comprar leche", "agrega pagar la luz a mis pendientes". */
    static const char *const ADD[] = {"anota", "anotame", "apunta", "apuntame", "toma nota de", "toma nota"};
    int add = -1;
    for (size_t i = 0; i < sizeof ADD / sizeof *ADD && add < 0; i++) add = sk_take(h, ADD[i]);
    if (add < 0 && list_named) {
        static const char *const PUT[] = {"agrega", "agregame", "anade", "pon", "ponme", "mete"};
        for (size_t i = 0; i < sizeof PUT / sizeof *PUT && add < 0; i++) add = sk_take(h, PUT[i]);
    }
    if (add < 0) return NULL;
    sk_take_any(h, " pendientes lista notas nota pendiente ");
    char *what = content(h);
    if (!*what) {
        free(what);
        return NULL;
    }
    wchar_t *path;
    cJSON *list, *data = load(&path, &list);
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "texto", what);
    cJSON_AddNumberToObject(it, "creado", (double)sk_now());
    cJSON_AddItemToArray(list, it);
    bool ok = save(path, data, list);
    cJSON_Delete(data);
    free(path);
    char *r = ok ? str_printf("Anotado: %s.", what) : xstrdup("No pude guardar la nota.");
    free(what);
    return r;
}

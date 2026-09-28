#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "radio.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "http.h"
#include "log.h"
#include "tools.h"
#include "util.h"

#define MAX_HISTORY 20
#define MAX_QUEUE 10

static SRWLOCK g_lock = SRWLOCK_INIT;
static cJSON *g_queue;  /* lo que se puede pasar con «siguiente» */
static int g_pos = -1;  /* cuál suena de la cola (-1: nada) */
static char *g_override; /* pruebas: búsqueda sin red */
static bool (*g_play)(const char *, char **);
static void (*g_stop)(void);

void radio_set_player(bool (*play)(const char *url, char **why), void (*stop)(void))
{
    g_play = play;
    g_stop = stop;
}

static bool do_play(const char *url, char **why)
{
    return g_play ? g_play(url, why) : radio_play(url, why);
}

static void do_stop(void)
{
    if (g_stop) g_stop();
    else radio_stop();
}

void radio_set_search_override(const char *json)
{
    free(g_override);
    g_override = json ? xstrdup(json) : NULL;
}

static wchar_t *music_path(void)
{
    ensure_dir(g_paths.memory_dir);
    return path_join(g_paths.memory_dir, L"musica.json");
}

static cJSON *load(void)
{
    wchar_t *p = music_path();
    char *t = read_file_all(p, NULL);
    free(p);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        j = cJSON_CreateObject();
    }
    const char *keys[] = {"favoritos", "historial"};
    for (int i = 0; i < 2; i++)
        if (!cJSON_IsArray(cJSON_GetObjectItem(j, keys[i]))) {
            cJSON_DeleteItemFromObject(j, keys[i]);
            cJSON_AddArrayToObject(j, keys[i]);
        }
    if (!cJSON_IsObject(cJSON_GetObjectItem(j, "playlists"))) {
        cJSON_DeleteItemFromObject(j, "playlists");
        cJSON_AddObjectToObject(j, "playlists");
    }
    return j;
}

static void save(const cJSON *j)
{
    char *t = cJSON_Print(j);
    wchar_t *p = music_path();
    write_file_atomic(p, t, strlen(t));
    free(p);
    free(t);
}

static cJSON *station(const char *name, const char *url)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "nombre", name);
    cJSON_AddStringToObject(s, "url", url);
    return s;
}

static const char *sname(const cJSON *s)
{
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(s, "nombre"));
    return n ? n : "";
}

static const char *surl(const cJSON *s)
{
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(s, "url"));
    return u ? u : "";
}

/* La de la lista cuyo nombre trae el texto (sin mayúsculas ni acentos). */
static const cJSON *find_named(const cJSON *list, const char *text)
{
    char *want = str_lower(text);
    const cJSON *hit = NULL, *s;
    cJSON_ArrayForEach(s, list) {
        char *n = str_lower(sname(s));
        if (*want && (strstr(n, want) || strstr(want, n))) hit = s;
        free(n);
        if (hit) break;
    }
    free(want);
    return hit;
}

/* Estaciones de radio-browser: por nombre y, si no hay, por género. */
static cJSON *search(const char *q)
{
    cJSON *out = g_override ? cJSON_Parse(g_override) : NULL;
    const char *by[] = {"name", "tag"};
    for (int i = 0; i < 2 && !g_override && (!out || !cJSON_GetArraySize(out)); i++) {
        cJSON_Delete(out);
        out = NULL;
        char *enc = url_encode(q);
        char *url = str_printf("https://all.api.radio-browser.info/json/stations/search?%s=%s&limit=%d&hidebroken=true"
                               "&order=clickcount&reverse=true",
                               by[i], enc, MAX_QUEUE);
        free(enc);
        HttpRequest hr = {.method = "GET", .url = url, .headers = "User-Agent: Sokari\r\n", .timeout_ms = 10000};
        HttpResponse r = http_request(&hr);
        free(url);
        if (r.status == 200) out = cJSON_Parse(r.body);
        http_response_free(&r);
    }
    /* Solo lo que sirve: nombre y dirección. */
    cJSON *list = cJSON_CreateArray(), *s;
    cJSON_ArrayForEach(s, out) {
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(s, "url_resolved"));
        if (!u || !*u) u = cJSON_GetStringValue(cJSON_GetObjectItem(s, "url"));
        char *n = str_trim(cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")) ?: "");
        if (u && !strncmp(u, "http", 4) && *n) cJSON_AddItemToArray(list, station(n, u));
        free(n);
    }
    cJSON_Delete(out);
    return list;
}

/* Toca la de la cola en pos y la anota en el historial. */
static char *play_at(cJSON *music, int pos)
{
    const cJSON *s = cJSON_GetArrayItem(g_queue, pos);
    if (!s) return xstrdup("Ya no hay más en la lista.");
    char *why = NULL;
    if (!do_play(surl(s), &why)) {
        char *r = str_printf("No pude poner %s: %s", sname(s), why ? why : "no suena");
        free(why);
        return r;
    }
    g_pos = pos;
    cJSON *h = cJSON_GetObjectItem(music, "historial");
    for (int i = cJSON_GetArraySize(h) - 1; i >= 0; i--)
        if (!strcmp(sname(cJSON_GetArrayItem(h, i)), sname(s))) cJSON_DeleteItemFromArray(h, i);
    cJSON_InsertItemInArray(h, 0, station(sname(s), surl(s)));
    while (cJSON_GetArraySize(h) > MAX_HISTORY) cJSON_DeleteItemFromArray(h, MAX_HISTORY);
    save(music);
    log_msg("Radio: %s (%s)", sname(s), surl(s));
    return str_printf("Listo, puse %s.", sname(s));
}

static void set_queue(cJSON *list)
{
    cJSON_Delete(g_queue);
    g_queue = list;
    g_pos = -1;
}

static char *names_of(const cJSON *list)
{
    StrBuf sb;
    sb_init(&sb);
    const cJSON *s;
    cJSON_ArrayForEach(s, list) {
        if (sb.len) sb_append(&sb, ", ");
        sb_append(&sb, s->string && !cJSON_GetObjectItem(s, "nombre") ? s->string : sname(s));
    }
    return sb.data ? sb.data : xstrdup("");
}

char *tool_radio(const cJSON *a)
{
    const char *acc = arg_str(a, "accion"), *q = arg_str(a, "busqueda"), *nombre = arg_str(a, "nombre");
    AcquireSRWLockExclusive(&g_lock);
    cJSON *music = load();
    cJSON *fav = cJSON_GetObjectItem(music, "favoritos"), *pls = cJSON_GetObjectItem(music, "playlists");
    const cJSON *cur = g_pos >= 0 ? cJSON_GetArrayItem(g_queue, g_pos) : NULL;
    char *r;
    if (!strcmp(acc, "parar")) {
        do_stop();
        g_pos = -1;
        r = xstrdup("Listo, la quité.");
    } else if (!strcmp(acc, "siguiente")) {
        r = g_queue && g_pos + 1 < cJSON_GetArraySize(g_queue) ? play_at(music, g_pos + 1)
                                                                 : xstrdup("No hay otra en la lista.");
    } else if (!strcmp(acc, "guardar_favorito")) {
        if (!cur) r = xstrdup("No está sonando nada para guardarlo.");
        else if (find_named(fav, sname(cur))) r = str_printf("%s ya estaba en tus favoritos.", sname(cur));
        else {
            cJSON_AddItemToArray(fav, station(sname(cur), surl(cur)));
            save(music);
            r = str_printf("Guardé %s en tus favoritos.", sname(cur));
        }
    } else if (!strcmp(acc, "agregar_a_playlist")) {
        if (!cur || !*nombre) r = xstrdup(!cur ? "No está sonando nada para agregarlo." : "¿A qué playlist?");
        else {
            cJSON *pl = cJSON_GetObjectItem(pls, nombre);
            if (!pl) pl = cJSON_AddArrayToObject(pls, nombre);
            if (!find_named(pl, sname(cur))) cJSON_AddItemToArray(pl, station(sname(cur), surl(cur)));
            save(music);
            r = str_printf("Agregué %s a tu playlist «%s».", sname(cur), nombre);
        }
    } else if (!strcmp(acc, "poner_playlist")) {
        const cJSON *pl = NULL, *p;
        cJSON_ArrayForEach(p, pls) if (str_contains_ci(p->string, nombre) || str_contains_ci(nombre, p->string)) pl = p;
        if (!pl || !cJSON_GetArraySize(pl)) r = str_printf("No tengo una playlist «%s».", nombre);
        else {
            set_queue(cJSON_Duplicate(pl, 1));
            r = play_at(music, 0);
        }
    } else if (!strcmp(acc, "listas")) {
        char *f = names_of(fav), *p = names_of(pls), *h = names_of(cJSON_GetObjectItem(music, "historial"));
        r = str_printf("Favoritos: %s. Playlists: %s. Lo último que pusiste: %s.", *f ? f : "ninguno",
                       *p ? p : "ninguna", *h ? h : "nada");
        free(f), free(p), free(h);
    } else if (!strcmp(acc, "poner")) {
        /* Primero lo tuyo (favoritos e historial), luego la búsqueda. */
        const cJSON *mine = *q ? find_named(fav, q) : cJSON_GetArrayItem(fav, 0);
        if (!mine && *q) mine = find_named(cJSON_GetObjectItem(music, "historial"), q);
        if (mine) {
            cJSON *one = cJSON_CreateArray();
            cJSON_AddItemToArray(one, station(sname(mine), surl(mine)));
            set_queue(one);
            r = play_at(music, 0);
        } else if (!*q) {
            r = xstrdup("¿Qué quieres escuchar? Dime una estación o un género (rock, noticias, jazz…).");
        } else {
            cJSON *found = search(q);
            if (!found || !cJSON_GetArraySize(found)) {
                cJSON_Delete(found);
                r = str_printf("No encontré estaciones de «%s».", q);
            } else {
                set_queue(found);
                r = play_at(music, 0);
            }
        }
    } else {
        r = xstrdup("No conozco esa acción de la radio.");
    }
    cJSON_Delete(music);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

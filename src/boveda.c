/* La memoria de Sokari en tu bóveda de Obsidian: ver boveda.h. */
#include "boveda.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "gustos.h"
#include "log.h"
#include "memory.h"
#include "radio.h"
#include "skills.h"
#include "tools.h"
#include "util.h"

#define TAG_FOREVER "#para-siempre"

static const char *const LEVELS[] = {"encanta", "gusta", "desagrada"};
static const char *const LEVEL_HEADS[] = {"Le encanta", "Le gusta", "Le desagrada"};
static const char *const KIND_IDS[BOVEDA_KINDS] = {"gustos", "datos", "pendientes", "musica"};

/* El estado (qué escribió Sokari en cada nota) y las notas: de a uno. */
static SRWLOCK g_lock = SRWLOCK_INIT;
static bool g_warned[BOVEDA_KINDS];

/* ------------------------------------------------------ dónde está --- */

wchar_t *boveda_detect(void)
{
    static const wchar_t *const CONFIGS[] = {
#ifdef _WIN32
        L"%APPDATA%\\obsidian\\obsidian.json",
#else
        L"~/.config/obsidian/obsidian.json",
        L"~/.var/app/md.obsidian.Obsidian/config/obsidian/obsidian.json", /* Flatpak */
        L"~/snap/obsidian/current/.config/obsidian/obsidian.json",
#endif
    };
    wchar_t *found = NULL;
    for (size_t i = 0; i < sizeof CONFIGS / sizeof *CONFIGS && !found; i++) {
        wchar_t *cfg = expand_env(CONFIGS[i]);
        cJSON *c = json_load_object(cfg);
        free(cfg);
        cJSON *vaults = cJSON_GetObjectItem(c, "vaults");
        /* Primero la que está abierta; si no, la primera que exista. */
        for (int pass = 0; pass < 2 && !found; pass++) {
            cJSON *v;
            cJSON_ArrayForEach(v, vaults)
            {
                bool open = cJSON_IsTrue(cJSON_GetObjectItem(v, "open"));
                const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(v, "path"));
                if ((pass == 0) != open || !path) continue;
                wchar_t *w = utf8_to_wide(path);
                if (dir_exists(w)) {
                    found = w;
                    break;
                }
                free(w);
            }
        }
        cJSON_Delete(c);
    }
    return found;
}

wchar_t *boveda_dir(void)
{
    char *v = config_obsidian_vault();
    wchar_t *r = NULL;
    if (*v) {
        wchar_t *w = utf8_to_wide(v);
        if (dir_exists(w)) {
            r = path_join(w, L"Sokari");
            if (!ensure_dir(r)) {
                free(r);
                r = NULL;
            }
        }
        free(w);
    }
    free(v);
    return r;
}

/* Un nombre que sirve para archivo y para enlace de Obsidian. */
static char *safe_name(const char *text)
{
    char *s = xstrdup(text);
    for (char *p = s; *p; p++)
        if (strchr("<>:\"/\\|?*#^[]", *p) || (unsigned char)*p < 32) *p = ' ';
    char *t = str_trim(s);
    free(s);
    str_collapse_spaces(t);
    if (!*t) {
        free(t);
        return xstrdup("sin nombre");
    }
    return t;
}

static bool is_me(const char *who)
{
    return !who || !*who || !strcmp(who, DEFAULT_PROFILE);
}

wchar_t *boveda_note_path(BovedaKind k, const char *who)
{
    wchar_t *dir = boveda_dir();
    if (!dir) return NULL;
    char *slug = is_me(who) ? NULL : safe_name(who), *name;
    switch (k) {
    case BOVEDA_GUSTOS:
        name = xstrdup("Gustos de Sokari.md");
        break;
    case BOVEDA_MUSICA:
        name = xstrdup("Música de Sokari.md");
        break;
    case BOVEDA_DATOS:
        name = slug ? str_printf("Datos de %s.md", slug) : xstrdup("Tus datos.md");
        break;
    default:
        name = slug ? str_printf("Pendientes de %s.md", slug) : xstrdup("Tus pendientes.md");
        break;
    }
    wchar_t *wn = utf8_to_wide(name), *r = path_join(dir, wn);
    free(wn);
    free(name);
    free(slug);
    free(dir);
    return r;
}

/* ----------------------------------------------------------- estado --- */

static wchar_t *state_file(void)
{
    return path_join(g_paths.local_dir, L"boveda.json");
}

/* El hash de lo último que Sokari escribió (o ya leyó) en esa nota (heap). */
static char *state_get(const wchar_t *note)
{
    wchar_t *sf = state_file();
    cJSON *s = json_load_object(sf);
    free(sf);
    char *key = wide_to_utf8(note);
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, key));
    char *r = v ? xstrdup(v) : NULL;
    free(key);
    cJSON_Delete(s);
    return r;
}

static void state_set(const wchar_t *note, const char *hash)
{
    wchar_t *sf = state_file();
    cJSON *s = json_load_object(sf);
    char *key = wide_to_utf8(note);
    const char *old = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, key));
    if (!old || strcmp(old, hash)) {
        cJSON_DeleteItemFromObjectCaseSensitive(s, key);
        cJSON_AddStringToObject(s, key, hash);
        json_save(sf, s);
    }
    free(key);
    cJSON_Delete(s);
    free(sf);
}

/* Un respaldo por tema y por día (el de antes del primer cambio del día), en
   <memoria>/respaldos. */
static void backup(const char *name, const char *ext, const char *content)
{
    char *day = format_epoch_local(now_epoch(), "%Y-%m-%d");
    char *file = str_printf("%s-%s.%s", name, day, ext);
    wchar_t *dir = path_join(g_paths.memory_dir, L"respaldos"), *wf = utf8_to_wide(file), *p = path_join(dir, wf);
    if (ensure_dir(dir) && !file_exists(p)) write_file_atomic(p, content, strlen(content));
    free(p);
    free(wf);
    free(dir);
    free(file);
    free(day);
}

static void backup_json(BovedaKind k, const char *who, const cJSON *data)
{
    char *slug = is_me(who) ? NULL : safe_name(who);
    char *name = slug ? str_printf("%s-%s", KIND_IDS[k], slug) : xstrdup(KIND_IDS[k]);
    char *text = cJSON_Print(data);
    if (text) backup(name, "json", text);
    free(text);
    free(name);
    free(slug);
}

/* ------------------------------------------------------ leer notas --- */

typedef struct {
    const char *p;
} Lines;

/* El siguiente renglón, sin espacios en las orillas (heap), o NULL al final. */
static char *next_line(Lines *l)
{
    if (!*l->p) return NULL;
    const char *e = strchr(l->p, '\n');
    size_t n = e ? (size_t)(e - l->p) : strlen(l->p);
    char *raw = xstrndup(l->p, n);
    l->p += n + (e ? 1 : 0);
    char *t = str_trim(raw);
    free(raw);
    return t;
}

/* "## Le encanta" → "Le encanta" (heap); NULL si no es un título. */
static char *heading(const char *line)
{
    if (*line != '#') return NULL;
    const char *p = line;
    while (*p == '#') p++;
    if (*p != ' ' && *p != '\t') return NULL; /* "#etiqueta" no es título */
    return str_trim(p);
}

/* "- algo", "* algo" o "+ algo" → "algo"; NULL si no es un renglón de lista. */
static const char *bullet(const char *line)
{
    if ((*line == '-' || *line == '*' || *line == '+') && (line[1] == ' ' || line[1] == '\t')) {
        const char *p = line + 1;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }
    return NULL;
}

/* "[ ] algo" → 0, "[x] algo" → 1 (y *p pasa el cuadrito); si no trae, -1. */
static int checkbox(const char **p)
{
    const char *s = *p;
    if (s[0] != '[' || !s[1] || s[2] != ']') return -1;
    int done = s[1] == 'x' || s[1] == 'X';
    if (!done && s[1] != ' ') return -1;
    s += 3;
    while (*s == ' ' || *s == '\t') s++;
    *p = s;
    return done;
}

/* Quita la etiqueta #para-siempre (o #siempre) del final; true si la traía. */
static bool strip_forever(char *s)
{
    static const char *const TAGS[] = {TAG_FOREVER, "#siempre"};
    bool had = false;
    for (bool again = true; again;) {
        again = false;
        size_t n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
        for (int i = 0; i < 2 && !again; i++) {
            size_t t = strlen(TAGS[i]);
            if (n >= t && str_eq_ci(s + n - t, TAGS[i]) && (n == t || s[n - t - 1] == ' ' || s[n - t - 1] == '\t')) {
                s[n - t] = 0;
                had = again = true;
            }
        }
    }
    return had;
}

/* Sin saltos de renglón (una nota es un renglón por cosa). */
static char *one_line(const char *s)
{
    char *r = xstrdup(s ? s : "");
    for (char *p = r; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    return r;
}

static int level_index(const char *nivel)
{
    for (int i = 0; nivel && i < 3; i++)
        if (!strcmp(nivel, LEVELS[i])) return i;
    return -1;
}

static const cJSON *find_by(const cJSON *arr, const char *field, const char *value)
{
    const cJSON *x;
    cJSON_ArrayForEach(x, arr)
    {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(x, field));
        if (v && str_eq_ci(v, value)) return x;
    }
    return NULL;
}

/* ----------------------------------------------------------- gustos --- */

static char *render_gustos(const cJSON *data)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "# Gustos de Sokari\n\n"
                   "Lo que le encanta, le gusta y le desagrada, según lo que han vivido. Puedes editar esta nota "
                   "(cambiar de lugar, borrar o agregar renglones): Sokari la lee antes de contestar. Lo marcado "
                   "con " TAG_FOREVER " es parte de ella y no cambia aunque le insistas.\n");
    for (int lv = 0; lv < 3; lv++) {
        sb_appendf(&sb, "\n## %s\n", LEVEL_HEADS[lv]);
        const cJSON *g;
        cJSON_ArrayForEach(g, data)
        {
            const char *cosa = cJSON_GetStringValue(cJSON_GetObjectItem(g, "cosa"));
            if (!cosa || level_index(cJSON_GetStringValue(cJSON_GetObjectItem(g, "nivel"))) != lv) continue;
            char *c = one_line(cosa);
            sb_appendf(&sb, "- %s%s\n", c, cJSON_IsTrue(cJSON_GetObjectItem(g, "siempre")) ? " " TAG_FOREVER : "");
            free(c);
        }
    }
    return sb_steal(&sb);
}

static int heading_level(const char *title)
{
    char *t = str_lower(title);
    int lv = -1;
    if (strstr(t, "desagrad") || strstr(t, "no le gusta") || strstr(t, "odia")) lv = 2;
    else if (strstr(t, "encanta")) lv = 0;
    else if (strstr(t, "gusta")) lv = 1;
    free(t);
    return lv;
}

/* Lo de la nota, con lo que no se ve en ella (los días) y en el orden de
   antes: lo que ya tenía va primero, lo nuevo al final. */
static cJSON *parse_gustos(const char *text, const cJSON *current)
{
    Lines l = {text};
    cJSON *seen = cJSON_CreateArray();
    bool any = false;
    int lv = -1;
    char *line;
    while ((line = next_line(&l))) {
        char *h = heading(line);
        if (h) {
            lv = heading_level(h);
            any |= lv >= 0;
            free(h);
        } else if (lv >= 0 && bullet(line)) {
            const char *p = bullet(line);
            checkbox(&p);
            char *cosa = xstrdup(p);
            bool forever = strip_forever(cosa);
            char *c = str_trim(cosa);
            c[utf8_truncate_len(c, 120)] = 0;
            if (*c && !find_by(seen, "cosa", c)) {
                cJSON *g = cJSON_CreateObject();
                cJSON_AddStringToObject(g, "cosa", c);
                cJSON_AddStringToObject(g, "nivel", LEVELS[lv]);
                if (forever) cJSON_AddBoolToObject(g, "siempre", true);
                cJSON_AddItemToArray(seen, g);
            }
            free(c);
            free(cosa);
        }
        free(line);
    }
    if (!any) {
        cJSON_Delete(seen);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    /* Primero en el orden de antes (el más viejo sale primero al llegar al tope). */
    const cJSON *old;
    cJSON_ArrayForEach(old, current)
    {
        const char *cosa = cJSON_GetStringValue(cJSON_GetObjectItem(old, "cosa"));
        const cJSON *now = cosa ? find_by(seen, "cosa", cosa) : NULL;
        if (!now || cJSON_GetObjectItem(now, "tomado")) continue;
        cJSON *g = cJSON_Duplicate(now, true);
        const char *was = cJSON_GetStringValue(cJSON_GetObjectItem(old, "nivel"));
        const cJSON *days = cJSON_GetObjectItem(old, "dias");
        if (was && !strcmp(was, cJSON_GetStringValue(cJSON_GetObjectItem(now, "nivel"))) && cJSON_IsArray(days))
            cJSON_AddItemToObject(g, "dias", cJSON_Duplicate(days, true));
        cJSON_AddItemToArray(out, g);
        cJSON_AddTrueToObject((cJSON *)now, "tomado");
    }
    cJSON *g;
    cJSON_ArrayForEach(g, seen)
    {
        if (cJSON_GetObjectItem(g, "tomado")) continue;
        cJSON_AddItemToArray(out, cJSON_Duplicate(g, true));
    }
    cJSON_Delete(seen);
    return out;
}

/* La primera vez: se juntan (lo que ya sabía gana; para siempre si en
   cualquiera de los dos lo era). */
static cJSON *union_gustos(const cJSON *data, const cJSON *note)
{
    cJSON *out = cJSON_Duplicate(data, true);
    const cJSON *n;
    cJSON_ArrayForEach(n, note)
    {
        const char *cosa = cJSON_GetStringValue(cJSON_GetObjectItem(n, "cosa"));
        cJSON *have = (cJSON *)find_by(out, "cosa", cosa);
        if (!have) cJSON_AddItemToArray(out, cJSON_Duplicate(n, true));
        else if (cJSON_IsTrue(cJSON_GetObjectItem(n, "siempre")) && !cJSON_IsTrue(cJSON_GetObjectItem(have, "siempre")))
            cJSON_AddBoolToObject(have, "siempre", true);
    }
    return out;
}

/* ------------------------------------------------------------ datos --- */

static char *render_datos(const char *who, const cJSON *data)
{
    StrBuf sb;
    sb_init(&sb);
    char *name = is_me(who) ? NULL : one_line(who);
    sb_appendf(&sb, "# Lo que Sokari sabe de %s\n\n", name ? name : "ti");
    free(name);
    sb_append(&sb, "Un dato por renglón: «- dato: valor». Puedes corregirlos, borrarlos o agregar los tuyos: Sokari "
                   "lee esta nota antes de usarlos.\n\n");
    const cJSON *f;
    cJSON_ArrayForEach(f, data)
    {
        if (!f->string) continue;
        char *v = cJSON_IsString(f) ? xstrdup(f->valuestring) : cJSON_PrintUnformatted(f);
        char *k = one_line(f->string), *vv = one_line(v);
        sb_appendf(&sb, "- %s: %s\n", k, vv);
        free(vv);
        free(k);
        free(v);
    }
    return sb_steal(&sb);
}

static cJSON *parse_datos(const char *text, const cJSON *current)
{
    (void)current;
    Lines l = {text};
    cJSON *out = cJSON_CreateObject();
    bool any = false;
    char *line;
    while ((line = next_line(&l))) {
        char *h = heading(line);
        if (h) any = true;
        free(h);
        const char *p = bullet(line);
        const char *colon = p ? strchr(p, ':') : NULL;
        if (colon) {
            char *k0 = xstrndup(p, (size_t)(colon - p)), *k1 = str_trim(k0), *key = str_lower(k1);
            char *val = str_trim(colon + 1);
            if (*key) {
                any = true;
                cJSON_DeleteItemFromObjectCaseSensitive(out, key);
                cJSON_AddStringToObject(out, key, val);
            }
            free(val);
            free(key);
            free(k1);
            free(k0);
        }
        free(line);
    }
    if (!any) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static cJSON *union_datos(const cJSON *data, const cJSON *note)
{
    cJSON *out = cJSON_Duplicate(data, true);
    const cJSON *n;
    cJSON_ArrayForEach(n, note)
    {
        if (!cJSON_GetObjectItemCaseSensitive(out, n->string))
            cJSON_AddItemToObject(out, n->string, cJSON_Duplicate(n, true));
    }
    return out;
}

/* ------------------------------------------------------- pendientes --- */

static char *render_pendientes(const char *who, const cJSON *data)
{
    StrBuf sb;
    sb_init(&sb);
    char *name = is_me(who) ? NULL : one_line(who);
    if (name) sb_appendf(&sb, "# Pendientes de %s\n\n", name);
    else sb_append(&sb, "# Tus pendientes\n\n");
    free(name);
    sb_append(&sb, "Márcalos como hechos ([x]) o bórralos y Sokari los tacha; también puedes agregar los tuyos.\n\n");
    const cJSON *it;
    cJSON_ArrayForEach(it, data)
    {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "texto"));
        if (!t) continue;
        char *tt = one_line(t);
        sb_appendf(&sb, "- [ ] %s\n", tt);
        free(tt);
    }
    return sb_steal(&sb);
}

static cJSON *parse_pendientes(const char *text, const cJSON *current)
{
    Lines l = {text};
    cJSON *out = cJSON_CreateArray();
    bool any = false;
    char *line;
    while ((line = next_line(&l))) {
        char *h = heading(line);
        if (h) any = true;
        free(h);
        const char *p = bullet(line);
        if (p) {
            any = true;
            int done = checkbox(&p);
            char *t = str_trim(p);
            if (done != 1 && *t && !find_by(out, "texto", t)) {
                const cJSON *old = find_by(current, "texto", t);
                const cJSON *when = old ? cJSON_GetObjectItem(old, "creado") : NULL;
                cJSON *it = cJSON_CreateObject();
                cJSON_AddStringToObject(it, "texto", t);
                cJSON_AddNumberToObject(it, "creado", cJSON_IsNumber(when) ? when->valuedouble : (double)(long long)now_epoch());
                cJSON_AddItemToArray(out, it);
            }
            free(t);
        }
        free(line);
    }
    if (!any) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static cJSON *union_pendientes(const cJSON *data, const cJSON *note)
{
    cJSON *out = cJSON_Duplicate(data, true);
    const cJSON *n;
    cJSON_ArrayForEach(n, note)
    {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(n, "texto"));
        if (t && !find_by(out, "texto", t)) cJSON_AddItemToArray(out, cJSON_Duplicate(n, true));
    }
    return out;
}

/* ----------------------------------------------------------- música --- */

static void render_stations(StrBuf *sb, const cJSON *list)
{
    const cJSON *s;
    cJSON_ArrayForEach(s, list)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(s, "nombre"));
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(s, "url"));
        if (!u) continue;
        char *name = one_line(n && *n ? n : u);
        for (char *p = name; *p; p++)
            if (*p == '[') *p = '(';
            else if (*p == ']') *p = ')';
        sb_appendf(sb, "- [%s](%s)\n", name, u);
        free(name);
    }
}

static char *render_musica(const cJSON *data)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "# Música de Sokari\n\n"
                   "Sus estaciones de radio. Borra un renglón para quitarla o agrega una con «- [Nombre](dirección "
                   "del stream)». Para una playlist nueva, un título «## Playlist: nombre».\n\n## Favoritos\n");
    render_stations(&sb, cJSON_GetObjectItem(data, "favoritos"));
    const cJSON *pl;
    cJSON_ArrayForEach(pl, cJSON_GetObjectItem(data, "playlists"))
    {
        if (!pl->string) continue;
        char *n = one_line(pl->string);
        sb_appendf(&sb, "\n## Playlist: %s\n", n);
        free(n);
        render_stations(&sb, pl);
    }
    sb_append(&sb, "\n## Historial\n\nLo último que sonó (solo para ver: los cambios aquí no se leen).\n\n");
    render_stations(&sb, cJSON_GetObjectItem(data, "historial"));
    return sb_steal(&sb);
}

/* "[Nombre](url)" o una dirección sola → {nombre, url}; NULL si no es una
   dirección http(s). */
static cJSON *parse_station(const char *p)
{
    char *name = NULL, *url = NULL;
    if (*p == '[') {
        const char *e = strstr(p, "](");
        const char *close = e ? strrchr(e + 2, ')') : NULL;
        if (!close) return NULL;
        char *n0 = xstrndup(p + 1, (size_t)(e - p - 1));
        name = str_trim(n0);
        free(n0);
        char *u0 = xstrndup(e + 2, (size_t)(close - e - 2));
        url = str_trim(u0);
        free(u0);
    } else {
        url = str_trim(p);
        name = xstrdup(url);
    }
    cJSON *s = NULL;
    if ((str_starts_with(url, "http://") || str_starts_with(url, "https://")) && !strchr(url, ' ')) {
        s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "nombre", *name ? name : url);
        cJSON_AddStringToObject(s, "url", url);
    }
    free(name);
    free(url);
    return s;
}

static cJSON *parse_musica(const char *text, const cJSON *current)
{
    Lines l = {text};
    cJSON *out = cJSON_CreateObject();
    cJSON *fav = cJSON_AddArrayToObject(out, "favoritos");
    const cJSON *hist = cJSON_GetObjectItem(current, "historial");
    cJSON_AddItemToObject(out, "historial", cJSON_IsArray(hist) ? cJSON_Duplicate(hist, true) : cJSON_CreateArray());
    cJSON *pls = cJSON_AddObjectToObject(out, "playlists");
    cJSON *into = NULL;
    bool any = false;
    char *line;
    while ((line = next_line(&l))) {
        char *h = heading(line);
        if (h) {
            char *t = str_lower(h);
            into = NULL;
            if (strstr(t, "favorit")) {
                into = fav;
                any = true;
            } else if (str_starts_with(t, "playlist")) {
                const char *c = strchr(h, ':');
                char *name = c ? str_trim(c + 1) : xstrdup("");
                if (*name) {
                    into = cJSON_GetObjectItemCaseSensitive(pls, name);
                    if (!into) into = cJSON_AddArrayToObject(pls, name);
                    any = true;
                }
                free(name);
            } else if (strstr(t, "historial")) {
                any = true;
            }
            free(t);
            free(h);
        } else if (into && bullet(line)) {
            cJSON *s = parse_station(bullet(line));
            const char *u = s ? cJSON_GetStringValue(cJSON_GetObjectItem(s, "url")) : NULL;
            if (s && !find_by(into, "url", u)) cJSON_AddItemToArray(into, s);
            else cJSON_Delete(s);
        }
        free(line);
    }
    if (!any) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static void union_stations(cJSON *into, const cJSON *from)
{
    const cJSON *s;
    cJSON_ArrayForEach(s, from)
    {
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(s, "url"));
        if (u && !find_by(into, "url", u)) cJSON_AddItemToArray(into, cJSON_Duplicate(s, true));
    }
}

static cJSON *union_musica(const cJSON *data, const cJSON *note)
{
    cJSON *out = cJSON_Duplicate(data, true);
    cJSON *fav = cJSON_GetObjectItem(out, "favoritos");
    if (!cJSON_IsArray(fav)) {
        cJSON_DeleteItemFromObject(out, "favoritos");
        fav = cJSON_AddArrayToObject(out, "favoritos");
    }
    union_stations(fav, cJSON_GetObjectItem(note, "favoritos"));
    cJSON *pls = cJSON_GetObjectItem(out, "playlists");
    if (!cJSON_IsObject(pls)) {
        cJSON_DeleteItemFromObject(out, "playlists");
        pls = cJSON_AddObjectToObject(out, "playlists");
    }
    const cJSON *pl;
    cJSON_ArrayForEach(pl, cJSON_GetObjectItem(note, "playlists"))
    {
        cJSON *mine = cJSON_GetObjectItemCaseSensitive(pls, pl->string);
        if (!mine) mine = cJSON_AddArrayToObject(pls, pl->string);
        union_stations(mine, pl);
    }
    return out;
}

/* ------------------------------------------------------- por tema --- */

char *boveda_render(BovedaKind k, const char *who, const cJSON *data)
{
    switch (k) {
    case BOVEDA_GUSTOS:
        return render_gustos(data);
    case BOVEDA_DATOS:
        return render_datos(who, data);
    case BOVEDA_PENDIENTES:
        return render_pendientes(who, data);
    default:
        return render_musica(data);
    }
}

cJSON *boveda_parse(BovedaKind k, const char *text, const cJSON *current)
{
    switch (k) {
    case BOVEDA_GUSTOS:
        return parse_gustos(text, current);
    case BOVEDA_DATOS:
        return parse_datos(text, current);
    case BOVEDA_PENDIENTES:
        return parse_pendientes(text, current);
    default:
        return parse_musica(text, current);
    }
}

static cJSON *union_of(BovedaKind k, const cJSON *data, const cJSON *note)
{
    switch (k) {
    case BOVEDA_GUSTOS:
        return union_gustos(data, note);
    case BOVEDA_DATOS:
        return union_datos(data, note);
    case BOVEDA_PENDIENTES:
        return union_pendientes(data, note);
    default:
        return union_musica(data, note);
    }
}

/* data se queda con lo de from (mismos hijos, mismo tipo). */
static void replace_children(cJSON *data, cJSON *from)
{
    while (data->child) cJSON_Delete(cJSON_DetachItemViaPointer(data, data->child));
    while (from->child) {
        cJSON *c = cJSON_DetachItemViaPointer(from, from->child);
        if (cJSON_IsArray(data)) cJSON_AddItemToArray(data, c);
        else cJSON_AddItemToObject(data, c->string, c);
    }
}

bool boveda_pull(BovedaKind k, const char *who, cJSON *data)
{
    wchar_t *path = boveda_note_path(k, who);
    if (!path || !data) {
        free(path);
        return false;
    }
    bool changed = false;
    AcquireSRWLockExclusive(&g_lock);
    char *text = read_file_all(path, NULL);
    if (text && !str_is_blank(text)) {
        char *h = sha256_hex(text);
        char *stored = state_get(path);
        if (!stored || strcmp(stored, h)) {
            cJSON *note = boveda_parse(k, text, data);
            /* La primera vez que la ve (otra PC, otra bóveda): se juntan. Si
               ya la había escrito, la cambiaste tú: lo que diga manda. */
            cJSON *want = note && !stored ? union_of(k, data, note) : note;
            if (want != note) cJSON_Delete(note);
            bool understood = want != NULL;
            if (want && !cJSON_Compare(want, data, true)) {
                backup_json(k, who, data);
                replace_children(data, want);
                changed = true;
                char *p = wide_to_utf8(path_basename(path));
                log_msg("Bóveda: tomé tus cambios de «%s».", p);
                free(p);
            }
            cJSON_Delete(want);
            /* Ya se leyó: no se vuelve a leer hasta que cambie. Si no se
               entendió, no: se respalda antes de reescribirla (boveda_push). */
            if (understood) state_set(path, h);
        }
        free(stored);
        free(h);
    }
    free(text);
    ReleaseSRWLockExclusive(&g_lock);
    free(path);
    return changed;
}

void boveda_push(BovedaKind k, const char *who, const cJSON *data)
{
    wchar_t *path = boveda_note_path(k, who);
    if (!path || !data) {
        free(path);
        return;
    }
    char *text = boveda_render(k, who, data);
    AcquireSRWLockExclusive(&g_lock);
    char *old = read_file_all(path, NULL);
    bool ok = true;
    if (!old || strcmp(old, text)) {
        /* Una nota que cambió y no se pudo leer (o que nunca había visto): se
           respalda tal cual antes de reescribirla. */
        if (old && !str_is_blank(old)) {
            char *oh = sha256_hex(old), *stored = state_get(path);
            if (!stored || strcmp(stored, oh)) {
                char *base = wide_to_utf8(path_basename(path));
                if (str_ends_with(base, ".md")) base[strlen(base) - 3] = 0;
                backup(base, "md", old);
                free(base);
            }
            free(stored);
            free(oh);
        }
        ok = write_file_atomic(path, text, strlen(text));
    }
    if (ok) {
        char *h = sha256_hex(text);
        state_set(path, h);
        free(h);
    } else if (!g_warned[k]) {
        g_warned[k] = true;
        char *p = wide_to_utf8(path);
        log_msg("Bóveda: no pude escribir «%s» (¿solo lectura?). Tu memoria sigue en la carpeta de Sokari.", p);
        free(p);
    }
    free(old);
    ReleaseSRWLockExclusive(&g_lock);
    free(text);
    free(path);
}

void boveda_sync_all(void)
{
    gustos_sync();
    radio_sync();
    memory_facts_sync();
    skills_notes_sync();
}

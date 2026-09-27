/* Tus propias skills: un archivo de texto (.md) por skill en la carpeta
   skills de tu memoria. Dos tipos:
   - Rutina: una frase tuya ("modo estudio") hace varios pasos de una lista
     cerrada (abrir apps y páginas, música, volumen, YouTube, escribir, decir
     algo). Se hace en tu PC, sin IA (0 tokens).
   - IA: cuando dices sus frases, sus instrucciones van al modelo en ese
     pedido ("resumen de noticias: dame 5, una frase cada una").
   Tus comandos propios de siempre (commands.json) también se hacen sin IA al
   decir su nombre. Formato de un archivo:

     # Modo estudio
     Tipo: rutina
     Frases: modo estudio | vamos a estudiar
     - abre: Spotify
     - volumen: 30
     - di: Listo, a estudiar. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "intents.h"
#include "log.h"
#include "memory.h"
#include "skills_internal.h"
#include "third_party/cJSON.h"
#include "tools.h"
#include "util.h"

#define MAX_PHRASES 12
#define MAX_STEPS 24

typedef struct {
    char *action, *value;
} Step;

typedef struct {
    char *name;
    bool routine;
    char *phrases[MAX_PHRASES]; /* normalizadas: "modo estudio" */
    int nphrases;
    char *when;
    char *body; /* instrucciones (IA) */
    Step steps[MAX_STEPS];
    int nsteps;
} UserSkill;

static bool in_list_words(const char *list, const char *w)
{
    char pat[64];
    if (strlen(w) + 3 > sizeof pat) return false;
    snprintf(pat, sizeof pat, " %s ", w);
    return strstr(list, pat) != NULL;
}

wchar_t *skills_user_dir(void)
{
    return memory_file(L"skills");
}

/* "  Modo Estudio!  " -> "modo estudio" (heap). */
static char *norm(const char *s)
{
    char *n = intents_normalize(s);
    char *t = str_trim(n);
    free(n);
    return t;
}

static void skill_free(UserSkill *s)
{
    free(s->name);
    free(s->when);
    free(s->body);
    for (int i = 0; i < s->nphrases; i++) free(s->phrases[i]);
    for (int i = 0; i < s->nsteps; i++) {
        free(s->steps[i].action);
        free(s->steps[i].value);
    }
    memset(s, 0, sizeof *s);
}

static void add_phrase(UserSkill *s, const char *p)
{
    char *n = norm(p);
    if (*n && s->nphrases < MAX_PHRASES) s->phrases[s->nphrases++] = n;
    else free(n);
}

/* Lee un archivo de skill. file: su nombre (sin .md: el nombre por defecto). */
static bool parse_skill(const char *text, const char *file, UserSkill *s)
{
    memset(s, 0, sizeof *s);
    StrBuf body;
    sb_init(&body);
    int type = 0; /* 0 sin decir, 1 rutina, 2 IA */
    char *copy = xstrdup(text), *ctx = NULL;
    for (char *line = strtok_s(copy, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx)) {
        char *l = str_trim(line);
        char *colon = strchr(l, ':');
        char *key = NULL;
        if (colon && colon - l < 20) {
            char *k = xstrndup(l, (size_t)(colon - l));
            key = norm(k);
            free(k);
        }
        if (l[0] == '#' && !s->name) {
            s->name = str_trim(l + 1 + (l[1] == '#'));
        } else if (key && !strcmp(key, "tipo")) {
            char *v = norm(colon + 1);
            type = !strncmp(v, "ia", 2) || strstr(v, "inteligencia") ? 2 : 1;
            free(v);
        } else if (key && !strcmp(key, "frases")) {
            char *list = xstrdup(colon + 1), *c2 = NULL;
            for (char *p = strtok_s(list, "|;,", &c2); p; p = strtok_s(NULL, "|;,", &c2)) add_phrase(s, p);
            free(list);
        } else if (key && (!strcmp(key, "cuando") || !strcmp(key, "para que"))) {
            free(s->when);
            s->when = str_trim(colon + 1);
        } else if ((l[0] == '-' || l[0] == '*') && colon && s->nsteps < MAX_STEPS) {
            char *a = xstrndup(l + 1, (size_t)(colon - l - 1));
            s->steps[s->nsteps].action = norm(a);
            s->steps[s->nsteps].value = str_trim(colon + 1);
            free(a);
            s->nsteps++;
        } else if (*l || body.len) {
            sb_appendf(&body, "%s\n", l);
        }
        free(key);
        free(l);
    }
    free(copy);
    if (!s->name || !*s->name) {
        free(s->name);
        s->name = xstrdup(file);
    }
    if (!s->nphrases) add_phrase(s, s->name);
    s->routine = type ? type == 1 : s->nsteps > 0;
    char *b = sb_steal(&body);
    s->body = str_trim(b ? b : "");
    free(b);
    sb_free(&body);
    return s->nphrases > 0 && (s->routine ? s->nsteps > 0 : *s->body != 0);
}

/* Todas tus skills (heap, *n). */
static UserSkill *load_all(int *n)
{
    *n = 0;
    wchar_t *dir = skills_user_dir();
    int nf;
    char **files = dir_list(dir, ".md", &nf);
    UserSkill *all = nf ? xcalloc((size_t)nf, sizeof *all) : NULL;
    for (int i = 0; i < nf; i++) {
        wchar_t *wn = utf8_to_wide(files[i]), *path = path_join(dir, wn);
        size_t len = 0;
        char *text = read_file_all(path, &len);
        char *stem = xstrndup(files[i], strlen(files[i]) - 3);
        if (text && len < 64 * 1024 && parse_skill(text, stem, &all[*n])) (*n)++;
        else if (text) log_msg("Skills: «%s» no tiene frases o pasos que entienda; la salto.", files[i]);
        free(stem);
        free(text);
        free(path);
        free(wn);
        free(files[i]);
    }
    free(files);
    free(dir);
    return all;
}

static void free_all(UserSkill *all, int n)
{
    for (int i = 0; i < n; i++) skill_free(&all[i]);
    free(all);
}

/* ------------------------------------------------------------ rutinas --- */

static char *tool_call(const char *tool, cJSON *args)
{
    char *json = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    char *r = run_tool(tool, json);
    free(json);
    return r;
}

static cJSON *arg(const char *k, const char *v)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, k, v);
    return a;
}

/* Un paso: qué herramienta es (o decir/esperar). NULL si no se entiende. */
static char *run_step(const Step *st, StrBuf *said)
{
    const char *a = st->action, *v = st->value;
    char *low = norm(v);
    char *r = NULL;
    if (in_list_words(" abre abrir app aplicacion programa pagina web url sitio link carpeta ", a)) {
        r = tool_call("open_app", arg("name", v));
    } else if (in_list_words(" musica cancion reproduccion ", a)) {
        const char *act = strstr(low, "sig") ? "next_track" : strstr(low, "ant") ? "previous_track" : "play_pause";
        r = tool_call("control_media", arg("action", act));
    } else if (in_list_words(" volumen ", a)) {
        cJSON *args = arg("action", "set_volume");
        cJSON_AddNumberToObject(args, "nivel", atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v));
        r = tool_call("control_media", args);
    } else if (in_list_words(" youtube pon ", a)) {
        r = tool_call("poner_en_youtube", arg("busqueda", v));
    } else if (in_list_words(" escribe escribir ", a)) {
        r = tool_call("type_text", arg("texto", v));
    } else if (in_list_words(" minimiza escritorio ", a)) {
        r = tool_call("control_desktop", arg("action", "minimize_all"));
    } else if (in_list_words(" di decir dile habla avisa ", a)) {
        sb_appendf(said, "%s%s", said->len ? " " : "", v);
        r = xstrdup("");
    } else if (in_list_words(" espera esperar pausa ", a)) {
        double s = atof(v);
        Sleep((DWORD)((s < 0 ? 0 : s > 10 ? 10 : s) * 1000));
        r = xstrdup("");
    }
    free(low);
    return r;
}

static char *run_routine(const UserSkill *s)
{
    StrBuf said, fails;
    sb_init(&said);
    sb_init(&fails);
    for (int i = 0; i < s->nsteps; i++) {
        char *r = run_step(&s->steps[i], &said);
        if (!r) sb_appendf(&fails, "%sno sé hacer «%s»", fails.len ? "; " : "", s->steps[i].action);
        else if (!strncmp(r, "No ", 3)) sb_appendf(&fails, "%s%s", fails.len ? " " : "", r);
        free(r);
        if (i + 1 < s->nsteps) Sleep(150);
    }
    char *r;
    if (fails.len) r = str_printf("%s%sNo todo salió: %s", said.len ? said.data : "", said.len ? " " : "", fails.data);
    else if (said.len) r = xstrdup(said.data);
    else r = str_printf("Listo, hice «%s».", s->name);
    sb_free(&said);
    sb_free(&fails);
    return r;
}

/* Tus comandos propios de siempre (commands.json): sus nombres. */
static char **macro_names(int *n)
{
    *n = 0;
    wchar_t *cf = memory_file(L"commands.json");
    cJSON *macros = json_load_object(cf);
    free(cf);
    char **out = NULL;
    const cJSON *m;
    cJSON_ArrayForEach(m, macros)
    {
        if (!m->string || !cJSON_IsArray(m)) continue;
        out = xrealloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = xstrdup(m->string);
    }
    cJSON_Delete(macros);
    return out;
}

static const char *const ROUTINE_VERBS =
    " activa activar activame pon ponme poner inicia iniciar ejecuta corre haz hazme empieza arranca modo rutina "
    "comando el la mi ";

/* ¿Lo que dijiste es la frase (con relleno y "activa" o "pon")? */
static bool says_phrase(Heard *h, const char *phrase)
{
    memset(h->used, 0, sizeof h->used);
    if (sk_take(h, phrase) < 0) return false;
    sk_take_any(h, ROUTINE_VERBS);
    return sk_rest_is_filler(h);
}

char *sk_user(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    int n;
    UserSkill *all = load_all(&n);
    char *r = NULL;
    for (int i = 0; i < n && !r; i++) {
        if (!all[i].routine) continue;
        for (int k = 0; k < all[i].nphrases && !r; k++) {
            if (!says_phrase(h, all[i].phrases[k])) continue;
            log_msg("Rutina «%s» (%d pasos), sin IA.", all[i].name, all[i].nsteps);
            r = run_routine(&all[i]);
        }
    }
    free_all(all, n);
    if (r) return r;
    int nm;
    char **macros = macro_names(&nm);
    for (int i = 0; i < nm; i++) {
        char *p = norm(macros[i]);
        if (!r && *p && says_phrase(h, p)) {
            log_msg("Comando propio «%s», sin IA.", macros[i]);
            cJSON *args = arg("name", macros[i]);
            r = tool_call("run_macro", args);
        }
        free(p);
        free(macros[i]);
    }
    free(macros);
    return r;
}

/* ------------------------------------------------------- skills de IA --- */

/* ¿Todas las palabras de la frase están en lo que dijiste (en cualquier orden)? */
static bool has_words(const char *said_norm, const char *phrase)
{
    char *copy = xstrdup(phrase), *ctx = NULL;
    bool all = true;
    int words = 0;
    for (char *w = strtok_s(copy, " ", &ctx); w && all; w = strtok_s(NULL, " ", &ctx)) {
        if (strlen(w) <= 2 || in_list_words(" las los del que con por para una unos unas ", w)) continue;
        char pat[64];
        snprintf(pat, sizeof pat, " %s ", w);
        all = strstr(said_norm, pat) != NULL;
        words++;
    }
    free(copy);
    return all && words > 0;
}

char *skills_ai_for(const char *text, char **name)
{
    *name = NULL;
    if (!config_skill_enabled("mis_skills")) return NULL;
    char *said = intents_normalize(text);
    int n;
    UserSkill *all = load_all(&n);
    char *r = NULL;
    for (int i = 0; i < n && !r; i++) {
        if (all[i].routine) continue;
        for (int k = 0; k < all[i].nphrases && !r; k++) {
            if (!has_words(said, all[i].phrases[k])) continue;
            r = xstrdup(all[i].body);
            *name = xstrdup(all[i].name);
        }
    }
    free_all(all, n);
    free(said);
    return r;
}

/* --------------------------------------------------------------- crear --- */

/* Un nombre de archivo seguro: sin / \ : * ? " < > | ni puntos al inicio. */
static wchar_t *skill_path(const char *name)
{
    StrBuf sb;
    sb_init(&sb);
    for (const unsigned char *p = (const unsigned char *)name; *p && sb.len < 60; p++) {
        if (strchr("/\\:*?\"<>|\r\n\t", *p) || (*p == '.' && !sb.len)) continue;
        sb_append_char(&sb, (char)*p);
    }
    char *clean = sb_steal(&sb), *t = str_trim(clean ? clean : "");
    free(clean);
    char *file = str_printf("%s.md", *t ? t : "skill");
    free(t);
    wchar_t *dir = skills_user_dir(), *wf = utf8_to_wide(file), *path = path_join(dir, wf);
    ensure_dir(dir);
    free(dir);
    free(wf);
    free(file);
    return path;
}

char *skills_create(const char *name, bool routine, const char *phrases, const char *when, const char *body,
                    const char *steps)
{
    char *nm = str_trim(name ? name : "");
    if (!*nm) {
        free(nm);
        return xstrdup("Necesito un nombre para la skill.");
    }
    StrBuf sb;
    sb_init(&sb);
    sb_appendf(&sb, "# %s\nTipo: %s\nFrases: %s\n", nm, routine ? "rutina" : "IA",
               phrases && *phrases ? phrases : nm);
    if (when && *when) sb_appendf(&sb, "Cuándo: %s\n", when);
    sb_append(&sb, "\n");
    if (routine) sb_appendf(&sb, "%s\n", steps ? steps : "");
    else sb_appendf(&sb, "%s\n", body ? body : "");
    /* Que se pueda leer de vuelta: si no, no se guarda. */
    UserSkill check;
    bool ok = parse_skill(sb.data, nm, &check);
    char *r;
    if (!ok) {
        r = xstrdup(routine ? "No guardé la rutina: no traía pasos que sepa hacer." : "No guardé la skill: no traía "
                                                                                      "instrucciones.");
    } else {
        wchar_t *path = skill_path(nm);
        if (!write_file_atomic(path, sb.data, sb.len)) {
            r = xstrdup("No pude guardar la skill.");
        } else {
            /* La primera frase, como la escribiste (con acentos). */
            char *first = xstrdup(phrases && *phrases ? phrases : nm);
            first[strcspn(first, "|;,")] = 0;
            char *shown = str_trim(first);
            r = str_printf("Listo, creé %s «%s». Se activa diciendo «%s».", routine ? "la rutina" : "la skill", nm,
                           *shown ? shown : nm);
            free(shown);
            free(first);
            log_msg("Skills: creé «%s» (%s).", nm, routine ? "rutina" : "IA");
        }
        free(path);
    }
    skill_free(&check);
    sb_free(&sb);
    free(nm);
    return r;
}

char *tool_crear_skill(const cJSON *a)
{
    const char *tipo = arg_str(a, "tipo");
    bool routine = !(tipo && (!strncmp(tipo, "ia", 2) || !strncmp(tipo, "IA", 2)));
    StrBuf steps;
    sb_init(&steps);
    const cJSON *list = cJSON_GetObjectItem(a, "pasos"), *p;
    cJSON_ArrayForEach(p, list)
    {
        const char *acc = cJSON_GetStringValue(cJSON_GetObjectItem(p, "accion"));
        const char *val = cJSON_GetStringValue(cJSON_GetObjectItem(p, "valor"));
        if (acc && val) sb_appendf(&steps, "- %s: %s\n", acc, val);
    }
    char *r = skills_create(arg_str(a, "nombre"), routine, arg_str(a, "frases"), arg_str(a, "cuando"),
                            arg_str(a, "instrucciones"), steps.data);
    sb_free(&steps);
    return r;
}

char *skills_user_summary(int *count)
{
    int n, nm;
    UserSkill *all = load_all(&n);
    char **macros = macro_names(&nm);
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < n; i++) sb_appendf(&sb, "%s%s", sb.len ? ", " : "", all[i].name);
    for (int i = 0; i < nm; i++) {
        sb_appendf(&sb, "%s%s", sb.len ? ", " : "", macros[i]);
        free(macros[i]);
    }
    free(macros);
    free_all(all, n);
    *count = n + nm;
    char *r = sb_steal(&sb);
    return r ? r : xstrdup("");
}

wchar_t *skills_new_template(void)
{
    wchar_t *path = NULL;
    for (int i = 1; i < 100; i++) {
        char *name = i == 1 ? xstrdup("Mi skill") : str_printf("Mi skill %d", i);
        path = skill_path(name);
        free(name);
        if (!file_exists(path)) break;
        free(path);
        path = NULL;
    }
    if (!path) return NULL;
    static const char TEMPLATE[] =
        "# Modo estudio\n"
        "Tipo: rutina\n"
        "Frases: modo estudio | vamos a estudiar\n"
        "\n"
        "- abre: Spotify\n"
        "- volumen: 30\n"
        "- di: Listo, a estudiar.\n"
        "\n"
        "Una rutina se hace sin IA (0 tokens). Pasos que sabe hacer: abre (una app, una página o una carpeta),\n"
        "música (play, pausa, siguiente, anterior), volumen (0 a 100), youtube (qué poner), escribe (un texto),\n"
        "minimiza (todo), di (lo que dice al terminar) y espera (segundos, hasta 10).\n"
        "\n"
        "Para una skill de IA, cambia a «Tipo: IA», agrega «Cuándo: …» y en vez de los pasos escribe las\n"
        "instrucciones: cuando digas sus frases, van a la IA en ese pedido.\n";
    if (!write_file_atomic(path, TEMPLATE, sizeof TEMPLATE - 1)) {
        free(path);
        return NULL;
    }
    return path;
}

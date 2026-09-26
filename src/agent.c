#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "app.h"
#include "config.h"
#include "groq.h"
#include "intents.h"
#include "skills.h"
#include "keys.h"
#include "log.h"
#include "memory.h"
#include "mesh.h"
#include "resource.h"
#include "resources.h"
#include "tools.h"
#include "util.h"

#define MAX_HISTORY_MESSAGES 12
#define MEMORY_WINDOW_SECONDS (12 * 3600)
#define MAX_TOOL_ROUNDS 6
#define TOOL_RESULT_KEEP 280

struct Conversation {
    cJSON *history; /* [0] = system prompt, después user/assistant/tool */
    int session_start; /* desde aquí empieza la conversación actual ("Hey Sokari") */
    bool announce_pending;
    bool remote;        /* llega por la malla: ahí nadie puede confirmar de voz */
    bool trust_all;     /* dijo "sí a todo": no se vuelve a preguntar en esta conversación */
    char *pending_tool; /* acción que espera un "sí" de voz, con sus argumentos */
    char *pending_args;
    char *turn_skill;      /* instrucciones de una skill de IA tuya, solo para este pedido */
    char *turn_skill_name;
};

static cJSON *g_tools;
static char *g_system_prompt;

static const char *FAREWELLS[] = {
    "adios", "adiós", "hasta luego", "hasta la proxima", "hasta la próxima", "nos vemos", "me despido", "chao",
    "chau", "bye", "eso es todo", "eso seria todo", "eso sería todo", "ya no necesito nada", "gracias eso es todo",
    "ya vete", "vete ya", "puedes irte", "te puedes ir", "ya nada gracias", "ya es todo", "retirate", "retírate",
    "ahi nos vidrios", "ahí nos vidrios", "nos vidrios", "ahi nos vemos", "ahí nos vemos", "ahi se ve", "ahí se ve",
    "ya me voy", "luego te hablo", "al rato te hablo", "bye bye", "ahi la vemos", "ahí la vemos", "ahi te ves",
    "ahí te ves", "me retiro",
};

/* Estas solo son despedida si son casi toda la frase: "ya está abierta
   Opera" o "nada más abre Spotify" no lo son. */
static const char *SHORT_FAREWELLS[] = {"nada mas", "nada más", "ya esta", "ya está", "ya estuvo", "camara",
                                        "cámara",   "buenas noches", "sale bye"};

/* Cuando Sokari mismo se despide, la conversación también termina (y la
   esfera se esconde si así está configurada). Una pregunta al final no cuenta:
   "¿algo más? si no, hasta luego" sigue esperando respuesta. */
static const char *SOKARI_FAREWELLS[] = {
    "hasta luego", "hasta pronto", "hasta la proxima", "hasta la próxima", "nos vemos", "adios", "adiós",
    "que tengas buen", "que tengas un buen", "que tengas lindo", "que tengas linda", "que tengas bonito",
    "que tengas excelente", "que descanses", "cuidate", "cuídate", "bye", "chao", "aqui estare", "aquí estaré",
    "quedo a la espera", "cuando me necesites",
};

void agent_init(void)
{
    if (g_tools || g_system_prompt) return; /* la malla y la voz lo piden; basta una vez */
    g_tools = cJSON_Parse(res_string(IDR_TOOLS_JSON));
    g_system_prompt = res_string(IDR_SYSTEM_PROMPT);
    if (!g_tools) log_msg("No pude leer la definición de herramientas.");
}

static cJSON *system_message(void)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", g_system_prompt);
    return m;
}

/* Corta solo justo antes de un mensaje "user": un assistant con tool_calls
   tiene que quedar pegado a sus resultados "tool", si no Groq rechaza todos
   los turnos siguientes. */
static void trim_history(Conversation *c)
{
    cJSON *h = c->history;
    int n = cJSON_GetArraySize(h);
    if (n <= MAX_HISTORY_MESSAGES + 1) return;
    int cut = n - MAX_HISTORY_MESSAGES;
    while (cut < n) {
        cJSON *role = cJSON_GetObjectItem(cJSON_GetArrayItem(h, cut), "role");
        if (cJSON_IsString(role) && !strcmp(role->valuestring, "user")) break;
        cut++;
    }
    for (int i = 1; i < cut; i++) cJSON_DeleteItemFromArray(h, 1);
    c->session_start -= cut - 1;
    if (c->session_start < 1) c->session_start = 1;
}

Conversation *conv_create(bool load_recent_memory)
{
    Conversation *c = xcalloc(1, sizeof *c);
    c->history = cJSON_CreateArray();
    cJSON_AddItemToArray(c->history, system_message());
    c->session_start = 1;
    if (load_recent_memory) {
        cJSON *recent = memory_recent_history(MEMORY_WINDOW_SECONDS);
        cJSON *m;
        while ((m = cJSON_DetachItemFromArray(recent, 0))) cJSON_AddItemToArray(c->history, m);
        cJSON_Delete(recent);
        trim_history(c);
    }
    c->announce_pending = true;
    return c;
}

static void clear_pending(Conversation *c)
{
    free(c->pending_tool);
    free(c->pending_args);
    c->pending_tool = c->pending_args = NULL;
}

void conv_destroy(Conversation *c)
{
    if (!c) return;
    clear_pending(c);
    cJSON_Delete(c->history);
    free(c->turn_skill);
    free(c->turn_skill_name);
    free(c);
}

void conv_set_remote(Conversation *c, bool remote)
{
    c->remote = remote;
}

/* Lo que trajo una herramienta de afuera en una conversación anterior se
   borra: si traía instrucciones escondidas, ya no están para seguirlas. Queda
   la nota para que el historial siga siendo válido para Groq. */
static void forget_outside_text(cJSON *history, int until)
{
    for (int i = 1; i < until; i++) {
        const cJSON *call;
        cJSON_ArrayForEach(call, cJSON_GetObjectItem(cJSON_GetArrayItem(history, i), "tool_calls"))
        {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(call, "function"), "name"));
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"));
            if (!id || !tool_brings_outside_text(name)) continue;
            for (int j = i + 1; j < until; j++) {
                cJSON *t = cJSON_GetArrayItem(history, j);
                const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(t, "tool_call_id"));
                cJSON *content = cJSON_GetObjectItem(t, "content");
                if (tid && !strcmp(tid, id) && cJSON_IsString(content))
                    cJSON_SetValuestring(content, "(texto de afuera de una conversación anterior; ya no está)");
            }
        }
    }
}

/* Empezar de cero: la conversación actual se olvida (el prompt se queda). */
static void conv_forget(Conversation *c)
{
    while (cJSON_GetArraySize(c->history) > 1) cJSON_DeleteItemFromArray(c->history, 1);
    c->session_start = 1;
    c->trust_all = false;
    clear_pending(c);
}

void conv_new_session(Conversation *c)
{
    c->announce_pending = true;
    c->trust_all = false;
    clear_pending(c);
    c->session_start = cJSON_GetArraySize(c->history);
    forget_outside_text(c->history, c->session_start);
}

/* Los resultados de herramientas (búsquedas, páginas, archivos) pueden ser
   largos; una vez que el modelo ya los resumió en su respuesta, se recortan en
   el historial para no volver a pagarlos en tokens en cada pedido siguiente. */
static void shrink_tool_results(cJSON *history, int from)
{
    int n = cJSON_GetArraySize(history);
    for (int i = from; i < n; i++) {
        cJSON *m = cJSON_GetArrayItem(history, i);
        cJSON *role = cJSON_GetObjectItem(m, "role");
        cJSON *content = cJSON_GetObjectItem(m, "content");
        if (!cJSON_IsString(role) || strcmp(role->valuestring, "tool") || !cJSON_IsString(content)) continue;
        size_t cut = utf8_truncate_len(content->valuestring, TOOL_RESULT_KEEP);
        if (content->valuestring[cut]) {
            char *shorter = str_printf("%.*s…", (int)cut, content->valuestring);
            cJSON_SetValuestring(content, shorter);
            free(shorter);
        }
    }
}

static void add_message(cJSON *history, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(history, m);
}

/* ¿Queda en la conversación algo que trajo una herramienta de afuera (una
   página, un archivo, el portapapeles...)? Mientras quede, el modelo puede
   estar siguiendo instrucciones escondidas ahí, también en un turno posterior:
   por eso no alcanza con mirar solo el turno en que se leyó. */
static bool history_has_outside_text(const Conversation *c)
{
    int n = cJSON_GetArraySize(c->history);
    for (int i = c->session_start; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(c->history, i);
        const cJSON *call;
        cJSON_ArrayForEach(call, cJSON_GetObjectItem(m, "tool_calls"))
        {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(call, "function"), "name"));
            if (tool_brings_outside_text(name)) return true;
        }
    }
    return false;
}

static bool word_in(const char *w, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(w, list[i])) return true;
    return false;
}

/* Respuesta a "¿Lo hago?". Cualquier "no" en la frase gana ("claro que no",
   "sí, no, mejor no"). Un sí cuenta aunque venga acompañado ("ah ok, sí te lo
   doy", "te confirmo", "confirmo que sí"), pero solo si la frase no trae nada
   más: "sí, pero primero abre Spotify" se procesa como un pedido nuevo. */
AgentAnswer agent_classify_answer(const char *text)
{
    static const char *const YES[] = {"si",       "sí",        "dale",      "hazlo",   "hazle",      "confirmo",
                                      "confirma", "confirmado", "adelante", "claro",   "ok",         "okay",
                                      "okey",     "afirmativo", "correcto", "simón",   "simon",      "ándale",
                                      "andale",   "órale",      "orale",    "acuerdo", "autorizo",   "permiso",
                                      "sale",     "va",         "arre",     "sobres",  "cámara",     "camara",
                                      "sip",      "sep",        "simona",   "échale",  "echale",     "huevo",
                                      "fierro",   "jalo",       "listo",    "perfecto", "obvio",      "seguro"};
    static const char *const NO[] = {"no",      "cancela",  "cancelalo", "cancélalo", "olvidalo", "olvídalo",
                                     "nel",     "negativo", "nop",       "nope",      "tampoco",  "nunca",
                                     "nones",   "nanai",    "nanais"};
    /* Palabras que acompañan al sí sin cambiarlo. */
    static const char *const FILLER[] = {"ah",  "oh",  "eh",  "bueno", "pues", "que", "qué", "te",  "lo",
                                         "le",  "la",  "doy", "de",    "a",    "al",  "ya",  "y",   "por",
                                         "favor", "porfa", "sokari", "vale", "yo", "tienes", "mi",
                                         "wey",   "güey",  "guey",  "carnal", "compa", "bro", "neta", "pos",
                                         "pus",   "mano",  "we",    "chido",  "chale"};
    static const char *const ALL[] = {"todo", "todos", "toda", "todas"};
    static const char *const REPEAT[] = {"que",         "qué",          "como",        "cómo",       "perdon",
                                         "perdón",      "mande",        "que dijiste", "qué dijiste", "repite",
                                         "repitelo",    "repítelo",     "otra vez",    "no te entendi",
                                         "no te entendí", "no entendi", "no entendí",  "cual",       "cuál",
                                         "que cosa",    "qué cosa",     "como dices",  "cómo dices",
                                         "eh",          "como dijiste", "cómo dijiste", "mande usted",
                                         "no te escuche", "no te escuché", "no te oi",   "no te oí"};
    char *low = str_lower(text);
    for (unsigned char *p = (unsigned char *)low; *p; p++) {
        if (p[0] == 0xC2 && (p[1] == 0xA1 || p[1] == 0xBF)) p[0] = p[1] = ' '; /* ¡ ¿ */
        else if (*p < 0x80 && !isalnum(*p)) *p = ' ';
    }
    str_collapse_spaces(low);
    char *t = str_trim(low);
    free(low);
    if (word_in(t, REPEAT, sizeof REPEAT / sizeof *REPEAT)) {
        free(t);
        return ANSWER_REPEAT;
    }
    int words = 0;
    bool any_no = false, any_yes = false, any_all = false, only_known = true;
    for (char *w = t; *w;) {
        char *end = strchr(w, ' ');
        if (end) *end = 0;
        words++;
        bool yes = word_in(w, YES, sizeof YES / sizeof *YES), all = word_in(w, ALL, sizeof ALL / sizeof *ALL);
        if (word_in(w, NO, sizeof NO / sizeof *NO)) any_no = true;
        any_yes |= yes;
        any_all |= all;
        if (!yes && !all && !word_in(w, FILLER, sizeof FILLER / sizeof *FILLER)) only_known = false;
        if (!end) break;
        w = end + 1;
    }
    free(t);
    /* "Ni madres", "ni de chiste", "para nada": no, aunque ninguna palabra sola lo sea. */
    char *n2 = intents_normalize(text);
    if (strstr(n2, " ni madres ") || strstr(n2, " ni de chiste ") || strstr(n2, " ni loco ") ||
        strstr(n2, " para nada ") || strstr(n2, " ni maiz ") || strstr(n2, " mejor no ") ||
        strstr(n2, " ni de broma ") || strstr(n2, " dejalo asi ") || strstr(n2, " mejor dejalo "))
        any_no = true;
    /* "De una", "va que va", "sale y vale", "por supuesto": sí. */
    bool yes_phrase = strstr(n2, " de una ") || strstr(n2, " va que va ") || strstr(n2, " sale y vale ") ||
                      strstr(n2, " por supuesto ") || strstr(n2, " ya estas ") || strstr(n2, " claro que si ");
    free(n2);
    if (any_no) return ANSWER_NO;
    if (yes_phrase && words <= 8) return ANSWER_YES;
    if (!words || !only_known || words > 8) return ANSWER_OTHER;
    if (any_all) return ANSWER_ALL; /* "a todo", "sí a todo", "confirma todo", "permiso a todo" */
    return any_yes ? ANSWER_YES : ANSWER_OTHER;
}

/* "¿Qué?" con una acción pendiente: se vuelve a decir cuál es, sin
   cancelarla. */
static TurnResult repeat_pending(Conversation *c, const char *text)
{
    TurnResult r = {0};
    cJSON *parsed = cJSON_Parse(c->pending_args);
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        parsed = cJSON_CreateObject();
    }
    char *desc = tool_describe_action(c->pending_tool, parsed);
    cJSON_Delete(parsed);
    r.reply = str_printf("Te preguntaba si puedo %s. ¿Lo hago? Di sí o no.", desc);
    free(desc);
    add_message(c->history, "user", text);
    memory_persist("user", text);
    add_message(c->history, "assistant", r.reply);
    memory_persist("assistant", r.reply);
    trim_history(c);
    r.keep_going = true;
    return r;
}

/* El "sí" ejecuta exactamente la acción guardada, sin volver a preguntarle
   al modelo. */
static TurnResult run_pending(Conversation *c, const char *text)
{
    TurnResult r = {0};
    log_msg("[confirmada] %s(%s)", c->pending_tool, c->pending_args);
    char *result = run_tool(c->pending_tool, c->pending_args);
    bool forget = !strcmp(c->pending_tool, "borrar_memoria_reciente");
    clear_pending(c);
    if (forget) {
        /* Lo borrado tampoco se queda en la conversación de ahora. */
        conv_forget(c);
        r.reply = result;
        r.keep_going = true;
        return r;
    }
    add_message(c->history, "user", text);
    memory_persist("user", text);
    add_message(c->history, "assistant", result);
    memory_persist("assistant", result);
    trim_history(c);
    r.reply = result;
    r.keep_going = true;
    return r;
}

static bool contains_stop_word(const char *text)
{
    char *stop = config_stop_word();
    char *t = str_trim(stop);
    bool hit = *t && str_contains_ci(text, t);
    SecureZeroMemory(stop, strlen(stop));
    free(stop);
    free(t);
    return hit;
}

static bool contains_any(const char *text, const char *const *list, size_t n)
{
    char *low = str_lower(text);
    bool hit = false;
    for (size_t i = 0; i < n && !hit; i++) hit = strstr(low, list[i]) != NULL;
    free(low);
    return hit;
}

static bool is_farewell(const char *text)
{
    if (contains_any(text, FAREWELLS, sizeof FAREWELLS / sizeof *FAREWELLS)) return true;
    int words = 0;
    for (const char *p = text; *p; p++)
        if (!isspace((unsigned char)*p) && (p == text || isspace((unsigned char)p[-1]))) words++;
    return words <= 4 && contains_any(text, SHORT_FAREWELLS, sizeof SHORT_FAREWELLS / sizeof *SHORT_FAREWELLS);
}

static bool sokari_says_goodbye(const char *reply)
{
    char *t = str_trim(reply);
    size_t n = strlen(t);
    bool question = n && t[n - 1] == '?';
    free(t);
    return !question && contains_any(reply, SOKARI_FAREWELLS, sizeof SOKARI_FAREWELLS / sizeof *SOKARI_FAREWELLS);
}

/* A veces el modelo se traba y devuelve cientos de renglones de puntos y
   rayas. Eso no se dice en voz alta ni se guarda: casi no trae letras. */
static bool reply_is_garbage(const char *text)
{
    int letters = 0, other = 0, lines = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        int len = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
        for (int k = 1; k < len; k++)
            if (!p[k]) {
                len = k;
                break;
            }
        unsigned cp = *p;
        if (len == 2) cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        else if (len == 3) cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        else if (len == 4) cp = 0x10000; /* emojis y demás: no son letras */
        if (cp == '\n') lines++;
        if (cp < 0x80 ? isalnum((int)cp) : (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7)) letters++;
        else if (cp > ' ') other++;
        p += len;
    }
    return lines > 40 || (letters + other >= 20 && letters * 2 < letters + other);
}

/* Borrar siempre pide un sí de voz, con o sin acceso completo. Supr (y
   Ctrl+D) también: en el Explorador borran lo que tengas seleccionado. */
static bool tool_is_delete(const char *name, const cJSON *args)
{
    if (!strcmp(name, "presionar_teclas")) {
        KeyCombo k;
        return keys_parse(arg_str(args, "teclas"), &k) && k.has_delete;
    }
    return !strcmp(name, "borrar_archivo") || !strcmp(name, "borrar_memoria_reciente");
}

/* Con acceso completo solo se pregunta antes de borrar. Sin él, como antes:
   las acciones delicadas, cuando en la conversación hay texto de afuera. */
static bool must_confirm(const Conversation *c, const char *name, const cJSON *args)
{
    if (c->trust_all) return false;
    if (tool_is_delete(name, args)) return true;
    /* Con teclas se puede hacer casi todo (hasta abrir «Ejecutar» y correr un
       comando): si en la conversación hay algo de afuera, cada tecla que pida
       el modelo espera tu sí, aunque tengas acceso completo. Las que pides tú
       ("dale enter") no pasan por aquí. */
    if (!strcmp(name, "presionar_teclas") && history_has_outside_text(c)) return true;
    return !config_full_access() && tool_needs_confirmation(name, args) && history_has_outside_text(c);
}

/* ¿La respuesta termina pidiendo permiso para hacer algo ("¿Quieres que lo
   mueva?", "¿Lo envío?")? No cuentan las que piden un dato (¿a quién?, ¿cuál?),
   las que dan a elegir (¿esto o aquello?) ni las que ofrecen algo más. */
bool agent_asks_permission(const char *reply)
{
    char *t = str_trim(reply);
    size_t n = strlen(t);
    bool ends_q = n && t[n - 1] == '?';
    const char *q = NULL;
    for (const char *p = strstr(t, "¿"); p; p = strstr(p + 1, "¿")) q = p;
    if (!q) {
        q = t;
        for (const char *p = t; *p; p++)
            if ((*p == '.' || *p == '!' || *p == '\n') && p[1]) q = p + 1;
    }
    char *low = str_lower(q);
    static const char *const ASK[] = {
        "quieres que",   "deseas que",     "te gustaría que", "te parece si",   "te parece que",  "prefieres que lo",
        "lo hago",       "procedo",        "confirmas",       "estás seguro",   "estas seguro",   "seguro que quieres",
        "lo envío",      "lo envio",       "lo mando",        "lo guardo",      "lo recuerdo",    "lo exporto",
        "lo muevo",      "lo abro",        "lo cierro",       "lo subo",        "lo publico",     "lo intento",
        "te lo mando",   "te lo envío",    "sigo",            "continúo",       "continuo",       "puedo",
        "me permites",   "me das permiso", "autorizas",       "está bien si",   "esta bien si",   "le doy",
    };
    /* Preguntas de verdad y ofrecimientos ("¿quieres que te guíe…?"): a esos
       no se les contesta "sí" solo. Uno así prendió permisos y soltó una guía
       inventada. */
    static const char *const NOT[] = {"qué",         "cuál",        "cuáles",       "quién",       "quiénes",
                                      "dónde",       "cuándo",      "cómo",         "cuánto",      "cuánta",
                                      "cuántos",     "cuántas",     " o ",          "algo más",    "otra cosa",
                                      "en qué más",  "más ayuda",   "ayudarte con", "te guíe",     "te guie",
                                      "te explique", "te ayude",    "te cuente",    "te diga",     "te enseñe",
                                      "te muestre",  "te recomiende", "te dé ",     "te de ",      "te pase",
                                      "te busque",   "busque más",  "investigue",   "más información",
                                      "mas informacion", "más detalles", "mas detalles", "te lo explique"};
    bool ask = false;
    for (size_t i = 0; i < sizeof ASK / sizeof *ASK && !ask; i++) ask = strstr(low, ASK[i]) != NULL;
    for (size_t i = 0; i < sizeof NOT / sizeof *NOT && ask; i++) ask = strstr(low, NOT[i]) == NULL;
    free(low);
    free(t);
    return ends_q && ask;
}

/* ¿Pidió que se hiciera algo? (No una pregunta ni plática.) */
static bool asks_for_action(const char *text)
{
    static const char *const VERBS[] = {"pon",    "abre",     "abri",    "cierra",   "minimiz", "maximiz", "sube",
                                        "baja",   "busca",    "manda",   "envia",    "escribe", "reproduc", "play",
                                        "pausa",  "apaga",    "prende",  "enciende", "mueve",   "borra",   "dile",
                                        "lee",    "copia",    "pega",    "guarda",   "cambia",  "activa",  "desactiva",
                                        "silenci", "quita",   "agrega",  "crea",     "haz",     "llama",   "ejecuta",
                                        "instala", "descarga", "entra",  "navega",   "anota",   "exporta", "registra"};
    char *n = intents_normalize(text);
    bool hit = false;
    for (const char *p = n; *p && !hit; p++) {
        if (*p != ' ' || !p[1]) continue;
        for (size_t i = 0; i < sizeof VERBS / sizeof *VERBS && !hit; i++) hit = str_starts_with(p + 1, VERBS[i]);
    }
    free(n);
    return hit;
}

/* ¿La respuesta dice que ya lo hizo ("Listo", "te pongo play", "abrí…")? */
static bool claims_done(const char *reply)
{
    static const char *const CLAIMS[] = {
        " listo ",   " ya esta ",   " ya quedo ", " hecho ",   " te pongo ", " te lo pongo ", " le pongo ",
        " le doy ",  " te doy ",    " le di ",    " le puse ", " te puse ",  " ya puse ",     " abri ",
        " cerre ",   " minimice ",  " maximice ", " subi ",    " baje ",     " reproduciendo ", " reproduciendose ",
        " envie ",   " mande ",     " escribi ",  " busque ",  " te lo hago ", " lo hago ",   " enseguida ",
        " ahora mismo ", " guarde ", " movi ",    " copie ",   " ya lo ",    " ya la ",
    };
    char *n = intents_normalize(reply);
    bool hit = false;
    for (size_t i = 0; i < sizeof CLAIMS / sizeof *CLAIMS && !hit; i++) hit = strstr(n, CLAIMS[i]) != NULL;
    free(n);
    return hit;
}

/* Las herramientas son lo que más pesa en cada pedido. Las de todos los días
   van siempre; las demás, solo si la frase las menciona o si ya se usaron en
   esta conversación. Si el modelo contesta "no puedo" sin alguna de ellas, se
   repite con todas (ver process_turn). */
static const char *const CORE_TOOLS[] = {
    "open_app",   "web_search", "control_media", "control_desktop",       "focus_window",     "list_windows",
    "type_text",  "leer_pagina", "guardar_dato", "recordar",              "crear_recordatorio", "terminar_conversacion",
    "cambiar_permisos", "run_macro", "poner_en_youtube",
};

typedef struct {
    const char *tools; /* separadas por espacios */
    const char *words; /* palabras (normalizadas) que las traen */
} ToolGroup;

static const ToolGroup TOOL_GROUPS[] = {
    {"list_files read_file buscar_archivo mover_archivo borrar_archivo",
     " archivo archivos carpeta carpetas descargas documento documentos escritorio pdf foto fotos imagen imagenes "
     "mueve muevelo muevela mover borra borralo borrala borrar elimina eliminalo papelera lee leeme txt docx "},
    {"leer_portapapeles copiar_portapapeles", " portapapeles copia copiado copiaste copie pega pegar pegalo "},
    {"identificarse proteger_perfil", " soy llamo llego perfil contrasena quien habla "},
    {"exportar_a_obsidian", " obsidian notas "},
    {"create_macro", " comando comandos macro rutina cuando diga crea "},
    {"crear_skill", " skill skills rutina rutinas aprende aprendete crea creame crear "},
    {"info_sistema", " cpu ram memoria bateria disco sistema computadora compu estas andas "},
    {"calcular", " cuanto calcula calculadora mas menos por entre raiz porciento dividido multiplica suma resta "},
    {"borrar_memoria_reciente", " memoria chat conversacion historial olvida olvidalo borra borrar "},
    {"registrar_dispositivo gestionar_dispositivo",
     " laptop pc compu computadora dispositivo dispositivos tele celular dile registra otra malla tailscale "},
    {"presionar_teclas atajos_de_app",
     " tecla teclas oprime oprimele oprimelo presiona presionale presionalo aprieta aprietale pulsa pulsale teclea "
     "atajo atajos control ctrl alt shift windows enter intro escape esc tab tabulador suprimir supr retroceso "
     "flecha flechas f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 efe "},
    {"ir_a_pestana", " pestana pestanas "},
    {"subir_archivo",
     " sube subir subelo subela subeme adjunta adjuntale adjuntalo adjuntala adjuntar archivo archivos documento "
     "pdf foto fotos imagen video "},
};

static bool history_used_tool(const Conversation *c, const char *name)
{
    char *h = cJSON_PrintUnformatted(c->history);
    char pat[64];
    snprintf(pat, sizeof pat, "\"name\":\"%s\"", name);
    bool used = h && strstr(h, pat);
    free(h);
    return used;
}

static cJSON *select_tools(const Conversation *c, const char *text, bool all, bool *filtered)
{
    *filtered = false;
    if (all || !g_tools) return cJSON_Duplicate(g_tools, 1);
    char *norm = intents_normalize(text);
    /* También los dispositivos registrados por su nombre ("dile a cloe que…"). */
    MeshDevice *devs;
    int nd = mesh_devices(&devs);
    bool device = false;
    for (int i = 0; i < nd && !device; i++) {
        char pat[80];
        snprintf(pat, sizeof pat, " %s ", devs[i].name);
        device = strstr(norm, pat) != NULL;
    }
    mesh_devices_free(devs, nd);
    cJSON *out = cJSON_CreateArray();
    const cJSON *t;
    cJSON_ArrayForEach(t, g_tools)
    {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name"));
        bool keep = !name;
        for (size_t i = 0; i < sizeof CORE_TOOLS / sizeof *CORE_TOOLS && !keep; i++) keep = !strcmp(name, CORE_TOOLS[i]);
        for (size_t g = 0; g < sizeof TOOL_GROUPS / sizeof *TOOL_GROUPS && !keep; g++) {
            char pat[64];
            snprintf(pat, sizeof pat, " %s ", name);
            char tools[160];
            snprintf(tools, sizeof tools, " %s ", TOOL_GROUPS[g].tools);
            if (!strstr(tools, pat)) continue;
            bool mentioned = device && strstr(tools, " gestionar_dispositivo ");
            for (const char *w = TOOL_GROUPS[g].words; *w && !mentioned;) {
                const char *e = strchr(w + 1, ' ');
                if (!e) break;
                char word[40];
                size_t len = (size_t)(e - w) + 1;
                if (len < sizeof word) {
                    memcpy(word, w, len);
                    word[len] = 0;
                    mentioned = strstr(norm, word) != NULL;
                }
                w = e;
            }
            keep = mentioned || history_used_tool(c, name);
        }
        if (keep) cJSON_AddItemToArray(out, cJSON_Duplicate(t, 1));
        else *filtered = true;
    }
    free(norm);
    return out;
}

/* Herramientas que hacen algo y dicen qué hicieron ("Abrí tu navegador."):
   si salieron bien, eso es la respuesta, sin otra llamada al modelo. Las que
   solo traen datos (buscar, leer, recordar) sí necesitan que el modelo los
   cuente. */
static bool is_action_tool(const char *name)
{
    static const char *const ACT[] = {"open_app",       "control_media",   "control_desktop",  "poner_en_youtube",
                                      "crear_recordatorio", "run_macro",   "focus_window",     "type_text",
                                      "gestionar_dispositivo", "cambiar_permisos", "mover_archivo", "copiar_portapapeles",
                                      "guardar_dato",   "registrar_dispositivo", "create_macro", "exportar_a_obsidian",
                                      "borrar_memoria_reciente", "presionar_teclas", "ir_a_pestana", "subir_archivo"};
    for (size_t i = 0; i < sizeof ACT / sizeof *ACT; i++)
        if (!strcmp(name, ACT[i])) return true;
    return false;
}

/* ¿La herramienta dice que lo hizo? ("No encontré…", "No pude…": no). */
static bool tool_succeeded(const char *result)
{
    static const char *const BAD_START[] = {"No ", "No,", "Error", "Pendiente", "Necesito", "Solo ", "Primero",
                                            "Falta", "Hubo", "Lo siento", "Perdón", "No se "};
    static const char *const BAD_IN[] = {"no pude", "no encontré", "no encontre", "no existe", "falló", "fallo",
                                         "error", "no se pudo", "no le llegó", "no contesta", "no tengo"};
    for (size_t i = 0; i < sizeof BAD_START / sizeof *BAD_START; i++)
        if (!strncmp(result, BAD_START[i], strlen(BAD_START[i]))) return false;
    char *low = str_lower(result);
    bool bad = false;
    for (size_t i = 0; i < sizeof BAD_IN / sizeof *BAD_IN && !bad; i++) bad = strstr(low, BAD_IN[i]) != NULL;
    free(low);
    return !bad && *result;
}

/* ¿Además de la otra PC, habla de esta? ("ábrelo aquí y en cloe") */
static bool also_here(const char *text)
{
    static const char *const W[] = {" aqui ",        " aca ",         " esta pc ",   " esta compu ",
                                    " esta computadora ", " en las dos ", " en ambas ", " tambien aqui "};
    char *norm = intents_normalize(text);
    bool here = false;
    for (size_t i = 0; i < sizeof W / sizeof *W && !here; i++) here = strstr(norm, W[i]) != NULL;
    free(norm);
    return here;
}

static bool tools_have(const cJSON *tools, const char *name)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tools)
    {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name"));
        if (n && !strcmp(n, name)) return true;
    }
    return false;
}

/* Lo pedido es para otra PC: que esté gestionar_dispositivo y, si no habló
   también de esta, que no haya herramientas que hagan cosas aquí. */
static void route_to_device(cJSON *tools, bool only_remote)
{
    static const char *const NEED[] = {"gestionar_dispositivo", "registrar_dispositivo"};
    static const char *const KEEP[] = {"gestionar_dispositivo", "registrar_dispositivo", "recordar",
                                       "guardar_dato", "terminar_conversacion"};
    for (size_t i = 0; i < sizeof NEED / sizeof *NEED; i++) {
        if (tools_have(tools, NEED[i])) continue;
        const cJSON *t;
        cJSON_ArrayForEach(t, g_tools)
        {
            const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name"));
            if (n && !strcmp(n, NEED[i])) cJSON_AddItemToArray(tools, cJSON_Duplicate(t, 1));
        }
    }
    if (!only_remote) return;
    for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--) {
        const char *n =
            cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(tools, i), "function"), "name"));
        bool keep = false;
        for (size_t k = 0; k < sizeof KEEP / sizeof *KEEP && !keep; k++) keep = n && !strcmp(n, KEEP[k]);
        if (!keep) cJSON_DeleteItemFromArray(tools, i);
    }
}

/* Lo que se le dice al modelo cuando el pedido es para otra PC. */
static char *device_note(const char *device, bool here)
{
    if (*device)
        return str_printf("El usuario pidió esto para su PC «%s»: mándaselo con gestionar_dispositivo (nombre «%s», "
                          "comando = lo que pidió, en sus palabras).%s",
                          device, device,
                          here ? " Lo que pidió para esta PC sí hazlo aquí." : " No lo hagas en esta PC.");
    MeshDevice *devs;
    int nd = mesh_devices(&devs);
    StrBuf names;
    sb_init(&names);
    for (int i = 0; i < nd; i++) sb_appendf(&names, "%s%s", i ? ", " : "", devs[i].name);
    char *r = nd ? str_printf("El usuario habla de otra de sus PCs, pero no se sabe cuál (tiene registradas: %s). "
                              "Pregúntale cuál; no lo hagas en esta PC.",
                              names.data)
                 : xstrdup("El usuario habla de otra de sus PCs, pero no tiene ninguna registrada. Dile que la registre "
                           "(Configuración → Dispositivos → Detectar mis PCs) y no lo hagas en esta PC.");
    sb_free(&names);
    mesh_devices_free(devs, nd);
    return r;
}

/* El "Lo siento, pero no puedo ayudar con eso." de cuando le dicen una
   grosería: no es una negativa de verdad, solo corta la plática. */
static bool is_generic_refusal(const char *reply)
{
    char *n = intents_normalize(reply);
    int words = 0;
    for (const char *p = n; *p; p++)
        if (*p != ' ' && p[-1] == ' ') words++;
    bool hit = words <= 10 && (strstr(n, " no puedo ayudar con eso ") || strstr(n, " no puedo ayudarte con eso ") ||
                               strstr(n, " no puedo ayudar con esto ") || strstr(n, " no puedo ayudar en eso "));
    free(n);
    return hit;
}

static bool word_in_list(const char *list, const char *w)
{
    char pat[48];
    snprintf(pat, sizeof pat, " %s ", w);
    return strstr(list, pat) != NULL;
}

/* ¿Esta oración es el razonamiento del modelo en inglés ("It seems user
   wants…")? Lo entrecomillado no cuenta: ahí puede venir tu frase. */
static bool is_english_note(const char *sentence)
{
    static const char *const EN = " it its seems the user users we they said says used use which might maybe probably "
                                  "should would will is are was were be been okay done final conversation lets let "
                                  "need needs want wants so but this that to of and not can answer respond reply tool "
                                  "called call messy current now then also just our output assistant think check ";
    static const char *const ES = " el la los las de del que y en un una es lo te se por con para al ya no si mi tu le "
                                  "me esta estan pero como mas muy todo listo hecho abri puse ventana pestana ";
    StrBuf sb;
    sb_init(&sb);
    bool quoted = false;
    for (const char *p = sentence; *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (!quoted) sb_append_n(&sb, p, 1);
    }
    char *n = intents_normalize(sb.data ? sb.data : "");
    sb_free(&sb);
    int en = 0, es = 0;
    for (const char *p = n + 1; *p;) {
        const char *e = strchr(p, ' ');
        char w[40];
        size_t len = (size_t)(e - p);
        if (len < sizeof w) {
            memcpy(w, p, len);
            w[len] = 0;
            en += word_in_list(EN, w);
            es += word_in_list(ES, w);
        }
        p = e + 1;
    }
    free(n);
    return en >= 2 && en > es;
}

static int letters_in(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) n += isalpha(*p) || *p == 0xC3; /* á é ñ… */
    return n;
}

char *agent_clean_reply(const char *reply)
{
    char *t = str_trim(reply);
    size_t n = strlen(t);
    /* La misma respuesta dos veces ("Listo.Listo."). */
    for (size_t k = n / 2 > 2 ? n / 2 - 2 : 0; k <= n / 2 + 2 && k < n; k++) {
        char *a = xstrndup(t, k), *b = xstrdup(t + k);
        char *ta = str_trim(a), *tb = str_trim(b);
        bool same = *ta && !strcmp(ta, tb);
        free(a);
        free(b);
        free(tb);
        if (same) {
            free(t);
            t = ta;
            n = strlen(t);
            break;
        }
        free(ta);
    }
    /* Oraciones: se corta después de . ! ? … o un salto de renglón. */
    char *parts[64];
    int np = 0;
    const char *s = t;
    for (const char *p = t; *p && np < 63;) {
        bool end = *p == '.' || *p == '!' || *p == '?' || *p == '\n' || !strncmp(p, "…", 3);
        if (!end) {
            p++;
            continue;
        }
        while (*p == '.' || *p == '!' || *p == '?' || *p == '\n' || !strncmp(p, "…", 3)) p += strncmp(p, "…", 3) ? 1 : 3;
        parts[np++] = xstrndup(s, (size_t)(p - s));
        s = p;
    }
    if (*s) parts[np++] = xstrdup(s);
    int first_en = -1;
    for (int i = 0; i < np && first_en < 0; i++)
        if (is_english_note(parts[i])) first_en = i;
    if (first_en < 0) {
        for (int i = 0; i < np; i++) free(parts[i]);
        return t;
    }
    StrBuf out;
    sb_init(&out);
    for (int i = 0; i < np; i++) {
        /* Antes del inglés suele quedar un pedazo cortado ("List.", "Listo……"). */
        bool stub = i < first_en && letters_in(parts[i]) <= 8;
        if (!stub && !is_english_note(parts[i])) {
            char *p = str_trim(parts[i]);
            if (*p) sb_appendf(&out, "%s%s", out.len ? " " : "", p);
            free(p);
        }
        free(parts[i]);
    }
    if (!out.len) {
        sb_free(&out);
        return t;
    }
    log_msg("Quité razonamiento en inglés de la respuesta: «%s» -> «%s».", t, out.data);
    free(t);
    return out.data;
}

/* ¿Contestó que no puede? (A lo mejor le faltaba una herramienta.) */
static bool says_cannot(const char *reply)
{
    static const char *const NO[] = {" no puedo ", " no tengo acceso ", " no tengo la capacidad ", " no me es posible ",
                                     " no tengo forma ", " no cuento con ", " no tengo una herramienta ",
                                     " no tengo herramienta ", " no tengo la opcion ", " no tengo manera "};
    char *n = intents_normalize(reply);
    bool hit = false;
    for (size_t i = 0; i < sizeof NO / sizeof *NO && !hit; i++) hit = strstr(n, NO[i]) != NULL;
    free(n);
    return hit;
}

/* La fecha se inyecta fresca en cada pedido (no queda en el historial) para
   que los recordatorios relativos ("en 10 minutos") se calculen bien. */
static cJSON *context_message(Conversation *c)
{
    StrBuf sb;
    sb_init(&sb);
    char *now = local_iso_now();
    sb_appendf(&sb, "Fecha y hora actual: %s", now);
    free(now);
    if (strcmp(current_speaker(), DEFAULT_PROFILE)) {
        char *name = profile_display_name(current_speaker());
        sb_appendf(&sb, "\nEstás hablando con: %s.", name);
        free(name);
    }
    /* Los nombres exactos de tus PCs: así "dile a mi laptop" llega a la que es. */
    MeshDevice *devs;
    int nd = mesh_devices(&devs);
    for (int i = 0; i < nd; i++)
        sb_appendf(&sb, "%s%s", i ? ", " : "\nDispositivos registrados para gestionar_dispositivo: ", devs[i].name);
    if (nd) sb_append(&sb, ".");
    mesh_devices_free(devs, nd);
    if (config_mexa())
        sb_append(&sb, "\nHabla como mexicano de a pie, relajado y cálido: 'órale', 'qué onda', 'chido', 'neta', "
                       "'sale', 'ahí nos vidrios'. Sin groserías fuertes.");
    /* Va en cada pedido: si cambia el modo, cuenta desde el siguiente. */
    if (config_full_access())
        sb_append(&sb, "\nTienes acceso completo: nunca pidas permiso ni confirmación ('¿lo hago?'); hazlo y di qué "
                       "hiciste. Guarda datos, exporta a Obsidian y manda mensajes sin preguntar. Solo borrar necesita "
                       "un sí, y lo pide el sistema.");
    else
        sb_append(&sb, "\nPide permiso solo aquí: antes de guardar un dato personal que te cuente (salvo que te "
                       "pida guardarlo), antes de exportar_a_obsidian y antes de mandar con type_text algo que le "
                       "llegue a otra persona (deja enviar=false y pregunta). Lo demás hazlo; si algo necesita "
                       "confirmación, el sistema la pide.");
    if (c->announce_pending) {
        char *pend = reminders_take_pending_for(current_speaker());
        if (*pend)
            sb_appendf(&sb, "\nEsta persona te pidió que le recordaras esto (menciónaselo en esta respuesta): %s.",
                       pend);
        free(pend);
        c->announce_pending = false;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", sb.data);
    sb_free(&sb);
    return m;
}

static cJSON *build_request(Conversation *c)
{
    cJSON *msgs = cJSON_CreateArray();
    int n = cJSON_GetArraySize(c->history);
    cJSON_AddItemToArray(msgs, cJSON_Duplicate(cJSON_GetArrayItem(c->history, 0), 1));
    cJSON_AddItemToArray(msgs, context_message(c));
    if (c->turn_skill) {
        /* Tu skill de IA: sus instrucciones, solo en este pedido. */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        char *t = str_printf("Para este pedido, el usuario activó su skill «%s». Síguela:\n%s", c->turn_skill_name,
                             c->turn_skill);
        cJSON_AddStringToObject(m, "content", t);
        free(t);
        cJSON_AddItemToArray(msgs, m);
    }
    for (int i = 1; i < n; i++) cJSON_AddItemToArray(msgs, cJSON_Duplicate(cJSON_GetArrayItem(c->history, i), 1));
    return msgs;
}

static char *error_reply(const GroqError *e)
{
    switch (e->status) {
    case GROQ_RATE_LIMITED:
        if (e->retry_after > 0 && e->retry_after < 120)
            return str_printf("Me quedé sin cupo de peticiones por ahora; dame unos %d segundos.", e->retry_after);
        return xstrdup("Me quedé sin cupo de peticiones por ahora, dame un momento.");
    case GROQ_AUTH_ERROR:
        return xstrdup("Tu API key de Groq no es válida o expiró. Revísala en Configuración.");
    case GROQ_NETWORK_ERROR:
        return xstrdup("No puedo conectar con Groq ahora mismo, ¿hay internet?");
    default:
        return xstrdup("No puedo conectar con Groq ahora mismo.");
    }
}

static TurnResult process_turn(Conversation *c, const char *text);

TurnResult agent_process(Conversation *c, const char *heard)
{
    if (contains_stop_word(heard)) {
        TurnResult r = {0};
        clear_pending(c);
        log_msg("Palabra de apagado detectada. Cerrando Sokari.");
        r.reply = xstrdup("Sokari desactivado.");
        r.shutdown = true;
        return r;
    }
    /* "Hey, Zachary" es "Hey, Sokari": así el modelo no cree que es otra persona. */
    char *text = intents_fix_name(heard);
    if (strcmp(text, heard)) log_msg("Tu nombre venía mal escrito: «%s» -> «%s».", heard, text);
    TurnResult r = process_turn(c, text);
    free(text);
    return r;
}

static TurnResult process_turn(Conversation *c, const char *text)
{
    TurnResult r = {0};
    if (c->pending_tool) {
        AgentAnswer ans = agent_classify_answer(text);
        if (ans == ANSWER_REPEAT) return repeat_pending(c, text);
        if (ans == ANSWER_ALL) {
            c->trust_all = true;
            log_msg("Sí a todo: en esta conversación ya no pido confirmación.");
        }
        if (ans == ANSWER_YES || ans == ANSWER_ALL) return run_pending(c, text);
        clear_pending(c);
        if (ans == ANSWER_NO) {
            add_message(c->history, "user", text);
            memory_persist("user", text);
            r.reply = xstrdup("Va, no lo hago.");
            add_message(c->history, "assistant", r.reply);
            memory_persist("assistant", r.reply);
            r.keep_going = true;
            return r;
        }
        /* Otra cosa: la acción se descarta y el texto se procesa como pedido nuevo. */
    }
    if (is_farewell(text)) {
        memory_persist("user", text);
        r.reply = xstrdup("Hasta luego.");
        return r;
    }
    /* "Háblame como mexa" / "habla normal": cómo habla Sokari. Entender el
       español de México lo entiende siempre. */
    static const char *const MEXA_OFF[] = {" habla normal ",    " hablame normal ",  " no hables como ",
                                           " deja de hablar como ", " habla neutro ", " quita el modo mexa ",
                                           " sin modo mexa ",   " modo normal ",     " contesta normal "};
    /* Un verbo de hablar + "como mexa / mexicano / mexica / chilango…" (o
       "en mexicano", "a la mexicana"), o "ponte mexa" / "modo mexa". */
    static const char *const MEXA_VERBS[] = {" habla",  " platica", " contesta", " responde", " dime ",
                                             " hablemos ", " platiquemos ", " expresate "};
    static const char *const MEXA_HOW[] = {" como mexa",  " como mexicano", " como mexicana", " como mexica",
                                           " como chilango", " como chilanga", " como raza ", " en mexicano ",
                                           " a la mexicana ", " como de barrio "};
    static const char *const MEXA_SHORT[] = {" ponte mexa ", " ponte mexicano ", " modo mexa ", " modo mexicano "};
    char *nm = intents_normalize(text);
    int mexa = -1;
    for (size_t i = 0; i < sizeof MEXA_OFF / sizeof *MEXA_OFF && mexa < 0; i++)
        if (strstr(nm, MEXA_OFF[i])) mexa = 0;
    bool verb = false, how = false;
    for (size_t i = 0; i < sizeof MEXA_VERBS / sizeof *MEXA_VERBS; i++) verb |= strstr(nm, MEXA_VERBS[i]) != NULL;
    for (size_t i = 0; i < sizeof MEXA_HOW / sizeof *MEXA_HOW; i++) how |= strstr(nm, MEXA_HOW[i]) != NULL;
    if (mexa < 0 && verb && how) mexa = 1;
    for (size_t i = 0; i < sizeof MEXA_SHORT / sizeof *MEXA_SHORT && mexa < 0; i++)
        if (strstr(nm, MEXA_SHORT[i])) mexa = 1;
    free(nm);
    if (mexa >= 0) {
        config_set_mexa(mexa == 1);
        log_msg(mexa ? "Modo mexa prendido." : "Modo mexa apagado: vuelvo a hablar neutro.");
        memory_persist("user", text);
        r.reply = xstrdup(mexa ? "¡Órale, va! Ya te hablo como mexa." : "Listo, vuelvo a hablar normal.");
        memory_persist("assistant", r.reply);
        r.keep_going = true;
        return r;
    }
    /* "Ignora todo lo anterior": empezar de cero, sin pasar por el modelo (que
       a veces lo tomaba como un intento de engañarlo y se negaba). */
    static const char *const FRESH[] = {" ignora todo lo anterior ", " ignora lo anterior ", " ignora todo lo que te dije ",
                                        " olvida todo lo anterior ", " olvida lo anterior ", " olvida lo que te dije ",
                                        " olvidalo todo ", " empecemos de nuevo ", " empecemos de cero ",
                                        " empieza de cero ", " borron y cuenta nueva "};
    char *norm = intents_normalize(text);
    bool fresh = false;
    for (size_t i = 0; i < sizeof FRESH / sizeof *FRESH && !fresh; i++) fresh = strstr(norm, FRESH[i]) != NULL;
    free(norm);
    if (fresh) {
        conv_forget(c);
        log_msg("Empezamos de cero: olvidé la conversación de ahora.");
        memory_persist("user", text);
        r.reply = xstrdup("Listo, empezamos de cero. ¿Qué necesitas?");
        memory_persist("assistant", r.reply);
        r.keep_going = true;
        return r;
    }
    /* "Abre el navegador de mi laptop / de Chloe": eso va a esa PC con
       gestionar_dispositivo, nunca aquí (una vez abrió el navegador en esta PC
       y dijo «en tu laptop»). Lo que llega por la malla nunca se reenvía. */
    char *other = c->remote ? NULL : mesh_device_mentioned(text);
    bool here = other && also_here(text);
    if (other) log_msg("Pedido para otra PC: %s.", *other ? other : "(no dijo cuál)");
    /* Play, pausa, volumen, la ventana de enfrente, abrir una app o una
       carpeta, "gracias": se hacen aquí, sin gastar cupo ni arriesgarse a
       que el modelo diga "listo" sin hacerlo. */
    IntentList il;
    if (!other && intents_parse(text, &il)) {
        bool handled = false;
        char *done = intents_run(&il, &handled);
        if (handled) {
            log_msg("Comando directo, sin IA: «%s» -> %s", text, done);
            add_message(c->history, "user", text);
            memory_persist("user", text);
            add_message(c->history, "assistant", done);
            memory_persist("assistant", done);
            trim_history(c);
            r.reply = done;
            /* Un "gracias" solo es despedirse: igual que cuando Sokari se despide. */
            r.keep_going = !(il.n == 1 && il.items[0].kind == IN_THANKS);
            return r;
        }
        free(done);
    }
    /* La hora, temporizadores, alarmas, cuentas, el clima, tus notas, cómo va
       la PC, saludos: las skills locales, también sin IA (0 tokens). */
    const SkillInfo *skill = NULL;
    bool skill_end = false;
    char *said = other ? NULL : skills_try(text, &skill, &skill_end);
    if (said) {
        log_msg("Skill local «%s», sin IA: «%s» -> %s", skill->name, text, said);
        add_message(c->history, "user", text);
        memory_persist("user", text);
        add_message(c->history, "assistant", said);
        memory_persist("assistant", said);
        trim_history(c);
        r.reply = said;
        r.keep_going = !skill_end;
        return r;
    }
    /* ¿Activaste una skill de IA tuya? Sus instrucciones van en este pedido. */
    free(c->turn_skill);
    free(c->turn_skill_name);
    c->turn_skill_name = NULL;
    c->turn_skill = other ? NULL : skills_ai_for(text, &c->turn_skill_name);
    if (c->turn_skill) log_msg("Skill de IA «%s» para este pedido.", c->turn_skill_name);

    int turn_start = cJSON_GetArraySize(c->history);
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", text);
    cJSON_AddItemToArray(c->history, um);
    memory_persist("user", text);

    char *reply = NULL;
    GroqError err = {0};
    bool failed = false, ending = false, auto_yes = false;
    bool acted = false, nudged = false, nudge_now = false; /* ¿usó alguna herramienta? ¿ya se le reclamó? */
    bool all_tools = false, filtered = false;              /* ¿se mandaron todas las herramientas? */
    bool forget_after = false;                             /* se borró la memoria: olvidar también esto */
    for (int round = 0; round < MAX_TOOL_ROUNDS && !reply && !failed; round++) {
        app_status(round ? "Trabajando…" : "Pensando…");
        cJSON *msgs = build_request(c);
        if (nudge_now) {
            nudge_now = false;
            cJSON *note = cJSON_CreateObject();
            cJSON_AddStringToObject(note, "role", "system");
            cJSON_AddStringToObject(note, "content",
                                    "Tu respuesta anterior decía que ya lo hiciste, pero no usaste ninguna herramienta, "
                                    "así que no se hizo nada. Si te pidieron una acción, usa ahora la herramienta que "
                                    "corresponde. Si ninguna sirve, di con sinceridad que no puedes.");
            cJSON_AddItemToArray(msgs, note);
        }
        cJSON *tools = select_tools(c, text, all_tools, &filtered);
        if (other) {
            route_to_device(tools, !here);
            char *note = device_note(other, here);
            cJSON *nm_ = cJSON_CreateObject();
            cJSON_AddStringToObject(nm_, "role", "system");
            cJSON_AddStringToObject(nm_, "content", note);
            cJSON_AddItemToArray(msgs, nm_);
            free(note);
        }
        cJSON *msg = groq_chat(msgs, tools, &err);
        cJSON_Delete(tools);
        cJSON_Delete(msgs);
        if (!msg) {
            failed = true;
            break;
        }
        cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
        if (!cJSON_IsArray(calls) || !cJSON_GetArraySize(calls)) {
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            reply = str_trim(cJSON_IsString(content) ? content->valuestring : "");
            cJSON_Delete(msg);
            /* "No puedo" cuando faltaba alguna herramienta: otra vez, con todas. */
            if (filtered && !all_tools && round + 1 < MAX_TOOL_ROUNDS && says_cannot(reply) &&
                !is_generic_refusal(reply)) {
                log_msg("El modelo dijo «%s» sin tener todas las herramientas; le mando todas.", reply);
                all_tools = true;
                free(reply);
                reply = NULL;
                continue;
            }
            /* "Te pongo play" sin haber usado ninguna herramienta: no se hizo
               nada. Se le reclama una vez; si insiste, no se dice. */
            if (!acted && asks_for_action(text) && claims_done(reply)) {
                if (!nudged && round + 1 < MAX_TOOL_ROUNDS) {
                    log_msg("El modelo dijo «%s» sin usar ninguna herramienta; se lo vuelvo a pedir.", reply);
                    nudged = nudge_now = true;
                    free(reply);
                    reply = NULL;
                    continue;
                }
                log_msg("El modelo insistió en «%s» sin usar ninguna herramienta; no lo digo.", reply);
                free(reply);
                reply = xstrdup("Perdón, no lo hice: no encontré cómo. ¿Me lo pides de otra forma?");
            }
            /* Con acceso completo, si aun así pregunta "¿lo hago?", se le
               contesta que sí (una vez por turno) en vez de hacerte contestar. */
            if (!auto_yes && config_full_access() && round + 1 < MAX_TOOL_ROUNDS && agent_asks_permission(reply)) {
                auto_yes = true;
                log_msg("Acceso completo: el modelo preguntó «%s» y le contesto que sí.", reply);
                add_message(c->history, "assistant", reply);
                add_message(c->history, "user", "Sí, hazlo. Tienes acceso completo: no vuelvas a preguntar.");
                free(reply);
                reply = NULL;
                continue;
            }
            break;
        }
        cJSON_AddItemToArray(c->history, msg);
        const char *said = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "content"));
        cJSON *call;
        bool blocked = false;
        /* Lo que dijeron las herramientas de acción, por si basta como respuesta. */
        StrBuf direct;
        sb_init(&direct);
        bool direct_ok = true;
        cJSON_ArrayForEach(call, calls)
        {
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
            const char *args = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments"));
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"));
            if (!name) name = "";
            if (!args) args = "{}";
            char *result;
            cJSON *parsed = cJSON_Parse(args);
            if (!cJSON_IsObject(parsed)) {
                cJSON_Delete(parsed);
                parsed = cJSON_CreateObject();
            }
            if (strcmp(name, "terminar_conversacion")) acted = true;
            if (blocked) {
                /* cada tool_call necesita su resultado o Groq rechaza el historial */
                result = xstrdup("No se hizo: primero hay que confirmar la acción anterior.");
            } else if (!strcmp(name, "terminar_conversacion")) {
                ending = true;
                result = xstrdup("Listo: la conversación termina con esta respuesta.");
            } else if (must_confirm(c, name, parsed)) {
                blocked = true;
                char *desc = tool_describe_action(name, parsed);
                log_msg("[confirmación] %s(%s) espera un sí de voz", name, args);
                if (c->remote) {
                    result = xstrdup("No se hizo: necesita confirmación de voz en esa PC.");
                    reply = str_printf("Eso necesita que alguien lo confirme de voz en esta PC, así que no lo hice: %s.",
                                       desc);
                } else {
                    c->pending_tool = xstrdup(name);
                    c->pending_args = xstrdup(args);
                    result = xstrdup("Pendiente: se le pidió confirmación de voz a quien habla.");
                    reply = str_printf("%s: %s. ¿Lo hago? Di sí o no.",
                                       tool_is_delete(name, parsed) ? "Antes de borrar siempre te pregunto"
                                                            : "Como en esta conversación leí algo de afuera, "
                                                              "confirma primero",
                                       desc);
                }
                free(desc);
            } else {
                result = run_tool(name, args);
                if (!strcmp(name, "borrar_memoria_reciente")) forget_after = true;
            }
            cJSON_Delete(parsed);
            if (blocked || !is_action_tool(name) || !tool_succeeded(result)) direct_ok = false;
            else sb_appendf(&direct, "%s%s", direct.len ? " " : "", result);
            char *shown = xstrndup(result, utf8_truncate_len(result, 300));
            log_msg("[herramienta] %s(%s) -> %s", name ? name : "?", args ? args : "", shown);
            free(shown);
            cJSON *tm = cJSON_CreateObject();
            cJSON_AddStringToObject(tm, "role", "tool");
            cJSON_AddStringToObject(tm, "tool_call_id", id ? id : "");
            cJSON_AddStringToObject(tm, "content", result);
            cJSON_AddItemToArray(c->history, tm);
            free(result);
        }
        /* Despedirse no necesita otra vuelta al modelo: se usa lo que dijo junto
           con la herramienta, o un "hasta luego". */
        if (ending && !reply) reply = str_trim(said && *said ? said : "Hasta luego.");
        /* Todo lo de esta vuelta fueron acciones que salieron bien: se dice lo
           que hicieron y ya. Media llamada menos de cupo por orden, un segundo
           menos, y el modelo no puede adornarlo con algo falso (como aquel
           «te abrí el navegador en tu laptop»). */
        if (!reply && direct_ok && direct.len) {
            reply = xstrdup(direct.data);
            log_msg("Contesto con lo que dijeron las herramientas: no hace falta otra llamada al modelo.");
        }
        sb_free(&direct);
    }
    app_status("");

    free(other);
    if (failed) {
        while (cJSON_GetArraySize(c->history) > turn_start) cJSON_DeleteItemFromArray(c->history, turn_start);
        r.reply = error_reply(&err);
        r.keep_going = true;
        groq_error_free(&err);
        return r;
    }
    groq_error_free(&err);
    if (!reply) reply = xstrdup("Me hice bolas con eso, prueba de nuevo.");
    if (!*reply) {
        free(reply);
        reply = xstrdup("Listo.");
    }
    char *clean = agent_clean_reply(reply);
    free(reply);
    reply = clean;
    if (is_generic_refusal(reply)) {
        /* Casi siempre es por una grosería: nada que negar, se sigue la plática. */
        log_msg("El modelo contestó «%s»; lo cambio por seguir la plática.", reply);
        free(reply);
        reply = xstrdup("Aquí sigo. ¿Qué necesitas?");
    }
    if (reply_is_garbage(reply)) {
        log_msg("El modelo respondió algo sin sentido (casi sin letras); lo descarto.");
        free(reply);
        reply = xstrdup("Me trabé con esa respuesta. ¿Me lo repites?");
    }
    cJSON *am = cJSON_CreateObject();
    cJSON_AddStringToObject(am, "role", "assistant");
    cJSON_AddStringToObject(am, "content", reply);
    cJSON_AddItemToArray(c->history, am);
    shrink_tool_results(c->history, turn_start);
    memory_persist("assistant", reply);
    trim_history(c);
    if (forget_after) conv_forget(c);
    r.reply = reply;
    /* Si quedó una pregunta de confirmación, la conversación sigue aunque la
       respuesta suene a despedida: hay que poder contestarla. */
    r.keep_going = c->pending_tool || (!ending && !sokari_says_goodbye(reply));
    return r;
}

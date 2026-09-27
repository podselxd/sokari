/* Las IA de respaldo: qué modelos se escogen de cada proveedor y, con un
   servidor falso en 127.0.0.1 que hace de Groq y de NVIDIA, que cuando Groq
   se queda sin cupo contesta la siguiente al instante (sin esperar), que una
   key mala se salta y que sin ninguna con cupo se dice cuánto falta. La
   configuración va en una carpeta temporal: nunca toca la tuya. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "groq.h"
#include "http.h"
#include "log.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static bool list_is(char **ids, int n, const char *const *want)
{
    int k = 0;
    while (want[k]) k++;
    if (n != k) return false;
    for (int i = 0; i < n; i++)
        if (strcmp(ids[i], want[i])) return false;
    return true;
}

static void free_list(char **ids, int n)
{
    for (int i = 0; i < n; i++) free(ids[i]);
    free(ids);
}

static void test_escoger(void)
{
    printf("-- qué modelos se usan de cada proveedor --\n");
    const char *groq = "{\"data\":["
                       "{\"id\":\"whisper-large-v3\",\"active\":true},"
                       "{\"id\":\"openai/gpt-oss-20b\",\"active\":true},"
                       "{\"id\":\"meta-llama/llama-guard-4-12b\",\"active\":true},"
                       "{\"id\":\"llama-3.3-70b-versatile\",\"active\":true},"
                       "{\"id\":\"groq/compound\",\"active\":true},"
                       "{\"id\":\"moonshotai/kimi-k2-instruct-0905\",\"active\":true},"
                       "{\"id\":\"qwen/qwen3-32b\",\"active\":true},"
                       "{\"id\":\"playai-tts\",\"active\":true},"
                       "{\"id\":\"openai/gpt-oss-120b\",\"active\":true},"
                       "{\"id\":\"llama-4-maverick-viejo\",\"active\":false}]}";
    char **ids;
    int n = groq_pick_models("groq", groq, &ids);
    static const char *const WANT[] = {"openai/gpt-oss-120b", "qwen/qwen3-32b", "moonshotai/kimi-k2-instruct-0905",
                                       "llama-3.3-70b-versatile", "openai/gpt-oss-20b", NULL};
    check(list_is(ids, n, WANT), "Groq: todos los que usan herramientas, del mejor al más flojo (cada uno suma su cupo)");
    for (int i = 0; i < n; i++) printf("      %d. %s\n", i + 1, ids[i]);
    free_list(ids, n);

    const char *router = "{\"data\":["
                         "{\"id\":\"openai/gpt-oss-120b\",\"supported_parameters\":[\"tools\"]},"
                         "{\"id\":\"openai/gpt-oss-120b:free\",\"supported_parameters\":[\"tools\",\"max_tokens\"]},"
                         "{\"id\":\"meta-llama/llama-3.3-70b-instruct:free\",\"supported_parameters\":[\"max_tokens\"]},"
                         "{\"id\":\"qwen/qwen3-235b-a22b:free\",\"supported_parameters\":[\"tools\"]}]}";
    n = groq_pick_models("openrouter", router, &ids);
    static const char *const WANT_OR[] = {"openai/gpt-oss-120b:free", "qwen/qwen3-235b-a22b:free", NULL};
    check(list_is(ids, n, WANT_OR), "OpenRouter: solo los gratis («:free») que aceptan herramientas");
    free_list(ids, n);

    n = groq_pick_models("groq", "no es json", &ids);
    check(n == 0 && !ids, "si la lista no se entiende, nada (se usan los de siempre)");
    n = groq_pick_models("otro", groq, &ids);
    check(n == 0, "un proveedor desconocido: nada");
}

/* ---------------------------------------- un servidor que hace de IA --- */

#define PORT 18777
static SOCKET g_ls = INVALID_SOCKET;
static volatile LONG g_groq_chats, g_nvidia_chats, g_deepseek_chats;
static volatile LONG g_groq_mode; /* 0 sin cupo por hoy, 1 key mala, 2 contesta */
static volatile LONG g_nvidia_mode; /* 0 contesta, 1 sin cupo */

static void reply(SOCKET c, int code, const char *extra, const char *body)
{
    char head[512];
    snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s"
                                "Connection: close\r\n\r\n",
             code, strlen(body), extra ? extra : "");
    send(c, head, (int)strlen(head), 0);
    send(c, body, (int)strlen(body), 0);
}

static const char *ANSWER = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\"}}],"
                            "\"usage\":{\"prompt_tokens\":1500,\"completion_tokens\":12}}";

static DWORD WINAPI server(LPVOID arg)
{
    for (;;) {
        SOCKET c = accept(g_ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        char buf[65536];
        int got = 0, n;
        while ((n = recv(c, buf + got, (int)sizeof buf - 1 - got, 0)) > 0) {
            got += n;
            buf[got] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) continue;
            const char *cl = strstr(buf, "Content-Length:");
            int want = cl ? atoi(cl + 15) : 0;
            if (got - (int)(end + 4 - buf) >= want) break;
        }
        buf[got > 0 ? got : 0] = 0;
        char body[512];
        if (strstr(buf, "GET /groq/models")) {
            reply(c, 200, NULL, "{\"data\":[{\"id\":\"openai/gpt-oss-120b\",\"active\":true}]}");
        } else if (strstr(buf, "GET /nvidia/models")) {
            reply(c, 200, NULL, "{\"data\":[{\"id\":\"openai/gpt-oss-120b\"},{\"id\":\"nvidia/nv-embed-v1\"}]}");
        } else if (strstr(buf, "POST /groq/chat/completions")) {
            InterlockedIncrement(&g_groq_chats);
            if (g_groq_mode == 0)
                reply(c, 429, "Retry-After: 456\r\n",
                      "{\"error\":{\"message\":\"Rate limit reached ... tokens per day (TPD): Limit 200000\"}}");
            else if (g_groq_mode == 1)
                reply(c, 401, NULL, "{\"error\":{\"message\":\"Invalid API Key\"}}");
            else {
                snprintf(body, sizeof body, ANSWER, "hola desde groq");
                reply(c, 200, NULL, body);
            }
        } else if (strstr(buf, "POST /nvidia/chat/completions")) {
            InterlockedIncrement(&g_nvidia_chats);
            if (g_nvidia_mode == 1)
                reply(c, 429, "Retry-After: 300\r\n", "{\"error\":{\"message\":\"too many requests\"}}");
            else {
                snprintf(body, sizeof body, ANSWER, "hola desde nvidia");
                reply(c, 200, NULL, body);
            }
        } else if (strstr(buf, "POST /deepseek/chat/completions")) {
            InterlockedIncrement(&g_deepseek_chats);
            reply(c, 401, NULL, "{\"error\":{\"message\":\"Authentication Fails\"}}");
        } else {
            reply(c, 404, NULL, "{\"error\":{\"message\":\"no existe\"}}");
        }
        shutdown(c, SD_BOTH);
        closesocket(c);
    }
    return 0;
}

static bool start_server(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
#ifndef _WIN32
    /* En Linux el puerto queda apartado un minuto después de cada conexión
       (TIME_WAIT): sin esto, correr la prueba dos veces seguidas falla. */
    int one = 1;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (bind(g_ls, (struct sockaddr *)&a, sizeof a) || listen(g_ls, 16)) return false;
    HANDLE t = CreateThread(NULL, 0, server, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return t != NULL;
}

static void set_keys(const char *groq, const char *nvidia, const char *deepseek)
{
    AppConfig c = config_snapshot();
    free(c.groq_api_key);
    c.groq_api_key = xstrdup(groq);
    free(c.nvidia_key);
    c.nvidia_key = xstrdup(nvidia);
    free(c.deepseek_key);
    c.deepseek_key = xstrdup(deepseek);
    config_apply(&c);
    config_free(&c);
    groq_reset_models();
}

static char *ask(GroqError *err, double *secs)
{
    cJSON *msgs = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", "hola");
    cJSON_AddItemToArray(msgs, m);
    uint64_t t0 = GetTickCount64();
    cJSON *r = groq_chat(msgs, NULL, err);
    *secs = (double)(GetTickCount64() - t0) / 1000.0;
    cJSON_Delete(msgs);
    char *text = r ? xstrdup(cJSON_GetStringValue(cJSON_GetObjectItem(r, "content"))) : NULL;
    cJSON_Delete(r);
    return text;
}

static void test_respaldo(void)
{
    printf("-- cuando Groq se queda sin cupo --\n");
    if (!start_server()) {
        check(false, "el servidor falso escucha en 127.0.0.1");
        return;
    }
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/groq", PORT);
    groq_set_base_url("groq", url);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/nvidia", PORT);
    groq_set_base_url("nvidia", url);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/deepseek", PORT);
    groq_set_base_url("deepseek", url);

    GroqError err = {0};
    double secs;
    set_keys("gsk_prueba", "", "");
    g_groq_mode = 2;
    char *t = ask(&err, &secs);
    check(t && !strcmp(t, "hola desde groq"), "con cupo, contesta Groq como siempre");
    free(t);

    g_groq_mode = 0;
    set_keys("gsk_prueba", "nvapi-prueba", "");
    t = ask(&err, &secs);
    char what[200];
    snprintf(what, sizeof what, "Groq sin cupo por hoy: contesta NVIDIA al instante (%.1f s, sin esperar)", secs);
    check(t && !strcmp(t, "hola desde nvidia") && secs < 5, what);
    free(t);
    LONG before = g_groq_chats;
    t = ask(&err, &secs);
    check(t && !strcmp(t, "hola desde nvidia") && g_groq_chats == before,
          "y el siguiente pedido va directo a NVIDIA (a Groq no se le vuelve a preguntar hasta que tenga cupo)");
    free(t);

    set_keys("gsk_prueba", "nvapi-prueba", "sk-mala");
    /* Orden con DeepSeek antes que NVIDIA: su key mala se salta. */
    AppConfig c = config_snapshot();
    free(c.ai_order);
    c.ai_order = xstrdup("groq,deepseek,nvidia");
    config_apply(&c);
    config_free(&c);
    groq_reset_models();
    t = ask(&err, &secs);
    check(t && !strcmp(t, "hola desde nvidia") && g_deepseek_chats >= 1,
          "una key de respaldo que no sirve se salta y contesta la siguiente");
    free(t);

    g_nvidia_mode = 1;
    groq_reset_models();
    t = ask(&err, &secs);
    snprintf(what, sizeof what, "si ninguna tiene cupo, no se queda esperando: avisa cuánto falta (%d s, en %.1f s)",
             err.retry_after, secs);
    check(!t && err.status == GROQ_RATE_LIMITED && err.retry_after > 12 && secs < 8, what);
    free(t);
    groq_error_free(&err);

    g_groq_mode = 1;
    set_keys("gsk_mala", "", "");
    t = ask(&err, &secs);
    check(!t && err.status == GROQ_AUTH_ERROR, "sin respaldos y con la key de Groq mala: dice que la revises");
    free(t);
    groq_error_free(&err);
    closesocket(g_ls);
}

static void test_orden(void)
{
    printf("-- el orden se guarda --\n");
    AppConfig c = config_snapshot();
    free(c.ai_order);
    c.ai_order = xstrdup("nvidia,groq");
    config_apply(&c);
    config_free(&c);
    config_load();
    char *o = config_ai_order();
    check(!strcmp(o, "nvidia,groq"), "«nvidia,groq» se guarda y se vuelve a leer");
    free(o);
    char *k = config_provider_key("nvidia");
    check(k && *k == 0, "sin key de NVIDIA: vacía");
    free(k);

    /* Lo que escribes en «Orden» (Configuración de Windows y de Linux). */
    struct {
        const char *typed, *want, *what;
    } cases[] = {
        {"NVIDIA, groq; glm", "nvidia,groq,glm", "«NVIDIA, groq; glm»: minúsculas, comas y sin espacios"},
        {"groq,groq,nvidia", "groq,nvidia", "repetidas: una sola vez"},
        {"groq, chatgpt, glm", "groq,glm", "una que no conoce se quita"},
        {"", DEFAULT_AI_ORDER, "vacío: el orden de fábrica"},
        {"chatgpt", DEFAULT_AI_ORDER, "nada conocido: el orden de fábrica"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *got = config_clean_ai_order(cases[i].typed);
        check(!strcmp(got, cases[i].want), cases[i].what);
        if (strcmp(got, cases[i].want)) printf("      dio: %s\n", got);
        free(got);
    }
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_respaldos");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");
    DeleteFileW(g_paths.config_file);
    config_load();
    http_init();

    test_escoger();
    test_respaldo();
    test_orden();

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

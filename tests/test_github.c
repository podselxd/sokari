/* Buscar la última versión en GitHub, con un GitHub de mentira en 127.0.0.1:
   por la API cuando deja, y por la página del release cuando la API responde
   403 (su límite de consultas por IP), sin instalar nunca algo sin su huella. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "github.h"
#include "http.h"
#include "util.h"

#define PORT 18781
#define HUELLA "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789ABCDEF"

static int g_fail, g_total;
static SOCKET g_ls = INVALID_SOCKET;

/* Cómo se porta el GitHub de mentira. */
static volatile LONG g_api_status = 200; /* la API: 200 o lo que responda */
static volatile LONG g_con_huella = 1;   /* ¿el release trae Sokari.exe.sha256? */
static volatile LONG g_mala_ruta = 0;    /* /releases/latest redirige a otra cosa (no a un tag) */
static volatile LONG g_pedidos_api = 0;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

static void reply(SOCKET c, int code, const char *extra, const char *body)
{
    char head[512];
    snprintf(head, sizeof head, "HTTP/1.1 %d X\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n", code,
             extra ? extra : "", strlen(body));
    send(c, head, (int)strlen(head), 0);
    send(c, body, (int)strlen(body), 0);
}

static DWORD WINAPI server(LPVOID arg)
{
    for (;;) {
        SOCKET c = accept(g_ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        char buf[4096];
        int n = recv(c, buf, (int)sizeof buf - 1, 0);
        buf[n > 0 ? n : 0] = 0;
        char loc[256];
        if (strstr(buf, "GET /repos/" GITHUB_REPO "/releases/latest ")) {
            InterlockedIncrement(&g_pedidos_api);
            if (g_api_status == 200)
                reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"tag_name\":\"v9.9.9\",\"assets\":[{\"name\":\"Sokari.deb\",\"size\":1},"
                      "{\"name\":\"Sokari.exe\",\"size\":3688960,\"digest\":\"sha256:" HUELLA "\","
                      "\"browser_download_url\":\"https://github.com/x/Sokari.exe\"}]}");
            else
                reply(c, (int)g_api_status, "Content-Type: application/json\r\nX-RateLimit-Remaining: 0\r\n",
                      "{\"message\":\"API rate limit exceeded\"}");
        } else if (strstr(buf, "GET /" GITHUB_REPO "/releases/latest ")) {
            if (g_mala_ruta) /* como si GitHub pidiera iniciar sesión */
                snprintf(loc, sizeof loc, "Location: http://127.0.0.1:%d/login?return_to=releases\r\n", PORT);
            else
                snprintf(loc, sizeof loc, "Location: http://127.0.0.1:%d/" GITHUB_REPO "/releases/tag/v9.9.8\r\n", PORT);
            reply(c, 302, loc, "");
        } else if (strstr(buf, "GET /" GITHUB_REPO "/releases/download/v9.9.8/Sokari.exe.sha256 ") && g_con_huella) {
            reply(c, 200, "Content-Type: application/octet-stream\r\n", HUELLA "  Sokari.exe\n");
        } else {
            reply(c, 404, NULL, "Not Found");
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
    int one = 1;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (bind(g_ls, (struct sockaddr *)&a, sizeof a) || listen(g_ls, 8)) return false;
    HANDLE t = CreateThread(NULL, 0, server, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return t != NULL;
}

static void test_piezas(void)
{
    printf("-- las piezas --\n");
    char *h = gh_parse_sha256(HUELLA "  Sokari.exe\n");
    check(h && strlen(h) == 64 && !strncmp(h, "0123456789abcdef", 16), "la huella de un .sha256, en minúsculas");
    free(h);
    h = gh_parse_sha256("  " HUELLA "\r\n");
    check(h != NULL, "con espacios y un Enter alrededor también");
    free(h);
    check(!gh_parse_sha256("abc123") && !gh_parse_sha256(HUELLA "00") && !gh_parse_sha256("<html>Not Found</html>") &&
              !gh_parse_sha256(NULL),
          "algo que no es una huella de 64 caracteres: nada");
    char *t = gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases/tag/v2.7.2");
    check(t && !strcmp(t, "v2.7.2"), "el tag de la redirección de /releases/latest");
    free(t);
    t = gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases/tag/v2.7.2?x=1");
    check(t && !strcmp(t, "v2.7.2"), "sin lo que venga después del tag");
    free(t);
    t = gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases/tag/v2.7.2/../../malo");
    check(t && !strcmp(t, "v2.7.2"), "de una ruta con «..» solo se toma el tag (la descarga la arma Sokari)");
    free(t);
    check(!gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases/tag/v2.7.2%2F..%2Fmalo") &&
              !gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases") &&
              !gh_tag_from_location("https://github.com/" GITHUB_REPO "/releases/tag/rc-1") && !gh_tag_from_location(NULL),
          "algo que no es un tag de versión no se usa para armar la descarga");
}

static void test_github(void)
{
    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", PORT);
    gh_set_bases(base, base);

    printf("-- la API deja --\n");
    GhRelease r;
    char *error = NULL;
    g_api_status = 200;
    bool ok = gh_latest("Sokari.exe", &r, &error);
    check(ok && !strcmp(r.tag, "v9.9.9") && r.url && r.size == 3688960 && r.sha256 && strlen(r.sha256) == 64 &&
              !r.por_pagina,
          "la versión, el archivo, su tamaño y su huella, de la API");
    gh_release_free(&r);

    printf("-- la API da 403 (límite de consultas de tu red) --\n");
    g_api_status = 403;
    ok = gh_latest("Sokari.exe", &r, &error);
    char want[160];
    snprintf(want, sizeof want, "%s/" GITHUB_REPO "/releases/download/v9.9.8/Sokari.exe", base);
    check(ok && r.por_pagina && !strcmp(r.tag, "v9.9.8") && r.url && !strcmp(r.url, want) && r.sha256 &&
              !strncmp(r.sha256, "0123456789abcdef", 16),
          "por la página del release: la versión, la descarga directa y la huella del .sha256");
    if (!ok) printf("      error: %s\n", error ? error : "?");
    gh_release_free(&r);
    free(error);
    error = NULL;
    g_api_status = 429;
    ok = gh_latest(NULL, &r, &error);
    check(ok && r.por_pagina && !strcmp(r.tag, "v9.9.8") && !r.url, "con 429 igual, y si solo importa la versión, sin bajar nada");
    gh_release_free(&r);

    printf("-- lo que no se instala --\n");
    g_api_status = 403;
    g_con_huella = 0;
    ok = gh_latest("Sokari.exe", &r, &error);
    check(!ok && error && strstr(error, "Sokari.exe.sha256") && strstr(error, "límite"),
          "sin su huella publicada no hay versión (y dice por qué, con lo de la API)");
    if (error) printf("      dice: %s\n", error);
    free(error);
    error = NULL;
    g_con_huella = 1;
    g_mala_ruta = 1;
    ok = gh_latest("Sokari.exe", &r, &error);
    check(!ok && error, "si la página redirige a otra cosa (iniciar sesión), nada");
    free(error);
    error = NULL;
    g_mala_ruta = 0;
    g_api_status = 500;
    LONG antes = g_pedidos_api;
    ok = gh_latest("Sokari.exe", &r, &error);
    check(ok && r.por_pagina && g_pedidos_api == antes + 1, "si la API se cae (500), también la página, con un solo intento a la API");
    gh_release_free(&r);
    gh_set_bases(NULL, NULL);
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_github");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    http_init();
    if (!start_server()) {
        printf("FALLA no pude abrir el GitHub de mentira en el puerto %d\n", PORT);
        return 1;
    }
    test_piezas();
    test_github();
    closesocket(g_ls);
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

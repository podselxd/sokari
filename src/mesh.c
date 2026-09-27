/* Malla entre tus propios dispositivos vía Tailscale: un servidor HTTP mínimo
   que escucha SOLO en la IP de Tailscale de esta PC (nunca en 0.0.0.0), y
   exige el secreto de malla en cada pedido. Lo mismo en Windows y en Linux;
   lo que cambia (dónde está Tailscale, el firewall) está en mesh_win.c y en
   src/linux/mesh_linux.c. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#ifdef _WIN32
#include <winhttp.h>
#else
#define ERROR_WINHTTP_CANNOT_CONNECT 12029 /* el que pone http_linux.c al no poder conectar */
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "config.h"
#include "http.h"
#include "intents.h"
#include "log.h"
#include "memory.h"
#include "mesh.h"
#include "mesh_os.h"
#include "tools.h"
#include "util.h"

#ifdef _WIN32
#define TAILSCALE_NAME "tailscale.exe"
#define INSTALL_TAILSCALE "Instálalo (botón Instalar Tailscale) y entra con tu cuenta."
#else
#define TAILSCALE_NAME "tailscale"
#define INSTALL_TAILSCALE "Instálalo (https://tailscale.com/download/linux) y entra con «sudo tailscale up»."
#endif
#define INSTALL_TAILSCALE_SHORT "instálalo y entra con tu cuenta."
#ifdef _WIN32
#define DETECT_PCS "dale a «Detectar mis PCs»"
#define DIAGNOSE_MESH "Dale a «Revisar la malla»"
#else
#define DETECT_PCS "corre «sokari --detectar-pcs»"
#define DIAGNOSE_MESH "Corre «sokari --revisar-malla»"
#endif
#define MAX_HEADER 16384
#define MAX_BODY 65536

static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread;
static MeshHandler g_handler;
static volatile LONG g_running;
/* Hilo que vigila Tailscale: levanta el servidor en cuanto se conecta (aunque
   haya sido después de abrir Sokari) y lo mueve si cambia de IP. */
static HANDLE g_watch, g_watch_quit;
static SRWLOCK g_ip_lock = SRWLOCK_INIT;
static char *g_listen_ip; /* donde escucha ahora, NULL si no escucha */
/* Cada pedido se atiende en su propio hilo: una orden larga ya no deja
   esperando a «Probar» ni a otra orden. */
#define MAX_CLIENTS 8
static volatile LONG g_clients;
/* Si una orden tarda más que esto, se contesta «recibido» y se sigue. */
static volatile LONG g_ack_ms = 5000;

/* Resuelve host y devuelve su IP de Tailscale ("100.x.y.z"), o NULL si no
   resuelve o si alguna de sus direcciones está fuera de 100.64.0.0/10. */
static char *resolve_tailscale(const char *host)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return NULL;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char *ip = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0) {
        bool all_ok = true;
        for (struct addrinfo *r = res; r; r = r->ai_next) {
            unsigned char *b = (unsigned char *)&((struct sockaddr_in *)r->ai_addr)->sin_addr;
            if (!mesh_is_tailscale_v4(b)) all_ok = false;
            else if (!ip) ip = str_printf("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
        if (!all_ok) {
            free(ip);
            ip = NULL;
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return ip;
}

static void send_all(SOCKET s, const char *data, size_t len)
{
    while (len) {
        /* MSG_NOSIGNAL: en Linux, escribirle a quien ya colgó no tira abajo a Sokari. */
        int n = send(s, data, (int)(len > 65536 ? 65536 : len), MSG_NOSIGNAL);
        if (n <= 0) return;
        data += n;
        len -= (size_t)n;
    }
}

static void respond(SOCKET s, int code, const char *reason, const char *body)
{
    char *msg = str_printf("HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
                           "Connection: close\r\n\r\n%s",
                           code, reason, strlen(body), body);
    send_all(s, msg, strlen(msg));
    free(msg);
}

static const char *find_header(const char *headers, const char *name, size_t *len)
{
    size_t n = strlen(name);
    for (const char *p = headers; p && *p;) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - p) > n && !_strnicmp(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ') v++;
            *len = (size_t)(eol - v);
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

static bool peer_is_mine(const char *ip);

/* ------------------------------------------ quién te ha mandado algo --- */

/* Las PCs que te mandaron algo (ya autorizadas) y cuándo: si luego tú no
   puedes mandarles, se sabe que su salida funciona y falla su entrada. */
typedef struct {
    char ip[16];
    time_t at;
} Inbound;
static Inbound g_inbound[16];
static SRWLOCK g_inbound_lock = SRWLOCK_INIT;

static void remember_inbound(const char *ip)
{
    AcquireSRWLockExclusive(&g_inbound_lock);
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(g_inbound[i].ip, ip)) {
            slot = i;
            break;
        }
        if (g_inbound[i].at < g_inbound[slot].at) slot = i;
    }
    snprintf(g_inbound[slot].ip, sizeof g_inbound[slot].ip, "%s", ip);
    g_inbound[slot].at = time(NULL);
    ReleaseSRWLockExclusive(&g_inbound_lock);
}

static time_t last_inbound(const char *ip)
{
    time_t t = 0;
    AcquireSRWLockShared(&g_inbound_lock);
    for (int i = 0; i < 16; i++)
        if (!strcmp(g_inbound[i].ip, ip)) t = g_inbound[i].at;
    ReleaseSRWLockShared(&g_inbound_lock);
    return t;
}

/* ------------------------------------------------ órdenes que tardan --- */

/* La orden corre en su propio hilo. Si termina antes de g_ack_ms, se
   contesta con lo que dijo Sokari; si no, con «recibido» (202) y la orden
   sigue aquí. state: 0 corriendo, 1 terminó a tiempo, 2 ya se contestó
   «recibido». */
typedef struct {
    char *comando, *origin, *reply;
    HANDLE done;
    volatile LONG state, refs;
} MeshJob;

static void job_release(MeshJob *j)
{
    if (InterlockedDecrement(&j->refs)) return;
    free(j->comando);
    free(j->origin);
    free(j->reply);
    CloseHandle(j->done);
    free(j);
}

static DWORD WINAPI job_thread(LPVOID arg)
{
    MeshJob *j = arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    j->reply = g_handler ? g_handler(j->comando, j->origin) : NULL;
    if (InterlockedCompareExchange(&j->state, 1, 0) == 2) {
        char *shown = j->reply ? xstrndup(j->reply, utf8_truncate_len(j->reply, 300)) : xstrdup("(estaba ocupado)");
        log_msg("Malla: terminé la orden de %s («%s»): %s", j->origin, j->comando, shown);
        free(shown);
    }
    SetEvent(j->done);
    CoUninitialize();
    job_release(j);
    return 0;
}

/* Toma comando (heap). */
static void run_command(SOCKET c, char *comando, const char *origin)
{
    MeshJob *j = xcalloc(1, sizeof *j);
    j->comando = comando;
    j->origin = xstrdup(origin);
    j->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    j->refs = 2;
    HANDLE t = j->done ? CreateThread(NULL, 1 << 20, job_thread, j, 0, NULL) : NULL;
    if (!t) {
        job_release(j); /* el hilo nunca existió: su referencia tampoco */
        j->reply = g_handler ? g_handler(j->comando, j->origin) : NULL;
        j->state = 1;
    } else {
        CloseHandle(t);
        WaitForSingleObject(j->done, (DWORD)InterlockedCompareExchange(&g_ack_ms, 0, 0));
    }
    if (InterlockedCompareExchange(&j->state, 2, 0) == 0) {
        log_msg("Malla: la orden de %s va para largo; le contesto «recibido» y sigo.", origin);
        respond(c, 202, "Accepted", "Recibido: lo estoy haciendo.");
    } else if (j->reply) {
        respond(c, 200, "OK", *j->reply ? j->reply : "Listo.");
    } else {
        respond(c, 503, "Service Unavailable", "Sokari está ocupado ahora, prueba en un momento.");
    }
    job_release(j);
}

void mesh_set_ack_ms(int ms)
{
    InterlockedExchange(&g_ack_ms, ms);
}

static void handle_client(SOCKET c, const char *origin, const char *ip)
{
#ifdef _WIN32
    DWORD timeout = 10000;
#else
    struct timeval timeout = {10, 0};
#endif
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    char *buf = xmalloc(MAX_HEADER + MAX_BODY + 1);
    size_t got = 0;
    char *hdr_end = NULL;
    while (got < MAX_HEADER && !hdr_end) {
        int n = recv(c, buf + got, (int)(MAX_HEADER - got), 0);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    if (!hdr_end) {
        respond(c, 400, "Bad Request", "pedido inválido");
        free(buf);
        return;
    }
    size_t hlen = (size_t)(hdr_end - buf) + 4;
    if (strncmp(buf, "POST /comando ", 14) != 0) {
        respond(c, 404, "Not Found", "no existe");
        free(buf);
        return;
    }
    size_t vlen = 0;
    const char *secret = find_header(buf, "X-Sokari-Secret", &vlen);
    char *expected = config_mesh_secret(false);
    char *given = secret ? xstrndup(secret, vlen) : xstrdup("");
    bool authorized = *expected && secure_equal(given, expected);
    free(given);
    free(expected);
    if (!authorized && peer_is_mine(ip)) {
        authorized = true;
        log_msg("Malla: %s es de tu misma cuenta de Tailscale, así que no hace falta el secreto.", origin);
    }
    if (!authorized) {
        respond(c, 401, "Unauthorized", "secreto invalido");
        free(buf);
        return;
    }
    remember_inbound(ip);
    const char *cl = find_header(buf, "Content-Length", &vlen);
    size_t body_len = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (body_len > MAX_BODY) {
        respond(c, 413, "Payload Too Large", "pedido demasiado grande");
        free(buf);
        return;
    }
    while (got < hlen + body_len) {
        int n = recv(c, buf + got, (int)(hlen + body_len - got), 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[hlen + body_len < got ? hlen + body_len : got] = 0;
    cJSON *j = cJSON_Parse(buf + hlen);
    cJSON *cmd = j ? cJSON_GetObjectItem(j, "comando") : NULL;
    char *comando = cJSON_IsString(cmd) ? str_trim(cmd->valuestring) : xstrdup("");
    cJSON_Delete(j);
    if (!*comando) {
        respond(c, 400, "Bad Request", "falta el comando");
        free(comando);
    } else {
        run_command(c, comando, origin);
    }
    free(buf);
}

typedef struct {
    SOCKET c;
    char ip[16];
} Client;

static DWORD WINAPI client_thread(LPVOID arg)
{
    Client *cl = arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    char *name = mesh_device_name_for_ip(cl->ip);
    handle_client(cl->c, name ? name : cl->ip, cl->ip);
    free(name);
    shutdown(cl->c, SD_BOTH);
    closesocket(cl->c);
    free(cl);
    CoUninitialize();
    InterlockedDecrement(&g_clients);
    return 0;
}

static DWORD WINAPI server_thread(LPVOID arg)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SOCKET listen_socket = (SOCKET)(ULONG_PTR)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        struct sockaddr_in peer = {0};
        socklen_t plen = sizeof peer;
        SOCKET c = accept(listen_socket, (struct sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) {
            if (!InterlockedCompareExchange(&g_running, 1, 1)) break;
            Sleep(200);
            continue;
        }
        Client *cl = xcalloc(1, sizeof *cl);
        cl->c = c;
        unsigned char *b = (unsigned char *)&peer.sin_addr;
        snprintf(cl->ip, sizeof cl->ip, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        HANDLE t = NULL;
        if (InterlockedIncrement(&g_clients) <= MAX_CLIENTS)
            t = CreateThread(NULL, 1 << 20, client_thread, cl, 0, NULL);
        if (t) {
            CloseHandle(t);
        } else {
            InterlockedDecrement(&g_clients);
            respond(c, 503, "Service Unavailable", "Sokari está ocupado ahora, prueba en un momento.");
            shutdown(c, SD_BOTH);
            closesocket(c);
            free(cl);
        }
    }
    CoUninitialize();
    return 0;
}

static bool listen_on(const char *ip)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MESH_PORT);
    inet_pton(AF_INET, ip, &addr.sin_addr);
#ifdef _WIN32
    BOOL excl = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
#else
    /* Que al volver a abrir Sokari no haya que esperar a que se suelten las
       conexiones de antes; otro programa igual no puede usar el mismo puerto. */
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(ls, 8) != 0) {
        closesocket(ls);
        return false;
    }
    g_listen = ls;
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, server_thread, (LPVOID)(ULONG_PTR)ls, 0, NULL);
    AcquireSRWLockExclusive(&g_ip_lock);
    free(g_listen_ip);
    g_listen_ip = xstrdup(ip);
    ReleaseSRWLockExclusive(&g_ip_lock);
    log_msg("Servidor de malla escuchando en %s:%d (solo alcanzable por tu Tailscale).", ip, MESH_PORT);
    return true;
}

static void stop_listening(void)
{
    if (g_listen == INVALID_SOCKET) return;
    InterlockedExchange(&g_running, 0);
#ifndef _WIN32
    shutdown(g_listen, SHUT_RDWR); /* en Linux, cerrarlo no despierta al accept() que espera */
#endif
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    AcquireSRWLockExclusive(&g_ip_lock);
    free(g_listen_ip);
    g_listen_ip = NULL;
    ReleaseSRWLockExclusive(&g_ip_lock);
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool warned = false, fw_checked = false;
    do {
        char *ip = mesh_tailscale_ip();
        AcquireSRWLockShared(&g_ip_lock);
        bool listening = g_listen_ip != NULL, same = listening && ip && !strcmp(ip, g_listen_ip);
        ReleaseSRWLockShared(&g_ip_lock);
        if (listening && !same) {
            log_msg(ip ? "Tailscale cambió de IP: muevo el servidor de malla."
                       : "Tailscale se desconectó: el servidor de malla espera a que vuelva.");
            stop_listening();
            listening = false;
        }
        if (!listening && ip) {
            if (listen_on(ip)) {
                warned = false;
                if (!fw_checked) {
                    fw_checked = true;
                    mesh_firewall_on_listen();
                }
            } else if (!warned) {
                log_msg("No pude levantar el servidor de malla en %s:%d; lo vuelvo a intentar.", ip, MESH_PORT);
                warned = true;
            }
        } else if (!ip && !warned) {
            log_msg("Tailscale no está activo: el servidor de malla arranca solo en cuanto se conecte.");
            warned = true;
        }
        free(ip);
    } while (WaitForSingleObject(g_watch_quit, 15000) == WAIT_TIMEOUT);
    stop_listening();
    CoUninitialize();
    return 0;
}

bool mesh_start(MeshHandler handler)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    char *secret = config_mesh_secret(true); /* que exista desde el principio */
    free(secret);
    g_handler = handler;
    g_watch_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_watch = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    return g_watch != NULL;
}

void mesh_stop(void)
{
    if (!g_watch) return;
    SetEvent(g_watch_quit);
    WaitForSingleObject(g_watch, 5000);
    CloseHandle(g_watch);
    CloseHandle(g_watch_quit);
    g_watch = g_watch_quit = NULL;
}

char *mesh_listening_ip(void)
{
    AcquireSRWLockShared(&g_ip_lock);
    char *ip = g_listen_ip ? xstrdup(g_listen_ip) : NULL;
    ReleaseSRWLockShared(&g_ip_lock);
    return ip;
}

bool mesh_listen_at(const char *ip, MeshHandler handler)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_handler = handler;
    return listen_on(ip);
}

void mesh_listen_stop(void)
{
    stop_listening();
}

/* --------------------------------------------------- probar / mandar --- */

static HttpResponse post_command(const char *ip, const char *comando, int timeout_ms)
{
    char *url = str_printf("http://%s:%d/comando", ip, MESH_PORT);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "comando", comando);
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char *secret = config_mesh_secret(true);
    char *headers = str_printf("Content-Type: application/json\r\nX-Sokari-Secret: %s\r\n", secret);
    SecureZeroMemory(secret, strlen(secret));
    free(secret);
    /* Conectar tarda poco si la otra PC está; esperar su respuesta puede tardar
       (Sokari allá piensa y usa herramientas). */
    HttpRequest req = {.method = "POST", .url = url, .headers = headers, .body = payload, .body_len = strlen(payload),
                       .timeout_ms = timeout_ms, .connect_timeout_ms = 6000};
    HttpResponse resp = http_request(&req);
    SecureZeroMemory(headers, strlen(headers));
    free(headers);
    free(payload);
    free(url);
    return resp;
}

static MeshProbe classify_response(const HttpResponse *r)
{
    if (r->status == 200 || r->status == 202 || r->status == 400) return MESH_OK; /* 400: la orden vacía de la prueba */
    if (r->status == 503) return MESH_BUSY;
    if (r->status == 401) return MESH_BAD_SECRET;
    if (r->status != 0) return MESH_ERROR;
    if (r->error_code == ERROR_WINHTTP_CANNOT_CONNECT) return MESH_NO_SOKARI;
    return MESH_NO_ANSWER;
}

const char *mesh_probe_text(MeshProbe p)
{
    switch (p) {
    case MESH_OK: return "responde y el secreto coincide ✓";
    case MESH_BUSY: return "responde ✓ (ahorita está ocupado con otra cosa)";
    case MESH_BAD_SECRET:
        return "responde, pero no te reconoce: revisa que las dos PCs estén en la misma cuenta de Tailscale (o copia "
               "el secreto de una, con Copiar secreto, en la otra)";
    case MESH_NO_SOKARI:
        return "está en tu red, pero Sokari no le contesta: ábrelo en esa PC (si ya está abierto, espera a que su "
               "Tailscale se conecte)";
    case MESH_NOT_FOUND: return "no la encontré en tu red de Tailscale";
    case MESH_BAD_HOST: return "no tiene una dirección de Tailscale (100.x.y.z o un nombre .ts.net)";
    case MESH_ERROR: return "respondió con un error";
    default:
        return "no contesta: revisa que esté prendida, con Tailscale conectado y sin otra VPN prendida; si todo eso "
               "está bien, en esa PC abre Configuración → Dispositivos, dale «Permitir en el firewall» y luego "
               "«Revisar la malla»";
    }
}

MeshProbe mesh_probe(const char *host)
{
    if (!mesh_host_allowed(host)) return MESH_BAD_HOST;
    char *ip = resolve_tailscale(host);
    if (!ip) return MESH_NOT_FOUND;
    MeshProbe p = mesh_send_ip(ip, "", 8000, NULL);
    free(ip);
    return p;
}

MeshProbe mesh_send_ip(const char *ip, const char *comando, int timeout_ms, char **reply)
{
    HttpResponse r = post_command(ip, comando, timeout_ms);
    MeshProbe p = classify_response(&r);
    if (reply) *reply = (r.status == 200 || r.status == 202) && r.body ? xstrdup(r.body) : NULL;
    http_response_free(&r);
    return p;
}

/* ------------------------------------------------ tailscale y firewall --- */

/* El JSON empieza en la primera llave: antes puede venir una advertencia
   ("Warning: client version ... != tailscaled server version ..."). */
static const char *json_start(const char *s)
{
    return s ? strchr(s, '{') : NULL;
}

bool tailscale_parse_status(const char *out, TailscaleStatus *st)
{
    memset(st, 0, sizeof *st);
    cJSON *j = cJSON_Parse(json_start(out));
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return false;
    }
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(j, "BackendState"));
    st->state = xstrdup(state ? state : "");
    /* La cuenta: Self.UserID apunta a una entrada de User. */
    const cJSON *uid = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "Self"), "UserID");
    const cJSON *u;
    cJSON_ArrayForEach(u, cJSON_GetObjectItem(j, "User"))
    {
        const cJSON *id = cJSON_GetObjectItem(u, "ID");
        const char *login = cJSON_GetStringValue(cJSON_GetObjectItem(u, "LoginName"));
        bool same = cJSON_IsNumber(uid) && cJSON_IsNumber(id) && !(id->valuedouble < uid->valuedouble) &&
                    !(id->valuedouble > uid->valuedouble);
        if (same && login && *login && !st->account) st->account = xstrdup(login);
    }
    st->peers = cJSON_GetArraySize(cJSON_GetObjectItem(j, "Peer"));
    cJSON_Delete(j);
    return true;
}

void tailscale_status_free(TailscaleStatus *st)
{
    free(st->state);
    free(st->account);
    memset(st, 0, sizeof *st);
}

char *tailscale_parse_whois_account(const char *out)
{
    cJSON *j = cJSON_Parse(json_start(out));
    const char *login = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "UserProfile"), "LoginName"));
    char *r = login && *login ? xstrdup(login) : NULL;
    cJSON_Delete(j);
    return r;
}

static SRWLOCK g_account_lock = SRWLOCK_INIT;
static char *g_account;
static uint64_t g_account_at;

/* Tu cuenta de Tailscale en esta PC (heap) o NULL. Se vuelve a leer cada
   10 minutos. */
static char *my_account(void)
{
    uint64_t now = GetTickCount64();
    AcquireSRWLockExclusive(&g_account_lock);
    if (!g_account || now - g_account_at > 10 * 60 * 1000) {
        static const char *const STATUS[] = {"status", "--json", NULL};
        char *out = mesh_run_tailscale(STATUS, 8000, NULL, NULL);
        TailscaleStatus st;
        if (tailscale_parse_status(out, &st) && st.account) {
            free(g_account);
            g_account = xstrdup(st.account);
            g_account_at = now;
        }
        tailscale_status_free(&st);
        free(out);
    }
    char *r = g_account ? xstrdup(g_account) : NULL;
    ReleaseSRWLockExclusive(&g_account_lock);
    return r;
}

/* ¿Esa IP de Tailscale es de un dispositivo de tu misma cuenta? Tailscale ya
   comprobó con criptografía quién manda cada paquete, así que entre tus
   propios dispositivos no hace falta compartir un secreto. Los dispositivos
   con etiqueta (sin dueño) nunca cuentan. */
static bool peer_is_mine(const char *ip)
{
    /* Se recuerda un rato: así no se corre tailscale.exe en cada pedido. */
    static struct {
        char ip[16];
        bool mine;
        uint64_t at;
    } cache[16];
    static SRWLOCK lock = SRWLOCK_INIT;
    uint64_t now = GetTickCount64();
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < 16; i++) {
        if (strcmp(cache[i].ip, ip)) continue;
        uint64_t ttl = cache[i].mine ? 10 * 60 * 1000 : 60 * 1000;
        if (now - cache[i].at < ttl) {
            bool mine = cache[i].mine;
            ReleaseSRWLockShared(&lock);
            return mine;
        }
    }
    ReleaseSRWLockShared(&lock);

    char *me = my_account();
    bool mine = false;
    if (me && strchr(me, '@')) {
        const char *args[] = {"whois", "--json", ip, NULL};
        char *out = mesh_run_tailscale(args, 8000, NULL, NULL);
        char *owner = tailscale_parse_whois_account(out);
        mine = owner && !_stricmp(owner, me);
        free(owner);
        free(out);
    }
    free(me);

    AcquireSRWLockExclusive(&lock);
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(cache[i].ip, ip)) {
            slot = i;
            break;
        }
        if (cache[i].at < cache[slot].at) slot = i;
    }
    snprintf(cache[slot].ip, sizeof cache[slot].ip, "%s", ip);
    cache[slot].mine = mine;
    cache[slot].at = now;
    ReleaseSRWLockExclusive(&lock);
    return mine;
}

/* De "tailscale ping": 1 si contestó (por la red local o por un relevo), 0
   si no llegó, -1 si no se sabe (Tailscale viejo, otro error). */
int tailscale_parse_ping(const char *out)
{
    if (!out) return -1;
    if (strstr(out, "pong from")) return 1;
    char *low = str_lower(out);
    int r = strstr(low, "timeout") || strstr(low, "timed out") || strstr(low, "offline") || strstr(low, "no reply") ? 0 : -1;
    free(low);
    return r;
}

/* De "tailscale debug prefs" (esta PC) o "tailscale whois --json" (otra):
   ¿tiene «Allow incoming connections» apagado (ShieldsUp)? */
bool tailscale_parse_shields_up(const char *out)
{
    cJSON *j = cJSON_Parse(json_start(out));
    const cJSON *v = cJSON_GetObjectItem(j, "ShieldsUp");
    if (!v) v = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "Node"), "Hostinfo"), "ShieldsUp");
    bool up = cJSON_IsTrue(v);
    cJSON_Delete(j);
    return up;
}

static int tailscale_ping(const char *ip)
{
    const char *args[] = {"ping", "--c=1", "--timeout=4s", "--until-direct=false", ip, NULL};
    char *out = mesh_run_tailscale(args, 8000, NULL, NULL);
    int r = tailscale_parse_ping(out);
    free(out);
    return r;
}

/* Por qué no contesta esa PC, con todo lo que se puede averiguar desde aquí. */
static char *explain_no_answer(const char *ip, const char *name)
{
    int reach = tailscale_ping(ip);
    if (reach == 0)
        return str_printf("%s no está conectada a Tailscale ahorita (apagada, dormida o con Tailscale cerrado): "
                          "préndela y abre Tailscale ahí.",
                          name);
    StrBuf sb;
    sb_init(&sb);
    if (reach == 1) sb_appendf(&sb, "Tailscale sí llega a %s, pero Sokari de allá no contesta: el bloqueo está EN %s. ", name, name);
    else sb_appendf(&sb, "%s no contesta. ", name);
    time_t seen = last_inbound(ip);
    if (seen) {
        struct tm tm;
        localtime_s(&tm, &seen);
        sb_appendf(&sb, "(%s sí te mandó algo a las %02d:%02d, así que su salida funciona: lo que falla es su entrada.) ",
                   name, tm.tm_hour, tm.tm_min);
    }
    const char *args[] = {"whois", "--json", ip, NULL};
    char *who = mesh_run_tailscale(args, 8000, NULL, NULL);
    if (tailscale_parse_shields_up(who))
        sb_appendf(&sb, "En %s, Tailscale tiene apagado «Allow incoming connections»: actívalo en su ícono de Tailscale. ",
                   name);
    free(who);
    sb_appendf(&sb, "En %s: abre Sokari (en Dispositivos debe decir «Esta PC recibe órdenes ✓»), dale «Permitir en el "
                    "firewall» y, si tiene otro antivirus con firewall (McAfee, Norton…), permite ahí a Sokari.",
               name);
    return sb.data;
}

char *mesh_probe_report(const char *name, const char *host)
{
    MeshProbe p = mesh_probe(host);
    if (p != MESH_NO_ANSWER) return str_printf("%s: %s.", name, mesh_probe_text(p));
    char *ip = resolve_tailscale(host);
    char *why = ip ? explain_no_answer(ip, name) : str_printf("%s: %s.", name, mesh_probe_text(p));
    free(ip);
    return why;
}

/* "LAPTOP-Ismael" -> "laptop-ismael": el nombre con el que se le habla. */
static char *device_name_from(const char *host_name)
{
    char *low = str_lower(host_name);
    StrBuf sb;
    sb_init(&sb);
    for (const char *p = low; *p; p++) {
        bool ok = isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80;
        if (ok) sb_append_char(&sb, *p);
        else if (sb.len && sb.data[sb.len - 1] != '-') sb_append_char(&sb, '-');
    }
    while (sb.len && sb.data[sb.len - 1] == '-') sb.data[--sb.len] = 0;
    free(low);
    return sb.data;
}

/* Los dispositivos con ese sistema ("windows", "linux") de ese JSON. */
static int parse_peers_os(const char *json, const char *os_want, MeshDevice **out)
{
    cJSON *j = cJSON_Parse(json_start(json));
    cJSON *peers = cJSON_GetObjectItem(j, "Peer");
    int n = 0, cap = cJSON_GetArraySize(peers);
    MeshDevice *list = xcalloc((size_t)cap + 1, sizeof *list);
    cJSON *p;
    cJSON_ArrayForEach(p, peers)
    {
        const char *os = cJSON_GetStringValue(cJSON_GetObjectItem(p, "OS"));
        const char *hn = cJSON_GetStringValue(cJSON_GetObjectItem(p, "HostName"));
        if (!os || !hn || _stricmp(os, os_want)) continue;
        const cJSON *addr;
        const char *ip4 = NULL;
        cJSON_ArrayForEach(addr, cJSON_GetObjectItem(p, "TailscaleIPs"))
        {
            if (!ip4 && cJSON_IsString(addr) && mesh_host_allowed(addr->valuestring)) ip4 = addr->valuestring;
        }
        if (!ip4) continue;
        list[n].name = device_name_from(hn);
        list[n].host = xstrdup(ip4);
        if (!*list[n].name) {
            free(list[n].name);
            free(list[n].host);
            continue;
        }
        n++;
    }
    cJSON_Delete(j);
    *out = list;
    return n;
}

int tailscale_parse_peers(const char *json, MeshDevice **out)
{
    return parse_peers_os(json, "windows", out);
}

/* Por qué Detectar no encontró ninguna PC, en palabras para el usuario. */
static char *peers_problem(const char *out, long code, const char *err)
{
    if (!tailscale_installed()) return xstrdup("No encontré Tailscale en esta PC: " INSTALL_TAILSCALE_SHORT);
    TailscaleStatus st;
    char *r;
    if (!tailscale_parse_status(out, &st)) {
        char *line = err ? xstrndup(err, strcspn(err, "\r\n")) : xstrdup("");
        r = *line ? str_printf("Tailscale no me contestó bien (código %ld: %s).", code, line)
                  : str_printf("Tailscale no me contestó bien (código %ld).", code);
        free(line);
        return r;
    }
    if (strcmp(st.state, "Running"))
        r = str_printf("Tailscale no está conectado en esta PC (estado: %s). Ábrelo y entra con tu cuenta.",
                       *st.state ? st.state : "desconocido");
    else if (!st.peers)
        r = str_printf("Tu cuenta de Tailscale (%s) no ve ningún otro dispositivo: en la otra PC entra a Tailscale con esa "
                       "misma cuenta.",
                       st.account ? st.account : "sin nombre");
    else
        r = str_printf("Tailscale ve %d dispositivo%s, pero ninguna PC con Windows (ni con Linux y Sokari abierto). "
                       DIAGNOSE_MESH " para ver cuáles son.",
                       st.peers, st.peers == 1 ? "" : "s");
    tailscale_status_free(&st);
    return r;
}

int tailscale_windows_peers(MeshDevice **out, char **why)
{
    long code;
    char *err;
    static const char *const STATUS[] = {"status", "--json", NULL};
    char *json = mesh_run_tailscale(STATUS, 8000, &code, &err);
    int n = tailscale_parse_peers(json, out);
    /* Las de Linux, solo si Sokari ya les contesta: un servidor o una
       Raspberry con Linux también salen en Tailscale y no son PCs donde se le
       hable a Sokari. */
    MeshDevice *lin;
    int nl = parse_peers_os(json, "linux", &lin);
    for (int i = 0; i < nl; i++) {
        MeshProbe p = mesh_send_ip(lin[i].host, "", 2500, NULL);
        if (p == MESH_OK || p == MESH_BUSY || p == MESH_BAD_SECRET) {
            *out = xrealloc(*out, sizeof **out * (size_t)(n + 2));
            (*out)[n++] = lin[i];
            memset(&(*out)[n], 0, sizeof **out);
        } else {
            free(lin[i].name);
            free(lin[i].host);
        }
    }
    free(lin);
    if (why) *why = n ? NULL : peers_problem(json, code, err);
    free(json);
    free(err);
    return n;
}

/* ------------------------------------------------------ revisar malla --- */

/* Lo que ve tu red de Tailscale: "laptop (windows, en línea), pixel (android,
   desconectado)". Devuelve cuántos son. */
static int describe_peers(const char *out, StrBuf *seen)
{
    cJSON *j = cJSON_Parse(json_start(out));
    const cJSON *p;
    int n = 0;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(j, "Peer"))
    {
        const char *hn = cJSON_GetStringValue(cJSON_GetObjectItem(p, "HostName"));
        const char *os = cJSON_GetStringValue(cJSON_GetObjectItem(p, "OS"));
        bool online = cJSON_IsTrue(cJSON_GetObjectItem(p, "Online"));
        sb_appendf(seen, "%s%s (%s, %s)", n ? ", " : "", hn ? hn : "?", os && *os ? os : "?",
                   online ? "en línea" : "desconectado");
        n++;
    }
    cJSON_Delete(j);
    return n;
}

char *mesh_diagnose(void)
{
    StrBuf r;
    sb_init(&r);
    sb_appendf(&r, "Revisión de la malla (Sokari %s)\n\n", SOKARI_VERSION);

    char *exe8 = mesh_tailscale_path();
    if (exe8) sb_appendf(&r, "✓ Tailscale instalado: %s\n", exe8);
    else sb_append(&r, "✗ No encontré Tailscale en esta PC. " INSTALL_TAILSCALE "\n");

    long code = 0;
    char *err = NULL;
    static const char *const STATUS[] = {"status", "--json", NULL};
    char *out = exe8 ? mesh_run_tailscale(STATUS, 8000, &code, &err) : NULL;
    free(exe8);
    TailscaleStatus st;
    bool parsed = tailscale_parse_status(out, &st);
    if (out && !parsed) {
        char *first = err ? xstrndup(err, strcspn(err, "\r\n")) : xstrdup("");
        sb_appendf(&r, "✗ Tailscale no contestó bien (código %ld)%s%s\n", code, *first ? ": " : "", first);
        free(first);
    } else if (!out && tailscale_installed()) {
        sb_append(&r, "✗ No pude correr " TAILSCALE_NAME "\n");
    } else if (parsed && strcmp(st.state, "Running")) {
        sb_appendf(&r, "✗ Tailscale no está conectado (estado: %s). Ábrelo y entra con tu cuenta.\n",
                   *st.state ? st.state : "desconocido");
    } else if (parsed) {
        sb_appendf(&r, "✓ Tailscale conectado con la cuenta %s\n", st.account ? st.account : "(sin nombre)");
    }
    free(err);

    char *ip = mesh_tailscale_ip(), *listening = mesh_listening_ip();
    if (ip) sb_appendf(&r, "✓ IP de esta PC en Tailscale: %s\n", ip);
    else sb_append(&r, "✗ No encontré la IP de Tailscale de esta PC (¿está conectado?)\n");
    if (listening) {
        sb_appendf(&r, "✓ Esta PC recibe órdenes en %s:%d\n", listening, MESH_PORT);
        MeshProbe p = mesh_send_ip(listening, "", 5000, NULL);
        if (p == MESH_OK) sb_append(&r, "✓ Prueba local: el servidor de esta PC contesta\n");
        else sb_appendf(&r, "✗ Prueba local: el servidor de esta PC %s\n", mesh_probe_text(p));
    } else {
        sb_append(&r, "✗ Esta PC todavía no recibe órdenes: se activa sola unos segundos después de que Tailscale se "
                      "conecta.\n");
    }
    free(ip);
    free(listening);

    mesh_diagnose_firewall(&r);
    if (parsed && !strcmp(st.state, "Running")) {
        static const char *const PREFS[] = {"debug", "prefs", NULL};
        char *prefs = mesh_run_tailscale(PREFS, 8000, NULL, NULL);
        if (prefs && json_start(prefs)) {
            if (tailscale_parse_shields_up(prefs))
                sb_append(&r, "✗ Tailscale tiene apagado «Allow incoming connections»: actívalo en su ícono (si no, "
                              "nadie te puede mandar órdenes)\n");
            else
                sb_append(&r, "✓ Tailscale acepta conexiones entrantes\n");
        }
        free(prefs);
    }

    if (parsed) {
        StrBuf seen;
        sb_init(&seen);
        int n = describe_peers(out, &seen);
        if (n) sb_appendf(&r, "✓ Tu red de Tailscale ve %d dispositivo%s: %s\n", n, n == 1 ? "" : "s", seen.data);
        else
            sb_appendf(&r, "✗ Tu red de Tailscale no ve ningún otro dispositivo: en la otra PC entra a Tailscale con %s\n",
                       st.account ? st.account : "la misma cuenta");
        sb_free(&seen);
    }
    tailscale_status_free(&st);
    free(out);

    MeshDevice *devs;
    int nd = mesh_devices(&devs);
    if (!nd) sb_append(&r, "· Todavía no tienes PCs registradas: " DETECT_PCS ".\n");
    for (int i = 0; i < nd; i++) {
        MeshProbe p = mesh_probe(devs[i].host);
        char *ip = p == MESH_NO_ANSWER ? resolve_tailscale(devs[i].host) : NULL;
        char *why = ip ? explain_no_answer(ip, devs[i].name) : xstrdup(mesh_probe_text(p));
        sb_appendf(&r, "%s %s (%s): %s\n", p == MESH_OK || p == MESH_BUSY ? "✓" : "✗", devs[i].name, devs[i].host, why);
        free(why);
        free(ip);
    }
    mesh_devices_free(devs, nd);
    return r.data;
}

char *tool_registrar_dispositivo(const cJSON *a)
{
    char *nombre = str_lower(arg_str(a, "nombre"));
    char *n = str_trim(nombre);
    char *host = str_trim(arg_str(a, "host"));
    free(nombre);
    char *r;
    if (!*n || !*host)
        r = xstrdup("Necesito un nombre y la dirección de Tailscale de ese dispositivo.");
    else if (!mesh_device_set(n, host))
        r = xstrdup("Solo registro direcciones de Tailscale: una IP entre 100.64.0.0 y 100.127.255.255, o un "
                    "nombre que termine en .ts.net.");
    else
        r = str_printf("Listo, registré '%s' en %s.", n, host);
    free(n);
    free(host);
    return r;
}

char *tool_gestionar_dispositivo(const cJSON *a)
{
    char *nombre = str_lower(arg_str(a, "nombre"));
    char *n = str_trim(nombre);
    free(nombre);
    const char *comando = arg_str(a, "comando");
    cJSON *devs = mesh_devices_load();
    cJSON *host = cJSON_GetObjectItem(devs, n);
    if (!cJSON_IsString(host)) {
        /* "Chloe", "mi laptop", "la otra compu": el que es, si no hay duda. */
        char *real = mesh_resolve_device(n);
        if (real && *real) {
            log_msg("Malla: «%s» es «%s».", n, real);
            free(n);
            n = real;
            host = cJSON_GetObjectItem(devs, n);
        } else {
            free(real);
        }
    }
    char *r;
    if (!cJSON_IsString(host)) {
        StrBuf known;
        sb_init(&known);
        cJSON *d;
        cJSON_ArrayForEach(d, devs) sb_appendf(&known, "%s%s", known.len ? ", " : "", d->string);
        r = str_printf("No tengo registrado un dispositivo llamado '%s'. Los que conozco: %s.", n,
                       known.len ? known.data : "ninguno todavía");
        sb_free(&known);
    } else if (str_is_blank(comando)) {
        r = xstrdup("No me dijiste qué comando mandarle.");
    } else if (!mesh_host_allowed(host->valuestring)) {
        r = str_printf("'%s' no tiene una dirección de Tailscale (%s), así que no le mando nada. Vuelve a registrarlo "
                       "con su IP 100.x.y.z o su nombre .ts.net.",
                       n, host->valuestring);
    } else {
        /* Se conecta a la IP ya revisada y no al nombre: si el DNS cambiara de
           respuesta entre la revisión y la conexión, igual queda en Tailscale. */
        char *ip = resolve_tailscale(host->valuestring);
        if (!ip) {
            r = str_printf("No pude encontrar a '%s' en tu red de Tailscale (¿está prendido y conectado?).", n);
        } else {
            HttpResponse resp = post_command(ip, comando, 90000);
            MeshProbe p = classify_response(&resp);
            if (resp.status == 200) r = str_printf("%s: %s", n, *resp.body ? resp.body : "Listo.");
            else if (resp.status == 202) r = str_printf("'%s' recibió la orden y la está haciendo.", n);
            else if (p == MESH_BUSY) r = str_printf("'%s' está ocupado ahorita; prueba en un momento.", n);
            else if (p == MESH_ERROR) r = str_printf("'%s' respondió con un error (%d).", n, resp.status);
            else if (p == MESH_NO_ANSWER) {
                char *why = explain_no_answer(ip, n);
                r = str_printf("No le llegó la orden a '%s'. %s", n, why);
                free(why);
            } else r = str_printf("No le llegó la orden a '%s': %s.", n, mesh_probe_text(p));
            http_response_free(&resp);
            free(ip);
        }
    }
    cJSON_Delete(devs);
    free(n);
    return r;
}

char *mesh_detect_devices(void)
{
    MeshDevice *peers, *known;
    char *why = NULL;
    int np = tailscale_windows_peers(&peers, &why), nk = mesh_devices(&known);
    StrBuf added;
    sb_init(&added);
    for (int i = 0; i < np; i++) {
        bool have = false;
        for (int k = 0; k < nk && !have; k++)
            have = !strcmp(known[k].host, peers[i].host) || !strcmp(known[k].name, peers[i].name);
        if (!have && mesh_device_set(peers[i].name, peers[i].host))
            sb_appendf(&added, "%s%s", added.len ? ", " : "", peers[i].name);
    }
    char *msg = np == 0 ? str_printf("No encontré otras PCs con Windows. %s", why ? why : "")
                : added.len ? str_printf("Agregué: %s. Dale a Probar para ver si contesta.", added.data)
                            : str_printf("Ya tenías registradas tus PCs de Tailscale (%d).", np);
    free(why);
    sb_free(&added);
    mesh_devices_free(peers, np);
    mesh_devices_free(known, nk);
    return msg;
}

/* Las skills locales (0 tokens), con frases como las dices: la hora y la
   fecha, temporizadores y cronómetro, alarmas y recordatorios, cuentas y
   conversiones, el clima (con un Open-Meteo falso en 127.0.0.1), tus notas,
   cómo va la PC y la plática. Y lo que NO es para ellas, que tiene que irse a
   la IA. Todo con la hora fija (sábado 26 de septiembre de 2026, 3:20 de la
   tarde) y en una carpeta temporal: nunca toca tu configuración ni tu memoria. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "http.h"
#include "log.h"
#include "memory.h"
#include "skills.h"
#include "util.h"

static int g_fail, g_total;
static time_t g_start;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

static char *ask(const char *text)
{
    const SkillInfo *which;
    bool end;
    return skills_try(text, &which, &end);
}

/* Lo dice exactamente así. */
static void says(const char *text, const char *want)
{
    char *got = ask(text);
    char what[400];
    snprintf(what, sizeof what, "«%s» → %s", text, want);
    bool ok = got && !strcmp(got, want);
    check(ok, what);
    if (!ok) printf("      dijo: %s\n", got ? got : "(se fue a la IA)");
    free(got);
}

/* Contesta algo que empieza así o lo trae. */
static void says_part(const char *text, const char *part)
{
    char *got = ask(text);
    char what[400];
    snprintf(what, sizeof what, "«%s» → «…%s…»", text, part);
    bool ok = got && strstr(got, part);
    check(ok, what);
    if (!ok) printf("      dijo: %s\n", got ? got : "(se fue a la IA)");
    free(got);
}

/* No es para las skills: se va a la IA. */
static void goes_to_ai(const char *text)
{
    char *got = ask(text);
    char what[400];
    snprintf(what, sizeof what, "«%s» → a la IA", text);
    check(!got, what);
    if (got) printf("      dijo: %s\n", got);
    free(got);
}

static void at(long offset)
{
    skills_set_clock(g_start + offset);
}

/* ---------------------------------------------- un Open-Meteo de mentira --- */

#define PORT 18779
static SOCKET g_ls = INVALID_SOCKET;

static void reply(SOCKET c, int code, const char *body)
{
    char head[256];
    snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
             code, strlen(body));
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
        if (strstr(buf, "GET /geo?")) {
            const char *city = strstr(buf, "name=Chihuahua") ? "Chihuahua" : strstr(buf, "name=Monterrey") ? "Monterrey"
                                                                                                           : NULL;
            char body[256];
            if (city)
                snprintf(body, sizeof body,
                         "{\"results\":[{\"name\":\"%s\",\"latitude\":28.63,\"longitude\":-106.08,\"country_code\":\"MX\"}]}",
                         city);
            else
                snprintf(body, sizeof body, "{\"generationtime_ms\":0.5}");
            reply(c, 200, body);
        } else if (strstr(buf, "GET /forecast?")) {
            reply(c, 200,
                  "{\"current\":{\"temperature_2m\":24.3,\"apparent_temperature\":23.1,\"weather_code\":0},"
                  "\"daily\":{\"time\":[\"2026-09-26\",\"2026-09-27\",\"2026-09-28\"],\"weather_code\":[0,61,3],"
                  "\"temperature_2m_max\":[30.2,27.8,26],\"temperature_2m_min\":[15.4,14.1,13],"
                  "\"precipitation_probability_max\":[10,70,35]}}");
        } else {
            reply(c, 404, "{}");
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

/* ------------------------------------------------------------- pruebas --- */

static void test_time(void)
{
    printf("-- hora y fecha --\n");
    says("¿Qué hora es?", "Son las 3 y 20 de la tarde.");
    says("Oye Sokari, ¿me dices la hora por favor?", "Son las 3 y 20 de la tarde.");
    says("¿Qué día es hoy?", "Hoy es sábado 26 de septiembre de 2026.");
    says("¿Qué día es mañana?", "Mañana es domingo 27 de septiembre de 2026.");
    says("¿Cuánto falta para Navidad?", "Faltan 90 días para Navidad.");
    says("¿Cuánto falta para el 1 de octubre?", "Faltan 5 días para el 1 de octubre.");
    says("¿Cuánto falta para el viernes?", "Faltan 6 días para el viernes.");
    says("¿Cuánto falta para las 5?", "Falta 1 hora y 40 minutos para las 5 de la tarde.");
    goes_to_ai("¿A qué hora abre el banco?");
    goes_to_ai("¿Qué hora es en Japón?");
    goes_to_ai("¿Qué día es el partido?");
}

static void test_timers(void)
{
    printf("-- temporizadores y cronómetro --\n");
    says("Pon un temporizador de 5 minutos", "Listo, temporizador de 5 minutos.");
    says("¿Cuánto le falta al temporizador?", "Le quedan 5 minutos.");
    at(120);
    says("¿Cuánto le queda al temporizador?", "Le quedan 3 minutos.");
    at(301);
    int n;
    char **due = skills_due_timers(&n);
    check(n == 1 && !strcmp(due[0], "¡Se acabó el temporizador de 5 minutos!"),
          "a los 5 minutos suena: «¡Se acabó el temporizador de 5 minutos!»");
    for (int i = 0; i < n; i++) free(due[i]);
    free(due);
    due = skills_due_timers(&n);
    check(n == 0, "y suena una sola vez");
    free(due);
    at(0);
    says("Avísame en 10 segundos", "Listo, temporizador de 10 segundos.");
    says("Cancela el temporizador", "Listo, cancelé el temporizador.");
    says("Cancela el temporizador", "No tienes temporizadores.");
    says("Pon un temporizador de una hora y media", "Listo, temporizador de 1 hora y 30 minutos.");
    says("Quita el temporizador", "Listo, cancelé el temporizador.");
    says("Inicia el cronómetro", "Cronómetro corriendo.");
    at(75);
    says("¿Cuánto lleva el cronómetro?", "Lleva 1 minuto y 15 segundos.");
    says("Detén el cronómetro", "Cronómetro detenido en 1 minuto y 15 segundos.");
    at(0);
    goes_to_ai("No pongas un temporizador");
}

static int reminders_saved(void)
{
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    int n = cJSON_GetArraySize(cJSON_GetObjectItem(data, current_speaker()));
    cJSON_Delete(data);
    free(rf);
    return n;
}

static void test_alarms(void)
{
    printf("-- alarmas y recordatorios --\n");
    says("Despiértame a las 7", "Listo, alarma para mañana a las 7 de la mañana.");
    says("Pon una alarma a las 10 de la noche", "Listo, alarma para hoy a las 10 de la noche.");
    says("¿Qué alarmas tengo?", "Tienes alarmas: hoy a las 10 de la noche y mañana a las 7 de la mañana.");
    says("Cancela las alarmas", "Listo, quité tus alarmas.");
    says("¿Qué alarmas tengo?", "No tienes alarmas puestas.");
    says("Recuérdame sacar la ropa en 20 minutos", "Listo, en 20 minutos te recuerdo: sacar la ropa.");
    says("Recuérdame mañana a las 9 pagar la luz", "Listo, mañana a las 9 de la mañana te recuerdo: pagar la luz.");
    says("Recuérdame el viernes que tengo examen", "Listo, el viernes a las 9 de la mañana te recuerdo: tengo examen.");
    check(reminders_saved() == 3, "los 3 recordatorios quedan guardados (como los que crea la IA)");
    goes_to_ai("Recuérdame algo importante");
    goes_to_ai("Avísame si llueve mañana");
    goes_to_ai("¿Cómo configuro una alarma en mi celular?");
}

static void test_calc(void)
{
    printf("-- cuentas y conversiones --\n");
    says("¿Cuánto es 25 por 4?", "25 por 4 da 100.");
    says("Cuánto es dos punto cinco por cuatro", "2.5 por 4 da 10.");
    says("¿Cuánto es 10 entre 4?", "10 entre 4 da 2.5.");
    says("Raíz cuadrada de 144 más 1", "La raíz cuadrada de 144 más 1 da 13.");
    says("¿Cuánto es el 15 por ciento de 300?", "15 por ciento de 300 da 45.");
    says("2 elevado a la 10", "2 elevado a 10 da 1024.");
    says("¿Cuántas libras son 70 kilos?", "70 kilos son 154.32 libras.");
    says("Convierte 100 grados Fahrenheit a centígrados", "100 grados Fahrenheit son 37.78 grados centígrados.");
    says("5 millas en kilómetros", "5 millas son 8.05 kilómetros.");
    goes_to_ai("¿Cuánto cuesta un iPhone?");
    goes_to_ai("¿Cuántos años tiene Messi?");
    goes_to_ai("Pon 5 canciones de Queen");
    goes_to_ai("5 cosas que hacer en Chihuahua");
    goes_to_ai("¿Cuánto es 5 dólares en pesos?");
}

static void test_weather(void)
{
    printf("-- clima (Open-Meteo de mentira) --\n");
    if (!start_server()) {
        check(false, "el servidor falso escucha en 127.0.0.1");
        return;
    }
    char geo[64], fc[64];
    snprintf(geo, sizeof geo, "http://127.0.0.1:%d/geo", PORT);
    snprintf(fc, sizeof fc, "http://127.0.0.1:%d/forecast", PORT);
    skills_set_weather_urls(geo, fc);
    says("¿Cómo está el clima?", "¿De qué ciudad? Dime «mi ciudad es…» una vez y la recuerdo.");
    says("Mi ciudad es Chihuahua", "Listo, tu ciudad es Chihuahua. Ya te puedo decir el clima.");
    says("¿Cómo está el clima?",
         "En Chihuahua hay 24 grados y está despejado. Hoy: máxima de 30 y mínima de 15, con 10% de probabilidad de "
         "lluvia.");
    says("¿Va a llover mañana?",
         "Mañana en Chihuahua hay 70% de probabilidad de lluvia: sí va a llover, mejor lleva paraguas.");
    says_part("Clima en Monterrey", "En Monterrey hay 24 grados");
    says_part("¿Cómo está el clima en Narnia?", "No pude encontrar Narnia");
    goes_to_ai("¿Cuánto tiempo tarda un avión a Cancún?");
    goes_to_ai("Busca el clima en internet");
    goes_to_ai("¿Cuál es la temperatura de ebullición del agua?");
    closesocket(g_ls);
}

static void test_notes(void)
{
    printf("-- notas y pendientes --\n");
    says("Anota comprar leche", "Anotado: comprar leche.");
    says("Agrega pagar la luz a mis pendientes", "Anotado: pagar la luz.");
    says("¿Qué tengo pendiente?", "Tienes 2 pendientes: comprar leche y pagar la luz.");
    says("Tacha comprar leche", "Listo, taché «comprar leche».");
    says("¿Qué tengo pendiente?", "Tienes un pendiente: pagar la luz.");
    says("Anota esto: comprar pan", "Anotado: comprar pan.");
    says("Quita comprar pan de la lista", "Listo, taché «comprar pan».");
    goes_to_ai("Ya hice la tarea");
    goes_to_ai("Anota la lista del súper en un archivo de Word");
}

static void test_pc(void)
{
    printf("-- cómo va la PC --\n");
    char *r = ask("¿Cómo va la compu?");
    check(r && (strstr(r, "procesador") || strstr(r, "RAM") || strstr(r, "disco") || strstr(r, "No pude")),
          "«¿Cómo va la compu?»: procesador, RAM y disco");
    if (r) printf("      dijo: %s\n", r);
    free(r);
    says_part("¿Cuánta batería tengo?", "batería");
    goes_to_ai("¿Cuánto dura la batería de un iPhone?");
}

static void test_chat(void)
{
    printf("-- plática --\n");
    says("Hola", "¡Buenas tardes! ¿En qué te ayudo?");
    says("Buenas, Sokari", "¡Buenas tardes! ¿En qué te ayudo?");
    says_part("¿Cómo estás?", "¿");
    says_part("Cuéntame un chiste", "¿");
    says_part("¿Qué puedes hacer?", "Sin gastar nada de IA");
    goes_to_ai("¿Qué es la fotosíntesis?");
    goes_to_ai("Hola, ¿me ayudas con la tarea de historia?");
    goes_to_ai("Escribe un poema sobre el mar");
}

static void test_off(void)
{
    printf("-- una skill apagada --\n");
    AppConfig c = config_snapshot();
    free(c.skills_off);
    c.skills_off = xstrdup("hora,clima");
    config_apply(&c);
    config_free(&c);
    goes_to_ai("¿Qué hora es?");
    c = config_snapshot();
    free(c.skills_off);
    c.skills_off = xstrdup("");
    config_apply(&c);
    config_free(&c);
    says("¿Qué hora es?", "Son las 3 y 20 de la tarde.");
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_skills");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.memory_dir);
    g_paths.memory_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");
    const wchar_t *files[] = {L"config.env", L"recordatorios.json", L"notas.json"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        wchar_t *f = path_join(dir, files[i]);
        DeleteFileW(f);
        free(f);
    }
    config_load();
    http_init();

    struct tm t = {0};
    t.tm_year = 2026 - 1900, t.tm_mon = 8, t.tm_mday = 26, t.tm_hour = 15, t.tm_min = 20, t.tm_isdst = -1;
    g_start = mktime(&t);
    at(0);

    test_time();
    test_timers();
    test_alarms();
    test_calc();
    test_weather();
    test_notes();
    test_pc();
    test_chat();
    test_off();

    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        wchar_t *f = path_join(dir, files[i]);
        DeleteFileW(f);
        free(f);
    }
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

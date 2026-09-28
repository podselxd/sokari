/* crear_archivo: crea texto donde le digas (solo el nombre: el Escritorio),
   no pisa lo que ya existe (agrega al final si se lo pides) y nunca crea algo
   que se pueda ejecutar ni escribe en las carpetas de Sokari. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/cJSON.h"
#include "config.h"
#include "gustos.h"
#include "radio.h"
#include "lector.h"
#include "tools.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static char g_playing[256];
static bool fake_play(const char *url, char **why)
{
    (void)why;
    snprintf(g_playing, sizeof g_playing, "%s", url);
    return true;
}
static void fake_stop(void)
{
    g_playing[0] = 0;
}

static char *radio(const char *json)
{
    cJSON *a = cJSON_Parse(json);
    char *r = tool_radio(a);
    cJSON_Delete(a);
    printf("      %s\n", r);
    return r;
}

static char *crear(const char *ruta, const char *contenido, bool agregar)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "ruta", ruta);
    cJSON_AddStringToObject(a, "contenido", contenido);
    if (agregar) cJSON_AddBoolToObject(a, "agregar", true);
    char *r = tool_crear_archivo(a);
    cJSON_Delete(a);
    printf("      %s\n", r);
    return r;
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    wchar_t *dir = exe_dir(), *sub = path_join(dir, L"prueba_crear_archivo");
    ensure_dir(sub);
    wchar_t *file = path_join(sub, L"lista del súper.txt");
    DeleteFileW(file);
    char *u = wide_to_utf8(file);

    char *r = crear(u, "leche\nhuevos ☕", false);
    size_t n = 0;
    char *got = read_file_all(file, &n);
    check(got && !strcmp(got, "leche\nhuevos ☕") && strstr(r, "creé"), "crea el archivo con el texto tal cual");
    free(got), free(r);

    r = crear(u, "otra cosa", false);
    got = read_file_all(file, &n);
    check(strstr(r, "no lo piso") && got && !strcmp(got, "leche\nhuevos ☕"), "no pisa uno que ya existe");
    free(got), free(r);

    r = crear(u, "pan", true);
    got = read_file_all(file, &n);
    check(got && !strcmp(got, "leche\nhuevos ☕\npan"), "con agregar: lo pone al final, en otro renglón");
    free(got), free(r);

    wchar_t *exe = path_join(sub, L"virus.exe"), *bat = path_join(sub, L"x.BAT");
    char *ue = wide_to_utf8(exe), *ub = wide_to_utf8(bat);
    r = crear(ue, "MZ", false);
    char *r2 = crear(ub, "del *", false);
    check(!file_exists(exe) && !file_exists(bat) && strstr(r, "texto") && strstr(r2, "texto"),
          "nunca crea algo que se pueda ejecutar (.exe, .bat…)");
    free(r), free(r2), free(ue), free(ub), free(exe), free(bat);

    wchar_t *mine = path_join(g_paths.local_dir, L"colado.txt");
    char *um = wide_to_utf8(mine);
    r = crear(um, "hola", false);
    check(!file_exists(mine) && strstr(r, "seguridad"), "no escribe en las carpetas de Sokari");
    free(r), free(um), free(mine);

    r = crear("", "hola", false);
    check(strstr(r, "cómo se llama") != NULL, "sin nombre, lo pregunta");
    free(r);

    /* Sus gustos: se anotan, se le recuerdan y puede cambiar de opinión. */
    wchar_t *gp = path_join(g_paths.memory_dir, L"gustos.json");
    DeleteFileW(gp);
    char *g0 = gustos_resumen();
    cJSON *a = cJSON_Parse("{\"cosa\":\"el rock de los 80\",\"nivel\":\"encanta\"}");
    free(tool_anotar_gusto(a));
    cJSON_Delete(a);
    a = cJSON_Parse("{\"cosa\":\"que le griten\",\"nivel\":\"desagrada\"}");
    free(tool_anotar_gusto(a));
    cJSON_Delete(a);
    char *g1 = gustos_resumen();
    check(!g0 && g1 && strstr(g1, "te encanta: el rock de los 80") && strstr(g1, "te desagrada: que le griten"),
          "sus gustos se guardan y se le recuerdan en cada pedido");
    a = cJSON_Parse("{\"cosa\":\"el rock de los 80\",\"nivel\":\"gusta\"}");
    free(tool_anotar_gusto(a));
    cJSON_Delete(a);
    char *g2 = gustos_resumen();
    check(g2 && strstr(g2, "te gusta: el rock de los 80") && !strstr(g2, "te encanta"), "y puede cambiar de opinión");
    free(g1), free(g2);
    DeleteFileW(gp);
    free(gp);

    /* La radio, sin red ni bocinas: busca, pone, siguiente, favoritos y playlists. */
    wchar_t *mp = path_join(g_paths.memory_dir, L"musica.json");
    DeleteFileW(mp);
    radio_set_player(fake_play, fake_stop);
    radio_set_search_override("[{\"name\":\"Rock FM\",\"url_resolved\":\"http://a/rock\"},"
                              "{\"name\":\"Rock 101\",\"url_resolved\":\"http://b/rock\"},"
                              "{\"name\":\"Sin url\",\"url_resolved\":\"\"}]");
    r = radio("{\"accion\":\"poner\",\"busqueda\":\"rock\"}");
    check(!strcmp(g_playing, "http://a/rock") && strstr(r, "Rock FM"), "«pon radio de rock»: busca y pone la primera");
    free(r);
    r = radio("{\"accion\":\"siguiente\"}");
    check(!strcmp(g_playing, "http://b/rock"), "«la siguiente» pasa a la otra estación");
    free(r);
    free(radio("{\"accion\":\"guardar_favorito\"}"));
    free(radio("{\"accion\":\"agregar_a_playlist\",\"nombre\":\"estudiar\"}"));
    r = radio("{\"accion\":\"parar\"}");
    check(!g_playing[0], "«quita la radio» la para");
    free(r);
    radio_set_search_override("[]");
    r = radio("{\"accion\":\"poner\",\"busqueda\":\"101\"}");
    check(!strcmp(g_playing, "http://b/rock"), "un favorito se pone sin buscar en internet");
    free(r);
    free(radio("{\"accion\":\"parar\"}"));
    r = radio("{\"accion\":\"poner_playlist\",\"nombre\":\"Estudiar\"}");
    check(!strcmp(g_playing, "http://b/rock"), "y se pone tu playlist");
    free(r);
    r = radio("{\"accion\":\"listas\"}");
    check(strstr(r, "Favoritos: Rock 101") && strstr(r, "estudiar") && strstr(r, "Rock FM"),
          "dice tus favoritos, playlists e historial");
    free(r);
    r = radio("{\"accion\":\"poner\",\"busqueda\":\"polka tibetana\"}");
    check(strstr(r, "No encontré") != NULL, "sin estaciones, lo dice");
    free(r);
    radio_set_search_override(NULL);
    radio_set_player(NULL, NULL);
    DeleteFileW(mp);
    free(mp);

    /* El lector: un archivo de texto se lee tal cual; lo que no es texto, no. */
    wchar_t *txt = path_join(sub, L"cuento.txt"), *bin = path_join(sub, L"foto.dat");
    write_file_atomic(txt, "Había una vez una esfera morada.", strlen("Había una vez una esfera morada."));
    write_file_atomic(bin, "\x89PNG\0\0\x01", 7);
    char *ut = wide_to_utf8(txt), *ub2 = wide_to_utf8(bin);
    cJSON *la = cJSON_CreateObject();
    cJSON_AddStringToObject(la, "fuente", "archivo");
    cJSON_AddStringToObject(la, "ruta", ut);
    char *said = tool_leer_en_voz(la);
    check(said && !strcmp(said, "Había una vez una esfera morada."), "«léeme este archivo»: lo lee tal cual");
    free(said);
    cJSON_ReplaceItemInObject(la, "ruta", cJSON_CreateString(ub2));
    said = tool_leer_en_voz(la);
    check(said && strstr(said, "no es de texto"), "lo que no es texto no lo lee");
    free(said);
    cJSON_Delete(la);
    DeleteFileW(txt), DeleteFileW(bin);
    free(txt), free(bin), free(ut), free(ub2);

    DeleteFileW(file);
    RemoveDirectoryW(sub);
    free(u), free(file), free(sub), free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

/* Cómo arranca Sokari (Inicio, directo, primera vez), lo que se escribe en la
   clave Run, que los ajustes (modos de pantalla, salida de audio, tamaño de la
   ventana) se guardan y se leen bien, y qué exe baja el
   actualizador. Todo en carpetas temporales: nunca toca tu
   configuración, tu memoria ni el registro. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autostart.h"
#include "config.h"
#include "memory.h"
#include "third_party/cJSON.h"
#include "tools.h"
#include "update.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static void test_launch(void)
{
    printf("-- cómo arranca --\n");
    check(launch_kind(false, false, false) == LAUNCH_FIRST_RUN, "sin API key: primera configuración");
    check(launch_kind(false, true, true) == LAUNCH_FIRST_RUN, "sin API key, aunque arranque con Windows");
    check(launch_kind(true, false, false) == LAUNCH_HOME, "abierto a mano: ventana de Inicio");
    check(launch_kind(true, true, false) == LAUNCH_DIRECT, "con Windows: directo");
    check(launch_kind(true, false, true) == LAUNCH_DIRECT, "después de actualizarse: directo");
}

static void test_run_key(void)
{
    printf("-- clave Run --\n");
    const wchar_t *exe = L"C:\\Users\\Ana Pérez\\Sokari\\Sokari.exe";
    wchar_t *cmd = autostart_command(exe);
    check(!wcscmp(cmd, L"\"C:\\Users\\Ana Pérez\\Sokari\\Sokari.exe\" --autostart"),
          "ruta entre comillas (tiene espacios) y --autostart");
    check(!autostart_needs_refresh(cmd, exe), "la nueva no se vuelve a escribir");
    free(cmd);
    check(autostart_needs_refresh(L"\"C:\\Users\\Ana Pérez\\Sokari\\Sokari.exe\"", exe),
          "la de la versión anterior (sin --autostart) se actualiza");
    check(autostart_needs_refresh(L"\"c:\\users\\ana pérez\\sokari\\SOKARI.EXE\"  ", exe),
          "sin importar mayúsculas ni espacios al final");
    check(autostart_needs_refresh(L"C:\\Sokari\\Sokari.exe", L"C:\\Sokari\\Sokari.exe"), "también sin comillas");
    check(!autostart_needs_refresh(L"\"D:\\Otra copia\\Sokari.exe\"", exe), "si apunta a otra copia, no se toca");
    check(!autostart_needs_refresh(L"\"C:\\Users\\Ana Pérez\\Sokari\\Sokari.exe\" --otra-cosa", exe),
          "si alguien le puso otros argumentos, no se toca");
    check(!autostart_needs_refresh(L"\"C:\\Users\\Ana Pérez\\Sokari\\Sokari.exe.bak\"", exe),
          "una ruta que solo empieza igual no cuenta");
    check(!autostart_needs_refresh(NULL, exe) && !autostart_needs_refresh(L"", exe), "vacía o sin valor: nada");

    const wchar_t *j = L"C:\\Sokari\\Sokari.exe";
    check(autostart_value_points_to(L"\"C:\\Sokari\\Sokari.exe\" --autostart", j) &&
              autostart_value_points_to(L"\"c:\\sokari\\SOKARI.EXE\"", j) &&
              autostart_value_points_to(L"C:\\Sokari\\Sokari.exe --autostart", j),
          "la clave vieja apunta a este exe (con o sin comillas y argumentos)");
    check(!autostart_value_points_to(L"\"C:\\Otra\\Sokari.exe\"", j) &&
              !autostart_value_points_to(L"\"C:\\Sokari\\Sokari.exe.bak\"", j) &&
              !autostart_value_points_to(L"C:\\Sokari\\Sokari.exe2", j) && !autostart_value_points_to(NULL, j),
          "otra copia o una ruta que solo empieza igual no cuentan");
}

static void test_olvidar(const wchar_t *dir)
{
    printf("-- borrar la memoria de hoy --\n");
    wchar_t *saved = g_paths.memory_dir;
    g_paths.memory_dir = path_join(dir, L"memoria_prueba");
    ensure_dir(g_paths.memory_dir);
    wchar_t *mf = path_join(g_paths.memory_dir, L"memoria.jsonl");
    const char *lines = "{\"ts\":1000,\"role\":\"user\",\"content\":\"ayer\"}\n"
                        "{\"ts\":2000,\"role\":\"assistant\",\"content\":\"ayer también\"}\n"
                        "{\"ts\":5000,\"role\":\"user\",\"content\":\"hoy\"}\n"
                        "{\"ts\":6000,\"role\":\"assistant\",\"content\":\"hoy también\"}\n";
    write_file_atomic(mf, lines, strlen(lines));
    int n = memory_forget_since(3000);
    char *left = read_file_all(mf, NULL);
    check(n == 2 && left && strstr(left, "ayer también") && !strstr(left, "\"hoy\""),
          "borra solo lo de desde ese momento y deja lo de antes");
    free(left);
    check(memory_forget_since(0) == 2, "«todo» borra lo que queda");
    DeleteFileW(mf);
    RemoveDirectoryW(g_paths.memory_dir);
    free(mf);
    free(g_paths.memory_dir);
    g_paths.memory_dir = saved;
}

static cJSON *assets(const char *a, const char *b)
{
    cJSON *arr = cJSON_CreateArray();
    const char *names[] = {a, b};
    for (int i = 0; i < 2; i++) {
        if (!names[i]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", names[i]);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static const char *picked(cJSON *arr)
{
    const cJSON *a = update_pick_asset(arr);
    const cJSON *n = a ? cJSON_GetObjectItemCaseSensitive(a, "name") : NULL;
    return n ? n->valuestring : "(ninguno)";
}

static void test_update_asset(void)
{
    printf("-- actualizador --\n");
    cJSON *both = assets("notas.txt", "Sokari.exe"), *other = assets("Otro.exe", NULL);
    check(!strcmp(picked(both), "Sokari.exe"), "baja Sokari.exe del release");
    check(update_pick_asset(other) == NULL && update_pick_asset(NULL) == NULL, "si el release no trae Sokari.exe: nada");
    cJSON_Delete(both);
    cJSON_Delete(other);
}

static void write_config(const char *text)
{
    write_file_atomic(g_paths.config_file, text, strlen(text));
}

static void test_config(const wchar_t *dir)
{
    printf("-- configuración nueva --\n");
    write_config("");
    config_load();
    AppConfig c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED_BORDERLESS && !*c.output_name && c.win_w == -1,
          "valores por defecto: la esfera flotante, salida predeterminada, ventana sin tamaño");
    check(c.full_access, "y acceso completo prendido");
    check(!c.mexa, "y contesta neutro (el modo mexa viene apagado)");
    check(c.end_silence == 1 && c.duck, "y espera 0.8 s cuando te callas, bajando el volumen mientras escucha");
    c.end_silence = 2;
    c.duck = false;
    config_apply(&c);
    config_free(&c);
    config_load();
    check(config_end_silence() == 2 && !config_duck(), "«Más» y sin bajar el volumen quedan guardados");
    c = config_snapshot();
    c.end_silence = 1;
    c.duck = true;
    config_apply(&c);
    config_free(&c);

    free(run_tool("cambiar_permisos", "{\"acceso_completo\":false}"));
    config_load();
    check(!config_full_access(), "«pregúntame antes»: se apaga y queda guardado");
    free(run_tool("cambiar_permisos", "{\"acceso_completo\":true}"));
    config_load();
    check(config_full_access(), "«tienes permiso para todo»: se prende y queda guardado");
    config_set_mexa(true);
    config_load();
    check(config_mexa(), "el modo mexa queda guardado");
    config_set_mexa(false);
    write_config("SOKARI_CONFIRM_NEVER=0\n");
    config_load();
    check(config_full_access(), "al actualizar desde la 2.4.0 queda prendido (la opción vieja ya no cuenta)");
    write_config("");
    config_load();

    config_set_display_mode(DISPLAY_WINDOWED);
    config_set_window_rect(-1200, 40, 800, 600);
    config_set_output("Audífonos (WH-1000XM4)");
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED, "modo Ventana se guarda y se vuelve a leer");
    check(c.win_x == -1200 && c.win_y == 40 && c.win_w == 800 && c.win_h == 600,
          "posición de la ventana (también en un monitor a la izquierda)");
    check(!strcmp(c.output_name, "Audífonos (WH-1000XM4)"), "salida de audio con acentos y paréntesis");
    config_free(&c);

    config_set_display_mode(DISPLAY_MINIMIZED);
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_MINIMIZED, "modo Minimizado");
    config_free(&c);
    config_set_display_mode(99);
    config_set_display_mode(-1);
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_MINIMIZED, "un modo fuera de rango no se guarda");
    config_free(&c);

    /* De la 2.7.2 o antes: Pantalla completa y Raúl eran las de fábrica. */
    write_config("SOKARI_DISPLAY_MODE=fullscreen\nSOKARI_VOICE=HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\"
                 "Voices\\Tokens\\MSTTS_V110_esMX_RaulM\nSOKARI_CONFIG_VERSION=2\n");
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED_BORDERLESS && !*c.voice,
          "de la 2.7.2: Pantalla completa y Raúl pasan una vez a la esfera flotante y a la voz de fábrica (Sabina)");
    config_free(&c);
    write_config("SOKARI_DISPLAY_MODE=fullscreen\nSOKARI_VOICE=MSTTS_V110_esMX_RaulM\nSOKARI_CONFIG_VERSION=3\n");
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_FULLSCREEN && str_contains_ci(c.voice, "Raul"),
          "ya en la versión nueva, si eliges Pantalla completa o Raúl se respetan");
    config_free(&c);

    write_config("SOKARI_DISPLAY_MODE=windowed_borderless\nSOKARI_WINDOW=10,20\nSOKARI_OUTPUT=\n");
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED_BORDERLESS, "la esfera flotante de antes se sigue leyendo");
    check(c.win_w == -1 && c.win_h == -1, "una posición de ventana incompleta se ignora");
    config_free(&c);

    write_config("SOKARI_DISPLAY_MODE=inventado\nSOKARI_WINDOW=0,0,0,500\n");
    config_load();
    c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED_BORDERLESS, "un modo desconocido deja el de por defecto");
    check(c.win_w == -1, "un tamaño de ventana en cero se ignora");
    config_free(&c);

    check(!strcmp(display_mode_key(DISPLAY_WINDOWED), "windowed") && !strcmp(display_mode_key(DISPLAY_MINIMIZED), "minimized") &&
              !strcmp(display_mode_key(42), "fullscreen_borderless"),
          "nombres de los modos en config.env");
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    /* Todo en una carpeta temporal: nunca se toca tu config.env. */
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_inicio");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");

    test_launch();
    test_run_key();
    test_config(dir);
    test_olvidar(dir);
    test_update_asset();

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

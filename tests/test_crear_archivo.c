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
#include "tools.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
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

    DeleteFileW(file);
    RemoveDirectoryW(sub);
    free(u), free(file), free(sub), free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

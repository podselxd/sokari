/* La ventana de Sokari en Linux:
   - el modo de pantalla: de fábrica la esfera flotante, y una configuración
     de antes (Linux no tenía modos) pasa a ella una vez;
   - su barra de arriba (Hablar, el menú y la X) se ve: en la 2.7.0 nacía
     escondida y la ventana quedaba sin menú y sin cómo moverla. Esto
     necesita pantalla; sin ella se omite (la prueba de GNOME, que sí tiene,
     la corre también). */
#include <windows.h>

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "linux/linux.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static int mode_after(const char *text)
{
    write_file_atomic(g_paths.config_file, text, strlen(text));
    config_load();
    AppConfig c = config_snapshot();
    int m = c.display_mode;
    config_free(&c);
    return m;
}

static void test_modo(void)
{
    printf("-- el modo de pantalla --\n");
    DeleteFileW(g_paths.config_file);
    config_load();
    AppConfig c = config_snapshot();
    check(c.display_mode == DISPLAY_WINDOWED_BORDERLESS, "de fábrica: la esfera flotante");
    config_apply(&c);
    config_free(&c);
    char *saved = read_file_all(g_paths.config_file, NULL);
    check(saved && strstr(saved, "SOKARI_CONFIG_VERSION=2\n"), "al guardar anota la versión de la configuración");
    free(saved);
    check(mode_after("GROQ_API_KEY=gsk_x\nSOKARI_DISPLAY_MODE=fullscreen\n") == DISPLAY_WINDOWED_BORDERLESS,
          "una configuración de antes (Linux no tenía modos: nadie eligió pantalla completa) pasa a la flotante");
    check(mode_after("SOKARI_CONFIG_VERSION=2\nSOKARI_DISPLAY_MODE=fullscreen\n") == DISPLAY_FULLSCREEN,
          "la que ya eligió su modo se respeta");
    check(mode_after("SOKARI_CONFIG_VERSION=2\nSOKARI_DISPLAY_MODE=windowed\n") == DISPLAY_WINDOWED, "y la ventana");
    DeleteFileW(g_paths.config_file);
}

int main(void)
{
    paths_init();
    test_modo();
    printf("-- la barra de la ventana --\n");
    if (!gtk_init_check(NULL, NULL)) {
        printf("      Sin pantalla: se omite (la prueba de GNOME la corre).\n");
    } else {
        char *problem = ui_window_problems();
        check(!problem, "la barra se ve (Hablar, el menú y la X) y el menú de arriba y ☰ son el mismo, con la "
                        "versión y «Buscar actualizaciones»");
        if (problem) printf("      (%s)\n", problem);
        free(problem);
    }
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

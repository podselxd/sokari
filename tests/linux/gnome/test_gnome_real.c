/* Las herramientas de Sokari contra un GNOME de verdad (sin pantalla) con su
   extensión: abrir una app, enfocarla, escribir con acentos, oprimir teclas,
   cambiar de pestaña, pegar un archivo, maximizar y cerrar; y en una
   "terminal" de prueba, que no escriba ni le dé Enter. Lo arranca
   probar_extension.sh, que deja en SOKARI_PRUEBA_DIR lo que dicen las
   ventanas de prueba (ventana_de_prueba.c). */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "config.h"
#include "linux/acciones.h"
#include "linux/gnome.h"
#include "linux/linux.h"
#include "linux/proc.h"
#include "tools.h"
#include "util.h"

static int g_fail, g_total;
static const char *g_dir;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

/* Lo que contestó la herramienta, y si empieza como se espera. */
static bool says(const char *tool, const char *args, const char *start, const char *what)
{
    char *r = run_tool(tool, args);
    bool ok = str_starts_with(r, start);
    if (!ok) printf("      %s contestó: %s\n", tool, r);
    check(ok, what);
    free(r);
    return ok;
}

/* La última línea de la ventana de prueba que empieza con prefix (heap). */
static char *last_line(const char *who, const char *prefix)
{
    char *path = str_printf("%s/%s.out", g_dir, who);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return NULL;
    char line[4096], *found = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (str_starts_with(line, prefix)) {
            free(found);
            found = xstrdup(line + strlen(prefix));
        }
    }
    fclose(f);
    return found;
}

/* ¿La ventana de prueba ya dijo esa línea (alguna vez)? */
static bool said(const char *who, const char *line_want)
{
    char *path = str_printf("%s/%s.out", g_dir, who);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        found = !strcmp(line, line_want);
    }
    fclose(f);
    return found;
}

/* Espera (hasta ms) a que lo último que dijo con prefix sea want; con prefix
   "", a que haya dicho want alguna vez. */
static bool wait_for(const char *who, const char *prefix, const char *want, int ms)
{
    for (int t = 0; t <= ms; t += 100) {
        char *l = *prefix ? last_line(who, prefix) : NULL;
        bool ok = *prefix ? l && !strcmp(l, want) : said(who, want);
        if (!ok && t + 100 > ms) printf("      (%s dice «%s%s»; esperaba «%s%s»)\n", who, prefix, l ? l : "(nada)", prefix, want);
        free(l);
        if (ok) return true;
        Sleep(100);
    }
    return false;
}

static uint64_t window_of(const char *app)
{
    GnomeWindow *w;
    int n;
    uint64_t id = 0;
    if (gnome_list_windows(&w, &n) == GN_OK)
        for (int i = 0; i < n && !id; i++)
            if (!strcmp(w[i].app, app)) id = w[i].id;
    gnome_windows_free(w, n);
    return id;
}

static char *focused_app(void)
{
    GnomeWindow w;
    bool shell = false;
    char *r = gnome_focused(&w, &shell) == GN_OK && w.id && !shell ? xstrdup(w.app) : xstrdup("");
    gnome_window_clear(&w);
    return r;
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

static void test_abrir_y_enfocar(void)
{
    printf("-- abrir, enfocar y ver ventanas --\n");
    says("open_app", "{\"name\":\"Prueba de Sokari\"}", "Abrí Prueba de Sokari.", "abre la app por su nombre");
    check(wait_for("prueba", "", "lista", 8000), "y la ventana aparece");
    Sleep(500);
    says("open_app", "{\"name\":\"prueba de sokari\"}", "Ya estaba abierto: traje al frente Prueba de Sokari.",
         "abierta otra vez: la trae al frente en vez de abrir otra");
    says("focus_window", "{\"title_contains\":\"ventana de prueba\"}",
         "Enfoqué «ventana de prueba» en Prueba de Sokari.", "enfoca por el título y dice la app, no el título");
    char *list = run_tool("list_windows", "{}");
    check(strstr(list, "Inicio - Ventana de prueba") != NULL, "list_windows trae los títulos");
    free(list);
    says("focus_window", "{\"title_contains\":\"banana\"}", "No encontré ninguna ventana con 'banana' abierta.",
         "una ventana que no existe");
}

static void test_escribir(void)
{
    printf("-- escribir y el portapapeles --\n");
    says("copiar_portapapeles", "{\"texto\":\"lo que yo tenía\"}", "Listo, lo copié.", "copia al portapapeles");
    says("leer_portapapeles", "{}", "lo que yo tenía", "y lo lee");
    says("type_text", "{\"texto\":\"Hola, ¿cómo estás? canción ñandú ☕\\nsegunda línea\"}",
         "Escribí el texto en Prueba de Sokari, sin enviarlo.", "escribe en la app de enfrente y dice cuál");
    check(wait_for("prueba", "texto: ", "Hola, ¿cómo estás? canción ñandú ☕\\nsegunda línea", 3000),
          "con acentos, ñ, emoji y el salto de línea tal cual");
    Sleep(2000);
    says("leer_portapapeles", "{}", "lo que yo tenía", "y lo que tenías copiado vuelve a su lugar");
    says("presionar_teclas", "{\"teclas\":\"control a\"}", "Oprimí Ctrl+A en Prueba de Sokari.", "Ctrl+A");
    says("presionar_teclas", "{\"teclas\":\"retroceso\"}", "Oprimí Retroceso en Prueba de Sokari.", "Retroceso");
    check(wait_for("prueba", "texto: ", "", 2000), "y el texto quedó borrado (las teclas llegaron)");
    says("presionar_teclas", "{\"teclas\":\"h\",\"veces\":3}", "Oprimí H 3 veces en Prueba de Sokari.", "una tecla 3 veces");
    check(wait_for("prueba", "texto: ", "hhh", 2000), "y llegaron las 3");
    says("type_text", "{\"texto\":\"mensaje\",\"enviar\":true}", "Escribí el texto en Prueba de Sokari y le di Enter.",
         "con enviar: escribe y da Enter");
    check(wait_for("prueba", "texto: ", "hhhmensaje\\n", 3000), "el Enter llega después del texto, no antes");
    says("type_text", "{\"texto\":\"javascript:alert(1)\"}", "Por seguridad no escribo «javascript:»",
         "nunca «javascript:»");
}

static void test_pestanas(void)
{
    printf("-- pestañas --\n");
    says("ir_a_pestana", "{\"titulo\":\"musica\"}", "Cambié a la pestaña de «musica» en Prueba de Sokari.",
         "va a la pestaña que dice «Música» con Ctrl+Tab");
    check(wait_for("prueba", "pestaña: ", "Música del día", 1000), "y quedó en esa");
    says("ir_a_pestana", "{\"titulo\":\"música\"}", "Ya estabas en la pestaña de «música».", "si ya estaba, lo dice");
    says("ir_a_pestana", "{\"titulo\":\"banana\"}",
         "No encontré una pestaña que diga «banana» en Prueba de Sokari: las revisé todas.",
         "una que no existe: da la vuelta y se detiene");
}

static void test_subir(void)
{
    printf("-- subir un archivo --\n");
    static const unsigned char PNG[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D', 'R',
                                        0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0x1F, 0x15, 0xC4, 0x89, 0, 0, 0, 0x0A,
                                        'I', 'D', 'A', 'T', 0x78, 0x9C, 0x63, 0, 1, 0, 0, 5, 0, 1, 0x0D, 0x0A, 0x2D,
                                        0xB4, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    char *png = str_printf("%s/home/foto.png", g_dir);
    write_file(png, PNG, sizeof PNG);
    char *args = str_printf("{\"ruta\":\"%s\",\"ventana\":\"Ventana de prueba\"}", png);
    says("subir_archivo", args, "Pegué «foto.png» en Prueba de Sokari, sin mandarlo.", "una imagen: la pega en la app");
    char *targets = NULL;
    for (int t = 0; t < 30 && !(targets && strstr(targets, "image/png")); t++) {
        free(targets);
        Sleep(100);
        targets = last_line("prueba", "pegado: ");
    }
    check(targets && strstr(targets, "image/png"), "y lo que pegó es la imagen (image/png)");
    free(targets);
    free(args);
    char *pdf = str_printf("%s/home/tarea.pdf", g_dir);
    write_file(pdf, "%PDF-1.4\n%%EOF\n", 15);
    args = str_printf("{\"ruta\":\"%s\",\"ventana\":\"Ventana de prueba\"}", pdf);
    char *r = run_tool("subir_archivo", args);
    check(str_starts_with(r, "Pegué «tarea.pdf» en Prueba de Sokari") && strstr(r, "solo aceptan pegar imágenes"),
          "otro archivo: lo pega como archivo y avisa que algunas apps solo aceptan imágenes");
    free(r);
    targets = NULL;
    for (int t = 0; t < 30 && !(targets && strstr(targets, "text/uri-list")); t++) {
        free(targets);
        Sleep(100);
        targets = last_line("prueba", "pegado: ");
    }
    check(targets && strstr(targets, "text/uri-list"), "y va como lista de archivos (text/uri-list)");
    free(targets);
    free(args);
    free(pdf);
    free(png);
}

static void test_terminal(void)
{
    printf("-- en una terminal no escribe ni da Enter --\n");
    says("open_app", "{\"name\":\"Terminal de prueba\"}", "Abrí Terminal de prueba.", "abre la terminal de prueba");
    check(wait_for("terminal", "", "lista", 8000), "y aparece");
    Sleep(500);
    says("focus_window", "{\"title_contains\":\"Terminal de prueba\"}", "Enfoqué", "la enfoca");
    says("type_text", "{\"texto\":\"rm -rf ~\"}", "Por seguridad no escribo en terminales", "no escribe en una terminal");
    says("presionar_teclas", "{\"teclas\":\"enter\"}", "No oprimí nada: por seguridad no le doy Enter a terminales",
         "ni le da Enter");
    says("presionar_teclas", "{\"teclas\":\"control c\"}", "Oprimí Ctrl+C en Terminal de prueba.",
         "otras teclas sí (Ctrl+C)");
    /* Y la extensión también lo revisa, por si alguien llamara sin pasar por Sokari. */
    uint64_t id = window_of("Terminal de prueba");
    const uint32_t enter = XK_Return;
    char *st = NULL;
    gnome_press_keys(id, &enter, 1, 1, &st);
    check(st && !strcmp(st, "terminal"), "la extensión tampoco le da Enter a una terminal");
    free(st);
    st = NULL;
    gnome_paste(id, "rm -rf ~", &st);
    check(st && !strcmp(st, "terminal"), "ni le pega texto");
    free(st);
    char *out = last_line("terminal", "texto: ");
    check(!out || !strstr(out, "rm"), "y a la terminal no le llegó nada");
    free(out);
    st = NULL;
    const uint32_t h = 'h';
    gnome_press_keys(id + 12345, &h, 1, 1, &st);
    check(st && !strcmp(st, "focus"), "si la ventana de enfrente ya no es la que revisó Sokari, no oprime nada");
    free(st);
}

static void test_ventanas(void)
{
    printf("-- cambiar, maximizar, minimizar y cerrar --\n");
    says("control_desktop", "{\"action\":\"switch_window\"}", "Listo.", "Alt+Tab");
    Sleep(600);
    char *app = focused_app();
    check(!strcmp(app, "Prueba de Sokari"), "y quedó enfrente la otra ventana");
    free(app);
    says("control_desktop", "{\"action\":\"maximize\"}", "Maximicé Prueba de Sokari.", "maximiza la de enfrente");
    check(wait_for("prueba", "maximizada: ", "sí", 2000), "y quedó maximizada");
    says("control_desktop", "{\"action\":\"minimize\"}", "Minimicé Prueba de Sokari.", "minimiza la de enfrente");
    Sleep(500);
    GnomeWindow *w;
    int n;
    bool min = false;
    if (gnome_list_windows(&w, &n) == GN_OK)
        for (int i = 0; i < n; i++)
            if (!strcmp(w[i].app, "Prueba de Sokari")) min = w[i].minimized;
    gnome_windows_free(w, n);
    check(min, "y quedó minimizada");
    says("focus_window", "{\"title_contains\":\"Ventana de prueba\"}", "Enfoqué", "la vuelve a traer");
    says("control_desktop", "{\"action\":\"close_window\"}", "Cerré Prueba de Sokari.", "cierra la de enfrente");
    check(wait_for("prueba", "", "cerrada", 3000), "como darle a la X");
}


static int count_titled(const char *title)
{
    GnomeWindow *w;
    int n, c = 0;
    if (gnome_list_windows(&w, &n) == GN_OK)
        for (int i = 0; i < n; i++)
            if (!strcmp(w[i].title, title)) c++;
    gnome_windows_free(w, n);
    return c;
}

static bool wait_titled(const char *title, int want, int ms)
{
    for (int t = 0; t <= ms; t += 100) {
        if (count_titled(title) == want) return true;
        Sleep(100);
    }
    return false;
}

static pid_t start_sokari(const char *arg)
{
    const char *bin = getenv("SOKARI_BIN");
    const char *argv[] = {bin, arg, NULL};
    return proc_spawn(argv, NULL, NULL, NULL, NULL);
}

static void test_ventana_de_sokari(void)
{
    printf("-- la ventana de Sokari --\n");
    char *problem = gtk_init_check(NULL, NULL) ? ui_window_problems() : xstrdup("GTK no abrió la pantalla");
    if (problem) printf("      (%s)\n", problem);
    check(!problem, "su barra de arriba se ve (Hablar, el menú y la X) y los dos menús son el mismo, con la "
                    "versión y «Buscar actualizaciones»");
    free(problem);
    if (!getenv("SOKARI_BIN")) {
        printf("      (sin SOKARI_BIN: se omite)\n");
        return;
    }
    pid_t pid = start_sokari(NULL);
    check(wait_titled("Sokari", 1, 10000), "abre su ventana con la esfera");
    check(wait_titled("Configuración de Sokari", 1, 3000), "la primera vez (sin API key) abre también Configuración");
    Sleep(2500);
    check(pid > 0 && kill(pid, 0) == 0, "la esfera se dibuja unos segundos sin problemas");
    pid_t again = start_sokari(NULL);
    int code = proc_finish(again, 8000);
    check(code == 0 && count_titled("Sokari") == 1, "abrirla otra vez no abre otra: muestra la que ya está");
    uint64_t id = 0;
    GnomeWindow *w;
    int n;
    if (gnome_list_windows(&w, &n) == GN_OK)
        for (int i = 0; i < n; i++)
            if (!strcmp(w[i].title, "Configuración de Sokari")) id = w[i].id;
    gnome_windows_free(w, n);
    bool ok = false;
    gnome_window_action(id, "close", &ok);
    check(wait_titled("Configuración de Sokari", 0, 3000), "Configuración se cierra");
    if (gnome_list_windows(&w, &n) == GN_OK)
        for (int i = 0; i < n; i++)
            if (!strcmp(w[i].title, "Sokari")) id = w[i].id;
    gnome_windows_free(w, n);
    gnome_window_action(id, "close", &ok);
    check(wait_titled("Sokari", 0, 3000) && kill(pid, 0) == 0,
          "cerrar la ventana la esconde, pero Sokari sigue corriendo (y escuchando)");
    again = start_sokari("--mostrar");
    proc_finish(again, 8000);
    check(wait_titled("Sokari", 1, 5000), "abrirla otra vez la vuelve a mostrar");
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    GVariant *r = bus ? g_dbus_connection_call_sync(bus, "io.github.podselxd.Sokari", "/io/github/podselxd/Sokari",
                                                    "org.gtk.Actions", "Activate",
                                                    g_variant_new("(sava{sv})", "salir", NULL, NULL), NULL,
                                                    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL)
                      : NULL;
    if (r) g_variant_unref(r);
    if (bus) g_object_unref(bus);
    code = proc_finish(pid, 6000);
    check(r && code == 0, "«Salir» de su menú la cierra del todo");
}

int wmain(void)
{
    g_dir = getenv("SOKARI_PRUEBA_DIR");
    if (!g_dir) {
        printf("Esta prueba la corre tests/linux/gnome/probar_extension.sh.\n");
        return 2;
    }
    paths_init();
    GnomeWindow *w;
    int n;
    GnomeStatus st = gnome_list_windows(&w, &n);
    gnome_windows_free(w, n);
    check(st == GN_OK, "la extensión contesta");
    if (st != GN_OK) {
        printf("      %s\n", gnome_status_message(st));
        return 1;
    }
    test_abrir_y_enfocar();
    test_escribir();
    test_pestanas();
    test_subir();
    test_terminal();
    test_ventanas();
    test_ventana_de_sokari();
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

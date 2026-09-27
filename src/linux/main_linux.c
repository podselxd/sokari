/* Sokari en Linux. Sin opciones abre su ventana (ui_linux.c): la esfera, lo
   que dices y lo que contesta, Configuración y Tus PCs. Sin ventana: en modo
   voz (sokari --voz) te escucha ("Hey Sokari") y te contesta hablando, con
   los avisos en la terminal; en modo texto (sokari --texto) le escribes. El
   mismo agente, las mismas herramientas y las mismas confirmaciones que en
   Windows; las ventanas, las teclas y el portapapeles, con su extensión de
   GNOME. */
#include <windows.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "agent.h"
#include "audio.h"
#include "config.h"
#include "http.h"
#include "linux/gnome.h"
#include "linux/linux.h"
#include "log.h"
#include "memory.h"
#include "uninstall.h"
#include "update.h"
#include "mesh.h"
#include "util.h"
#include "voice.h"

static void usage(void)
{
    printf("Sokari %s para Linux\n\n"
           "  sokari                     abrir Sokari (la ventana con la esfera; si ya está abierta, la muestra)\n"
           "  sokari --hablar            que te escuche ahora (lo mismo que Ctrl+Alt+J)\n"
           "  sokari --voz               sin ventana: escucharte y contestarte hablando; Ctrl+C para salir\n"
           "  sokari --texto             platicar escribiendo (Ctrl+D para salir)\n"
           "  sokari --simular x.wav     como --voz, pero con ese audio (16 kHz) en vez del micrófono\n"
           "  sokari --revisar-malla     revisa la red con tus otras PCs (Tailscale, firewall, cada PC)\n"
           "  sokari --detectar-pcs      registra tus otras PCs de Tailscale (con Windows o Linux)\n"
           "  sokari --permitir-firewall abre el puerto de la malla solo para tu red de Tailscale\n"
           "  sokari --revisar-gnome     revisa que la extensión de GNOME te atienda (ventanas y teclas)\n"
           "  sokari --actualizar        baja e instala la versión nueva, si hay (pide tu contraseña)\n"
           "  sokari --desinstalar       quita Sokari (pregunta antes y pide tu contraseña)\n"
           "  sokari --version           la versión\n\n"
           "La configuración está en ~/.config/sokari/config.env (tu API key de Groq va en\n"
           "GROQ_API_KEY=...). La memoria y tus datos, en ~/.local/share/sokari.\n",
           SOKARI_VERSION);
}

static bool has_groq_key(void)
{
    AppConfig cfg = config_snapshot();
    bool has = *cfg.groq_api_key != 0;
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    return has;
}

static int run_text(void)
{
    if (!has_groq_key()) {
        char *path = wide_to_utf8(g_paths.config_file);
        printf("Falta tu API key de Groq. Sácala gratis en https://console.groq.com/keys y ponla en\n"
               "%s así:\n\n  GROQ_API_KEY=gsk_...\n\n",
               path);
        free(path);
        return 2;
    }
    agent_init();
    printf("Sokari %s (modo texto). Escribe lo que le dirías; Ctrl+D para salir.\n", SOKARI_VERSION);
    Conversation *c = conv_create(true);
    char line[4096];
    for (;;) {
        printf("\ntú> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        char *text = str_trim(line);
        if (!*text) {
            free(text);
            continue;
        }
        TurnResult r = agent_process(c, text);
        free(text);
        if (r.reply && *r.reply) printf("sokari> %s\n", r.reply);
        free(r.reply);
        if (r.shutdown || app_quit_requested()) break;
        if (!r.keep_going) {
            /* Como cuando te despides en voz: la siguiente vez empieza de nuevo. */
            conv_destroy(c);
            c = conv_create(true);
        }
    }
    conv_destroy(c);
    printf("\n");
    return 0;
}

static volatile sig_atomic_t g_sigint;

static void on_sigint(int sig)
{
    (void)sig;
    g_sigint = 1;
}

static int run_voice(const char *wav)
{
    if (!has_groq_key()) return run_text(); /* dice dónde poner la key */
    log_to_console(1);
    if (wav) {
        /* La simulación nunca toca tu memoria real: usa una carpeta temporal. */
        char tmpl[] = "/tmp/sokari-simulacion-XXXXXX";
        if (mkdtemp(tmpl)) {
            free(g_paths.memory_dir);
            g_paths.memory_dir = utf8_to_wide(tmpl);
        }
        wchar_t *w = utf8_to_wide(wav);
        voice_set_input_wav(w);
        free(w);
    } else {
        AppConfig cfg = config_snapshot();
        speaker_set_device(cfg.output_name);
        config_free(&cfg);
        voice_mesh_start();
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (!voice_start()) return 1;
    while (!voice_wait(200)) {
        if (g_sigint || app_quit_requested()) {
            voice_stop();
            break;
        }
    }
    if (!wav) voice_mesh_stop();
    return 0;
}

/* «Detectar mis PCs» de la ventana de Windows: las otras PCs de tu Tailscale. */
static int detect_pcs(void)
{
    MeshDevice *list;
    char *why = NULL;
    int n = tailscale_windows_peers(&list, &why);
    if (!n) printf("%s\n", why ? why : "No encontré otras PCs en tu red de Tailscale.");
    for (int i = 0; i < n; i++) {
        bool ok = mesh_device_set(list[i].name, list[i].host);
        printf("%s %s (%s)\n", ok ? "Registré" : "No pude registrar", list[i].name, list[i].host);
    }
    if (n) printf("\nYa les puedes decir: «Sokari, en %s abre Spotify».\n", list[0].name);
    free(why);
    mesh_devices_free(list, n);
    return n ? 0 : 1;
}

/* ¿La extensión de GNOME le hace caso a este Sokari? */
static int check_gnome(void)
{
    GnomeWindow *w;
    int n = 0;
    GnomeStatus st = gnome_list_windows(&w, &n);
    if (st == GN_OK) printf("La extensión de GNOME funciona: veo %d ventana%s tuya%s.\n", n, n == 1 ? "" : "s",
                            n == 1 ? "" : "s");
    else printf("%s\n", gnome_status_message(st));
    gnome_windows_free(w, n);
    return st == GN_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    bool text = false, voice = false, gui = argc == 1, uninstall = false;
    const char *wav = NULL, *mesh_cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hablar") || !strcmp(argv[i], "--autostart") || !strcmp(argv[i], "--mostrar")) {
            gui = true;
            continue;
        }
        if (!strcmp(argv[i], "--texto")) {
            text = true;
        } else if (!strcmp(argv[i], "--voz")) {
            voice = true;
        } else if (!strcmp(argv[i], "--simular") && i + 1 < argc) {
            wav = argv[++i];
        } else if (!strcmp(argv[i], "--revisar-malla") || !strcmp(argv[i], "--detectar-pcs") ||
                   !strcmp(argv[i], "--permitir-firewall") || !strcmp(argv[i], "--revisar-gnome") ||
                   !strcmp(argv[i], "--actualizar")) {
            mesh_cmd = argv[i];
        } else if (!strcmp(argv[i], "--desinstalar")) {
            uninstall = true;
        } else if (!strcmp(argv[i], "--version")) {
            printf("%s\n", SOKARI_VERSION);
            return 0;
        } else {
            usage();
            return strcmp(argv[i], "--ayuda") && strcmp(argv[i], "--help") ? 1 : 0;
        }
    }
    if (uninstall) {
        paths_init();
        return uninstall_run();
    }
    if (!text && !voice && !wav && !mesh_cmd && !gui) {
        usage();
        return 0;
    }
    paths_init();
    log_init(g_paths.log_file);
    log_msg("Sokari %s arrancando (Linux, modo %s).", SOKARI_VERSION,
            gui ? "ventana" : mesh_cmd ? mesh_cmd + 2 : text ? "texto" : wav ? "simulación" : "voz");
    config_load();
    memory_init();
    http_init();
    if (gui && !text && !voice && !wav && !mesh_cmd) {
        /* Una sola Sokari: si ya hay una abierta, esto solo le avisa (y sale). */
        int rc = ui_run(argc, argv);
        affect_save();
        log_msg("Sokari cerrado.");
        return rc;
    }
    if (mesh_cmd) {
        int rc = 0;
        if (!strcmp(mesh_cmd, "--revisar-malla")) {
            char *r = mesh_diagnose();
            printf("%s", r);
            free(r);
        } else if (!strcmp(mesh_cmd, "--detectar-pcs")) {
            rc = detect_pcs();
        } else if (!strcmp(mesh_cmd, "--revisar-gnome")) {
            rc = check_gnome();
        } else if (!strcmp(mesh_cmd, "--actualizar")) {
            char *r = update_check_now();
            printf("%s\n", r);
            rc = str_starts_with(r, "Listo") || str_starts_with(r, "Ya tienes") ? 0 : 1;
            free(r);
        } else {
            bool ok = mesh_allow_firewall();
            printf(ok ? "Listo: la malla puede recibir órdenes de tu red de Tailscale.\n"
                      : "No pude cambiar el firewall (¿cancelaste la contraseña?). Los detalles están en el registro.\n");
            rc = ok ? 0 : 1;
        }
        log_msg("Sokari cerrado.");
        return rc;
    }
    /* Sin la extensión de GNOME no puede ver ventanas ni oprimir teclas. */
    if (!wav) gnome_extension_enable();
    int rc = text ? run_text() : run_voice(wav);
    affect_save();
    log_msg("Sokari cerrado.");
    return rc;
}

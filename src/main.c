#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "app.h"
#include "audio.h"
#include "autostart.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "memory.h"
#include "resources.h"
#include "ui.h"
#include "update.h"
#include "util.h"
#include "voice.h"

#define INSTANCE_MUTEX L"Local\\SokariAsistenteDeVoz"

static HINSTANCE g_inst;
static bool g_voice_started;

/* Menús (bandeja, listas de Configuración) en modo oscuro: uxtheme lo expone
   solo por ordinal (135). Si no existe en esta versión de Windows, no pasa
   nada: quedan con el tema normal. */
static void enable_dark_menus(void)
{
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppModeFn)(int);
    typedef void(WINAPI * FlushMenuThemesFn)(void);
    SetPreferredAppModeFn set_mode = (SetPreferredAppModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(2);
    if (flush) flush();
}

/* Se llama al guardar la primera configuración, al darle a Iniciar y al
   arrancar directo. */
static void on_settings_saved(bool first_run)
{
    if (!g_voice_started) {
        g_voice_started = voice_start();
        update_start_background();
        ui_show_hud(HUD_SHOW_QUIET);
    }
    if (first_run)
        app_notify("Sokari", res_has_wake_word() ? "Listo. Di \"Hey Sokari\" (o Ctrl+Alt+J) cuando quieras hablarle."
                                                 : "Listo. Háblame con Ctrl+Alt+J.");
}

static void wait_for_pid(const wchar_t *arg)
{
    DWORD pid = (DWORD)wcstoul(arg, NULL, 10);
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) {
        WaitForSingleObject(h, 15000);
        CloseHandle(h);
    }
}

static int run_simulation(const wchar_t *wav)
{
    log_to_console(1);
    log_msg("Simulación con %ls", wav);
    voice_set_input_wav(wav);
    if (!voice_start()) return 1;
    while (!voice_wait(1000)) {
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *simulate = NULL;
    bool updated = false, from_autostart = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--wait-for-pid") && i + 1 < argc) wait_for_pid(argv[++i]);
        else if (!wcscmp(argv[i], L"--simulate") && i + 1 < argc) simulate = argv[++i];
        else if (!wcscmp(argv[i], L"--updated")) updated = true;
        else if (!wcscmp(argv[i], AUTOSTART_FLAG)) from_autostart = true;
    }
    if (simulate) AttachConsole(ATTACH_PARENT_PROCESS);

    paths_init();
    if (simulate) {
        /* La simulación nunca toca tu memoria real: usa una carpeta temporal. */
        free(g_paths.memory_dir);
        g_paths.memory_dir = expand_env(L"%TEMP%\\sokari_simulacion");
        free(g_paths.log_file);
        g_paths.log_file = path_join(g_paths.memory_dir, L"sokari.log");
        ensure_dir(g_paths.memory_dir);
    }
    log_init(g_paths.log_file);

    HANDLE mutex = NULL;
    if (!simulate) {
        mutex = CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            /* Ya está abierto: abrirlo otra vez a mano muestra su ventana de Inicio. */
            HWND other = FindWindowW(SOKARI_MSG_CLASS, NULL);
            if (other && !from_autostart) PostMessageW(other, WM_APP_HOME, 0, 0);
            CloseHandle(mutex);
            return 0;
        }
    }

    log_msg("Sokari %s arrancando.", SOKARI_VERSION);
    config_load();
    config_migrate_legacy();
    memory_init();
    http_init();
    update_cleanup_old();

    if (simulate) return run_simulation(simulate);

    autostart_refresh();
    enable_dark_menus();
    AppConfig cfg = config_snapshot();
    speaker_set_device(cfg.output_name);
    bool has_key = *cfg.groq_api_key != 0;
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);

    LaunchKind kind = launch_kind(has_key, from_autostart, updated);
    if (!ui_init(inst, on_settings_saved, kind == LAUNCH_DIRECT)) {
        MessageBoxW(NULL, L"No pude abrir la interfaz de Sokari.", L"Sokari", MB_ICONERROR);
        return 1;
    }
    voice_mesh_start();
    if (kind == LAUNCH_DIRECT) {
        on_settings_saved(false);
        if (updated) app_notify("Sokari", "Me actualicé a la versión " SOKARI_VERSION ".");
    } else if (kind == LAUNCH_HOME) {
        home_open(inst, true, on_settings_saved);
    } else {
        settings_open(inst, true, on_settings_saved);
    }

    int rc = ui_run();
    voice_mesh_stop();
    affect_save();
    log_msg("Sokari cerrado.");
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    LocalFree(argv);
    CoUninitialize();
    return rc;
}

/* El campo «Secreto» de Configuración → Dispositivos: se puede escribir (o
   pegar) otro y guardarlo, y una IP pegada ahí por error no se guarda. Abre
   la ventana de verdad y teclea en ella, con la configuración en una carpeta
   temporal: nunca toca la tuya. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "memory.h"
#include "ui.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static void pump(int ms)
{
    uint64_t end = GetTickCount64() + (uint64_t)ms;
    MSG m;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

static HWND open_devices(void)
{
    settings_open(GetModuleHandleW(NULL), false, NULL);
    HWND h = settings_window();
    pump(500);
    /* Barra lateral: Inicio, Cuenta, Pantalla, Voz y audio, General, Skills, Dispositivos. */
    UINT dpi = GetDpiForWindow(h);
    int x = MulDiv(60, (int)dpi, 96), y = MulDiv(110 + 6 * 46 + 20, (int)dpi, 96);
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    pump(300);
    return h;
}

static void type_into(HWND ed, const wchar_t *text)
{
    SetFocus(ed);
    SendMessageW(ed, EM_SETSEL, 0, -1);
    SendMessageW(ed, WM_CHAR, VK_BACK, 0);
    for (const wchar_t *p = text; *p; p++) SendMessageW(ed, WM_CHAR, *p, 0);
}

static wchar_t *text_of(HWND ed)
{
    int n = GetWindowTextLengthW(ed);
    wchar_t *w = calloc((size_t)n + 1, sizeof *w);
    GetWindowTextW(ed, w, n + 1);
    return w;
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_secreto");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");
    DeleteFileW(g_paths.config_file);
    config_load();
    AppConfig c = config_snapshot();
    free(c.groq_api_key);
    c.groq_api_key = xstrdup("gsk_solo_para_la_prueba");
    free(c.mesh_secret);
    c.mesh_secret = xstrdup("100.121.139.36");
    config_apply(&c);
    config_free(&c);
    memory_init();

    printf("-- el secreto se puede cambiar --\n");
    HWND h = open_devices();
    HWND ed = GetDlgItem(h, 100 + 4 /* F_MESH */);
    check(ed && IsWindowVisible(ed) && IsWindowEnabled(ed), "el campo del secreto se ve y está activo en Dispositivos");
    const wchar_t *nuevo = L"a1b2c3d4e5f6a7b8c9d0a1b2c3d4e5f6a7b8c9d0a1b2c3d4e5f6a7b8c9d0a1b2";
    type_into(ed, nuevo);
    wchar_t *got = text_of(ed);
    check(!wcscmp(got, nuevo), "lo que escribes queda en el campo");
    free(got);
    SendMessageW(ed, WM_KEYDOWN, VK_RETURN, 0);
    pump(300);
    char *saved = config_mesh_secret(false);
    char *want = wide_to_utf8(nuevo);
    check(!strcmp(saved, want), "al guardar, queda el secreto nuevo");
    free(saved);
    free(want);
    if (IsWindow(h)) DestroyWindow(h);
    pump(100);

    printf("-- una IP en el secreto no se guarda --\n");
    h = open_devices();
    ed = GetDlgItem(h, 100 + 4 /* F_MESH */);
    type_into(ed, L"100.121.139.36");
    SendMessageW(ed, WM_KEYDOWN, VK_RETURN, 0);
    pump(300);
    check(IsWindow(h), "la ventana se queda abierta para que lo corrijas");
    saved = config_mesh_secret(false);
    want = wide_to_utf8(nuevo);
    check(!strcmp(saved, want), "y el secreto que ya estaba no cambia");
    free(saved);
    free(want);
    if (IsWindow(h)) DestroyWindow(h);
    pump(100);

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

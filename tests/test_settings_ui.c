/* Abre la ventana de Configuración y guarda una captura de cada sección
   usando PrintWindow sobre esa ventana sola (nunca captura el escritorio). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "memory.h"
#include "ui.h"
#include "util.h"

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

static void capture(HWND h, const wchar_t *dir, const wchar_t *name)
{
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB}};
    void *bits;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_RENDERFULLCONTENT);
    wchar_t path[1024];
    swprintf(path, 1024, L"%ls\\%ls.bmp", dir, name);
    size_t row = (size_t)w * 4, size = 54 + row * hh;
    unsigned char *b = calloc(1, size);
    b[0] = 'B', b[1] = 'M';
    memcpy(b + 2, &(uint32_t){(uint32_t)size}, 4);
    memcpy(b + 10, &(uint32_t){54}, 4);
    memcpy(b + 14, &(uint32_t){40}, 4);
    memcpy(b + 18, &(int32_t){w}, 4);
    memcpy(b + 22, &(int32_t){-hh}, 4);
    memcpy(b + 26, &(uint16_t){1}, 2);
    memcpy(b + 28, &(uint16_t){32}, 2);
    memcpy(b + 54, bits, row * hh);
    write_file_atomic(path, b, size);
    free(b);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    paths_init();
    config_load();
    memory_init();
    bool first = argc > 2;
    settings_open(GetModuleHandleW(NULL), first, NULL);
    HWND h = settings_window();
    pump(600);
    capture(h, argv[1], first ? L"ui_first" : L"ui_cuenta");
    if (!first) {
        /* Barra lateral: Inicio, Cuenta (ya capturada), Pantalla, Voz y audio, General, Skills,
           Dispositivos, IA de respaldo. */
        const wchar_t *names[] = {L"ui_inicio", NULL,        L"ui_pantalla",     L"ui_audio",
                                  L"ui_general", L"ui_skills", L"ui_dispositivos", L"ui_ia"};
        UINT dpi = GetDpiForWindow(h);
        for (int i = 0; i < 8; i++) {
            if (!names[i]) continue;
            int y = MulDiv(110 + i * 46 + 20, (int)dpi, 96);
            int x = MulDiv(60, (int)dpi, 96);
            SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            pump(400);
            capture(h, argv[1], names[i]);
        }
    }
    DestroyWindow(h);
    pump(100);
    return 0;
}

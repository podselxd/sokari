/* La captura en Windows: el monitor donde está el cursor, sin las ventanas
   de Sokari. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

#include "pantalla.h"
#include "ui.h"
#include "util.h"

uint8_t *screen_grab(int *w, int *h, char **why)
{
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        if (why) *why = xstrdup("no encontré el monitor");
        return NULL;
    }
    int W = mi.rcMonitor.right - mi.rcMonitor.left, H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(NULL), mem = CreateCompatibleDC(scr);
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    uint8_t *rgb = NULL;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ui_capture_exclude(true);
        BOOL ok = BitBlt(mem, 0, 0, W, H, scr, mi.rcMonitor.left, mi.rcMonitor.top, SRCCOPY | CAPTUREBLT);
        ui_capture_exclude(false);
        SelectObject(mem, old);
        if (ok) {
            rgb = xmalloc((size_t)W * H * 3);
            const uint8_t *s = bits;
            for (size_t i = 0; i < (size_t)W * H; i++) {
                rgb[i * 3] = s[i * 4 + 2];
                rgb[i * 3 + 1] = s[i * 4 + 1];
                rgb[i * 3 + 2] = s[i * 4];
            }
        } else if (why) {
            *why = xstrdup("Windows no dejó copiar la pantalla");
        }
    } else if (why) {
        *why = xstrdup("no hubo memoria para la captura");
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    *w = W;
    *h = H;
    return rgb;
}

/* PDF en Windows: sin una biblioteca de PDF no se puede sacar su texto. */
#include "lector.h"
char *pdf_text(const wchar_t *path, char **why)
{
    (void)path;
    if (why) *why = xstrdup("En Windows todavía no leo PDF: ábrelo en Edge y usa «Leer en voz alta» (Ctrl+Shift+U).");
    return NULL;
}

/* Lo que hay en el escritorio de Windows, sin capturas: monitores, ventanas,
   la activa, el cursor, donde escribes y si algo está en pantalla completa. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string.h>

#include "desk.h"
#include "desk_win.h"

static DeskRect from_rect(const RECT *r)
{
    return (DeskRect){r->left, r->top, r->right - r->left, r->bottom - r->top};
}

static BOOL CALLBACK add_monitor(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp)
{
    DeskView *v = (DeskView *)lp;
    MONITORINFO mi = {sizeof mi};
    if (v->nmon < DESK_MAX_MONITORS && GetMonitorInfoW(m, &mi)) {
        v->mon[v->nmon].area = from_rect(&mi.rcMonitor);
        v->mon[v->nmon].work = from_rect(&mi.rcWork);
        v->nmon++;
    }
    return TRUE;
}

/* Una ventana de verdad: visible, no minimizada, no escondida por Windows
   (otras apps de la tienda en otro escritorio virtual) y no una de las del
   propio escritorio o la barra de tareas. */
static bool real_window(HWND h, HWND own)
{
    if (h == own || !IsWindowVisible(h) || IsIconic(h)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    static const wchar_t *const SHELL[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    for (size_t i = 0; i < sizeof SHELL / sizeof *SHELL; i++)
        if (!wcscmp(cls, SHELL[i])) return false;
    return true;
}

static void frame_rect(HWND h, RECT *r)
{
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof *r))) GetWindowRect(h, r);
}

static bool covers_monitor(HWND h)
{
    RECT r;
    frame_rect(h, &r);
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

void desk_view_windows(DeskView *v, HWND own)
{
    memset(v, 0, sizeof *v);
    EnumDisplayMonitors(NULL, NULL, add_monitor, (LPARAM)v);
    HWND fg = GetForegroundWindow();
    HWND fg_root = fg ? GetAncestor(fg, GA_ROOT) : NULL;
    for (HWND h = GetTopWindow(NULL); h && v->nwin < DESK_MAX_WINDOWS; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!real_window(h, own)) continue;
        RECT r;
        frame_rect(h, &r);
        if (r.right - r.left < 40 || r.bottom - r.top < 40) continue;
        v->win[v->nwin].r = from_rect(&r);
        v->win[v->nwin].active = h == fg_root;
        v->nwin++;
    }
    POINT p;
    if (GetCursorPos(&p)) v->cursor_x = p.x, v->cursor_y = p.y;
    GUITHREADINFO gi = {sizeof gi};
    if (fg && GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gi) && gi.hwndCaret &&
        gi.rcCaret.bottom > gi.rcCaret.top) {
        POINT a = {gi.rcCaret.left, gi.rcCaret.top}, b = {gi.rcCaret.right, gi.rcCaret.bottom};
        ClientToScreen(gi.hwndCaret, &a);
        ClientToScreen(gi.hwndCaret, &b);
        v->has_caret = true;
        v->caret = (DeskRect){a.x, a.y, b.x - a.x > 2 ? b.x - a.x : 2, b.y - a.y};
    }
    /* Pantalla completa: lo que dice Windows (juegos, presentaciones) o una
       ventana enfrente que tapa todo su monitor (un video). */
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)))
        v->fullscreen = q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE;
    if (!v->fullscreen && fg_root && real_window(fg_root, own) && covers_monitor(fg_root)) v->fullscreen = true;
}

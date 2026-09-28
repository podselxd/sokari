/* Ventana del HUD (la esfera) + ventana oculta de mensajes (bandeja, atajo de
   teclado, avisos). La esfera se dibuja en su propio hilo, sincronizado con el
   refresco del monitor; el hilo de la interfaz solo maneja mensajes.
   En pantalla completa y como esfera flotante la ventana nunca toma el foco
   (WS_EX_NOACTIVATE): las teclas que manda Sokari siempre llegan a la app que
   estás usando. En modo Ventana es una ventana normal (se puede mover,
   minimizar, F11); ahí, antes de mandar teclas, le pasa el foco a la ventana
   que sigue. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "app.h"
#include "config.h"
#include "desk.h"
#include "desk_win.h"
#include "face.h"
#include "log.h"
#include "resource.h"
#include "resources.h"
#include "sphere.h"
#include "ui.h"
#include "util.h"
#include "voice.h"

#define HUD_CLASS L"SokariHUD"
#define HOTKEY_TALK 1
/* "Aparecer solo cuando le hablas": al terminar de hablar, la esfera espera
   un momento (que se vea que terminó) y se esconde. */
#define WM_APP_AUTOHIDE (WM_APP + 9)
/* El hilo de dibujo terminó de sacar la esfera: ya se puede esconder la ventana. */
#define WM_APP_LEFT (WM_APP + 10)
/* El botón Probar de la animación (wParam: cuál). */
#define WM_APP_ANIMTEST (WM_APP + 11)
/* Mientras toma una captura, las ventanas de Sokari no salen en ella. */
#define WM_APP_CAPTURE (WM_APP + 12)
#define TIMER_AUTOHIDE 1
#define AUTOHIDE_MS 1500
#define SUBTITLE_SECONDS 8.0
#define ORB_FRACTION 0.40f

static struct {
    HINSTANCE inst;
    HWND msg, hud;
    SettingsSavedFn on_saved;
    UINT taskbar_created;

    int mode, style, res;
    int anim; /* cómo entra y sale (SphereAnim) */
    bool face_symbols;
    bool subtitles;
    volatile LONG visible;
    volatile LONG yielded;
    /* Se está yendo (o la muestra Probar): la ventana sigue en pantalla hasta
       que el hilo de dibujo avisa que terminó (WM_APP_LEFT). */
    volatile LONG leaving;
    /* Lo último que se le pidió al hilo de dibujo: (número << 5) | (animación << 3) | pedido. */
    volatile LONG anim_req;
    LONG anim_seq, leave_seq;
    RECT mon; /* monitor; en los modos con ventana, el rectángulo de la ventana */
    /* Esfera flotante: orb_base es el cuadro de la esfera; la ventana (orb_size)
       le suma orb_pad transparente de cada lado para que las ondas de las líneas
       no se corten. La posición guardada es la del cuadro de la esfera. */
    int orb_base, orb_pad, orb_size;
    bool win_full, in_sizemove, size_changed, close_hint_shown;
    volatile LONG desk_moved; /* la arrastraste: el caminar empieza desde ahí */
    WINDOWPLACEMENT win_place;

    HANDLE thread, wake;
    volatile LONG running, reconfig, resized;
    SRWLOCK hud_lock;

    SRWLOCK text_lock;
    wchar_t *sub_user, *sub_sokari, *status;
    double sub_time;

    volatile LONG state;
    volatile LONG level_milli;
    /* Ya le hablaste al menos una vez: antes de eso, "Aparecer solo cuando le
       hablas" no la esconde (al abrir Sokari se ve y se queda). */
    volatile LONG conversed;
} U = {.hud_lock = SRWLOCK_INIT, .text_lock = SRWLOCK_INIT};

/* ---------------------------------------------------------- puente app --- */

void app_set_state(JvState s)
{
    LONG prev = InterlockedExchange(&U.state, (LONG)s);
    if (U.wake) SetEvent(U.wake);
    if (!U.msg) return;
    if (s == JV_LISTENING) InterlockedExchange(&U.conversed, 1);
    if (s == JV_LISTENING && prev != JV_LISTENING) PostMessageW(U.msg, WM_APP_SHOWHUD, 2, 0);
    if (s != JV_IDLE && prev == JV_IDLE) PostMessageW(U.msg, WM_APP_SHOWHUD, 4, 0);
    if (s == JV_IDLE && prev != JV_IDLE) PostMessageW(U.msg, WM_APP_AUTOHIDE, 0, 0);
}

JvState app_get_state(void)
{
    return (JvState)InterlockedCompareExchange(&U.state, 0, 0);
}

void app_set_level(float level)
{
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    InterlockedExchange(&U.level_milli, (LONG)(level * 1000));
}

static void set_text(wchar_t **field, const char *text)
{
    AcquireSRWLockExclusive(&U.text_lock);
    free(*field);
    *field = text && *text ? utf8_to_wide(text) : NULL;
    U.sub_time = now_epoch();
    ReleaseSRWLockExclusive(&U.text_lock);
}

void app_subtitle(bool from_user, const char *text)
{
    if (from_user) {
        set_text(&U.sub_user, text);
        set_text(&U.sub_sokari, NULL);
    } else {
        set_text(&U.sub_sokari, text);
    }
}

void app_status(const char *text)
{
    set_text(&U.status, text);
}

void app_notify(const char *title, const char *text)
{
    if (!U.msg) {
        log_msg("[aviso] %s: %s", title, text);
        return;
    }
    wchar_t **pair = xmalloc(sizeof(wchar_t *) * 2);
    pair[0] = utf8_to_wide(title);
    pair[1] = utf8_to_wide(text);
    if (!PostMessageW(U.msg, WM_APP_NOTIFY, 0, (LPARAM)pair)) {
        free(pair[0]);
        free(pair[1]);
        free(pair);
    }
}

bool app_is_own_window(HWND h)
{
    return h && (h == U.hud || h == U.msg || h == settings_window());
}

/* En "Pantalla completa" la esfera está siempre encima: antes de que Sokari
   abra algo o mande teclas se aparta, para que se vea el resultado. Vuelve
   sola la próxima vez que le hables. */
void app_yield_focus(void)
{
    if (U.msg) SendMessageTimeoutW(U.msg, WM_APP_YIELD, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL);
}

void app_request_quit(void)
{
    if (U.msg) PostMessageW(U.msg, WM_APP_QUIT, 0, 0);
}

HWND ui_message_window(void)
{
    return U.msg;
}

/* Mientras el exe no traiga el modelo de "Hey Sokari", el atajo es la forma
   de hablarle y así lo dicen los textos. */
static const wchar_t *tray_tip(void)
{
    if (config_mic_muted()) return L"Sokari — micrófono silenciado";
    return res_has_wake_word() ? L"Sokari — escuchando \"Hey Sokari\"" : L"Sokari — Ctrl+Alt+J para hablarle";
}

HICON ui_app_icon(int size)
{
    return (HICON)LoadImageW(U.inst, MAKEINTRESOURCEW(IDI_SOKARI), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
}

/* ------------------------------------------------------------- ventana --- */

static bool windowed(int mode)
{
    return mode == DISPLAY_WINDOWED || mode == DISPLAY_MINIMIZED;
}

static void primary_monitor(RECT *out)
{
    HMONITOR m = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(m, &mi);
    *out = mi.rcMonitor;
}

static volatile LONG g_preview_style = -1;

void ui_preview_style(int style)
{
    LONG v = style >= 0 && style < SPHERE_STYLE_COUNT ? style : -1;
    if (InterlockedExchange(&g_preview_style, v) != v) ui_config_changed();
}

static void load_display_config(void)
{
    AppConfig c = config_snapshot();
    LONG preview = InterlockedCompareExchange(&g_preview_style, 0, 0);
    U.mode = c.display_mode;
    U.style = preview >= 0 ? (int)preview : c.sphere_style;
    U.anim = c.appear_anim;
    U.face_symbols = c.face_symbols;
    U.res = c.resolution;
    U.subtitles = c.subtitles;
    primary_monitor(&U.mon);
    int mh = U.mon.bottom - U.mon.top;
    U.orb_base = (int)(mh * ORB_FRACTION) & ~3;
    U.orb_pad = (int)(U.orb_base * (sphere_room((SphereStyle)U.style) - 1.0f) * 0.5f) & ~1;
    U.orb_size = U.orb_base + 2 * U.orb_pad;
    if (U.mode == DISPLAY_WINDOWED_BORDERLESS) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int x = c.orb_x, y = c.orb_y;
        if (x < 0 || y < 0) {
            x = wa.right - U.orb_base - mh / 40;
            y = wa.bottom - U.orb_base - mh / 40;
        }
        x -= U.orb_pad;
        y -= U.orb_pad;
        U.mon = (RECT){x, y, x + U.orb_size, y + U.orb_size};
    } else if (windowed(U.mode)) {
        RECT r = {c.win_x, c.win_y, c.win_x + c.win_w, c.win_y + c.win_h};
        if (c.win_w <= 0 || c.win_h <= 0 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            RECT wa;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int h = (wa.bottom - wa.top) * 3 / 5, w = h * 4 / 3;
            r.left = wa.left + (wa.right - wa.left - w) / 2;
            r.top = wa.top + (wa.bottom - wa.top - h) / 2;
            r.right = r.left + w;
            r.bottom = r.top + h;
        }
        U.mon = r;
    }
    config_free(&c);
}

/* Al mostrarla sin que la pidas (al arrancar, al cambiar de modo) nunca le
   quita el foco a lo que estás usando; en Minimizado queda en la barra de
   tareas. */
static int quiet_show_cmd(void)
{
    return U.mode == DISPLAY_MINIMIZED ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE;
}

/* Lo que se le pide al hilo de dibujo para entrar y salir. */
enum { AREQ_ENTER = 1, AREQ_ENTER_ZERO, AREQ_LEAVE, AREQ_TEST_SHOWN, AREQ_TEST_HIDDEN, AREQ_RESET };

static SphereAnim hud_anim(void)
{
    /* Minimizado vive en la barra de tareas: aparece y se va de golpe. */
    return U.mode == DISPLAY_MINIMIZED ? SPHERE_ANIM_NONE : (SphereAnim)U.anim;
}

static LONG anim_request(int req, SphereAnim anim)
{
    LONG seq = ++U.anim_seq & 0x3ffffff;
    InterlockedExchange(&U.anim_req, (seq << 5) | ((LONG)anim << 3) | req);
    SetEvent(U.wake);
    return seq;
}

/* La esfera entra: si se estaba yendo, regresa desde donde iba; si estaba
   oculta, desde fuera. Se llama antes de mostrar la ventana. */
static void begin_enter(void)
{
    SphereAnim an = hud_anim();
    bool was_leaving = InterlockedExchange(&U.leaving, 0) != 0;
    if (an == SPHERE_ANIM_NONE) anim_request(AREQ_RESET, an);
    else if (was_leaving) anim_request(AREQ_ENTER, an);
    else if (U.hud && !IsWindowVisible(U.hud)) anim_request(AREQ_ENTER_ZERO, an);
}

/* La esfera se va con su animación y la ventana se esconde al terminar
   (WM_APP_LEFT). Sin animación, o si no se ve, se esconde ya. */
static void begin_leave(void)
{
    if (U.hud && IsWindowVisible(U.hud) && !IsIconic(U.hud) && hud_anim() != SPHERE_ANIM_NONE) {
        InterlockedExchange(&U.leaving, 1);
        U.leave_seq = anim_request(AREQ_LEAVE, hud_anim());
    } else {
        InterlockedExchange(&U.leaving, 0);
        if (U.hud) ShowWindow(U.hud, SW_HIDE);
    }
}

static void create_hud(void)
{
    bool win = windowed(U.mode);
    DWORD ex = win ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (U.mode == DISPLAY_FULLSCREEN || U.mode == DISPLAY_WINDOWED_BORDERLESS) ex |= WS_EX_TOPMOST;
    if (U.mode == DISPLAY_WINDOWED_BORDERLESS) ex |= WS_EX_LAYERED;
    U.win_full = false;
    U.hud = CreateWindowExW(ex, HUD_CLASS, L"Sokari", win ? WS_OVERLAPPEDWINDOW : WS_POPUP, U.mon.left, U.mon.top,
                            U.mon.right - U.mon.left, U.mon.bottom - U.mon.top, NULL, NULL, U.inst, NULL);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(U.hud, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (InterlockedCompareExchange(&U.visible, 1, 1)) {
        begin_enter();
        ShowWindow(U.hud, quiet_show_cmd());
    }
}

static void rebuild_hud(void)
{
    AcquireSRWLockExclusive(&U.hud_lock);
    if (U.hud) DestroyWindow(U.hud);
    U.hud = NULL;
    InterlockedExchange(&U.leaving, 0);
    load_display_config();
    create_hud();
    InterlockedExchange(&U.reconfig, 1);
    ReleaseSRWLockExclusive(&U.hud_lock);
    SetEvent(U.wake);
}

void ui_config_changed(void)
{
    if (U.msg) PostMessageW(U.msg, WM_APP_CONFIG, 0, 0);
}

static void show_hud(bool show, HudShow how)
{
    InterlockedExchange(&U.visible, show ? 1 : 0);
    InterlockedExchange(&U.yielded, 0);
    if (U.hud) {
        bool front = how == HUD_SHOW_FRONT || (how == HUD_SHOW_STARTED && U.mode == DISPLAY_WINDOWED);
        if (!show) {
            begin_leave();
        } else {
            begin_enter();
            if (windowed(U.mode) && front) {
                ShowWindow(U.hud, IsIconic(U.hud) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(U.hud);
            } else if (!windowed(U.mode) || !IsWindowVisible(U.hud)) {
                ShowWindow(U.hud, quiet_show_cmd());
            }
        }
    }
    SetEvent(U.wake);
}

/* Minimizada cuenta como oculta: un clic en la bandeja la trae. */
static bool hud_shown(void)
{
    return InterlockedCompareExchange(&U.visible, 1, 1) && !(U.hud && windowed(U.mode) && IsIconic(U.hud));
}

static void toggle_hud(void)
{
    /* Antes de darle a Iniciar no hay nada que mostrar: la esfera no te
       escucharía. Se trae la ventana de Inicio. */
    if (!voice_running()) {
        home_open(U.inst, false, U.on_saved);
        return;
    }
    show_hud(!hud_shown(), HUD_SHOW_FRONT);
}

void ui_show_hud(HudShow how)
{
    if (U.msg) PostMessageW(U.msg, WM_APP_SHOWHUD, (WPARAM)how, 0);
}

bool ui_preview_appear(int anim)
{
    return U.msg && U.hud && U.mode != DISPLAY_MINIMIZED && PostMessageW(U.msg, WM_APP_ANIMTEST, (WPARAM)anim, 0);
}

void ui_set_display_mode(int mode)
{
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) return;
    config_set_display_mode(mode);
    rebuild_hud();
    settings_sync();
}

/* F11 (o doble clic) en modo Ventana: la misma ventana ocupa todo su monitor,
   como la pantalla completa de un navegador; F11 o Esc la regresan. Como
   tiene el foco, la tapa cualquier ventana que abras. No se guarda: Sokari
   siempre vuelve a abrir en ventana. */
static void toggle_full(HWND h)
{
    if (IsIconic(h)) return;
    if (!U.win_full) {
        U.win_place.length = sizeof U.win_place;
        GetWindowPlacement(h, &U.win_place);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        U.win_full = true;
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        U.win_full = false;
        SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(h, &U.win_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static void save_window_rect(HWND h)
{
    if (U.win_full || IsIconic(h) || IsZoomed(h)) return;
    RECT r;
    GetWindowRect(h, &r);
    config_set_window_rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/* En modo Ventana la esfera sí puede tener el foco: antes de que Sokari mande
   teclas se lo pasa a la ventana que sigue (la que usabas antes), para que lo
   que escriba no le llegue a la esfera. */
static void activate_next_window(void)
{
    for (HWND w = GetWindow(U.hud, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w) || IsIconic(w) || app_is_own_window(w)) continue;
        LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        if (cloaked) continue;
        wchar_t cls[32];
        GetClassNameW(w, cls, 32);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) continue;
        SetForegroundWindow(w);
        return;
    }
}

bool ui_click_on_sphere(int mode, int x, int y, int cw, int ch)
{
    if (mode == DISPLAY_WINDOWED_BORDERLESS) return true; /* la ventana es la esfera */
    if (cw <= 0 || ch <= 0) return false;
    double r = 0.45 * (cw < ch ? cw : ch), dx = x - cw / 2.0, dy = y - ch / 2.0;
    return dx * dx + dy * dy <= r * r;
}

bool ui_is_click(int dx, int dy)
{
    return abs(dx) <= 4 && abs(dy) <= 4;
}

/* Un clic en la esfera mientras habla o piensa la calla (y no dice lo que
   tenía pendiente). Quieta, un clic no hace nada: no se activa sin querer. */
bool ui_click_silences(int state)
{
    return state == JV_SPEAKING || state == JV_THINKING;
}

static void sphere_clicked(void)
{
    if (ui_click_silences(app_get_state())) voice_skip();
}

static POINT g_down; /* dónde se apretó el botón (pantalla) */

static LRESULT CALLBACK hud_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_LBUTTONDOWN:
        GetCursorPos(&g_down);
        break;
    case WM_LBUTTONUP: {
        POINT up;
        GetCursorPos(&up);
        RECT rc;
        GetClientRect(h, &rc);
        if (ui_is_click(up.x - g_down.x, up.y - g_down.y) &&
            ui_click_on_sphere(U.mode, (short)LOWORD(l), (short)HIWORD(l), rc.right, rc.bottom))
            sphere_clicked();
        break;
    }
    case WM_NCLBUTTONDOWN:
        /* La esfera flotante se arrastra desde cualquier parte: Windows se queda
           en su ciclo de mover hasta que sueltas el botón. Si casi no se movió,
           fue un clic. */
        if (U.mode == DISPLAY_WINDOWED_BORDERLESS && w == HTCAPTION) {
            POINT a, b;
            GetCursorPos(&a);
            LRESULT r = DefWindowProcW(h, m, w, l);
            GetCursorPos(&b);
            if (ui_is_click(b.x - a.x, b.y - a.y)) sphere_clicked();
            return r;
        }
        break;
    case WM_MOUSEACTIVATE:
        if (!windowed(U.mode)) return MA_NOACTIVATE;
        break;
    case WM_NCHITTEST:
        if (U.mode == DISPLAY_WINDOWED_BORDERLESS) return HTCAPTION;
        break;
    case WM_NCLBUTTONDBLCLK:
        if (windowed(U.mode)) break; /* doble clic en la barra de título: maximizar */
        show_hud(false, HUD_SHOW_QUIET);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (windowed(U.mode)) toggle_full(h);
        else show_hud(false, HUD_SHOW_QUIET);
        return 0;
    case WM_KEYDOWN:
        if (windowed(U.mode) && (w == VK_F11 || (w == VK_ESCAPE && U.win_full))) toggle_full(h);
        return 0;
    case WM_NCRBUTTONUP:
        if (windowed(U.mode)) break; /* clic derecho en la barra de título: menú de la ventana */
        tray_show_menu(U.msg, true, config_mic_muted(), U.mode);
        return 0;
    case WM_RBUTTONUP:
        tray_show_menu(U.msg, true, config_mic_muted(), U.mode);
        return 0;
    case WM_ENTERSIZEMOVE:
        U.in_sizemove = true;
        U.size_changed = false;
        break;
    case WM_EXITSIZEMOVE: {
        U.in_sizemove = false;
        if (U.mode == DISPLAY_WINDOWED_BORDERLESS) {
            RECT r;
            GetWindowRect(h, &r);
            config_set_orb_pos(r.left + U.orb_pad, r.top + U.orb_pad);
            InterlockedExchange(&U.desk_moved, 1);
        } else if (windowed(U.mode)) {
            save_window_rect(h);
            if (U.size_changed) {
                InterlockedExchange(&U.reconfig, 1);
                SetEvent(U.wake);
            }
        }
        return 0;
    }
    case WM_SIZE:
        /* Mientras arrastras el borde solo se ajusta el lienzo; la esfera se
           vuelve a armar a su nuevo tamaño cuando sueltas. */
        if (windowed(U.mode) && w != SIZE_MINIMIZED) {
            if (U.in_sizemove) U.size_changed = true;
            InterlockedExchange(U.in_sizemove ? &U.resized : &U.reconfig, 1);
            SetEvent(U.wake);
        }
        break;
    case WM_GETMINMAXINFO:
        if (windowed(U.mode)) {
            MINMAXINFO *mm = (MINMAXINFO *)l;
            int dpi = (int)GetDpiForWindow(h);
            mm->ptMinTrackSize.x = MulDiv(320, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(240, dpi, 96);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (windowed(U.mode) && !U.win_full) {
            RECT *r = (RECT *)l;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_CLOSE:
        /* La X oculta: Sokari sigue escuchando. Se cierra desde la bandeja. */
        show_hud(false, HUD_SHOW_QUIET);
        if (windowed(U.mode) && !U.close_hint_shown) {
            U.close_hint_shown = true;
            tray_notify(L"Sokari", L"Sigo escuchando aquí en la bandeja. Para cerrarme: clic derecho en el ícono > Salir.");
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (U.mode != DISPLAY_WINDOWED_BORDERLESS) FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        SetEvent(U.wake);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------- render --- */

typedef struct {
    HDC dc;
    HBITMAP bmp, old;
    uint32_t *px;
    int w, h;
} Surface;

static void surface_free(Surface *s)
{
    if (s->dc) {
        SelectObject(s->dc, s->old);
        DeleteObject(s->bmp);
        DeleteDC(s->dc);
    }
    memset(s, 0, sizeof *s);
}

static bool surface_alloc(Surface *s, int w, int h)
{
    surface_free(s);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    s->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->bmp) return false;
    s->dc = CreateCompatibleDC(NULL);
    s->old = SelectObject(s->dc, s->bmp);
    s->px = bits;
    s->w = w;
    s->h = h;
    memset(bits, 0, sizeof(uint32_t) * (size_t)w * h);
    return true;
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
}

static void draw_text_block(HDC dc, const wchar_t *text, RECT *area, HFONT font, COLORREF color, int max_lines)
{
    SelectObject(dc, font);
    RECT calc = *area;
    DrawTextW(dc, text, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int max_h = tm.tmHeight * max_lines;
    int h = calc.bottom - calc.top;
    if (h > max_h) h = max_h;
    RECT r = {area->left, area->bottom - h, area->right, area->bottom};
    RECT shadow = r;
    OffsetRect(&shadow, 0, 2);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, text, -1, &shadow, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
    area->bottom = r.top - tm.tmHeight / 3;
}

static void draw_overlay(Surface *back, HFONT big, HFONT small, HFONT status_font)
{
    int W = back->w, H = back->h;
    RECT area = {W / 8, (int)(H * 0.70), W - W / 8, (int)(H * 0.955)};
    AcquireSRWLockShared(&U.text_lock);
    bool fresh = now_epoch() - U.sub_time < SUBTITLE_SECONDS || InterlockedCompareExchange(&U.state, 0, 0) != JV_IDLE;
    SetBkMode(back->dc, TRANSPARENT);
    if (U.status && *U.status) draw_text_block(back->dc, U.status, &(RECT){area.left, area.top, area.right, (int)(H * 0.975)}, status_font, RGB(0x9d, 0x95, 0xff), 1);
    if (U.subtitles && fresh) {
        if (U.sub_sokari) draw_text_block(back->dc, U.sub_sokari, &area, big, RGB(0xec, 0xe8, 0xff), 3);
        if (U.sub_user) draw_text_block(back->dc, U.sub_user, &area, small, RGB(0x8f, 0x93, 0xb3), 2);
    }
    ReleaseSRWLockShared(&U.text_lock);
}

static void target_params(JvState st, SphereParams *out)
{
    switch (st) {
    case JV_SPEAKING:
        *out = SPHERE_SPEAK;
        break;
    case JV_THINKING:
        sphere_lerp(out, &SPHERE_IDLE, &SPHERE_SPEAK, 0.5f);
        out->rotation_speed = 0.55f;
        break;
    case JV_LISTENING:
        *out = SPHERE_IDLE;
        out->rotation_speed = 0.22f;
        out->ripple = 0.30f;
        out->glow = 10.0f;
        break;
    default:
        *out = SPHERE_IDLE;
    }
}

static DWORD WINAPI render_main(LPVOID arg)
{
    SphereRenderer *sr = NULL;
    Face *face = NULL; /* la cara (beta): gestos y parpadeos, de cuadro en cuadro */
    Surface sphere = {0}, back = {0};
    HFONT big = NULL, small = NULL, status_font = NULL;
    SphereParams cur = SPHERE_IDLE;
    double angle = 0, voice_t = 0, env = 0, pulse_env = 0, pulse_peak = 0;
    int sphere_base = 0; /* lado para el que está pensada la esfera; el lienzo puede ser más grande */
    LARGE_INTEGER freq, last, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;
    InterlockedExchange(&U.reconfig, 1);
    int frame = 0;
    /* Entrar y salir de la pantalla. Deslizarse mueve la esfera: la flotante
       mueve su ventana desde home; en las demás baja dentro del cuadro. */
    SphereAppear ap;
    sphere_appear_init(&ap);
    LONG ap_seq = 0;
    bool slid = false, back_moved = false;
    POINT home = {0, 0};
    /* Moverse sola por el escritorio (esfera flotante). */
    DeskWalker walker;
    DeskView view;
    DeskStep dstep = {0};
    bool walker_ready = false, was_walking = false;
    double last_busy = 0, last_view = -1;
    int home_bottom = 0;

    while (InterlockedCompareExchange(&U.running, 1, 1)) {
        LONG rq = InterlockedExchange(&U.anim_req, 0);
        if (rq) {
            SphereAnim an = (SphereAnim)((rq >> 3) & 3);
            ap_seq = rq >> 5;
            switch (rq & 7) {
            case AREQ_ENTER: sphere_appear_enter(&ap, an, false); break;
            case AREQ_ENTER_ZERO: sphere_appear_enter(&ap, an, true); break;
            case AREQ_LEAVE: sphere_appear_leave(&ap, an); break;
            case AREQ_TEST_SHOWN: sphere_appear_test(&ap, an, true); break;
            case AREQ_TEST_HIDDEN: sphere_appear_test(&ap, an, false); break;
            default: sphere_appear_init(&ap); break;
            }
        }
        AcquireSRWLockShared(&U.hud_lock);
        HWND hud = U.hud;
        /* Oculta, apartada o minimizada: no se dibuja nada (salvo mientras se va). */
        bool leaving = InterlockedCompareExchange(&U.leaving, 1, 1) != 0;
        bool shown = InterlockedCompareExchange(&U.visible, 1, 1) && !InterlockedCompareExchange(&U.yielded, 1, 1);
        if ((!shown && !leaving) || (hud && windowed(U.mode) && IsIconic(hud))) {
            ReleaseSRWLockShared(&U.hud_lock);
            WaitForSingleObject(U.wake, 300);
            QueryPerformanceCounter(&last);
            continue;
        }
        bool full_reconfig = InterlockedExchange(&U.reconfig, 0) || !sr;
        if (InterlockedExchange(&U.resized, 0) || full_reconfig) {
            int W = U.mon.right - U.mon.left, H = U.mon.bottom - U.mon.top;
            if (windowed(U.mode) && hud) {
                RECT rc;
                GetClientRect(hud, &rc);
                W = rc.right > 1 ? rc.right : 1;
                H = rc.bottom > 1 ? rc.bottom : 1;
            }
            if (full_reconfig) {
                /* Lo que mide el lado del cuadro donde va la esfera (el alto,
                   salvo en una ventana o un monitor más altos que anchos). */
                int side = W < H ? W : H;
                bool orb_mode = U.mode == DISPLAY_WINDOWED_BORDERLESS;
                int base = orb_mode ? U.orb_base : U.res > 0 ? U.res : side;
                if (base > 2160) base = 2160;
                /* Las líneas necesitan más lienzo que la esfera misma (ver
                   sphere_room): la esfera se ve del mismo tamaño y lo que
                   sobra queda para las ondas al hablar. */
                int canvas = orb_mode ? U.orb_size : (int)(base * sphere_room((SphereStyle)U.style));
                sphere_destroy(sr);
                sr = sphere_create_fit(canvas, (float)base / (float)canvas);
                canvas = sphere_size(sr);
                surface_alloc(&sphere, canvas, canvas);
                sphere_base = base;
                slid = back_moved = false;
            }
            if (U.mode == DISPLAY_WINDOWED_BORDERLESS) {
                surface_free(&back);
            } else {
                surface_alloc(&back, W, H);
                if (big) DeleteObject(big), DeleteObject(small), DeleteObject(status_font);
                big = make_font(H / 34 > 15 ? H / 34 : 15, 350);
                small = make_font(H / 46 > 12 ? H / 46 : 12, FW_NORMAL);
                status_font = make_font(H / 54 > 11 ? H / 54 : 11, FW_NORMAL);
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        if (dt > 0.1) dt = 0.1;
        last = now;
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;

        JvState st = (JvState)InterlockedCompareExchange(&U.state, 0, 0);
        SphereParams target;
        target_params(st, &target);
        sphere_lerp(&cur, &cur, &target, (float)(1.0 - exp(-dt / 0.25)));
        double level = InterlockedCompareExchange(&U.level_milli, 0, 0) / 1000.0;
        double k = level > env ? 1.0 - exp(-dt / 0.03) : 1.0 - exp(-dt / 0.18);
        env += (level - env) * k;
        float voice = st == JV_SPEAKING ? (float)fmin(1.0, env * 1.6) : st == JV_LISTENING ? (float)fmin(1.0, env * 0.8) : 0.0f;
        /* El pulso sigue cada sílaba: sube tan rápido como la voz y baja en
           menos de una décima de segundo, así la esfera late al hablar. */
        double kp = level > pulse_env ? 1.0 - exp(-dt / 0.025) : 1.0 - exp(-dt / 0.09);
        pulse_env += (level - pulse_env) * kp;
        /* Relativo a lo fuerte que viene hablando: late igual con el volumen
           bajo o con una voz más suave. */
        pulse_peak = fmax(pulse_env, pulse_peak * exp(-dt / 1.5));
        float pulse = st == JV_SPEAKING ? (float)fmin(1.0, pulse_env / fmax(0.3, pulse_peak)) : 0.0f;
        angle += cur.rotation_speed * dt * (1.0 + voice * 0.8);
        voice_t += dt * (1.0 + 2.5 * voice);

        bool gone = sphere_appear_step(&ap, dt);
        bool slide = ap.anim == SPHERE_ANIM_SLIDE && ap.presence < 1.0f;
        bool orb = U.mode == DISPLAY_WINDOWED_BORDERLESS;
        sphere_set_presence(sr, ap.anim, ap.presence);
        /* Sola por el escritorio: después de un rato sin hablarle, si la
           dejas, y nunca mientras habla, escucha o la arrastras. */
        bool desk_moved = false;
        if (st != JV_IDLE) last_busy = t;
        if (orb && hud && !slide && !slid && IsWindowVisible(hud)) {
            if (!walker_ready || InterlockedExchange(&U.desk_moved, 0)) {
                RECT wr;
                GetWindowRect(hud, &wr);
                if (!walker_ready) desk_init(&walker, wr.left, wr.top, sphere.w, sphere_base, (unsigned)GetTickCount());
                else desk_place(&walker, wr.left, wr.top);
                walker_ready = true;
            }
            if (t - last_view >= 0.15) {
                desk_view_windows(&view, hud);
                last_view = t;
            }
            bool allowed = config_desk_move() && !desk_held() && !U.in_sizemove && st == JV_IDLE &&
                           ap.presence >= 1.0f && t - last_busy >= config_desk_after();
            desk_moved = desk_step(&walker, &view, dt, allowed, &dstep);
            /* Donde se queda es donde la encuentras la próxima vez. */
            if (was_walking && !dstep.moving) config_set_orb_pos(dstep.x + U.orb_pad, dstep.y + U.orb_pad);
            was_walking = dstep.moving;
        } else {
            dstep.moving = false;
        }
        /* La cara (beta): su pose y sus colores salen del estado afectivo. */
        SphereParams draw = cur;
        if (sphere_style_is_face((SphereStyle)U.style)) {
            if (!face) face = face_create((unsigned)GetTickCount());
            FaceInput in;
            memset(&in, 0, sizeof in);
            in.activity = (FaceActivity)st; /* mismo orden que JvState */
            in.voice = voice;
            in.pulse = pulse;
            in.affect = affect_get();
            in.cue = affect_last_cue(&in.cue_seq);
            in.level = FACE_LEVEL_HIGH; /* siempre «mucho» */
            in.symbols = U.face_symbols;
            in.look_x = walker_ready ? dstep.look_x : 0;
            in.look_y = walker_ready ? dstep.look_y : 0;
            in.walking = dstep.moving;
            SphereFace pose;
            face_step(face, dt, &in, &pose);
            face_sphere_colors(&in.affect, &cur, &draw);
            sphere_set_face(sr, &pose);
        } else {
            sphere_set_face(sr, NULL);
        }
        sphere_render(sr, t, angle, voice_t, &draw, voice, pulse, (SphereStyle)U.style, sphere.px, sphere.w, orb);

        if (hud && orb) {
            SIZE sz = {sphere.w, sphere.h};
            POINT src = {0, 0}, pos = {0, 0}, *at = NULL;
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            if (slide || slid) {
                /* La ventana baja hasta salir por abajo de su monitor; al
                   terminar de entrar vuelve exacto a donde la dejaste. */
                if (!slid) {
                    RECT wr;
                    GetWindowRect(hud, &wr);
                    home = (POINT){wr.left, wr.top};
                    MONITORINFO mi = {sizeof mi};
                    GetMonitorInfoW(MonitorFromPoint(home, MONITOR_DEFAULTTONEAREST), &mi);
                    home_bottom = mi.rcMonitor.bottom;
                }
                float off = slide ? sphere_slide_offset(ap.presence) : 0.0f;
                pos = (POINT){home.x, home.y + (int)lroundf(off * (float)(home_bottom - home.y))};
                at = &pos;
                slid = slide;
            } else if (desk_moved) {
                pos = (POINT){dstep.x, dstep.y};
                at = &pos;
            }
            UpdateLayeredWindow(hud, NULL, at, &sz, sphere.dc, &src, 0, &bf, ULW_ALPHA);
        } else if (hud && back.dc) {
            int W = back.w, H = back.h;
            int band = (int)(H * 0.68);
            memset(back.px + (size_t)band * W, 0, sizeof(uint32_t) * (size_t)(H - band) * W);
            int side = H < W ? H : W;
            /* Si el lienzo es más grande que la esfera (líneas), se dibuja más
               grande en la misma proporción: la esfera queda del tamaño de
               siempre y sus ondas pueden llegar al borde de la pantalla. */
            int dst = sphere_base > 0 ? MulDiv(side, sphere.w, sphere_base) : side;
            /* Deslizarse: sube desde abajo del borde de la pantalla. Lo que
               deja atrás se borra. */
            int off = slide ? (int)lroundf(sphere_slide_offset(ap.presence) * (float)(H + dst) * 0.5f) : 0;
            if (off || back_moved) memset(back.px, 0, sizeof(uint32_t) * (size_t)W * H);
            back_moved = off != 0;
            int x0 = (W - dst) / 2, y0 = (H - dst) / 2 + off;
            if (sphere.w == dst) {
                BitBlt(back.dc, x0, y0, dst, dst, sphere.dc, 0, 0, SRCCOPY);
            } else {
                SetStretchBltMode(back.dc, HALFTONE);
                SetBrushOrgEx(back.dc, 0, 0, NULL);
                StretchBlt(back.dc, x0, y0, dst, dst, sphere.dc, 0, 0, sphere.w, sphere.h, SRCCOPY);
            }
            draw_overlay(&back, big, small, status_font);
            HDC dc = GetDC(hud);
            if (dc) {
                BitBlt(dc, 0, 0, W, H, back.dc, 0, 0, SRCCOPY);
                ReleaseDC(hud, dc);
            }
        }
        ReleaseSRWLockShared(&U.hud_lock);
        if (gone) PostMessageW(U.msg, WM_APP_LEFT, (WPARAM)ap_seq, 0);

        /* Sincronizado con el monitor; en reposo a la mitad de cuadros (el
           movimiento es lento, no se nota, y ahorra batería), salvo al entrar
           o salir. */
        DwmFlush();
        if (st == JV_IDLE && !sphere_appear_moving(&ap) && (++frame & 1)) DwmFlush();
    }
    sphere_destroy(sr);
    face_destroy(face);
    surface_free(&sphere);
    surface_free(&back);
    if (big) DeleteObject(big), DeleteObject(small), DeleteObject(status_font);
    return 0;
}

/* ------------------------------------------------- ventana de mensajes --- */

static void open_data_folder(void)
{
    ShellExecuteW(NULL, L"open", g_paths.local_dir, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == U.taskbar_created && m) {
        tray_readd();
        return 0;
    }
    switch (m) {
    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            toggle_hud();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            tray_show_menu(h, hud_shown(), config_mic_muted(), U.mode);
            break;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= IDM_MODE_BASE && LOWORD(w) < IDM_MODE_BASE + DISPLAY_MODE_COUNT) {
            ui_set_display_mode(LOWORD(w) - IDM_MODE_BASE);
            /* Elegido desde la bandeja: que se vea cómo quedó. */
            if (voice_running()) show_hud(true, HUD_SHOW_QUIET);
            return 0;
        }
        switch (LOWORD(w)) {
        case IDM_TALK:
            voice_trigger();
            break;
        case IDM_TOGGLE_HUD:
            toggle_hud();
            break;
        case IDM_MUTE: {
            bool muted = !config_mic_muted();
            config_set_mic_muted(muted);
            tray_set_tooltip(tray_tip());
            tray_notify(L"Sokari", muted ? L"Micrófono silenciado. Sokari no escucha hasta que lo actives."
                   : res_has_wake_word() ? L"Micrófono activado. Di \"Hey Sokari\" cuando quieras."
                                         : L"Micrófono activado. Háblame con Ctrl+Alt+J.");
            settings_sync();
            break;
        }
        case IDM_FULL_ACCESS: {
            bool on = !config_full_access();
            config_set_full_access(on);
            tray_notify(L"Sokari", on ? L"Acceso completo: ya no te pregunto nada, salvo antes de borrar."
                                      : L"Vuelvo a pedirte permiso antes de acciones delicadas.");
            settings_sync();
            break;
        }
        case IDM_SETTINGS:
            settings_open(U.inst, false, U.on_saved);
            break;
        case IDM_HOME:
            home_open(U.inst, false, U.on_saved);
            break;
        case IDM_OPEN_DATA:
            open_data_folder();
            break;
        case IDM_QUIT:
            PostMessageW(h, WM_APP_QUIT, 0, 0);
            break;
        }
        return 0;
    case WM_HOTKEY:
        if (w == HOTKEY_TALK) voice_trigger();
        return 0;
    case WM_APP_NOTIFY: {
        wchar_t **pair = (wchar_t **)l;
        tray_notify(pair[0], pair[1]);
        free(pair[0]);
        free(pair[1]);
        free(pair);
        return 0;
    }
    case WM_APP_YIELD:
        if (U.mode == DISPLAY_FULLSCREEN && InterlockedCompareExchange(&U.visible, 1, 1) && U.hud) {
            InterlockedExchange(&U.yielded, 1);
            begin_leave();
        } else if (windowed(U.mode) && U.hud && IsWindowVisible(U.hud)) {
            /* Si tenía el foco se lo pasa a tu ventana; y si estaba encima de
               ella (por ejemplo en F11), se pone detrás para que veas lo que
               Sokari escribe o abre. */
            if (GetForegroundWindow() == U.hud) activate_next_window();
            HWND fg = GetForegroundWindow();
            if (fg && fg != U.hud) SetWindowPos(U.hud, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    case WM_APP_SHOWHUD:
        /* 2: empezó a escucharte; 4: empezó a escuchar, pensar o hablar (con
           "Aparecer solo cuando le hablas" es cuando la esfera aparece); si no,
           un HudShow. */
        if (w == 4) {
            KillTimer(h, TIMER_AUTOHIDE);
            if (config_show_only_talking() && U.mode != DISPLAY_MINIMIZED && !hud_shown())
                show_hud(true, HUD_SHOW_QUIET);
        } else if (w == 2) {
            bool vis = InterlockedCompareExchange(&U.visible, 1, 1);
            if (InterlockedExchange(&U.yielded, 0) && U.hud && vis) {
                begin_enter();
                ShowWindow(U.hud, SW_SHOWNOACTIVATE);
            }
            bool raise = U.mode == DISPLAY_FULLSCREEN_BORDERLESS || (U.mode == DISPLAY_WINDOWED && !IsIconic(U.hud));
            if (U.hud && vis && raise)
                SetWindowPos(U.hud, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetEvent(U.wake);
        } else {
            show_hud(true, (HudShow)w);
        }
        return 0;
    case WM_APP_AUTOHIDE:
        if (config_show_only_talking() && U.mode != DISPLAY_MINIMIZED && InterlockedCompareExchange(&U.conversed, 1, 1))
            SetTimer(h, TIMER_AUTOHIDE, AUTOHIDE_MS, NULL);
        return 0;
    case WM_TIMER:
        if (w == TIMER_AUTOHIDE) {
            KillTimer(h, TIMER_AUTOHIDE);
            if (config_show_only_talking() && U.mode != DISPLAY_MINIMIZED && app_get_state() == JV_IDLE && hud_shown())
                show_hud(false, HUD_SHOW_QUIET);
        }
        return 0;
    case WM_APP_LEFT:
        /* Ya terminó de irse: ahora sí se esconde, si nadie la volvió a
           llamar mientras se iba. */
        if ((LONG)w == U.leave_seq && InterlockedCompareExchange(&U.leaving, 0, 1) == 1 && U.hud)
            ShowWindow(U.hud, SW_HIDE);
        return 0;
    case WM_APP_CAPTURE: {
        /* 0x11 = WDA_EXCLUDEFROMCAPTURE (Windows 10 2004 en adelante). */
        DWORD pid = GetCurrentProcessId();
        for (HWND x = GetTopWindow(NULL); x; x = GetWindow(x, GW_HWNDNEXT)) {
            DWORD wp = 0;
            GetWindowThreadProcessId(x, &wp);
            if (wp == pid && IsWindowVisible(x)) SetWindowDisplayAffinity(x, w ? 0x11 : WDA_NONE);
        }
        return 0;
    }
    case WM_APP_ANIMTEST: {
        /* Probar (Configuración): con la animación elegida, aunque no esté
           guardada todavía. En pantalla: sale y vuelve. Oculta: se asoma y
           se va. */
        SphereAnim an = (SphereAnim)w;
        if (!U.hud || U.mode == DISPLAY_MINIMIZED || an >= SPHERE_ANIM_NONE || IsIconic(U.hud)) return 0;
        if (IsWindowVisible(U.hud) && !InterlockedCompareExchange(&U.leaving, 1, 1)) {
            anim_request(AREQ_TEST_SHOWN, an);
        } else {
            InterlockedExchange(&U.leaving, 1);
            U.leave_seq = anim_request(AREQ_TEST_HIDDEN, an);
            if (!IsWindowVisible(U.hud)) ShowWindow(U.hud, quiet_show_cmd());
        }
        return 0;
    }
    case WM_APP_SETTINGS:
        settings_open(U.inst, false, U.on_saved);
        return 0;
    case WM_APP_HOME:
        home_open(U.inst, false, U.on_saved);
        return 0;
    case WM_APP_CONFIG:
        rebuild_hud();
        return 0;
    case WM_APP_QUIT:
        tray_set_tooltip(L"Sokari — cerrando…");
        InterlockedExchange(&U.visible, 0);
        InterlockedExchange(&U.leaving, 0);
        if (U.hud) ShowWindow(U.hud, SW_HIDE);
        voice_stop();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_TALK);
        tray_remove();
        InterlockedExchange(&U.running, 0);
        SetEvent(U.wake);
        WaitForSingleObject(U.thread, 3000);
        if (U.hud) DestroyWindow(U.hud);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool ui_init(HINSTANCE inst, SettingsSavedFn on_saved, bool show)
{
    U.inst = inst;
    U.on_saved = on_saved;
    U.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    U.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = msg_proc;
    wc.hInstance = inst;
    wc.lpszClassName = SOKARI_MSG_CLASS;
    wc.hIcon = ui_app_icon(32);
    RegisterClassExW(&wc);
    WNDCLASSEXW hc = {sizeof hc};
    hc.style = CS_DBLCLKS;
    hc.lpfnWndProc = hud_proc;
    hc.hInstance = inst;
    hc.lpszClassName = HUD_CLASS;
    hc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    hc.hIcon = ui_app_icon(32);
    hc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&hc);

    U.msg = CreateWindowExW(0, SOKARI_MSG_CLASS, L"Sokari", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!U.msg) return false;
    tray_init(U.msg, ui_app_icon(GetSystemMetrics(SM_CXSMICON)));
    tray_set_tooltip(tray_tip());
    if (!RegisterHotKey(U.msg, HOTKEY_TALK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'J'))
        log_msg("No pude registrar el atajo Ctrl+Alt+J (otra app lo usa).");

    InterlockedExchange(&U.visible, show ? 1 : 0);
    load_display_config();
    create_hud();
    InterlockedExchange(&U.running, 1);
    U.thread = CreateThread(NULL, 0, render_main, NULL, 0, NULL);
    SetThreadPriority(U.thread, THREAD_PRIORITY_BELOW_NORMAL);
    return true;
}

int ui_run(void)
{
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        HWND sw = settings_window();
        if (sw && IsDialogMessageW(sw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

void ui_capture_exclude(bool on)
{
    if (!U.msg) return;
    DWORD_PTR res;
    SendMessageTimeoutW(U.msg, WM_APP_CAPTURE, on, 0, SMTO_BLOCK, 2000, &res);
    Sleep(on ? 80 : 0); /* que el escritorio ya la dibuje sin ella */
}

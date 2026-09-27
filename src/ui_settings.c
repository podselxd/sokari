/* Ventana de Configuración: tema oscuro propio (inspirado en los ajustes de
   Google), dibujada a mano con bordes suavizados — nada de controles grises
   de Windows 95. Barra lateral con secciones; los campos de texto son EDIT
   nativos (IME, portapapeles, deshacer) metidos dentro de cajas dibujadas. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "autostart.h"
#include "config.h"
#include "log.h"
#include "memory.h"
#include "mesh.h"
#include "resources.h"
#include "skills.h"
#include "sounds.h"
#include "tts.h"
#include "ui.h"
#include "update.h"
#include "util.h"
#include "voice.h"

#define SETTINGS_CLASS L"SokariSettings"
#define WM_APP_ASYNC_DONE (WM_APP + 40)

#define C_BG RGB(0x13, 0x13, 0x18)
#define C_SIDEBAR RGB(0x18, 0x18, 0x1f)
#define C_SURFACE RGB(0x21, 0x21, 0x2a)
#define C_BORDER RGB(0x36, 0x36, 0x44)
#define C_TEXT RGB(0xec, 0xec, 0xf2)
#define C_MUTED RGB(0x9b, 0x9b, 0xab)
#define C_ACCENT RGB(0x7b, 0x6c, 0xff)
#define C_ACCENT2 RGB(0xff, 0x3e, 0xc9)
#define C_NAV_SEL RGB(0x28, 0x26, 0x3a)
#define C_LINK RGB(0x9d, 0x92, 0xff)

enum { SEC_HOME, SEC_ACCOUNT, SEC_DISPLAY, SEC_AUDIO, SEC_GENERAL, SEC_SKILLS, SEC_DEVICES, SEC_AI, SEC_COUNT };
static const wchar_t *SECTION_NAMES[SEC_COUNT] = {L"Inicio",  L"Cuenta", L"Pantalla",     L"Voz y audio",
                                                  L"General", L"Skills", L"Dispositivos", L"IA de respaldo"};

typedef enum {
    W_LABEL,
    W_HELP,
    W_EDIT,
    W_SEGMENT,
    W_CARDS,
    W_SLIDER,
    W_TOGGLE,
    W_DROPDOWN,
    W_LINK,
    W_BUTTON,
} WType;

enum {
    F_API, F_NAME, F_PROFILE_PW, F_STOP, F_MESH, F_NVIDIA, F_DEEPSEEK, F_OPENROUTER, F_GLM, F_AI_ORDER, F_CITY,
    F_EDIT_COUNT,
};

enum {
    A_NONE, A_GROQ_LINK, A_RESET_PW, A_SHOW_API, A_SHOW_STOP, A_TAILSCALE, A_COPY_SECRET, A_OBSIDIAN, A_PICK_SOUND,
    A_CLEAR_SOUND, A_OPEN_FOLDER, A_CHECK_UPDATE, A_SAVE, A_CANCEL, A_START, A_GO_SETTINGS, A_MUTE, A_TEST_AUDIO,
    A_QUIT, A_MODE_CHANGED, A_OUTPUT_CHANGED, A_TEST_VOICE, A_FIREWALL, A_DETECT, A_DIAGNOSE, A_FULL_ACCESS,
    A_SHOW_MESH, A_OPEN_SKILLS, A_NEW_SKILL, A_TEST_ANIM, A_STYLE,
    /* Por dispositivo de la lista: + su número. */
    A_DEV_PROBE = 200, A_DEV_REMOVE = 300,
    /* Por IA de respaldo (0 NVIDIA, 1 DeepSeek, 2 OpenRouter, 3 GLM): ver la key y dónde sacarla. */
    A_SHOW_KEY = 400, A_KEY_LINK = 450,
};
#define MAX_DEVICE_ROWS 5

typedef struct {
    WType type;
    RECT r;
    const wchar_t *text;
    int *value;        /* segment/cards/slider/toggle/dropdown */
    int options;       /* número de opciones */
    const wchar_t *const *labels;
    const wchar_t *const *descs;
    int action;        /* botón/enlace; en una lista, se llama al elegir */
    int edit;          /* índice de F_* para W_EDIT */
    int item_h;        /* tarjetas: alto de cada una (0 = CARD_H) */
    bool primary;
} Widget;

static struct {
    HWND hwnd;
    HINSTANCE inst;
    bool first_run;
    bool home_mode;     /* abierta como ventana de Inicio: Guardar/Cancelar regresan a Inicio */
    bool quit_on_close; /* primera vez, o Inicio antes de arrancar: cerrarla cierra Sokari */
    SettingsSavedFn on_saved;
    int section;
    int dpi;
    HFONT f_title, f_section, f_body, f_small, f_nav, f_button;
    HBRUSH surface_brush;
    HWND edits[F_EDIT_COUNT];
    Widget widgets[48];
    int nwidgets;
    int ncontent; /* los de la sección, sin Guardar ni Cancelar */
    int hover, pressed, drag;
    AppConfig cfg;
    int autostart;
    int subtitles;
    int show_only_talking;
    int face_level, face_symbols; /* la cara: qué tanto se le nota y los símbolos */
    int full_access; /* acceso completo: no pregunta nada salvo antes de borrar */
    int skill_on[16]; /* las skills locales prendidas (en el orden de skills_get) */
    TtsVoice *voices;
    int nvoices;
    int voice_index;
    char **mics;
    int nmics;
    int mic_index;
    char **outputs;
    int nouts;
    int output_index;
    int home_display, home_output, home_full; /* lo que está en uso ahora (Inicio lo aplica al momento) */
    int display_mode, resolution_index, style;
    int appear; /* cómo entra y sale la esfera (SphereAnim) */
    int volume, sensitivity;
    int end_silence, duck; /* cuánto espera cuando te callas; bajar el volumen mientras te escucha */
    wchar_t *status_line;
    bool api_visible, stop_visible, mesh_visible, key_visible[4];
    MeshDevice *devs; /* tus dispositivos, releídos cada vez que se arma la sección */
    int ndevs;
    wchar_t dev_text[MAX_DEVICE_ROWS][200];
    uint32_t *px;
    HDC mem;
    HBITMAP bmp, old_bmp;
    int bw, bh;
} S;

static const int RESOLUTIONS[] = {0, 720, 1080, 1440, 2160};
static const wchar_t *RES_LABELS[] = {L"Automática", L"720p", L"1080p", L"1440p", L"4K"};
const wchar_t *const DISPLAY_MODE_LABELS[DISPLAY_MODE_COUNT] = {
    L"Pantalla completa", L"Pantalla completa sin bordes", L"Esfera flotante", L"Ventana", L"Minimizado",
};
static const wchar_t *const MODE_DESCS[DISPLAY_MODE_COUNT] = {
    L"Siempre encima de todo. Se aparta sola cuando Sokari abre algo.",
    L"Tus ventanas pueden ir encima; sale al frente cuando le hablas.",
    L"Una esfera transparente, siempre visible. Arrástrala a donde quieras.",
    L"Una ventana normal: muévela, agrándala. F11 = pantalla completa.",
    L"Como Ventana, pero arranca minimizada y no se asoma al hablarle.",
};
#define CARD_H 56
#define CARD_GAP 6
static const wchar_t *STYLE_LABELS[] = {L"Halo de puntos", L"Líneas (beta)", L"Cara: solo ojos (beta)",
                                        L"Cara: ojos y boca (beta)", L"Cara: de puntos (beta)"};
static const wchar_t *FACE_LEVEL_LABELS[] = {L"Poco", L"Normal", L"Mucho"};
static const wchar_t *APPEAR_LABELS[] = {L"Materializarse", L"Deslizarse", L"Zoom", L"Ninguna"};
static const wchar_t *END_LABELS[] = {L"Poco", L"Normal", L"Más"};

static int dp(int v)
{
    return MulDiv(v, S.dpi, 96);
}

/* ----------------------------------------------------------- dibujo AA --- */

static void blend(uint32_t *p, COLORREF c, float a)
{
    if (a <= 0) return;
    if (a > 1) a = 1;
    uint32_t d = *p;
    float r = (float)GetRValue(c), g = (float)GetGValue(c), b = (float)GetBValue(c);
    float dr = (float)((d >> 16) & 255), dg = (float)((d >> 8) & 255), db = (float)(d & 255);
    uint32_t nr = (uint32_t)(dr + (r - dr) * a), ng = (uint32_t)(dg + (g - dg) * a), nb = (uint32_t)(db + (b - db) * a);
    *p = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
}

static COLORREF lerp_color(COLORREF a, COLORREF b, float t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

/* Rectángulo redondeado con antialiasing por distancia firmada; si c2 !=
   c1 hace degradado horizontal. border > 0 dibuja solo el contorno. */
static void round_rect(RECT r, float radius, COLORREF c1, COLORREF c2, float border)
{
    float x0 = (float)r.left, y0 = (float)r.top, x1 = (float)r.right, y1 = (float)r.bottom;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    int ix0 = r.left < 0 ? 0 : r.left, iy0 = r.top < 0 ? 0 : r.top;
    int ix1 = r.right > S.bw ? S.bw : r.right, iy1 = r.bottom > S.bh ? S.bh : r.bottom;
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
            float qx = fabsf(px) - (hw - radius), qy = fabsf(py) - (hh - radius);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - radius;
            float a = 0.5f - d;
            if (border > 0) a = fminf(a, d + border + 0.5f);
            if (a <= 0) continue;
            COLORREF c = c1 == c2 ? c1 : lerp_color(c1, c2, (float)(x - r.left) / (float)(r.right - r.left));
            blend(&S.px[(size_t)y * S.bw + x], c, a);
        }
    }
}

static void circle(float cx, float cy, float rad, COLORREF c)
{
    RECT r = {(int)(cx - rad - 1), (int)(cy - rad - 1), (int)(cx + rad + 2), (int)(cy + rad + 2)};
    for (int y = r.top < 0 ? 0 : r.top; y < r.bottom && y < S.bh; y++)
        for (int x = r.left < 0 ? 0 : r.left; x < r.right && x < S.bw; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            blend(&S.px[(size_t)y * S.bw + x], c, rad + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

static void text(const wchar_t *t, RECT r, HFONT f, COLORREF c, UINT fmt)
{
    SelectObject(S.mem, f);
    SetTextColor(S.mem, c);
    SetBkMode(S.mem, TRANSPARENT);
    DrawTextW(S.mem, t, -1, &r, fmt | DT_NOPREFIX);
}

static int text_height(const wchar_t *t, int width, HFONT f)
{
    SelectObject(S.mem, f);
    RECT r = {0, 0, width, 0};
    DrawTextW(S.mem, t, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    return r.bottom;
}

/* ------------------------------------------------------------- layout --- */

static Widget *add(WType type, RECT r)
{
    Widget *w = &S.widgets[S.nwidgets++];
    memset(w, 0, sizeof *w);
    w->type = type;
    w->r = r;
    w->edit = -1;
    return w;
}

static HFONT font(int px, int weight)
{
    return CreateFontW(-dp(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
}

static void make_fonts(void)
{
    HFONT *fs[] = {&S.f_title, &S.f_section, &S.f_body, &S.f_small, &S.f_nav, &S.f_button};
    for (size_t i = 0; i < sizeof fs / sizeof *fs; i++)
        if (*fs[i]) DeleteObject(*fs[i]);
    S.f_title = font(24, FW_SEMIBOLD);
    S.f_section = font(20, FW_SEMIBOLD);
    S.f_body = font(14, FW_NORMAL);
    S.f_small = font(12, FW_NORMAL);
    S.f_nav = font(14, FW_NORMAL);
    S.f_button = font(14, FW_SEMIBOLD);
    for (int i = 0; i < F_EDIT_COUNT; i++)
        if (S.edits[i]) SendMessageW(S.edits[i], WM_SETFONT, (WPARAM)S.f_body, TRUE);
}

static int layout_edit(int x, int y, int w, const wchar_t *label, int field, const wchar_t *help)
{
    Widget *l = add(W_LABEL, (RECT){x, y, x + w, y + dp(20)});
    l->text = label;
    y += dp(22);
    Widget *e = add(W_EDIT, (RECT){x, y, x + w, y + dp(40)});
    e->edit = field;
    bool backup_key = field >= F_NVIDIA && field <= F_GLM;
    bool has_eye = field == F_API || field == F_STOP || field == F_MESH || backup_key;
    if (has_eye) {
        bool visible = field == F_API    ? S.api_visible
                       : field == F_STOP ? S.stop_visible
                       : field == F_MESH ? S.mesh_visible
                                         : S.key_visible[field - F_NVIDIA];
        Widget *eye = add(W_LINK, (RECT){x + w - dp(64), y, x + w - dp(8), y + dp(40)});
        eye->text = visible ? L"Ocultar" : L"Ver";
        eye->action = field == F_API    ? A_SHOW_API
                      : field == F_STOP ? A_SHOW_STOP
                      : field == F_MESH ? A_SHOW_MESH
                                        : A_SHOW_KEY + (field - F_NVIDIA);
    }
    HWND ed = S.edits[field];
    int right_pad = has_eye ? dp(70) : dp(12);
    MoveWindow(ed, x + dp(12), y + dp(10), w - dp(12) - right_pad, dp(22), TRUE);
    ShowWindow(ed, SW_SHOW);
    y += dp(44);
    if (help) {
        int h = text_height(help, w, S.f_small);
        Widget *hw = add(W_HELP, (RECT){x, y, x + w, y + h});
        hw->text = help;
        y += h + dp(4);
    }
    return y + dp(14);
}

static int layout_link(int x, int y, const wchar_t *t, int action)
{
    SelectObject(S.mem, S.f_small);
    SIZE sz;
    GetTextExtentPoint32W(S.mem, t, (int)wcslen(t), &sz);
    Widget *w = add(W_LINK, (RECT){x, y, x + sz.cx + dp(4), y + dp(20)});
    w->text = t;
    w->action = action;
    return y + dp(26);
}

static int layout_label(int x, int y, int w, const wchar_t *t)
{
    Widget *l = add(W_LABEL, (RECT){x, y, x + w, y + dp(20)});
    l->text = t;
    return y + dp(26);
}

static int layout_help(int x, int y, int w, const wchar_t *t)
{
    int h = text_height(t, w, S.f_small);
    Widget *hw = add(W_HELP, (RECT){x, y, x + w, y + h});
    hw->text = t;
    return y + h + dp(8);
}

static int layout_segment(int x, int y, int w, int *value, const wchar_t *const *labels, int n)
{
    Widget *s = add(W_SEGMENT, (RECT){x, y, x + w, y + dp(38)});
    s->value = value;
    s->labels = labels;
    s->options = n;
    return y + dp(52);
}

static int layout_slider(int x, int y, int w, int *value)
{
    Widget *s = add(W_SLIDER, (RECT){x, y, x + w, y + dp(30)});
    s->value = value;
    return y + dp(44);
}

static int layout_toggle(int x, int y, int w, int *value, const wchar_t *label)
{
    Widget *t = add(W_TOGGLE, (RECT){x, y, x + w, y + dp(30)});
    t->value = value;
    t->text = label;
    return y + dp(42);
}

static int layout_dropdown(int x, int y, int w, int *value, const wchar_t *const *labels, int n, int action)
{
    Widget *d = add(W_DROPDOWN, (RECT){x, y, x + w, y + dp(40)});
    d->value = value;
    d->labels = labels;
    d->options = n;
    d->action = action;
    return y + dp(54);
}

static int layout_button(int x, int y, int w, const wchar_t *t, int action, bool primary)
{
    Widget *b = add(W_BUTTON, (RECT){x, y, x + w, y + dp(36)});
    b->text = t;
    b->action = action;
    b->primary = primary;
    return y + dp(46);
}

static const wchar_t **g_voice_labels;
static const wchar_t **g_mic_labels;
static const wchar_t **g_output_labels;

static void free_labels(void)
{
    for (int i = 0; g_voice_labels && i < S.nvoices; i++) free((void *)g_voice_labels[i]);
    free(g_voice_labels);
    g_voice_labels = NULL;
    for (int i = 0; g_mic_labels && i < S.nmics + 1; i++) free((void *)g_mic_labels[i]);
    free(g_mic_labels);
    g_mic_labels = NULL;
    for (int i = 0; g_output_labels && i < S.nouts + 1; i++) free((void *)g_output_labels[i]);
    free(g_output_labels);
    g_output_labels = NULL;
}

static void build_labels(void)
{
    free_labels();
    g_voice_labels = xcalloc((size_t)S.nvoices + 1, sizeof(wchar_t *));
    for (int i = 0; i < S.nvoices; i++) g_voice_labels[i] = utf8_to_wide(S.voices[i].name);
    g_mic_labels = xcalloc((size_t)S.nmics + 2, sizeof(wchar_t *));
    g_mic_labels[0] = xwcsdup(L"Predeterminado de Windows");
    for (int i = 0; i < S.nmics; i++) g_mic_labels[i + 1] = utf8_to_wide(S.mics[i]);
    g_output_labels = xcalloc((size_t)S.nouts + 2, sizeof(wchar_t *));
    g_output_labels[0] = xwcsdup(L"Predeterminada de Windows");
    for (int i = 0; i < S.nouts; i++) g_output_labels[i + 1] = utf8_to_wide(S.outputs[i]);
}

/* 0 = la predeterminada; si la guardada ya no está conectada, también. */
static int output_index_of(const char *name)
{
    for (int i = 0; name && *name && i < S.nouts; i++)
        if (!strcmp(S.outputs[i], name)) return i + 1;
    return 0;
}

static const char *output_name_at(int index)
{
    return index > 0 && index <= S.nouts ? S.outputs[index - 1] : "";
}

static wchar_t g_tailscale_text[320];
static wchar_t g_sound_text[160];
static wchar_t g_skills_text[600]; /* la sección Skills: cuáles tuyas hay */
static wchar_t g_home_title[96];
static wchar_t g_home_text[200];

static void layout_home(int x, int y, int w)
{
    AppConfig c = config_snapshot();
    S.home_display = c.display_mode;
    S.home_output = output_index_of(c.output_name);
    S.home_full = c.full_access;
    bool muted = c.mic_muted, running = voice_running();
    config_free(&c);
    bool word = res_has_wake_word();
    swprintf(g_home_text, 200, L"%ls%ls%ls",
             running ? L"Sokari está activo: " : L"Todo listo. Dale a Iniciar y ",
             word ? L"di \"Hey Sokari\" (o Ctrl+Alt+J) para hablarle."
                  : L"háblale con Ctrl+Alt+J (\"Hey Sokari\" todavía no está listo).",
             muted ? L" El micrófono está silenciado." : L"");
    y = layout_help(x, y - dp(6), w, g_home_text);
    y = layout_button(x, y + dp(6), dp(220), running ? L"Mostrar la esfera" : L"Iniciar Sokari", A_START, true) + dp(8);
    y = layout_label(x, y, w, L"Modo de pantalla");
    y = layout_dropdown(x, y, w, &S.home_display, DISPLAY_MODE_LABELS, DISPLAY_MODE_COUNT, A_MODE_CHANGED);
    y = layout_help(x, y - dp(10), w, MODE_DESCS[S.home_display]);
    y = layout_label(x, y, w, L"Salida de audio (bocinas o audífonos)");
    y = layout_dropdown(x, y, w, &S.home_output, g_output_labels, S.nouts + 1, A_OUTPUT_CHANGED);
    y = layout_help(x, y - dp(10), w,
                    L"Por aquí sale la voz de Sokari y su tono. Si pusiste un sonido de activación propio, ese "
                    L"sale por la predeterminada de Windows.") + dp(6);
    y = layout_toggle(x, y - dp(4), w, &S.home_full, L"Acceso completo (menos borrar): no te pregunta nada");
    S.widgets[S.nwidgets - 1].action = A_FULL_ACCESS;
    int bw = (w - dp(24)) / 3;
    layout_button(x, y, bw, L"Configuración", A_GO_SETTINGS, false);
    layout_button(x + bw + dp(12), y, bw, muted ? L"Activar micrófono" : L"Silenciar micrófono", A_MUTE, false);
    layout_button(x + 2 * (bw + dp(12)), y, bw, L"Probar audio", A_TEST_AUDIO, false);
    y += dp(46);
    layout_button(x, y, bw, L"Buscar actualizaciones", A_CHECK_UPDATE, false);
    layout_button(x + bw + dp(12), y, bw, L"Abrir carpeta de datos", A_OPEN_FOLDER, false);
    layout_button(x + 2 * (bw + dp(12)), y, bw, L"Salir", A_QUIT, false);
}

static void layout(void)
{
    S.nwidgets = 0;
    for (int i = 0; i < F_EDIT_COUNT; i++) ShowWindow(S.edits[i], SW_HIDE);
    RECT cr;
    GetClientRect(S.hwnd, &cr);
    int side = dp(210);
    int x = side + dp(40), w = cr.right - x - dp(40);
    int y = dp(34);
    Widget *title = add(W_LABEL, (RECT){x, y, x + w, y + dp(32)});
    title->text = S.first_run ? L"Configuremos Sokari" : SECTION_NAMES[S.section];
    if (S.section == SEC_HOME && !S.first_run) {
        char *name = config_user_name();
        wchar_t *wn = utf8_to_wide(name);
        if (*wn) swprintf(g_home_title, 96, L"Hola, %ls", wn);
        else wcscpy(g_home_title, L"Inicio");
        title->text = g_home_title;
        free(wn);
        free(name);
    }
    title->primary = true;
    y += dp(46);
    if (S.first_run) {
        y = layout_help(x, y, w,
                        L"Solo la API key de Groq es obligatoria (es gratis y no pide tarjeta). Todo lo demás "
                        L"lo puedes cambiar después desde la tuerca en el ícono de la bandeja.");
    }

    switch (S.section) {
    case SEC_HOME:
        layout_home(x, y, w);
        break;
    case SEC_ACCOUNT:
        y = layout_edit(x, y, w, L"API key de Groq", F_API, NULL);
        y = layout_link(x, y - dp(10), L"Consíguela gratis en console.groq.com  →", A_GROQ_LINK) + dp(6);
        y = layout_edit(x, y, w, L"Tu nombre", F_NAME, L"Para que Sokari sepa con quién habla desde que arranca.");
        y = layout_edit(x, y, w, L"Contraseña de tu perfil (opcional)", F_PROFILE_PW,
                        L"Protege tus datos guardados si otras personas usan Sokari en esta PC. Déjala vacía para no cambiarla.");
        y = layout_link(x, y - dp(8), L"¿Olvidaste una contraseña de perfil? Restablecer todas", A_RESET_PW) + dp(6);
        y = layout_edit(x, y, w, L"Palabra de apagado (opcional)", F_STOP,
                        L"Si la dices, Sokari se apaga al instante. Se revisa en tu PC: nunca se le manda a la IA.");
        break;
    case SEC_DISPLAY: {
        /* Con la escala de Windows alta la ventana es más baja: si todo no cabe
           arriba de Guardar/Cancelar, las tarjetas pierden su descripción (solo
           se ve la del modo elegido). */
        int first = S.nwidgets, y0 = y;
        for (int compact = 0; compact < 2; compact++) {
            S.nwidgets = first;
            y = layout_label(x, y0, w, L"Modo de pantalla");
            int item = dp(compact ? 36 : CARD_H);
            int ch = item * DISPLAY_MODE_COUNT + dp(CARD_GAP) * (DISPLAY_MODE_COUNT - 1);
            Widget *c = add(W_CARDS, (RECT){x, y, x + w, y + ch});
            c->value = &S.display_mode;
            c->labels = DISPLAY_MODE_LABELS;
            c->descs = compact ? NULL : MODE_DESCS;
            c->options = DISPLAY_MODE_COUNT;
            c->item_h = item;
            y += ch + dp(compact ? 6 : 14);
            if (compact) y = layout_help(x, y, w, MODE_DESCS[S.display_mode]) + dp(2);
            y = layout_label(x, y, w, L"Resolución de la esfera");
            y = layout_segment(x, y, w, &S.resolution_index, RES_LABELS, 5);
            /* Estilo y animación en un solo renglón: con 1366×768 todo tiene
               que caber arriba de Guardar. */
            /* Con una cara, en medio va «Se nota» (poco, normal, mucho). */
            bool face = S.style >= 2;
            int sw = w * 2 / 5, lx = x + sw + dp(12), lw = face ? dp(110) : 0;
            int ax = face ? lx + lw + dp(12) : x + sw + dp(16), aw = x + w - ax;
            layout_label(x, y, sw, L"Estilo");
            if (face) layout_label(lx, y, lw, L"Se nota");
            y = layout_label(ax, y, aw, L"Al aparecer y desaparecer");
            layout_dropdown(x, y, sw, &S.style, STYLE_LABELS, 5, A_STYLE);
            if (face) layout_dropdown(lx, y, lw, &S.face_level, FACE_LEVEL_LABELS, 3, A_NONE);
            layout_button(x + w - dp(96), y + dp(2), dp(96), L"Probar", A_TEST_ANIM, false);
            y = layout_dropdown(ax, y, aw - dp(108), &S.appear, APPEAR_LABELS, 4, A_NONE);
            if (face) y = layout_toggle(x, y, w, &S.face_symbols, L"Símbolos en la cara (lágrima, destellos, «?»…)");
            y = layout_toggle(x, y, w, &S.subtitles, L"Mostrar subtítulos de lo que dices y lo que responde");
            y = layout_toggle(x, y, w, &S.show_only_talking, L"Aparecer solo cuando le hablas (y esconderse al terminar)");
            if (S.widgets[S.nwidgets - 1].r.bottom <= cr.bottom - dp(64) - dp(10)) break;
        }
        break;
    }
    case SEC_AUDIO:
        y = layout_label(x, y, w, L"Volumen de la voz de Sokari");
        y = layout_slider(x, y, w, &S.volume);
        y = layout_label(x, y, w, L"Voz");
        layout_button(x + w - dp(110), y + dp(2), dp(110), L"Probar", A_TEST_VOICE, false);
        y = layout_dropdown(x, y, w - dp(122), &S.voice_index, g_voice_labels, S.nvoices, A_NONE);
        y = layout_label(x, y, w, L"Micrófono");
        y = layout_dropdown(x, y, w, &S.mic_index, g_mic_labels, S.nmics + 1, A_NONE);
        y = layout_label(x, y, w, L"Salida de audio");
        y = layout_dropdown(x, y, w, &S.output_index, g_output_labels, S.nouts + 1, A_NONE);
        y = layout_label(x, y, w, L"Sensibilidad de \"Hey Sokari\"");
        y = layout_slider(x, y, w, &S.sensitivity);
        y = layout_help(x, y - dp(8), w,
                        L"Más alta: te escucha aunque lo digas bajito o lejos, pero puede activarse solo con ruido.");
        /* En un solo renglón: con 1366×768 todo tiene que caber arriba de Guardar. */
        layout_label(x, y + dp(9), w * 2 / 5, L"Cuánto espero cuando te callas");
        y = layout_segment(x + w * 2 / 5, y, w - w * 2 / 5, &S.end_silence, END_LABELS, 3) - dp(4);
        y = layout_toggle(x, y, w, &S.duck, L"Bajar el volumen de la PC mientras te escucho");
        break;
    case SEC_GENERAL:
        y = layout_toggle(x, y, w, &S.autostart, L"Iniciar Sokari con Windows");
        y = layout_help(x, y - dp(6), w, L"Atajo: Ctrl+Alt+J para hablarle sin decir \"Hey Sokari\".") + dp(6);
        y = layout_label(x, y, w, L"Sonido de activación");
        y = layout_help(x, y - dp(4), w, g_sound_text);
        layout_button(x, y, dp(170), L"Elegir archivo…", A_PICK_SOUND, false);
        layout_button(x + dp(182), y, dp(150), L"Usar el de Sokari", A_CLEAR_SOUND, false);
        y += dp(50);
        {
            wchar_t *ob = expand_env(L"%LOCALAPPDATA%\\Programs\\obsidian\\Obsidian.exe");
            if (!file_exists(ob)) {
                layout_button(x, y, dp(170), L"Instalar Obsidian", A_OBSIDIAN, false);
                layout_button(x + dp(182), y, dp(210), L"Abrir carpeta de Sokari", A_OPEN_FOLDER, false);
            } else {
                layout_button(x, y, dp(210), L"Abrir carpeta de Sokari", A_OPEN_FOLDER, false);
            }
            free(ob);
            y += dp(50);
        }
        y = layout_toggle(x, y, w, &S.full_access, L"Acceso completo (menos borrar)");
        y = layout_help(x, y - dp(8), w,
                        L"Prendido, hace todo sin preguntarte: mover archivos, mandar mensajes, subir archivos, "
                        L"guardar datos. Solo pide un «sí» antes de borrar. Riesgo: si lee una página con "
                        L"instrucciones escondidas, podría obedecerlas sin avisarte. Apagado, pregunta antes de "
                        L"acciones delicadas cuando leyó algo de afuera.");
        break;
    case SEC_SKILLS: {
        y = layout_help(x, y - dp(6), w,
                        L"Contestan en tu PC, sin gastar nada de IA (0 tokens): «¿qué hora es?», «pon un temporizador "
                        L"de 10 minutos», «¿va a llover?», «anota comprar leche». Lo que no entienden se lo pasan a la "
                        L"IA. Apaga las que no quieras.") +
            dp(6);
        static const wchar_t *const LABELS[] = {L"Tus rutinas y skills",   L"Hora y fecha",
                                                L"Temporizadores y cronómetro", L"Alarmas y recordatorios",
                                                L"Cuentas y conversiones", L"El clima",
                                                L"Notas y pendientes",     L"Cómo va la PC",
                                                L"Saludos, plática y chistes"};
        int n = skills_count() < 9 ? skills_count() : 9, half = (n + 1) / 2, col = (w - dp(16)) / 2;
        for (int i = 0; i < n; i++)
            layout_toggle(x + (i / half) * (col + dp(16)), y + (i % half) * dp(42), col, &S.skill_on[i], LABELS[i]);
        y += half * dp(42) + dp(6);
        y = layout_edit(x, y, w, L"Tu ciudad (para el clima)", F_CITY, NULL) - dp(8);
        y = layout_help(x, y, w, L"También se la puedes decir: «mi ciudad es Chihuahua». El clima sale de Open-Meteo, "
                                 L"gratis y sin key.") + dp(8);
        y = layout_help(x, y, w, g_skills_text) + dp(2);
        layout_button(x, y, dp(220), L"Abrir carpeta de skills", A_OPEN_SKILLS, false);
        layout_button(x + dp(232), y, dp(160), L"Nueva skill", A_NEW_SKILL, false);
        y += dp(50);
        break;
    }
    case SEC_AI: {
        y = layout_help(x, y - dp(6), w,
                        L"Cuando se te acaba el cupo gratis de Groq, Sokari sigue con estas, en orden. Todas son "
                        L"opcionales: pega solo las que tengas. Tu voz se sigue pasando a texto con Groq.") +
            dp(4);
        static const wchar_t *const NAMES[] = {L"NVIDIA (build.nvidia.com, gratis con límite)",
                                               L"DeepSeek (de pago, muy barata)",
                                               L"OpenRouter (modelos «:free»)", L"GLM de Z.ai (tiene uno gratis)"};
        for (int i = 0; i < 4; i++) {
            layout_link(x + w - dp(92), y, L"Sacar key →", A_KEY_LINK + i);
            y = layout_edit(x, y, w - dp(100), NAMES[i], F_NVIDIA + i, NULL) - dp(8);
        }
        y = layout_edit(x, y + dp(4), w, L"Orden (de izquierda a derecha)", F_AI_ORDER, NULL) - dp(8);
        y = layout_help(x, y, w,
                        L"Ojo: en OpenRouter, muchos modelos gratis solo funcionan si permites que usen tus mensajes "
                        L"para entrenar; DeepSeek guarda los datos en China. Sokari les manda tus frases y lo que "
                        L"recuerda de ti.");
        break;
    }
    case SEC_DEVICES: {
        y = layout_help(x, y - dp(6), w, g_tailscale_text) + dp(2);
        int bx = x;
        if (!tailscale_installed()) {
            layout_button(bx, y, dp(170), L"Instalar Tailscale", A_TAILSCALE, false);
            bx += dp(182);
        }
        layout_button(bx, y, dp(220), L"Permitir en el firewall", A_FIREWALL, false);
        layout_button(bx + dp(232), y, dp(190), L"Revisar la malla", A_DIAGNOSE, false);
        y += dp(52);
        int ey = y + dp(22);
        y = layout_edit(x, y, w - dp(182), L"Secreto (solo si tus PCs usan cuentas distintas de Tailscale)", F_MESH,
                        NULL);
        layout_button(x + w - dp(170), ey + dp(2), dp(170), L"Copiar secreto", A_COPY_SECRET, false);
        int ly = y;
        y = layout_label(x, y, w - dp(240), L"Tus PCs");
        layout_button(x + w - dp(230), ly - dp(8), dp(230), L"Detectar mis PCs", A_DETECT, false);
        y += dp(10);
        mesh_devices_free(S.devs, S.ndevs);
        S.ndevs = mesh_devices(&S.devs);
        if (!S.ndevs)
            y = layout_help(x, y, w,
                            L"Todavía no tienes ninguna. Con Tailscale conectado en las dos PCs dale a Detectar, o "
                            L"dile a Sokari: «registra mi laptop en 100.x.y.z».");
        for (int i = 0; i < S.ndevs && i < MAX_DEVICE_ROWS; i++) {
            wchar_t *nm = utf8_to_wide(S.devs[i].name), *hs = utf8_to_wide(S.devs[i].host);
            swprintf(S.dev_text[i], 200, L"%ls  ·  %ls", nm, hs);
            free(nm);
            free(hs);
            Widget *row = add(W_LABEL, (RECT){x, y, x + w - dp(236), y + dp(36)});
            row->text = S.dev_text[i];
            layout_button(x + w - dp(224), y, dp(106), L"Probar", A_DEV_PROBE + i, false);
            layout_button(x + w - dp(106), y, dp(106), L"Quitar", A_DEV_REMOVE + i, false);
            y += dp(44);
        }
        if (S.ndevs)
            y = layout_help(x, y + dp(2), w, L"Para mandarle algo, dile a Sokari: «dile a mi laptop que abra Spotify».");
        break;
    }
    }

    S.ncontent = S.nwidgets;
    if (S.section != SEC_HOME) {
        int by = cr.bottom - dp(64);
        layout_button(cr.right - dp(40) - dp(150), by, dp(150), S.first_run ? L"Empezar" : L"Guardar", A_SAVE, true);
        if (!S.first_run)
            layout_button(cr.right - dp(40) - dp(150) - dp(12) - dp(120), by, dp(120), L"Cancelar", A_CANCEL, false);
    }
    if (!S.first_run) SetWindowTextW(S.hwnd, S.section == SEC_HOME ? L"Sokari" : L"Sokari — Configuración");
    /* Que no quede el foco en un campo que ya no se ve (lo que escribas iría a
       parar ahí sin que lo notes). */
    HWND f = GetFocus();
    if (f && GetParent(f) == S.hwnd && !IsWindowVisible(f)) SetFocus(S.hwnd);
    InvalidateRect(S.hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------ pintado --- */

static void paint_widget(Widget *wd, int index)
{
    bool hot = index == S.hover;
    RECT r = wd->r;
    switch (wd->type) {
    case W_LABEL:
        text(wd->text, r, wd->primary ? S.f_title : S.f_body, wd->primary ? C_TEXT : C_MUTED,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case W_HELP:
        text(wd->text, r, S.f_small, C_MUTED, DT_LEFT | DT_WORDBREAK);
        break;
    case W_EDIT: {
        HWND ed = S.edits[wd->edit];
        bool focus = GetFocus() == ed;
        round_rect(r, (float)dp(10), C_SURFACE, C_SURFACE, 0);
        round_rect(r, (float)dp(10), focus ? C_ACCENT : C_BORDER, focus ? C_ACCENT : C_BORDER, focus ? 2.0f : 1.0f);
        break;
    }
    case W_LINK:
        text(wd->text, r, S.f_small, hot ? C_TEXT : C_LINK, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        break;
    case W_BUTTON:
        if (wd->primary) {
            round_rect(r, (float)dp(18), hot ? RGB(0x8e, 0x80, 0xff) : C_ACCENT, hot ? RGB(0xff, 0x5c, 0xd4) : C_ACCENT2, 0);
            text(wd->text, r, S.f_button, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            round_rect(r, (float)dp(18), hot ? RGB(0x2c, 0x2b, 0x38) : C_BG, hot ? RGB(0x2c, 0x2b, 0x38) : C_BG, 0);
            round_rect(r, (float)dp(18), C_BORDER, C_BORDER, 1.0f);
            text(wd->text, r, S.f_button, C_TEXT, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        break;
    case W_SEGMENT: {
        round_rect(r, (float)dp(19), C_SURFACE, C_SURFACE, 0);
        int n = wd->options, cw = (r.right - r.left) / n;
        for (int i = 0; i < n; i++) {
            RECT c = {r.left + i * cw + dp(3), r.top + dp(3), r.left + (i + 1) * cw - dp(3), r.bottom - dp(3)};
            if (*wd->value == i) round_rect(c, (float)dp(16), C_ACCENT, RGB(0x9b, 0x5c, 0xff), 0);
            text(wd->labels[i], c, S.f_body, *wd->value == i ? RGB(255, 255, 255) : C_MUTED,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        break;
    }
    case W_CARDS: {
        int ch = wd->item_h ? wd->item_h : dp(CARD_H);
        for (int i = 0; i < wd->options; i++) {
            RECT c = {r.left, r.top + i * (ch + dp(CARD_GAP)), r.right, r.top + i * (ch + dp(CARD_GAP)) + ch};
            bool sel = *wd->value == i;
            round_rect(c, (float)dp(12), sel ? C_NAV_SEL : C_SURFACE, sel ? C_NAV_SEL : C_SURFACE, 0);
            round_rect(c, (float)dp(12), sel ? C_ACCENT : C_BORDER, sel ? C_ACCENT : C_BORDER, sel ? 2.0f : 1.0f);
            circle((float)(c.left + dp(22)), (float)(c.top + ch / 2), (float)dp(8), sel ? C_ACCENT : C_BORDER);
            circle((float)(c.left + dp(22)), (float)(c.top + ch / 2), (float)dp(6), sel ? C_ACCENT : C_SURFACE);
            if (sel) circle((float)(c.left + dp(22)), (float)(c.top + ch / 2), (float)dp(3), RGB(255, 255, 255));
            if (!wd->descs) {
                RECT t = {c.left + dp(44), c.top, c.right - dp(12), c.bottom};
                text(wd->labels[i], t, S.f_body, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                continue;
            }
            RECT t = {c.left + dp(44), c.top + dp(8), c.right - dp(12), c.top + dp(28)};
            text(wd->labels[i], t, S.f_body, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT d = {c.left + dp(44), c.top + dp(28), c.right - dp(12), c.bottom - dp(8)};
            text(wd->descs[i], d, S.f_small, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        break;
    }
    case W_SLIDER: {
        int cy = (r.top + r.bottom) / 2;
        int x0 = r.left + dp(8), x1 = r.right - dp(64);
        RECT track = {x0, cy - dp(3), x1, cy + dp(3)};
        round_rect(track, (float)dp(3), C_BORDER, C_BORDER, 0);
        int pos = x0 + (x1 - x0) * *wd->value / 100;
        RECT fill = {x0, cy - dp(3), pos, cy + dp(3)};
        if (pos > x0 + dp(2)) round_rect(fill, (float)dp(3), C_ACCENT, C_ACCENT2, 0);
        circle((float)pos, (float)cy, (float)dp(hot || S.drag == index ? 10 : 9), RGB(255, 255, 255));
        wchar_t v[16];
        swprintf(v, 16, L"%d%%", *wd->value);
        RECT vr = {x1 + dp(12), r.top, r.right, r.bottom};
        text(v, vr, S.f_body, C_TEXT, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        break;
    }
    case W_TOGGLE: {
        int cy = (r.top + r.bottom) / 2;
        RECT pill = {r.right - dp(46), cy - dp(12), r.right, cy + dp(12)};
        bool on = *wd->value;
        round_rect(pill, (float)dp(12), on ? C_ACCENT : C_BORDER, on ? C_ACCENT2 : C_BORDER, 0);
        circle((float)(on ? pill.right - dp(12) : pill.left + dp(12)), (float)cy, (float)dp(9), RGB(255, 255, 255));
        RECT t = {r.left, r.top, r.right - dp(60), r.bottom};
        text(wd->text, t, S.f_body, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        break;
    }
    case W_DROPDOWN: {
        round_rect(r, (float)dp(10), hot ? RGB(0x27, 0x27, 0x32) : C_SURFACE, hot ? RGB(0x27, 0x27, 0x32) : C_SURFACE, 0);
        round_rect(r, (float)dp(10), C_BORDER, C_BORDER, 1.0f);
        const wchar_t *cur = (*wd->value >= 0 && *wd->value < wd->options) ? wd->labels[*wd->value] : L"—";
        RECT t = {r.left + dp(12), r.top, r.right - dp(36), r.bottom};
        text(cur, t, S.f_body, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT ch = {r.right - dp(34), r.top, r.right - dp(8), r.bottom};
        text(L"▾", ch, S.f_body, C_MUTED, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        break;
    }
    }
}

static void paint(HDC dc)
{
    RECT cr;
    GetClientRect(S.hwnd, &cr);
    if (cr.right != S.bw || cr.bottom != S.bh || !S.mem) {
        if (S.mem) {
            SelectObject(S.mem, S.old_bmp);
            DeleteObject(S.bmp);
            DeleteDC(S.mem);
        }
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), cr.right, -cr.bottom, 1, 32, BI_RGB}};
        void *bits;
        S.bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        S.mem = CreateCompatibleDC(dc);
        S.old_bmp = SelectObject(S.mem, S.bmp);
        S.px = bits;
        S.bw = cr.right;
        S.bh = cr.bottom;
    }
    uint32_t bg = 0xFF000000u | (GetRValue(C_BG) << 16) | (GetGValue(C_BG) << 8) | GetBValue(C_BG);
    uint32_t sb = 0xFF000000u | (GetRValue(C_SIDEBAR) << 16) | (GetGValue(C_SIDEBAR) << 8) | GetBValue(C_SIDEBAR);
    int side = dp(210);
    for (int y = 0; y < S.bh; y++)
        for (int x = 0; x < S.bw; x++) S.px[(size_t)y * S.bw + x] = x < side ? sb : bg;
    GdiFlush();

    RECT brand = {dp(24), dp(28), side - dp(12), dp(62)};
    text(L"Sokari", brand, S.f_title, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT ver = {dp(24), dp(62), side - dp(12), dp(84)};
    text(L"versión " SOKARI_VERSION_W, ver, S.f_small, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    GdiFlush();
    if (!S.first_run) {
        for (int i = 0; i < SEC_COUNT; i++) {
            RECT nr = {dp(12), dp(110) + i * dp(46), side - dp(12), dp(110) + i * dp(46) + dp(40)};
            if (i == S.section) {
                round_rect(nr, (float)dp(10), C_NAV_SEL, C_NAV_SEL, 0);
                RECT bar = {nr.left + dp(6), nr.top + dp(10), nr.left + dp(9), nr.bottom - dp(10)};
                round_rect(bar, (float)dp(2), C_ACCENT, C_ACCENT2, 0);
            }
            GdiFlush();
            RECT tr = {nr.left + dp(20), nr.top, nr.right, nr.bottom};
            text(SECTION_NAMES[i], tr, S.f_nav, i == S.section ? C_TEXT : C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
    for (int i = 0; i < S.nwidgets; i++) {
        paint_widget(&S.widgets[i], i);
        GdiFlush();
    }
    if (S.status_line) {
        int right = S.section == SEC_HOME ? S.bw - dp(40) : S.bw - dp(40) - dp(290);
        RECT st = {side + dp(40), S.bh - dp(58), right, S.bh - dp(26)};
        text(S.status_line, st, S.f_small, C_LINK, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
    BitBlt(dc, 0, 0, S.bw, S.bh, S.mem, 0, 0, SRCCOPY);
}

/* ------------------------------------------------------------ lógica --- */

static void set_status(const wchar_t *t)
{
    free(S.status_line);
    S.status_line = t ? xwcsdup(t) : NULL;
    InvalidateRect(S.hwnd, NULL, FALSE);
}

static char *edit_text(int field)
{
    int n = GetWindowTextLengthW(S.edits[field]);
    wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)(n + 1));
    GetWindowTextW(S.edits[field], w, n + 1);
    char *u = wide_to_utf8(w);
    SecureZeroMemory(w, sizeof(wchar_t) * (size_t)(n + 1));
    free(w);
    char *t = str_trim(u);
    free(u);
    return t;
}

static void refresh_tailscale_text(void)
{
    char *ip = tailscale_installed() ? mesh_tailscale_ip() : NULL;
    char *listening = mesh_listening_ip();
    if (!tailscale_installed())
        swprintf(g_tailscale_text, 320,
                 L"Tailscale conecta tus PCs para mandarles órdenes («dile a mi laptop que…»). Instálalo en cada PC y "
                 L"entra con la misma cuenta.");
    else if (!ip)
        swprintf(g_tailscale_text, 320, L"Tailscale está instalado, pero no está conectado. Ábrelo y entra con tu cuenta.");
    else if (listening && mesh_firewall_ok() == 0)
        swprintf(g_tailscale_text, 320,
                 L"Tailscale conectado ✓ (IP %hs). Esta PC escucha, pero el firewall todavía no deja entrar las "
                 L"órdenes: dale «Permitir en el firewall».",
                 ip);
    else if (listening)
        swprintf(g_tailscale_text, 320, L"Tailscale conectado ✓ (IP %hs). Esta PC recibe órdenes de tus otras PCs ✓", ip);
    else
        swprintf(g_tailscale_text, 320,
                 L"Tailscale conectado ✓ (IP %hs). Esta PC todavía no recibe órdenes: se activa sola unos segundos "
                 L"después de abrir Sokari.",
                 ip);
    free(ip);
    free(listening);
}

static void refresh_sound_text(void)
{
    const wchar_t *names[] = {L"activacion.mp3", L"activacion.wav"};
    bool custom = false;
    for (int i = 0; i < 2 && !custom; i++) {
        wchar_t *p = path_join(g_paths.sounds_dir, names[i]);
        custom = file_exists(p);
        free(p);
    }
    wcscpy(g_sound_text, custom ? L"Usando tu sonido personalizado." : L"Usando el tono de Sokari.");
}

/* "Tus skills (2): modo estudio, noticias." para la sección Skills. */
static void refresh_skills_text(void)
{
    int n;
    char *names = skills_user_summary(&n);
    char *t = n ? str_printf("Tus skills (%d): %s. Cada una es un archivo de texto en la carpeta de skills; también se "
                             "crean diciéndole «crea una rutina…».",
                             n, names)
                : xstrdup("Todavía no tienes skills tuyas. Crea una con «Nueva skill» (un archivo de texto de ejemplo) "
                          "o diciéndole «crea una rutina que abra Spotify cuando diga modo estudio».");
    wchar_t *w = utf8_to_wide(t);
    wcsncpy(g_skills_text, w, sizeof g_skills_text / sizeof *g_skills_text - 1);
    g_skills_text[sizeof g_skills_text / sizeof *g_skills_text - 1] = 0;
    free(w);
    free(t);
    free(names);
}

static void load_values(void)
{
    config_free(&S.cfg);
    S.cfg = config_snapshot();
    SetWindowTextW(S.edits[F_API], L"");
    wchar_t *w;
    w = utf8_to_wide(S.cfg.groq_api_key);
    SetWindowTextW(S.edits[F_API], w);
    free(w);
    w = utf8_to_wide(S.cfg.user_name);
    SetWindowTextW(S.edits[F_NAME], w);
    free(w);
    w = utf8_to_wide(S.cfg.stop_word);
    SetWindowTextW(S.edits[F_STOP], w);
    free(w);
    SetWindowTextW(S.edits[F_PROFILE_PW], L"");
    w = utf8_to_wide(S.cfg.mesh_secret);
    SetWindowTextW(S.edits[F_MESH], w);
    free(w);
    const char *backup[] = {S.cfg.nvidia_key, S.cfg.deepseek_key, S.cfg.openrouter_key, S.cfg.glm_key, S.cfg.ai_order};
    for (int k = 0; k < 5; k++) {
        w = utf8_to_wide(backup[k]);
        SetWindowTextW(S.edits[F_NVIDIA + k], w);
        SecureZeroMemory(w, wcslen(w) * sizeof *w);
        free(w);
    }
    S.display_mode = S.cfg.display_mode;
    S.resolution_index = 0;
    for (int i = 0; i < 5; i++)
        if (RESOLUTIONS[i] == S.cfg.resolution) S.resolution_index = i;
    S.style = S.cfg.sphere_style >= 0 && S.cfg.sphere_style < 5 ? S.cfg.sphere_style : 0;
    S.appear = S.cfg.appear_anim;
    S.face_level = S.cfg.face_level >= 0 && S.cfg.face_level <= 2 ? S.cfg.face_level : 1;
    S.face_symbols = S.cfg.face_symbols;
    S.end_silence = S.cfg.end_silence;
    S.duck = S.cfg.duck;
    S.subtitles = S.cfg.subtitles;
    S.show_only_talking = S.cfg.show_only_talking;
    S.full_access = S.cfg.full_access;
    w = utf8_to_wide(S.cfg.city);
    SetWindowTextW(S.edits[F_CITY], w);
    free(w);
    for (int i = 0; i < skills_count() && i < 16; i++) S.skill_on[i] = config_skill_enabled(skills_get(i)->id);
    refresh_skills_text();
    S.volume = S.cfg.volume;
    S.sensitivity = S.cfg.wake_sensitivity;
    S.autostart = autostart_is_enabled();

    tts_free_voices(S.voices, S.nvoices);
    S.nvoices = tts_list_voices(&S.voices);
    S.voice_index = 0;
    for (int i = 0; i < S.nvoices; i++)
        if (!strcmp(S.voices[i].id, S.cfg.voice)) S.voice_index = i;
    if (!*S.cfg.voice)
        for (int i = 0; i < S.nvoices; i++)
            if (str_contains_ci(S.voices[i].name, "Raul")) S.voice_index = i;
    free_string_list(S.mics, S.nmics);
    S.nmics = mic_list_devices(&S.mics);
    S.mic_index = 0;
    for (int i = 0; i < S.nmics; i++)
        if (!strcmp(S.mics[i], S.cfg.mic_name)) S.mic_index = i + 1;
    free_string_list(S.outputs, S.nouts);
    S.nouts = speaker_list_devices(&S.outputs);
    S.output_index = output_index_of(S.cfg.output_name);
    build_labels();
    refresh_tailscale_text();
    refresh_sound_text();
}

/* "NVIDIA, groq" -> "nvidia,groq"; lo que no sea un proveedor conocido se
   quita, y si no queda nada, el de fábrica. Toma t (heap). */
static void save(void)
{
    char *api = edit_text(F_API);
    if (!*api) {
        free(api);
        S.section = SEC_ACCOUNT;
        layout();
        SetFocus(S.edits[F_API]);
        set_status(L"Necesitas una API key de Groq para continuar (es gratis).");
        return;
    }
    AppConfig c = config_snapshot();
    free(c.groq_api_key);
    c.groq_api_key = api;
    free(c.user_name);
    c.user_name = edit_text(F_NAME);
    free(c.stop_word);
    c.stop_word = edit_text(F_STOP);
    char *mesh = edit_text(F_MESH);
    bool mesh_changed = *mesh && strcmp(mesh, c.mesh_secret);
    const char *bad = mesh_changed ? config_secret_problem(mesh) : NULL;
    if (bad) {
        /* Una IP pegada donde va el secreto: no se guarda nada y se dice por qué. */
        free(mesh);
        config_free(&c);
        S.section = SEC_DEVICES;
        layout();
        SetFocus(S.edits[F_MESH]);
        wchar_t *w = utf8_to_wide(bad);
        set_status(w);
        free(w);
        return;
    }
    if (*mesh) {
        free(c.mesh_secret);
        c.mesh_secret = mesh;
        if (mesh_changed) log_msg("Secreto de malla cambiado desde Configuración.");
    } else {
        free(mesh);
    }
    char **backup[] = {&c.nvidia_key, &c.deepseek_key, &c.openrouter_key, &c.glm_key};
    for (int k = 0; k < 4; k++) {
        SecureZeroMemory(*backup[k], strlen(*backup[k]));
        free(*backup[k]);
        *backup[k] = edit_text(F_NVIDIA + k);
    }
    free(c.ai_order);
    char *order = edit_text(F_AI_ORDER);
    c.ai_order = config_clean_ai_order(order);
    free(order);
    free(c.city);
    c.city = edit_text(F_CITY);
    StrBuf off;
    sb_init(&off);
    for (int i = 0; i < skills_count() && i < 16; i++)
        if (!S.skill_on[i]) sb_appendf(&off, "%s%s", off.len ? "," : "", skills_get(i)->id);
    free(c.skills_off);
    c.skills_off = off.data ? sb_steal(&off) : xstrdup("");
    sb_free(&off);
    c.display_mode = S.display_mode;
    c.resolution = RESOLUTIONS[S.resolution_index];
    c.sphere_style = S.style;
    c.appear_anim = S.appear;
    c.face_level = S.face_level;
    c.face_symbols = S.face_symbols != 0;
    c.end_silence = S.end_silence;
    c.duck = S.duck != 0;
    c.subtitles = S.subtitles != 0;
    c.show_only_talking = S.show_only_talking != 0;
    c.full_access = S.full_access != 0;
    c.volume = S.volume;
    c.wake_sensitivity = S.sensitivity;
    c.autostart = S.autostart != 0;
    free(c.voice);
    c.voice = xstrdup(S.voice_index < S.nvoices ? S.voices[S.voice_index].id : "");
    free(c.mic_name);
    c.mic_name = xstrdup(S.mic_index > 0 && S.mic_index <= S.nmics ? S.mics[S.mic_index - 1] : "");
    free(c.output_name);
    c.output_name = xstrdup(output_name_at(S.output_index));
    config_apply(&c);
    speaker_set_device(c.output_name);

    char *pw = edit_text(F_PROFILE_PW);
    if (*pw) set_profile_password(c.user_name, pw);
    SecureZeroMemory(pw, strlen(pw));
    free(pw);
    config_free(&c);

    autostart_set(S.autostart != 0);
    ui_config_changed();
    voice_settings_changed();
    bool first = S.first_run;
    SettingsSavedFn cb = S.on_saved;
    S.first_run = false;
    if (S.home_mode && !first) {
        load_values();
        S.section = SEC_HOME;
        layout();
        set_status(mesh_changed ? L"Cambios guardados. El secreto nuevo tiene que ser el mismo en tus otras PCs (si "
                                  L"usan la misma cuenta de Tailscale, ni hace falta)."
                                : L"Cambios guardados.");
        return;
    }
    S.quit_on_close = false;
    DestroyWindow(S.hwnd);
    if (cb) cb(first);
}

static DWORD WINAPI chime_worker(LPVOID arg)
{
    sound_chime();
    return 0;
}

static void start_or_show(void)
{
    SettingsSavedFn cb = S.on_saved;
    bool running = voice_running();
    S.quit_on_close = false;
    DestroyWindow(S.hwnd);
    if (!running && cb) cb(false);
    ui_show_hud(running ? HUD_SHOW_FRONT : HUD_SHOW_STARTED);
}

typedef struct {
    int action;
    char *name, *host; /* A_DEV_PROBE: qué dispositivo */
    wchar_t *result;
} AsyncJob;

/* Busca tus otras PCs en Tailscale y registra las que falten. */
static wchar_t *detect_devices(void)
{
    char *msg = mesh_detect_devices();
    wchar_t *w = utf8_to_wide(msg);
    free(msg);
    return w;
}

static DWORD WINAPI async_worker(LPVOID arg)
{
    AsyncJob *job = arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (job->action == A_CHECK_UPDATE) {
        char *msg = update_check_now();
        job->result = utf8_to_wide(msg ? msg : "");
        free(msg);
    } else if (job->action == A_FIREWALL) {
        job->result = xwcsdup(mesh_allow_firewall()
                                  ? L"Listo: el firewall ya deja pasar las órdenes de tus otras PCs (solo por Tailscale)."
                                  : L"El firewall no cambió (¿le dijiste que no al permiso de administrador?).");
    } else if (job->action == A_DETECT) {
        job->result = detect_devices();
    } else if (job->action == A_DIAGNOSE) {
        char *rep = mesh_diagnose();
        log_msg("%s", rep);
        job->result = utf8_to_wide(rep);
        free(rep);
    } else if (job->action == A_DEV_PROBE) {
        char *msg = mesh_probe_report(job->name, job->host);
        job->result = utf8_to_wide(msg);
        free(msg);
    } else {
        const wchar_t *id = job->action == A_TAILSCALE ? L"Tailscale.Tailscale" : L"Obsidian.Obsidian";
        wchar_t cmd[256];
        swprintf(cmd, 256, L"winget install --id %ls -e --silent --accept-package-agreements --accept-source-agreements", id);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        const wchar_t *what = job->action == A_TAILSCALE ? L"Tailscale" : L"Obsidian";
        wchar_t buf[200];
        if (code == 0) swprintf(buf, 200, L"%ls quedó instalado ✓", what);
        else swprintf(buf, 200, L"No pude instalar %ls automáticamente (código %lu). Puedes bajarlo de su página.", what, (unsigned long)code);
        job->result = xwcsdup(buf);
    }
    CoUninitialize();
    free(job->name);
    free(job->host);
    job->name = job->host = NULL;
    HWND h = S.hwnd;
    if (h && IsWindow(h)) PostMessageW(h, WM_APP_ASYNC_DONE, 0, (LPARAM)job);
    else {
        free(job->result);
        free(job);
    }
    return 0;
}

static void run_async_on(int action, const MeshDevice *dev, const wchar_t *busy)
{
    AsyncJob *job = xcalloc(1, sizeof *job);
    job->action = action;
    if (dev) {
        job->name = xstrdup(dev->name);
        job->host = xstrdup(dev->host);
    }
    set_status(busy);
    HANDLE t = CreateThread(NULL, 0, async_worker, job, 0, NULL);
    if (t) CloseHandle(t);
}

static void run_async(int action, const wchar_t *busy)
{
    run_async_on(action, NULL, busy);
}

static void pick_sound(void)
{
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = S.hwnd;
    of.lpstrFilter = L"Audio (*.mp3;*.wav)\0*.mp3;*.wav\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    const wchar_t *ext = wcsrchr(file, L'.');
    bool mp3 = ext && !_wcsicmp(ext, L".mp3");
    ensure_dir(g_paths.sounds_dir);
    wchar_t *dst = path_join(g_paths.sounds_dir, mp3 ? L"activacion.mp3" : L"activacion.wav");
    wchar_t *other = path_join(g_paths.sounds_dir, mp3 ? L"activacion.wav" : L"activacion.mp3");
    bool ok = CopyFileW(file, dst, FALSE);
    if (ok) DeleteFileW(other);
    free(dst);
    free(other);
    refresh_sound_text();
    set_status(ok ? L"Listo, Sokari va a usar ese sonido al activarse." : L"No pude copiar ese archivo.");
    layout();
}

static void clear_sound(void)
{
    const wchar_t *names[] = {L"activacion.mp3", L"activacion.wav"};
    for (int i = 0; i < 2; i++) {
        wchar_t *p = path_join(g_paths.sounds_dir, names[i]);
        DeleteFileW(p);
        free(p);
    }
    refresh_sound_text();
    set_status(L"Listo, vuelve a usar el tono de Sokari.");
    layout();
}

static bool copy_text(const wchar_t *w)
{
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (g) {
        memcpy(GlobalLock(g), w, bytes);
        GlobalUnlock(g);
        if (OpenClipboard(S.hwnd)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
            CloseClipboard();
        }
        if (!ok) GlobalFree(g);
    }
    return ok;
}

static void copy_secret(void)
{
    char *secret = config_mesh_secret(true);
    wchar_t *w = utf8_to_wide(secret);
    bool ok = copy_text(w);
    SetWindowTextW(S.edits[F_MESH], w);
    SecureZeroMemory(secret, strlen(secret));
    free(secret);
    free(w);
    set_status(ok ? L"Secreto copiado. Pégalo en este mismo campo en tu otra PC. No lo compartas con nadie."
                  : L"No pude copiar al portapapeles.");
}

static void do_action(int action)
{
    if (action >= A_DEV_PROBE && action < A_DEV_PROBE + MAX_DEVICE_ROWS && action - A_DEV_PROBE < S.ndevs) {
        const MeshDevice *d = &S.devs[action - A_DEV_PROBE];
        wchar_t *nm = utf8_to_wide(d->name);
        wchar_t busy[200];
        swprintf(busy, 200, L"Probando %ls…", nm);
        free(nm);
        run_async_on(A_DEV_PROBE, d, busy);
        return;
    }
    if (action >= A_DEV_REMOVE && action < A_DEV_REMOVE + MAX_DEVICE_ROWS && action - A_DEV_REMOVE < S.ndevs) {
        char *msg = str_printf("Quité %s de tus PCs.", S.devs[action - A_DEV_REMOVE].name);
        mesh_device_remove(S.devs[action - A_DEV_REMOVE].name);
        wchar_t *w = utf8_to_wide(msg);
        set_status(w);
        free(w);
        free(msg);
        layout();
        return;
    }
    switch (action) {
    case A_FIREWALL:
        run_async(A_FIREWALL, L"Windows te va a pedir permiso de administrador para el firewall…");
        break;
    case A_DETECT:
        run_async(A_DETECT, L"Buscando tus PCs en Tailscale…");
        break;
    case A_DIAGNOSE:
        run_async(A_DIAGNOSE, L"Revisando la malla paso a paso (tarda unos segundos)…");
        break;
    case A_GROQ_LINK:
        ShellExecuteW(NULL, L"open", L"https://console.groq.com/keys", NULL, NULL, SW_SHOWNORMAL);
        break;
    case A_SHOW_API:
        S.api_visible = !S.api_visible;
        SendMessageW(S.edits[F_API], EM_SETPASSWORDCHAR, S.api_visible ? 0 : 0x25CF, 0);
        InvalidateRect(S.edits[F_API], NULL, TRUE);
        layout();
        break;
    case A_SHOW_STOP:
        S.stop_visible = !S.stop_visible;
        SendMessageW(S.edits[F_STOP], EM_SETPASSWORDCHAR, S.stop_visible ? 0 : 0x25CF, 0);
        InvalidateRect(S.edits[F_STOP], NULL, TRUE);
        layout();
        break;
    case A_SHOW_KEY:
    case A_SHOW_KEY + 1:
    case A_SHOW_KEY + 2:
    case A_SHOW_KEY + 3: {
        int k = action - A_SHOW_KEY;
        S.key_visible[k] = !S.key_visible[k];
        SendMessageW(S.edits[F_NVIDIA + k], EM_SETPASSWORDCHAR, S.key_visible[k] ? 0 : 0x25CF, 0);
        InvalidateRect(S.edits[F_NVIDIA + k], NULL, TRUE);
        layout();
        break;
    }
    case A_KEY_LINK:
    case A_KEY_LINK + 1:
    case A_KEY_LINK + 2:
    case A_KEY_LINK + 3: {
        static const wchar_t *const URLS[] = {L"https://build.nvidia.com", L"https://platform.deepseek.com/api_keys",
                                              L"https://openrouter.ai/keys", L"https://z.ai"};
        ShellExecuteW(NULL, L"open", URLS[action - A_KEY_LINK], NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case A_SHOW_MESH:
        S.mesh_visible = !S.mesh_visible;
        SendMessageW(S.edits[F_MESH], EM_SETPASSWORDCHAR, S.mesh_visible ? 0 : 0x25CF, 0);
        InvalidateRect(S.edits[F_MESH], NULL, TRUE);
        layout();
        break;
    case A_RESET_PW:
        if (MessageBoxW(S.hwnd,
                        L"Esto quita la contraseña de TODOS los perfiles guardados en esta PC (el tuyo y el de "
                        L"cualquier otra persona que haya protegido el suyo). Solo se puede hacer desde aquí, nunca "
                        L"por voz.\n\n¿Continuar?",
                        L"Restablecer contraseñas", MB_YESNO | MB_ICONWARNING) == IDYES) {
            int n = reset_all_profile_passwords();
            wchar_t msg[96];
            swprintf(msg, 96, n ? L"Se quitaron %d contraseña(s)." : L"No había ninguna contraseña puesta.", n);
            set_status(msg);
        }
        break;
    case A_TAILSCALE:
        run_async(A_TAILSCALE, L"Instalando Tailscale… (Windows puede pedirte permiso de administrador)");
        break;
    case A_OBSIDIAN:
        run_async(A_OBSIDIAN, L"Instalando Obsidian…");
        break;
    case A_COPY_SECRET:
        copy_secret();
        break;
    case A_PICK_SOUND:
        pick_sound();
        break;
    case A_CLEAR_SOUND:
        clear_sound();
        break;
    case A_OPEN_FOLDER:
        ShellExecuteW(NULL, L"open", g_paths.local_dir, NULL, NULL, SW_SHOWNORMAL);
        break;
    case A_OPEN_SKILLS: {
        wchar_t *dir = skills_user_dir();
        ensure_dir(dir);
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
        free(dir);
        break;
    }
    case A_NEW_SKILL: {
        wchar_t *path = skills_new_template();
        if (path) ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
        else set_status(L"No pude crear la skill nueva.");
        free(path);
        refresh_skills_text();
        layout();
        break;
    }
    case A_CHECK_UPDATE:
        run_async(A_CHECK_UPDATE, L"Buscando actualizaciones…");
        break;
    case A_SAVE:
        save();
        break;
    case A_CANCEL:
        if (S.home_mode) {
            load_values();
            S.section = SEC_HOME;
            set_status(NULL);
            layout();
        } else {
            DestroyWindow(S.hwnd);
        }
        break;
    case A_START:
        start_or_show();
        break;
    case A_GO_SETTINGS:
        S.section = SEC_ACCOUNT;
        layout();
        break;
    case A_MUTE:
        if (ui_message_window()) SendMessageW(ui_message_window(), WM_COMMAND, IDM_MUTE, 0);
        break;
    case A_TEST_AUDIO: {
        wchar_t msg[200];
        const wchar_t *out = g_output_labels[S.home_output];
        if (voice_running()) {
            voice_test_audio();
            swprintf(msg, 200, L"Probando por «%ls»: suena el tono y luego mi voz.", out);
        } else {
            HANDLE t = CreateThread(NULL, 0, chime_worker, NULL, 0, NULL);
            if (t) CloseHandle(t);
            swprintf(msg, 200, L"Sonó el tono por «%ls». La voz se prueba con Sokari iniciado.", out);
        }
        set_status(msg);
        break;
    }
    case A_STYLE:
        layout(); /* con cara aparecen sus opciones */
        if (S.style >= 2)
            set_status(L"La cara (beta) expresa el estado de Sokari, no sentimientos. Le pide a la IA la emoción de "
                       L"cada respuesta: unos 70 tokens más.");
        InvalidateRect(S.hwnd, NULL, FALSE);
        break;
    case A_TEST_ANIM:
        if (S.appear == 3) {
            set_status(L"Con «Ninguna» aparece y desaparece de golpe: no hay nada que probar.");
        } else if (S.display_mode == DISPLAY_MINIMIZED) {
            set_status(L"En Minimizado no hay animación: la esfera vive en la barra de tareas.");
        } else if (!ui_preview_appear(S.appear)) {
            set_status(L"La animación se prueba con Sokari iniciado y fuera de Minimizado.");
        } else {
            set_status(L"Mira la esfera. Si te gusta, dale a Guardar.");
        }
        break;
    case A_TEST_VOICE:
        if (S.voice_index >= S.nvoices) {
            set_status(L"No encontré voces instaladas en Windows.");
        } else if (voice_running()) {
            voice_preview(S.voices[S.voice_index].id);
            set_status(L"Escucha: así suena esa voz. Si te gusta, dale a Guardar.");
        } else {
            set_status(L"La voz se prueba con Sokari iniciado.");
        }
        break;
    case A_QUIT:
        if (ui_message_window()) PostMessageW(ui_message_window(), WM_APP_QUIT, 0, 0);
        break;
    case A_FULL_ACCESS:
        config_set_full_access(S.home_full != 0);
        S.full_access = S.home_full;
        set_status(S.home_full ? L"Acceso completo prendido: ya no te pregunta nada, salvo antes de borrar."
                               : L"Acceso completo apagado: vuelve a preguntar antes de acciones delicadas.");
        break;
    case A_MODE_CHANGED:
        S.display_mode = S.home_display;
        ui_set_display_mode(S.home_display);
        break;
    case A_OUTPUT_CHANGED: {
        const char *name = output_name_at(S.home_output);
        S.output_index = S.home_output;
        config_set_output(name);
        speaker_set_device(name);
        wchar_t msg[160];
        swprintf(msg, 160, L"Salida de audio: %ls. Dale a Probar audio para oírla.", g_output_labels[S.home_output]);
        set_status(msg);
        layout();
        break;
    }
    }
}

static int hit(int x, int y)
{
    for (int i = S.nwidgets - 1; i >= 0; i--) {
        Widget *w = &S.widgets[i];
        if (w->type == W_LABEL || w->type == W_HELP || w->type == W_EDIT) continue;
        if (x >= w->r.left && x < w->r.right && y >= w->r.top && y < w->r.bottom) return i;
    }
    return -1;
}

static void slider_set(Widget *w, int x)
{
    int x0 = w->r.left + dp(8), x1 = w->r.right - dp(64);
    int v = (x - x0) * 100 / (x1 - x0 ? x1 - x0 : 1);
    *w->value = v < 0 ? 0 : v > 100 ? 100 : v;
    InvalidateRect(S.hwnd, NULL, FALSE);
}

static void click(int i, int x, int y)
{
    Widget *w = &S.widgets[i];
    switch (w->type) {
    case W_SEGMENT: {
        int cw = (w->r.right - w->r.left) / w->options;
        int idx = (x - w->r.left) / (cw ? cw : 1);
        if (idx >= 0 && idx < w->options) *w->value = idx;
        break;
    }
    case W_CARDS: {
        int idx = (y - w->r.top) / ((w->item_h ? w->item_h : dp(CARD_H)) + dp(CARD_GAP));
        if (idx >= 0 && idx < w->options && idx != *w->value) {
            *w->value = idx;
            if (!w->descs) {
                layout(); /* compactas: debajo va la descripción del modo elegido */
                return;
            }
        }
        break;
    }
    case W_TOGGLE:
        *w->value = !*w->value;
        if (w->action) do_action(w->action);
        break;
    case W_SLIDER:
        S.drag = i;
        SetCapture(S.hwnd);
        slider_set(w, x);
        break;
    case W_DROPDOWN: {
        HMENU m = CreatePopupMenu();
        for (int k = 0; k < w->options; k++)
            AppendMenuW(m, MF_STRING | (*w->value == k ? MF_CHECKED : 0), (UINT_PTR)(k + 1), w->labels[k]);
        POINT pt = {w->r.left, w->r.bottom + dp(4)};
        ClientToScreen(S.hwnd, &pt);
        int sel = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, S.hwnd, NULL);
        DestroyMenu(m);
        if (sel > 0 && sel - 1 != *w->value) {
            *w->value = sel - 1;
            if (w->action) {
                do_action(w->action); /* puede volver a armar la ventana: w ya no sirve */
                return;
            }
        }
        break;
    }
    case W_LINK:
    case W_BUTTON:
        do_action(w->action);
        return;
    default:
        break;
    }
    InvalidateRect(S.hwnd, NULL, FALSE);
}

static LRESULT CALLBACK edit_subclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(S.hwnd, NULL, FALSE);
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        do_action(A_SAVE);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        S.hwnd = h;
        S.dpi = (int)GetDpiForWindow(h);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
        COLORREF cap = C_BG;
        DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
        S.surface_brush = CreateSolidBrush(C_SURFACE);
        for (int i = 0; i < F_EDIT_COUNT; i++) {
            bool secret = i == F_API || i == F_PROFILE_PW || i == F_STOP || i == F_MESH || (i >= F_NVIDIA && i <= F_GLM);
            DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0);
            S.edits[i] = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)(100 + i), S.inst, NULL);
            SetWindowTheme(S.edits[i], L"DarkMode_CFD", NULL);
            SetWindowSubclass(S.edits[i], edit_subclass, 1, 0);
        }
        SendMessageW(S.edits[F_API], EM_SETCUEBANNER, TRUE, (LPARAM)L"gsk_...");
        SendMessageW(S.edits[F_NAME], EM_SETCUEBANNER, TRUE, (LPARAM)L"Tu nombre");
        SendMessageW(S.edits[F_STOP], EM_SETCUEBANNER, TRUE, (LPARAM)L"Solo tú la sabes");
        SendMessageW(S.edits[F_PROFILE_PW], EM_SETCUEBANNER, TRUE, (LPARAM)L"Sin cambios");
        SendMessageW(S.edits[F_API], EM_SETPASSWORDCHAR, 0x25CF, 0);
        SendMessageW(S.edits[F_PROFILE_PW], EM_SETPASSWORDCHAR, 0x25CF, 0);
        SendMessageW(S.edits[F_STOP], EM_SETPASSWORDCHAR, 0x25CF, 0);
        SendMessageW(S.edits[F_MESH], EM_SETPASSWORDCHAR, 0x25CF, 0);
        for (int k = F_NVIDIA; k <= F_GLM; k++) {
            SendMessageW(S.edits[k], EM_SETPASSWORDCHAR, 0x25CF, 0);
            SendMessageW(S.edits[k], EM_SETCUEBANNER, TRUE, (LPARAM)L"Opcional");
        }
        SendMessageW(S.edits[F_MESH], EM_SETCUEBANNER, TRUE, (LPARAM)L"Se genera solo; pega aquí el de tu otra PC");
        make_fonts();
        load_values();
        S.hover = S.pressed = S.drag = -1;
        HDC dc = GetDC(h);
        paint(dc);
        ReleaseDC(h, dc);
        layout();
        SetFocus(S.section == SEC_ACCOUNT ? S.edits[F_API] : h);
        return 0;
    }
    case WM_DPICHANGED: {
        S.dpi = HIWORD(w);
        RECT *r = (RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        make_fonts();
        layout();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_SURFACE);
        return (LRESULT)S.surface_brush;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (S.drag >= 0) {
            slider_set(&S.widgets[S.drag], x);
            return 0;
        }
        int hv = hit(x, y);
        if (hv != S.hover) {
            S.hover = hv;
            InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (S.hover != -1) {
            S.hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && S.hover >= 0) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int side = dp(210);
        if (x < side && !S.first_run) {
            int idx = (y - dp(110)) / dp(46);
            if (y >= dp(110) && idx >= 0 && idx < SEC_COUNT && idx != S.section) {
                S.section = idx;
                set_status(NULL);
                layout();
            }
            return 0;
        }
        int i = hit(x, y);
        if (i >= 0) click(i, x, y);
        else SetFocus(h);
        return 0;
    }
    case WM_LBUTTONUP:
        if (S.drag >= 0) {
            S.drag = -1;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        /* Esc: en una sección vuelve a Inicio (o cierra, si se abrió como
           Configuración); en Inicio cierra, salvo que Sokari no haya arrancado. */
        if (w == VK_ESCAPE && !S.first_run) {
            if (S.section != SEC_HOME) do_action(A_CANCEL);
            else if (!S.quit_on_close) DestroyWindow(h);
        }
        return 0;
    case WM_APP_ASYNC_DONE: {
        AsyncJob *job = (AsyncJob *)l;
        if (job->action == A_DIAGNOSE) {
            bool copied = copy_text(job->result);
            MessageBoxW(h, job->result, L"Revisión de la malla", MB_OK | (wcsstr(job->result, L"✗") ? MB_ICONWARNING : MB_ICONINFORMATION));
            set_status(copied ? L"Copié el reporte: si algo salió con ✗ y no sabes qué hacer, pégamelo."
                              : L"Revisé la malla (el reporte también quedó en sokari.log).");
        } else if (job->action == A_DEV_PROBE && wcslen(job->result) > 90) {
            /* Lo que hay que hacer no cabe abajo: completo, en su ventana. */
            MessageBoxW(h, job->result, L"Probar", MB_OK | MB_ICONWARNING);
            set_status(L"No contestó: te dejé en la ventana qué revisar y en qué PC.");
        } else {
            set_status(job->result);
        }
        refresh_tailscale_text();
        layout();
        free(job->result);
        free(job);
        return 0;
    }
    case WM_CLOSE:
        if (S.first_run &&
            MessageBoxW(h, L"Sin una API key de Groq, Sokari no puede funcionar. ¿Cerrar Sokari?", L"Sokari",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: {
        bool quit = S.quit_on_close;
        for (int i = 0; i < F_EDIT_COUNT; i++) S.edits[i] = NULL;
        if (S.mem) {
            SelectObject(S.mem, S.old_bmp);
            DeleteObject(S.bmp);
            DeleteDC(S.mem);
            S.mem = NULL;
        }
        DeleteObject(S.surface_brush);
        free(S.status_line);
        S.status_line = NULL;
        mesh_devices_free(S.devs, S.ndevs);
        S.devs = NULL;
        S.ndevs = 0;
        S.hwnd = NULL;
        if (quit) PostMessageW(ui_message_window(), WM_APP_QUIT, 0, 0);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

HWND settings_window(void)
{
    return S.hwnd;
}

int settings_overflow(void)
{
    if (!S.hwnd) return 0;
    RECT cr;
    GetClientRect(S.hwnd, &cr);
    int limit = cr.bottom - dp(64) - dp(10), over = 0;
    for (int i = 0; i < S.ncontent; i++)
        if (S.widgets[i].r.bottom - limit > over) over = S.widgets[i].r.bottom - limit;
    return over;
}

void settings_sync(void)
{
    if (!S.hwnd) return;
    AppConfig c = config_snapshot();
    S.display_mode = c.display_mode;
    S.full_access = S.home_full = c.full_access;
    config_free(&c);
    layout();
}

static void open_window(HINSTANCE inst, bool first_run, bool home, bool starting, SettingsSavedFn on_saved)
{
    if (S.hwnd) {
        if (!S.first_run) {
            if (home) S.home_mode = true;
            S.section = home ? SEC_HOME : SEC_ACCOUNT;
            set_status(NULL);
            layout();
        }
        ShowWindow(S.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(S.hwnd);
        return;
    }
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = SETTINGS_CLASS;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = ui_app_icon(32);
        wc.hIconSm = ui_app_icon(16);
        RegisterClassExW(&wc);
        registered = true;
    }
    S.inst = inst;
    S.first_run = first_run;
    S.home_mode = home;
    S.quit_on_close = first_run || starting;
    S.on_saved = on_saved;
    S.section = home ? SEC_HOME : SEC_ACCOUNT;
    S.api_visible = S.stop_visible = S.mesh_visible = false;
    memset(S.key_visible, 0, sizeof S.key_visible);
    UINT dpi = GetDpiForSystem();
    int cw = MulDiv(900, (int)dpi, 96), ch = MulDiv(740, (int)dpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (ch > wa.bottom - wa.top - 40) ch = wa.bottom - wa.top - 40;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    CreateWindowExW(0, SETTINGS_CLASS, first_run ? L"Bienvenido a Sokari" : home ? L"Sokari" : L"Sokari — Configuración",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, NULL,
                    NULL, inst, NULL);
    ShowWindow(S.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(S.hwnd);
}

void settings_open(HINSTANCE inst, bool first_run, SettingsSavedFn on_saved)
{
    open_window(inst, first_run, false, false, on_saved);
}

void home_open(HINSTANCE inst, bool starting, SettingsSavedFn on_start)
{
    open_window(inst, false, true, starting, on_start);
}

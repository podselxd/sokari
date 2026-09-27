#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* candados (en Linux, src/linux/include/windows.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "util.h"

AppPaths g_paths;

static AppConfig g_cfg;
static SRWLOCK g_lock = SRWLOCK_INIT;

/* "windowed_borderless" es la esfera flotante: el nombre viene de antes de que
   existiera el modo Ventana y se queda así para leer configuraciones viejas. */
static const char *DISPLAY_KEYS[DISPLAY_MODE_COUNT] = {"fullscreen", "fullscreen_borderless", "windowed_borderless",
                                                      "windowed", "minimized"};

static const char *const APPEAR_KEYS[] = {"materializar", "deslizar", "zoom", "ninguna"};
static const char *const STYLE_KEYS[] = {"puntos", "lineas", "cara_ojos", "cara_boca", "cara_puntos"};
#define STYLE_COUNT (int)(sizeof STYLE_KEYS / sizeof *STYLE_KEYS)

const char *sphere_style_key(int style)
{
    return STYLE_KEYS[style >= 0 && style < STYLE_COUNT ? style : 0];
}

int sphere_style_from_key(const char *key)
{
    for (int i = 0; i < STYLE_COUNT; i++)
        if (key && !strcmp(key, STYLE_KEYS[i])) return i;
    return 0;
}

const char *appear_anim_key(int anim)
{
    return APPEAR_KEYS[anim >= 0 && anim < 4 ? anim : 0];
}

int appear_anim_from_key(const char *key)
{
    for (int i = 0; i < 4; i++)
        if (key && !strcmp(key, APPEAR_KEYS[i])) return i;
    return 0;
}

char *config_clean_ai_order(const char *text)
{
    static const char *const KNOWN[] = {"groq", "nvidia", "deepseek", "openrouter", "glm"};
    StrBuf sb;
    sb_init(&sb);
    char *low = str_lower(text ? text : ""), *ctx = NULL;
    for (char *tok = strtok_s(low, ", ;", &ctx); tok; tok = strtok_s(NULL, ", ;", &ctx)) {
        for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) {
            char pat[24];
            snprintf(pat, sizeof pat, ",%s,", KNOWN[i]);
            char *have = str_printf(",%s,", sb.data ? sb.data : "");
            if (!strcmp(tok, KNOWN[i]) && !strstr(have, pat)) sb_appendf(&sb, "%s%s", sb.len ? "," : "", KNOWN[i]);
            free(have);
        }
    }
    free(low);
    if (!sb.len) {
        sb_free(&sb);
        return xstrdup(DEFAULT_AI_ORDER);
    }
    return sb.data;
}

const char *display_mode_key(int mode)
{
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) mode = DISPLAY_FULLSCREEN_BORDERLESS;
    return DISPLAY_KEYS[mode];
}

static void set_str(char **field, const char *value)
{
    free(*field);
    *field = value ? xstrdup(value) : xstrdup("");
}

static bool parse_bool(const char *v)
{
    return v && (!strcmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "si") || !_stricmp(v, "yes"));
}

static void defaults(AppConfig *c)
{
    memset(c, 0, sizeof *c);
    c->groq_api_key = xstrdup("");
    c->user_name = xstrdup("");
    c->stop_word = xstrdup("");
    c->mesh_secret = xstrdup("");
    c->voice = xstrdup("");
    c->mic_name = xstrdup("");
    c->output_name = xstrdup("");
    c->fw_asked = xstrdup("");
    c->nvidia_key = xstrdup("");
    c->deepseek_key = xstrdup("");
    c->openrouter_key = xstrdup("");
    c->glm_key = xstrdup("");
    c->ai_order = xstrdup(DEFAULT_AI_ORDER);
    c->skills_off = xstrdup("");
    c->city = xstrdup("");
    c->display_mode = DISPLAY_FULLSCREEN;
    c->resolution = 0;
    c->volume = 100;
    c->wake_sensitivity = 67;
    c->sphere_style = 0;
    c->appear_anim = 0;
    c->face_level = 1;
    c->face_symbols = true;
    c->orb_x = c->orb_y = -1;
    c->win_x = c->win_y = c->win_w = c->win_h = -1;
    c->subtitles = true;
    c->autostart = false;
    c->mic_muted = false;
    c->show_only_talking = true;
    c->full_access = true;
    c->end_silence = 1;
    c->duck = true;
}

static void apply_kv(AppConfig *c, const char *key, const char *value)
{
    if (!strcmp(key, "GROQ_API_KEY")) set_str(&c->groq_api_key, value);
    else if (!strcmp(key, "SOKARI_USER_NAME")) set_str(&c->user_name, value);
    else if (!strcmp(key, "SOKARI_STOP_WORD")) set_str(&c->stop_word, value);
    else if (!strcmp(key, "SOKARI_MESH_SECRET")) set_str(&c->mesh_secret, value);
    else if (!strcmp(key, "SOKARI_VOICE")) set_str(&c->voice, value);
    else if (!strcmp(key, "SOKARI_MIC")) set_str(&c->mic_name, value);
    else if (!strcmp(key, "SOKARI_OUTPUT")) set_str(&c->output_name, value);
    else if (!strcmp(key, "SOKARI_FW_ASKED")) set_str(&c->fw_asked, value);
    else if (!strcmp(key, "NVIDIA_API_KEY")) set_str(&c->nvidia_key, value);
    else if (!strcmp(key, "DEEPSEEK_API_KEY")) set_str(&c->deepseek_key, value);
    else if (!strcmp(key, "OPENROUTER_API_KEY")) set_str(&c->openrouter_key, value);
    else if (!strcmp(key, "GLM_API_KEY")) set_str(&c->glm_key, value);
    else if (!strcmp(key, "SOKARI_AI_ORDER")) set_str(&c->ai_order, *value ? value : DEFAULT_AI_ORDER);
    else if (!strcmp(key, "SOKARI_SKILLS_OFF")) set_str(&c->skills_off, value);
    else if (!strcmp(key, "SOKARI_CITY")) set_str(&c->city, value);
    else if (!strcmp(key, "SOKARI_DISPLAY_MODE")) {
        for (int i = 0; i < DISPLAY_MODE_COUNT; i++)
            if (!strcmp(value, DISPLAY_KEYS[i])) c->display_mode = i;
    } else if (!strcmp(key, "SOKARI_RESOLUTION")) {
        c->resolution = atoi(value);
        if (c->resolution != 720 && c->resolution != 1080 && c->resolution != 1440 && c->resolution != 2160)
            c->resolution = 0;
    } else if (!strcmp(key, "SOKARI_VOLUME")) {
        int v = atoi(value);
        c->volume = v < 0 ? 0 : v > 100 ? 100 : v;
    } else if (!strcmp(key, "SOKARI_WAKE_SENSITIVITY")) {
        int v = atoi(value);
        c->wake_sensitivity = v < 0 ? 0 : v > 100 ? 100 : v;
    } else if (!strcmp(key, "SOKARI_END_SILENCE")) {
        c->end_silence = !strcmp(value, "corta") ? 0 : !strcmp(value, "larga") ? 2 : 1;
    } else if (!strcmp(key, "SOKARI_DUCK")) {
        c->duck = parse_bool(value);
    } else if (!strcmp(key, "SOKARI_SPHERE_STYLE")) {
        c->sphere_style = sphere_style_from_key(value);
    } else if (!strcmp(key, "SOKARI_ANIMATION")) {
        c->appear_anim = appear_anim_from_key(value);
    } else if (!strcmp(key, "SOKARI_FACE_LEVEL")) {
        c->face_level = !strcmp(value, "poco") ? 0 : !strcmp(value, "mucho") ? 2 : 1;
    } else if (!strcmp(key, "SOKARI_FACE_SYMBOLS")) {
        c->face_symbols = parse_bool(value);
    } else if (!strcmp(key, "SOKARI_ORB_POS")) {
        if (sscanf(value, "%d,%d", &c->orb_x, &c->orb_y) != 2) c->orb_x = c->orb_y = -1;
    } else if (!strcmp(key, "SOKARI_WINDOW")) {
        if (sscanf(value, "%d,%d,%d,%d", &c->win_x, &c->win_y, &c->win_w, &c->win_h) != 4 || c->win_w <= 0 ||
            c->win_h <= 0)
            c->win_x = c->win_y = c->win_w = c->win_h = -1;
    } else if (!strcmp(key, "SOKARI_SUBTITLES")) c->subtitles = parse_bool(value);
    else if (!strcmp(key, "SOKARI_AUTOSTART")) c->autostart = parse_bool(value);
    else if (!strcmp(key, "SOKARI_MIC_MUTED")) c->mic_muted = parse_bool(value);
    else if (!strcmp(key, "SOKARI_SHOW_ONLY_TALKING")) c->show_only_talking = parse_bool(value);
    else if (!strcmp(key, "SOKARI_FULL_ACCESS")) c->full_access = parse_bool(value);
    else if (!strcmp(key, "SOKARI_MEXA")) c->mexa = parse_bool(value);
}

static bool load_env_file(const wchar_t *path, AppConfig *c)
{
    size_t len;
    char *text = read_file_all(path, &len);
    if (!text) return false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = str_trim(line);
        char *eq = strchr(t, '=');
        if (*t && *t != '#' && eq) {
            *eq = 0;
            char *k = str_trim(t), *v = str_trim(eq + 1);
            apply_kv(c, k, v);
            free(k);
            free(v);
        }
        free(t);
    }
    free(text);
    return true;
}

void config_load(void)
{
    AcquireSRWLockExclusive(&g_lock);
    defaults(&g_cfg);
    load_env_file(g_paths.config_file, &g_cfg);
    ReleaseSRWLockExclusive(&g_lock);
}

static void put_kv(StrBuf *sb, const char *key, const char *value)
{
    char *clean = xstrdup(value ? value : "");
    for (char *p = clean; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
    sb_appendf(sb, "%s=%s\n", key, clean);
    free(clean);
}

static bool save_locked(void)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "# Configuración de Sokari. Se edita desde la ventana de Configuración.\n");
    put_kv(&sb, "GROQ_API_KEY", g_cfg.groq_api_key);
    put_kv(&sb, "SOKARI_USER_NAME", g_cfg.user_name);
    put_kv(&sb, "SOKARI_STOP_WORD", g_cfg.stop_word);
    put_kv(&sb, "SOKARI_MESH_SECRET", g_cfg.mesh_secret);
    put_kv(&sb, "SOKARI_VOICE", g_cfg.voice);
    put_kv(&sb, "SOKARI_MIC", g_cfg.mic_name);
    put_kv(&sb, "SOKARI_OUTPUT", g_cfg.output_name);
    put_kv(&sb, "SOKARI_FW_ASKED", g_cfg.fw_asked);
    put_kv(&sb, "NVIDIA_API_KEY", g_cfg.nvidia_key);
    put_kv(&sb, "DEEPSEEK_API_KEY", g_cfg.deepseek_key);
    put_kv(&sb, "OPENROUTER_API_KEY", g_cfg.openrouter_key);
    put_kv(&sb, "GLM_API_KEY", g_cfg.glm_key);
    put_kv(&sb, "SOKARI_AI_ORDER", g_cfg.ai_order);
    put_kv(&sb, "SOKARI_SKILLS_OFF", g_cfg.skills_off);
    put_kv(&sb, "SOKARI_CITY", g_cfg.city);
    put_kv(&sb, "SOKARI_DISPLAY_MODE", display_mode_key(g_cfg.display_mode));
    sb_appendf(&sb, "SOKARI_RESOLUTION=%d\n", g_cfg.resolution);
    sb_appendf(&sb, "SOKARI_VOLUME=%d\n", g_cfg.volume);
    sb_appendf(&sb, "SOKARI_WAKE_SENSITIVITY=%d\n", g_cfg.wake_sensitivity);
    sb_appendf(&sb, "SOKARI_SPHERE_STYLE=%s\n", sphere_style_key(g_cfg.sphere_style));
    sb_appendf(&sb, "SOKARI_ANIMATION=%s\n", appear_anim_key(g_cfg.appear_anim));
    sb_appendf(&sb, "SOKARI_FACE_LEVEL=%s\n", g_cfg.face_level == 0 ? "poco" : g_cfg.face_level == 2 ? "mucho" : "normal");
    sb_appendf(&sb, "SOKARI_FACE_SYMBOLS=%d\n", g_cfg.face_symbols ? 1 : 0);
    sb_appendf(&sb, "SOKARI_END_SILENCE=%s\n", g_cfg.end_silence == 0 ? "corta" : g_cfg.end_silence == 2 ? "larga" : "normal");
    sb_appendf(&sb, "SOKARI_DUCK=%d\n", g_cfg.duck ? 1 : 0);
    sb_appendf(&sb, "SOKARI_ORB_POS=%d,%d\n", g_cfg.orb_x, g_cfg.orb_y);
    sb_appendf(&sb, "SOKARI_WINDOW=%d,%d,%d,%d\n", g_cfg.win_x, g_cfg.win_y, g_cfg.win_w, g_cfg.win_h);
    sb_appendf(&sb, "SOKARI_SUBTITLES=%d\n", g_cfg.subtitles ? 1 : 0);
    sb_appendf(&sb, "SOKARI_AUTOSTART=%d\n", g_cfg.autostart ? 1 : 0);
    sb_appendf(&sb, "SOKARI_MIC_MUTED=%d\n", g_cfg.mic_muted ? 1 : 0);
    sb_appendf(&sb, "SOKARI_SHOW_ONLY_TALKING=%d\n", g_cfg.show_only_talking ? 1 : 0);
    sb_appendf(&sb, "SOKARI_FULL_ACCESS=%d\n", g_cfg.full_access ? 1 : 0);
    sb_appendf(&sb, "SOKARI_MEXA=%d\n", g_cfg.mexa ? 1 : 0);
    ensure_dir(g_paths.local_dir);
    bool ok = write_file_atomic(g_paths.config_file, sb.data, sb.len);
    sb_free(&sb);
    if (!ok) log_msg("No pude guardar la configuración.");
    return ok;
}

bool config_save(void)
{
    AcquireSRWLockShared(&g_lock);
    bool ok = save_locked();
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static void copy_cfg(AppConfig *dst, const AppConfig *src)
{
    *dst = *src;
    dst->groq_api_key = xstrdup(src->groq_api_key);
    dst->user_name = xstrdup(src->user_name);
    dst->stop_word = xstrdup(src->stop_word);
    dst->mesh_secret = xstrdup(src->mesh_secret);
    dst->voice = xstrdup(src->voice);
    dst->mic_name = xstrdup(src->mic_name);
    dst->output_name = xstrdup(src->output_name);
    dst->fw_asked = xstrdup(src->fw_asked);
    dst->nvidia_key = xstrdup(src->nvidia_key);
    dst->deepseek_key = xstrdup(src->deepseek_key);
    dst->openrouter_key = xstrdup(src->openrouter_key);
    dst->glm_key = xstrdup(src->glm_key);
    dst->ai_order = xstrdup(src->ai_order);
    dst->skills_off = xstrdup(src->skills_off);
    dst->city = xstrdup(src->city);
}

void config_free(AppConfig *c)
{
    free(c->groq_api_key);
    free(c->user_name);
    free(c->stop_word);
    free(c->mesh_secret);
    free(c->voice);
    free(c->mic_name);
    free(c->output_name);
    free(c->fw_asked);
    char *keys[] = {c->nvidia_key, c->deepseek_key, c->openrouter_key, c->glm_key};
    for (int i = 0; i < 4; i++) {
        if (keys[i]) SecureZeroMemory(keys[i], strlen(keys[i]));
        free(keys[i]);
    }
    free(c->ai_order);
    free(c->skills_off);
    free(c->city);
    memset(c, 0, sizeof *c);
}

AppConfig config_snapshot(void)
{
    AppConfig c;
    AcquireSRWLockShared(&g_lock);
    copy_cfg(&c, &g_cfg);
    ReleaseSRWLockShared(&g_lock);
    return c;
}

void config_apply(const AppConfig *c)
{
    AcquireSRWLockExclusive(&g_lock);
    AppConfig old = g_cfg;
    copy_cfg(&g_cfg, c);
    config_free(&old);
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

#define GETTER_STR(fn, field)                  \
    char *fn(void)                             \
    {                                          \
        AcquireSRWLockShared(&g_lock);         \
        char *r = xstrdup(g_cfg.field);        \
        ReleaseSRWLockShared(&g_lock);         \
        return r;                              \
    }

GETTER_STR(config_api_key, groq_api_key)
GETTER_STR(config_stop_word, stop_word)
GETTER_STR(config_user_name, user_name)

int config_volume(void)
{
    AcquireSRWLockShared(&g_lock);
    int v = g_cfg.volume;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

float config_wake_threshold(void)
{
    AcquireSRWLockShared(&g_lock);
    int s = g_cfg.wake_sensitivity;
    ReleaseSRWLockShared(&g_lock);
    /* sensibilidad 67 ~= 0.4, el umbral que usaba la versión anterior */
    return 0.9f - 0.75f * (float)s / 100.0f;
}

bool config_mic_muted(void)
{
    AcquireSRWLockShared(&g_lock);
    bool m = g_cfg.mic_muted;
    ReleaseSRWLockShared(&g_lock);
    return m;
}

bool config_show_only_talking(void)
{
    AcquireSRWLockShared(&g_lock);
    bool v = g_cfg.show_only_talking;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

bool config_skill_enabled(const char *id)
{
    AcquireSRWLockShared(&g_lock);
    char *pat = str_printf(",%s,", id), *list = str_printf(",%s,", g_cfg.skills_off ? g_cfg.skills_off : "");
    ReleaseSRWLockShared(&g_lock);
    for (char *p = list; *p; p++)
        if (*p == ' ' || *p == ';') *p = ',';
    bool on = !strstr(list, pat);
    free(pat);
    free(list);
    return on;
}

char *config_city(void)
{
    AcquireSRWLockShared(&g_lock);
    char *r = xstrdup(g_cfg.city ? g_cfg.city : "");
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void config_set_city(const char *city)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_cfg.city);
    g_cfg.city = xstrdup(city ? city : "");
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

bool config_full_access(void)
{
    AcquireSRWLockShared(&g_lock);
    bool v = g_cfg.full_access;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

int config_end_silence(void)
{
    AcquireSRWLockShared(&g_lock);
    int v = g_cfg.end_silence;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

bool config_duck(void)
{
    AcquireSRWLockShared(&g_lock);
    bool v = g_cfg.duck;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

bool config_mexa(void)
{
    AcquireSRWLockShared(&g_lock);
    bool v = g_cfg.mexa;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

bool config_face(void)
{
    AcquireSRWLockShared(&g_lock);
    bool v = g_cfg.sphere_style >= 2;
    ReleaseSRWLockShared(&g_lock);
    return v;
}

void config_set_mexa(bool on)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.mexa = on;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_full_access(bool on)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.full_access = on;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_mic_muted(bool muted)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.mic_muted = muted;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_orb_pos(int x, int y)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.orb_x = x;
    g_cfg.orb_y = y;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_window_rect(int x, int y, int w, int h)
{
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.win_x = x;
    g_cfg.win_y = y;
    g_cfg.win_w = w;
    g_cfg.win_h = h;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_display_mode(int mode)
{
    if (mode < 0 || mode >= DISPLAY_MODE_COUNT) return;
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.display_mode = mode;
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void config_set_output(const char *name)
{
    AcquireSRWLockExclusive(&g_lock);
    set_str(&g_cfg.output_name, name);
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

static bool is_address(const char *s)
{
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4) return true;
    char *low = str_lower(s);
    bool ts = strstr(low, ".ts.net") != NULL;
    free(low);
    return ts;
}

const char *config_secret_problem(const char *s)
{
    if (is_address(s))
        return "Eso es la dirección de una PC, no el secreto. El secreto es una clave larga que Sokari genera solo: "
               "cópiala de tu otra PC con «Copiar secreto» (si tus PCs usan la misma cuenta de Tailscale, ni hace "
               "falta).";
    if (strlen(s) < 12)
        return "Ese secreto es muy corto: tiene que tener al menos 12 caracteres (el que Sokari genera tiene 64).";
    return NULL;
}

char *config_provider_key(const char *provider)
{
    AcquireSRWLockShared(&g_lock);
    const char *k = !strcmp(provider, "nvidia")       ? g_cfg.nvidia_key
                    : !strcmp(provider, "deepseek")   ? g_cfg.deepseek_key
                    : !strcmp(provider, "openrouter") ? g_cfg.openrouter_key
                    : !strcmp(provider, "glm")        ? g_cfg.glm_key
                    : !strcmp(provider, "groq")       ? g_cfg.groq_api_key
                                                      : "";
    char *r = xstrdup(k ? k : "");
    ReleaseSRWLockShared(&g_lock);
    return r;
}

char *config_ai_order(void)
{
    AcquireSRWLockShared(&g_lock);
    char *r = xstrdup(*g_cfg.ai_order ? g_cfg.ai_order : DEFAULT_AI_ORDER);
    ReleaseSRWLockShared(&g_lock);
    return r;
}

char *config_fw_asked(void)
{
    AcquireSRWLockShared(&g_lock);
    char *r = xstrdup(g_cfg.fw_asked);
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void config_set_fw_asked(const char *exe)
{
    AcquireSRWLockExclusive(&g_lock);
    set_str(&g_cfg.fw_asked, exe);
    save_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

/* Se genera una sola vez (32 bytes aleatorios) y nunca lo elige ni lo dice
   el usuario en voz alta: es lo que autentica a tus otros dispositivos. Si
   quedó guardada una dirección en su lugar (una IP pegada en ese campo), se
   cambia por uno de verdad. */
char *config_mesh_secret(bool create)
{
    AcquireSRWLockExclusive(&g_lock);
    bool address = *g_cfg.mesh_secret && is_address(g_cfg.mesh_secret);
    if (create && (!*g_cfg.mesh_secret || address)) {
        unsigned char raw[32];
        random_bytes(raw, sizeof raw);
        free(g_cfg.mesh_secret);
        g_cfg.mesh_secret = hex_encode(raw, sizeof raw);
        save_locked();
        log_msg(address ? "El secreto de malla era una dirección (una IP), no un secreto: generé uno de verdad."
                        : "Secreto de malla nuevo generado (Configuración > Dispositivos para copiarlo a tus otras PCs).");
    }
    char *r = xstrdup(g_cfg.mesh_secret);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static void copy_if_missing(const wchar_t *src_dir, const wchar_t *name, const wchar_t *dst_dir)
{
    wchar_t *src = path_join(src_dir, name);
    wchar_t *dst = path_join(dst_dir, name);
    if (file_exists(src) && !file_exists(dst)) {
        ensure_dir(dst_dir);
        if (copy_file(src, dst, false)) {
            char *n = wide_to_utf8(name);
            log_msg("Migrado desde la versión anterior: %s", n);
            free(n);
        }
    }
    free(src);
    free(dst);
}

/* La versión en Python guardaba .env, commands.json, dispositivos.json y
   los sonidos junto al .exe (o al script). La primera vez que corre esta
   versión, los copia a %LOCALAPPDATA%\Sokari para que nada quede regado —
   sin borrar los originales. */
void config_migrate_legacy(void)
{
    bool fresh = !file_exists(g_paths.config_file);
    wchar_t *dir = exe_dir();
    wchar_t *parent = path_dirname(dir);
    const wchar_t *candidates[] = {dir, parent};

    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < 2; i++) {
        const wchar_t *d = candidates[i];
        if (fresh) {
            wchar_t *env = path_join(d, L".env");
            if (load_env_file(env, &g_cfg)) {
                log_msg("Configuración migrada desde el .env de la versión anterior.");
                fresh = false;
                save_locked();
            }
            free(env);
        }
        copy_if_missing(d, L"commands.json", g_paths.memory_dir);
        copy_if_missing(d, L"dispositivos.json", g_paths.local_dir);
        wchar_t *snd = path_join(d, L"sounds");
        const wchar_t *sounds[] = {L"activacion.mp3", L"activacion.wav", L"busqueda.mp3", L"busqueda.wav"};
        for (int s = 0; s < 4; s++) copy_if_missing(snd, sounds[s], g_paths.sounds_dir);
        free(snd);
    }
    ReleaseSRWLockExclusive(&g_lock);
    free(dir);
    free(parent);
}

/* La ventana de Sokari en Linux (GTK 3): la esfera que late cuando habla, lo
   que dijiste y lo que contestó, y los botones Hablar y Configuración; el
   ícono en la barra de arriba (si tu escritorio lo muestra), los avisos de
   GNOME, arrancar con tu sesión y Ctrl+Alt+J para hablarle. Una sola Sokari
   a la vez: abrirla otra vez muestra la que ya está, y «sokari --hablar» le
   dice a esa que te escuche. La voz, la malla y el agente son los mismos que
   en modo voz; aquí solo se ven. */
#include <windows.h>

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "audio.h"
#include "autostart.h"
#include "config.h"
#include "linux/gnome.h"
#include "linux/linux.h"
#include "log.h"
#include "mesh.h"
#include "resource.h"
#include "resources.h"
#include "skills.h"
#include "sphere.h"
#include "update.h"
#include "util.h"
#include "voice.h"

#define APP_ID "io.github.podselxd.Sokari"
#define SPHERE_MAX 560 /* más grande se ve igual y cuesta más dibujarla */

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *area, *status, *sub_user, *sub_sokari, *settings;
    AppIndicator *tray;
    GtkWidget *tray_mute;
    SphereRenderer *sr;
    cairo_surface_t *surf;
    int base, style, want_style;
    gint64 last_us, start_us;
    SphereParams cur;
    double angle, voice_t, env, pulse_env, pulse_peak;
    float voice, pulse; /* los del cuadro que sigue */
    char *status_text; /* lo que dijo app_status; "" = el de cada estado */
    bool voice_started, talk_pending, hide_notice_shown, fullscreen;
} Ui;

static Ui U;
static volatile gint g_active, g_state, g_level_milli, g_own_active;

/* ------------------------------------------------- lo que manda la voz --- */

bool ui_active(void)
{
    return g_atomic_int_get(&g_active) != 0;
}

bool ui_own_window_active(void)
{
    return g_atomic_int_get(&g_own_active) != 0;
}

void ui_post_level(float level)
{
    g_atomic_int_set(&g_level_milli, (gint)(level * 1000.0f));
}

static const char *default_status(int st)
{
    switch (st) {
    case JV_LISTENING: return "Te escucho…";
    case JV_THINKING: return "Pensando…";
    case JV_SPEAKING: return "Hablando (un clic en la esfera me calla)";
    default: return res_has_wake_word() ? "Di «Hey Sokari» o Ctrl+Alt+J" : "Háblame con Ctrl+Alt+J";
    }
}

static void refresh_status(void)
{
    if (!U.status) return;
    const char *t = U.status_text && *U.status_text ? U.status_text : default_status(g_atomic_int_get(&g_state));
    gtk_label_set_text(GTK_LABEL(U.status), t);
}

static gboolean on_state_idle(gpointer u)
{
    refresh_status();
    return G_SOURCE_REMOVE;
}

void ui_post_state(int st)
{
    g_atomic_int_set(&g_state, st);
    g_idle_add(on_state_idle, NULL);
}

typedef struct {
    int kind; /* 0 subtítulo tuyo, 1 de Sokari, 2 estado, 3 aviso */
    char *a, *b;
} Post;

static gboolean on_post_idle(gpointer u)
{
    Post *p = u;
    if (p->kind == 0 && U.sub_user) {
        gtk_label_set_text(GTK_LABEL(U.sub_user), p->a);
        gtk_label_set_text(GTK_LABEL(U.sub_sokari), "");
    } else if (p->kind == 1 && U.sub_sokari) {
        gtk_label_set_text(GTK_LABEL(U.sub_sokari), p->a);
    } else if (p->kind == 2) {
        free(U.status_text);
        U.status_text = xstrdup(p->a);
        refresh_status();
    } else if (p->kind == 3 && U.app) {
        GNotification *n = g_notification_new(p->a);
        g_notification_set_body(n, p->b);
        g_application_send_notification(G_APPLICATION(U.app), NULL, n);
        g_object_unref(n);
    }
    free(p->a);
    free(p->b);
    free(p);
    return G_SOURCE_REMOVE;
}

static void post(int kind, const char *a, const char *b)
{
    Post *p = xcalloc(1, sizeof *p);
    p->kind = kind;
    p->a = xstrdup(a ? a : "");
    p->b = xstrdup(b ? b : "");
    g_idle_add(on_post_idle, p);
}

void ui_post_subtitle(bool from_user, const char *text)
{
    post(from_user ? 0 : 1, text, NULL);
}

void ui_post_status(const char *text)
{
    post(2, text, NULL);
}

void ui_post_notify(const char *title, const char *text)
{
    post(3, title ? title : "Sokari", text);
}

static gboolean on_quit_idle(gpointer u)
{
    g_action_group_activate_action(G_ACTION_GROUP(U.app), "salir", NULL);
    return G_SOURCE_REMOVE;
}

void ui_post_quit(void)
{
    g_idle_add(on_quit_idle, NULL);
}

/* ------------------------------------------------------------- esfera --- */

static void target_params(int st, SphereParams *out)
{
    switch (st) {
    case JV_SPEAKING: *out = SPHERE_SPEAK; break;
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
    default: *out = SPHERE_IDLE;
    }
}

/* El mismo movimiento que en Windows: se acerca a lo que pide el estado, se
   agranda con la voz y late con cada sílaba. */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer u)
{
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    if (!U.start_us) U.start_us = U.last_us = now;
    double dt = (double)(now - U.last_us) / 1e6;
    if (dt > 0.1) dt = 0.1;
    if (dt < 1.0 / 40) return G_SOURCE_CONTINUE; /* ~30 cuadros por segundo alcanzan */
    U.last_us = now;
    int st = g_atomic_int_get(&g_state);
    SphereParams target;
    target_params(st, &target);
    sphere_lerp(&U.cur, &U.cur, &target, (float)(1.0 - exp(-dt / 0.25)));
    double level = g_atomic_int_get(&g_level_milli) / 1000.0;
    double k = level > U.env ? 1.0 - exp(-dt / 0.03) : 1.0 - exp(-dt / 0.18);
    U.env += (level - U.env) * k;
    double kp = level > U.pulse_env ? 1.0 - exp(-dt / 0.025) : 1.0 - exp(-dt / 0.09);
    U.pulse_env += (level - U.pulse_env) * kp;
    U.pulse_peak = fmax(U.pulse_env, U.pulse_peak * exp(-dt / 1.5));
    U.voice = st == JV_SPEAKING    ? (float)fmin(1.0, U.env * 1.6)
              : st == JV_LISTENING ? (float)fmin(1.0, U.env * 0.8)
                                   : 0.0f;
    U.pulse = st == JV_SPEAKING ? (float)fmin(1.0, U.pulse_env / fmax(0.3, U.pulse_peak)) : 0.0f;
    U.angle += U.cur.rotation_speed * dt * (1.0 + U.voice * 0.8);
    U.voice_t += dt * (1.0 + 2.5 * U.voice);
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer u)
{
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    int side = W < H ? W : H;
    if (side < 16) return FALSE;
    int base = side > SPHERE_MAX ? SPHERE_MAX : side;
    int style = U.want_style;
    if (!U.sr || base != U.base || style != U.style) {
        sphere_destroy(U.sr);
        if (U.surf) cairo_surface_destroy(U.surf);
        /* Las líneas se deforman más: más lienzo para que no se corten. */
        int canvas = (int)(base * sphere_room((SphereStyle)style));
        U.sr = sphere_create_fit(canvas, (float)base / (float)canvas);
        canvas = sphere_size(U.sr);
        U.surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas, canvas);
        U.base = base;
        U.style = style;
    }
    double t = (double)(U.last_us - U.start_us) / 1e6;
    cairo_surface_flush(U.surf);
    int canvas = cairo_image_surface_get_width(U.surf);
    sphere_render(U.sr, t, U.angle, U.voice_t, &U.cur, U.voice, U.pulse, (SphereStyle)style,
                  (uint32_t *)cairo_image_surface_get_data(U.surf), cairo_image_surface_get_stride(U.surf) / 4, false);
    cairo_surface_mark_dirty(U.surf);

    cairo_set_source_rgb(cr, 0.02, 0.016, 0.047);
    cairo_paint(cr);
    /* Del tamaño de siempre; lo que sobra del lienzo es para las ondas. */
    double dst = (double)side * canvas / base;
    double scale = dst / canvas;
    cairo_save(cr);
    cairo_translate(cr, (W - dst) / 2, (H - dst) / 2);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, U.surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

/* Un clic en la esfera: si habla o piensa, la calla; si no, te escucha. */
static gboolean on_sphere_click(GtkWidget *w, GdkEventButton *e, gpointer u)
{
    if (e->type != GDK_BUTTON_PRESS || e->button != 1) return FALSE;
    int st = g_atomic_int_get(&g_state);
    if (st == JV_SPEAKING || st == JV_THINKING) voice_skip();
    else if (U.voice_started) voice_trigger();
    return TRUE;
}

/* -------------------------------------------------------- arrancar voz --- */

static bool subtitles_on(void)
{
    AppConfig cfg = config_snapshot();
    bool on = cfg.subtitles;
    U.want_style = cfg.sphere_style;
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    return on;
}

static bool has_key(void)
{
    AppConfig cfg = config_snapshot();
    bool has = *cfg.groq_api_key != 0;
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    return has;
}

static void start_voice(bool first_run)
{
    if (U.voice_started) return;
    U.voice_started = voice_start();
    if (!U.voice_started) return;
    update_start_background();
    if (first_run)
        app_notify("Sokari", res_has_wake_word() ? "Listo. Di «Hey Sokari» (o Ctrl+Alt+J) cuando quieras hablarle."
                                                 : "Listo. Háblame con Ctrl+Alt+J.");
    if (U.talk_pending) {
        U.talk_pending = false;
        voice_trigger();
    }
}

static void request_talk(void)
{
    if (U.voice_started) voice_trigger();
    else U.talk_pending = true;
}

/* ------------------------------------------------------ Configuración --- */

typedef struct {
    GtkWidget *key, *name, *mic, *out, *volume, *subtitles, *duck, *full, *autostart, *style, *city;
    GtkWidget *skills[16]; /* las skills locales, en el orden de skills_get */
    bool first_run;
} SettingsForm;

static GtkWidget *add_row(GtkGrid *g, int row, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(g, l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(g, w, 1, row, 1, 1);
    return w;
}

static GtkWidget *device_combo(bool mics, const char *current)
{
    GtkWidget *c = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), "", mics ? "El predeterminado" : "La predeterminada");
    char **names = NULL;
    int n = mics ? mic_list_devices(&names) : speaker_list_devices(&names);
    for (int i = 0; i < n; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), names[i], names[i]);
    free_string_list(names, n);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), current && *current ? current : ""))
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    return c;
}

static GtkWidget *check(const char *label, bool on)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    return c;
}

static void on_settings_response(GtkDialog *d, int response, gpointer u)
{
    SettingsForm *f = u;
    if (response == GTK_RESPONSE_ACCEPT) {
        const char *key = gtk_entry_get_text(GTK_ENTRY(f->key));
        char *k = str_trim(key);
        if (!*k) {
            free(k);
            GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(d), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                                  "Me falta tu API key de Groq.");
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(m), "Sácala gratis en console.groq.com/keys y pégala aquí (empieza con gsk_).");
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            return;
        }
        AppConfig cfg = config_snapshot();
        SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
        free(cfg.groq_api_key);
        cfg.groq_api_key = k;
        free(cfg.user_name);
        cfg.user_name = str_trim(gtk_entry_get_text(GTK_ENTRY(f->name)));
        free(cfg.mic_name);
        const char *mic = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->mic));
        cfg.mic_name = xstrdup(mic ? mic : "");
        free(cfg.output_name);
        const char *out = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->out));
        cfg.output_name = xstrdup(out ? out : "");
        cfg.volume = (int)gtk_range_get_value(GTK_RANGE(f->volume));
        cfg.subtitles = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->subtitles));
        cfg.duck = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->duck));
        cfg.full_access = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->full));
        cfg.autostart = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->autostart));
        cfg.sphere_style = gtk_combo_box_get_active(GTK_COMBO_BOX(f->style)) == 1 ? 1 : 0;
        U.want_style = cfg.sphere_style;
        free(cfg.city);
        cfg.city = str_trim(gtk_entry_get_text(GTK_ENTRY(f->city)));
        StrBuf off;
        sb_init(&off);
        for (int i = 0; i < skills_count() && i < 16; i++)
            if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->skills[i])))
                sb_appendf(&off, "%s%s", off.len ? "," : "", skills_get(i)->id);
        free(cfg.skills_off);
        cfg.skills_off = off.data ? sb_steal(&off) : xstrdup("");
        sb_free(&off);
        config_apply(&cfg);
        bool saved = config_save();
        autostart_set(cfg.autostart);
        speaker_set_device(cfg.output_name);
        SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
        config_free(&cfg);
        if (!saved) log_msg("Configuración: no pude guardarla.");
        gtk_widget_set_visible(U.sub_user, subtitles_on());
        gtk_widget_set_visible(U.sub_sokari, subtitles_on());
        if (U.voice_started) voice_settings_changed();
        else start_voice(f->first_run);
    } else if (f->first_run && !has_key()) {
        app_notify("Sokari", "Sin tu API key de Groq todavía no puedo contestarte. Abre Configuración cuando la tengas.");
    }
    gtk_widget_destroy(GTK_WIDGET(d));
    U.settings = NULL;
    free(f);
}

static void settings_open(bool first_run)
{
    if (U.settings) {
        gtk_window_present(GTK_WINDOW(U.settings));
        return;
    }
    SettingsForm *f = xcalloc(1, sizeof *f);
    f->first_run = first_run;
    GtkWidget *d = gtk_dialog_new_with_buttons("Configuración de Sokari", GTK_WINDOW(U.win),
                                               GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR, "Cancelar",
                                               GTK_RESPONSE_CANCEL, "Guardar", GTK_RESPONSE_ACCEPT, NULL);
    U.settings = d;
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    if (first_run) {
        GtkWidget *hi = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hi), "<b>Para empezar, pega tu API key de Groq.</b> Es gratis: sácala en "
                                            "<a href=\"https://console.groq.com/keys\">console.groq.com/keys</a>.");
        gtk_label_set_line_wrap(GTK_LABEL(hi), TRUE);
        gtk_label_set_xalign(GTK_LABEL(hi), 0);
        gtk_box_pack_start(GTK_BOX(box), hi, FALSE, FALSE, 0);
    }
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_box_pack_start(GTK_BOX(box), grid, TRUE, TRUE, 0);
    AppConfig cfg = config_snapshot();
    int r = 0;
    f->key = add_row(GTK_GRID(grid), r++, "API key de Groq", gtk_entry_new());
    gtk_entry_set_visibility(GTK_ENTRY(f->key), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->key), "gsk_…");
    gtk_entry_set_text(GTK_ENTRY(f->key), cfg.groq_api_key);
    gtk_entry_set_activates_default(GTK_ENTRY(f->key), TRUE);
    f->name = add_row(GTK_GRID(grid), r++, "Cómo te llamas", gtk_entry_new());
    gtk_entry_set_text(GTK_ENTRY(f->name), cfg.user_name);
    f->mic = add_row(GTK_GRID(grid), r++, "Micrófono", device_combo(true, cfg.mic_name));
    f->out = add_row(GTK_GRID(grid), r++, "Por dónde hablo", device_combo(false, cfg.output_name));
    f->volume = add_row(GTK_GRID(grid), r++, "Volumen de mi voz",
                        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5));
    gtk_range_set_value(GTK_RANGE(f->volume), cfg.volume);
    f->style = add_row(GTK_GRID(grid), r++, "La esfera", gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(f->style), "Halo de puntos");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(f->style), "Líneas");
    gtk_combo_box_set_active(GTK_COMBO_BOX(f->style), cfg.sphere_style == 1 ? 1 : 0);
    f->subtitles = check("Mostrar lo que dices y lo que contesto", cfg.subtitles);
    gtk_grid_attach(GTK_GRID(grid), f->subtitles, 0, r++, 2, 1);
    f->duck = check("Bajar el volumen de la PC mientras te escucho", cfg.duck);
    gtk_grid_attach(GTK_GRID(grid), f->duck, 0, r++, 2, 1);
    f->full = check("Acceso completo: no te pregunto nada, salvo antes de borrar", cfg.full_access);
    gtk_grid_attach(GTK_GRID(grid), f->full, 0, r++, 2, 1);
    f->autostart = check("Abrir Sokari al iniciar tu sesión", autostart_is_enabled());
    gtk_grid_attach(GTK_GRID(grid), f->autostart, 0, r++, 2, 1);

    /* Skills locales: contestan sin IA (0 tokens). Dos columnas. */
    GtkWidget *sk = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sk), "<b>Skills</b>: contestan en tu PC, sin gastar nada de IA (0 tokens). "
                                        "Lo que no entienden se lo pasan a la IA.");
    gtk_label_set_line_wrap(GTK_LABEL(sk), TRUE);
    gtk_label_set_xalign(GTK_LABEL(sk), 0);
    gtk_widget_set_margin_top(sk, 8);
    gtk_grid_attach(GTK_GRID(grid), sk, 0, r++, 2, 1);
    int n = skills_count() < 16 ? skills_count() : 16, half = (n + 1) / 2;
    for (int i = 0; i < n; i++) {
        const SkillInfo *info = skills_get(i);
        f->skills[i] = check(info->name, config_skill_enabled(info->id));
        gtk_widget_set_tooltip_text(f->skills[i], info->example);
        gtk_grid_attach(GTK_GRID(grid), f->skills[i], i / half, r + i % half, 1, 1);
    }
    r += half;
    f->city = add_row(GTK_GRID(grid), r++, "Tu ciudad (para el clima)", gtk_entry_new());
    gtk_entry_set_text(GTK_ENTRY(f->city), cfg.city);
    gtk_entry_set_placeholder_text(GTK_ENTRY(f->city), "Chihuahua");
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    g_signal_connect(d, "response", G_CALLBACK(on_settings_response), f);
    gtk_widget_show_all(d);
}

/* ------------------------------------------------------------ Tus PCs --- */

typedef struct {
    GtkWidget *dialog, *status, *list, *result, *buttons;
    int kind; /* 0 detectar, 1 revisar, 2 firewall */
    char *text;
} MeshJob;

static void mesh_fill(MeshJob *j)
{
    char *ip = mesh_listening_ip();
    char *s = ip ? str_printf("✓ Esta PC recibe órdenes de tus otras PCs (en %s).", ip)
                 : xstrdup("Esta PC todavía no recibe órdenes: se activa sola cuando Tailscale se conecta.");
    gtk_label_set_text(GTK_LABEL(j->status), s);
    free(s);
    free(ip);
    MeshDevice *d;
    int n = mesh_devices(&d);
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < n; i++) sb_appendf(&sb, "%s• %s (%s)", i ? "\n" : "", d[i].name, d[i].host);
    if (!n) sb_append(&sb, "Todavía no tienes PCs registradas: dale a «Detectar mis PCs».");
    gtk_label_set_text(GTK_LABEL(j->list), sb.data);
    sb_free(&sb);
    mesh_devices_free(d, n);
}

static void mesh_work(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    MeshJob *j = data;
    if (j->kind == 1) {
        j->text = mesh_diagnose();
    } else if (j->kind == 2) {
        j->text = xstrdup(mesh_allow_firewall() ? "Listo: la malla puede recibir órdenes de tu red de Tailscale."
                                                : "No pude cambiar el firewall (¿cancelaste la contraseña?).");
    } else {
        MeshDevice *list;
        char *why = NULL;
        int n = tailscale_windows_peers(&list, &why);
        StrBuf sb;
        sb_init(&sb);
        for (int i = 0; i < n; i++)
            sb_appendf(&sb, "%s %s (%s)\n", mesh_device_set(list[i].name, list[i].host) ? "Registré" : "No pude registrar",
                       list[i].name, list[i].host);
        if (!n) sb_append(&sb, why ? why : "No encontré otras PCs en tu red de Tailscale.");
        j->text = sb_steal(&sb);
        free(why);
        mesh_devices_free(list, n);
    }
    g_task_return_boolean(t, TRUE);
}

static void mesh_done(GObject *src, GAsyncResult *res, gpointer data)
{
    MeshJob *j = data;
    gtk_label_set_text(GTK_LABEL(j->result), j->text ? j->text : "");
    gtk_widget_set_sensitive(j->buttons, TRUE);
    free(j->text);
    j->text = NULL;
    mesh_fill(j);
}

static void mesh_run(GtkButton *b, gpointer data)
{
    MeshJob *j = data;
    j->kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "kind"));
    gtk_widget_set_sensitive(j->buttons, FALSE);
    gtk_label_set_text(GTK_LABEL(j->result), j->kind == 1 ? "Revisando (tarda unos segundos)…" : "Un momento…");
    GTask *t = g_task_new(NULL, NULL, mesh_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, mesh_work);
    g_object_unref(t);
}

static void on_mesh_response(GtkDialog *d, int response, gpointer data)
{
    MeshJob *j = data;
    /* Si algo sigue corriendo, mesh_done todavía lo va a usar: se queda. */
    if (!gtk_widget_get_sensitive(j->buttons)) return;
    gtk_widget_destroy(GTK_WIDGET(d));
    free(j);
}

static GtkWidget *mesh_button(GtkWidget *box, const char *label, int kind, MeshJob *j)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(b), "kind", GINT_TO_POINTER(kind));
    g_signal_connect(b, "clicked", G_CALLBACK(mesh_run), j);
    gtk_box_pack_start(GTK_BOX(box), b, TRUE, TRUE, 0);
    return b;
}

static void mesh_open(void)
{
    MeshJob *j = xcalloc(1, sizeof *j);
    j->dialog = gtk_dialog_new_with_buttons("Tus PCs", GTK_WINDOW(U.win), GTK_DIALOG_DESTROY_WITH_PARENT, "Cerrar",
                                            GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(j->dialog), 560, 420);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(j->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    GtkWidget *intro = gtk_label_new("Con Tailscale (gratis) le hablas a Sokari en una PC y lo hace en otra: «en la "
                                     "laptop abre Spotify».");
    gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    j->status = gtk_label_new("");
    j->list = gtk_label_new("");
    j->result = gtk_label_new("");
    GtkWidget *labels[] = {intro, j->status, j->list};
    for (int i = 0; i < 3; i++) {
        gtk_label_set_line_wrap(GTK_LABEL(labels[i]), TRUE);
        gtk_label_set_xalign(GTK_LABEL(labels[i]), 0);
        gtk_box_pack_start(GTK_BOX(box), labels[i], FALSE, FALSE, 0);
    }
    j->buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    mesh_button(j->buttons, "Detectar mis PCs", 0, j);
    mesh_button(j->buttons, "Revisar la malla", 1, j);
    mesh_button(j->buttons, "Permitir en el firewall", 2, j);
    gtk_box_pack_start(GTK_BOX(box), j->buttons, FALSE, FALSE, 0);
    gtk_label_set_selectable(GTK_LABEL(j->result), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(j->result), TRUE);
    gtk_label_set_xalign(GTK_LABEL(j->result), 0);
    gtk_label_set_yalign(GTK_LABEL(j->result), 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), j->result);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    mesh_fill(j);
    g_signal_connect(j->dialog, "response", G_CALLBACK(on_mesh_response), j);
    gtk_widget_show_all(j->dialog);
}

/* ------------------------------------------------------------- acciones --- */

static void show_window(void)
{
    if (!U.win) return;
    gtk_widget_show(U.win);
    gtk_window_present(GTK_WINDOW(U.win));
}

static void act_show(GSimpleAction *a, GVariant *p, gpointer u)
{
    show_window();
}

static void act_talk(GSimpleAction *a, GVariant *p, gpointer u)
{
    request_talk();
}

static void act_settings(GSimpleAction *a, GVariant *p, gpointer u)
{
    show_window();
    settings_open(false);
}

static void act_mesh(GSimpleAction *a, GVariant *p, gpointer u)
{
    show_window();
    mesh_open();
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer u)
{
    log_msg("Sokari: salir desde la ventana.");
    if (U.voice_started) voice_stop();
    voice_mesh_stop();
    U.voice_started = false;
    g_atomic_int_set(&g_active, 0);
    g_application_quit(G_APPLICATION(U.app));
}

static void update_work(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    g_task_return_pointer(t, update_check_now(), free);
}

static void update_done(GObject *src, GAsyncResult *res, gpointer data)
{
    char *msg = g_task_propagate_pointer(G_TASK(res), NULL);
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(U.win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK, "%s", msg ? msg : "No pude revisar.");
    g_signal_connect(m, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(m);
    free(msg);
}

static void act_update(GSimpleAction *a, GVariant *p, gpointer u)
{
    show_window();
    app_notify("Sokari", "Busco si hay versión nueva…");
    GTask *t = g_task_new(NULL, NULL, update_done, NULL);
    g_task_run_in_thread(t, update_work);
    g_object_unref(t);
}

static void act_mute(GSimpleAction *a, GVariant *p, gpointer u)
{
    bool muted = !config_mic_muted();
    config_set_mic_muted(muted);
    config_save();
    GVariant *v = g_variant_new_boolean(muted);
    g_simple_action_set_state(a, v);
    app_notify("Sokari", muted ? "Micrófono en silencio: no te escucho hasta que lo actives." : "Ya te escucho otra vez.");
}

static const GActionEntry ACTIONS[] = {
    {"mostrar", act_show, NULL, NULL, NULL, {0}},    {"hablar", act_talk, NULL, NULL, NULL, {0}},
    {"configuracion", act_settings, NULL, NULL, NULL, {0}}, {"malla", act_mesh, NULL, NULL, NULL, {0}},
    {"salir", act_quit, NULL, NULL, NULL, {0}},      {"silencio", act_mute, NULL, "false", NULL, {0}},
    {"actualizar", act_update, NULL, NULL, NULL, {0}},
};

/* ------------------------------------------------------ el ícono de arriba --- */

static void tray_item(GtkWidget *menu, const char *label, const char *action)
{
    GtkWidget *it = gtk_menu_item_new_with_label(label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(it), action);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

/* El PNG del ícono en ~/.cache/sokari/iconos (el indicador lo pide por nombre y carpeta). */
static char *icon_dir(void)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "sokari", "iconos", NULL);
    char *file = g_build_filename(dir, "sokari.png", NULL);
    size_t n = 0;
    const void *png = res_data(IDR_ICON_PNG, &n);
    if (png && !g_file_test(file, G_FILE_TEST_EXISTS)) {
        g_mkdir_with_parents(dir, 0700);
        g_file_set_contents(file, png, (gssize)n, NULL);
    }
    g_free(file);
    char *r = xstrdup(dir);
    g_free(dir);
    return r;
}

/* ¿Hay dónde mostrar el ícono? En Ubuntu sí (su extensión de indicadores);
   en Fedora, solo con la extensión «AppIndicator and KStatusNotifierItem
   Support». Sin eso, sin ícono: todo sigue en el menú de la ventana. */
static bool tray_host(void)
{
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!c) return false;
    GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "NameHasOwner",
                                              g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
                                              G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    gboolean has = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &has);
        g_variant_unref(r);
    }
    g_object_unref(c);
    return has;
}

static void tray_init(void)
{
    if (!tray_host()) {
        log_msg("Sin ícono en la barra de arriba: este escritorio no tiene dónde mostrarlo (en Fedora, con la "
                "extensión «AppIndicator and KStatusNotifierItem Support»). El menú está en la ventana.");
        return;
    }
    char *dir = icon_dir();
    /* En Fedora 44 la librería marca esta forma como vieja (quiere su versión
       sin GTK), pero es la que hay en Ubuntu 24.04 y funciona en las dos. */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    U.tray = app_indicator_new(APP_ID, "sokari", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_icon_theme_path(U.tray, dir);
    app_indicator_set_title(U.tray, "Sokari");
    G_GNUC_END_IGNORE_DEPRECATIONS
    free(dir);
    GtkWidget *menu = gtk_menu_new();
    tray_item(menu, "Mostrar Sokari", "app.mostrar");
    tray_item(menu, "Hablarle ahora", "app.hablar");
    U.tray_mute = gtk_check_menu_item_new_with_label("Micrófono en silencio");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(U.tray_mute), "app.silencio");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), U.tray_mute);
    tray_item(menu, "Configuración", "app.configuracion");
    tray_item(menu, "Tus PCs", "app.malla");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    tray_item(menu, "Salir", "app.salir");
    gtk_widget_insert_action_group(menu, "app", G_ACTION_GROUP(U.app));
    gtk_widget_show_all(menu);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    app_indicator_set_menu(U.tray, GTK_MENU(menu));
    app_indicator_set_status(U.tray, APP_INDICATOR_STATUS_ACTIVE);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

/* ------------------------------------------------------------ la ventana --- */

static const char CSS[] =
    "window.sokari, window.sokari .fondo { background-color: #05040c; }"
    ".sokari .tu { color: #8f93b3; font-size: 13pt; }"
    ".sokari .ella { color: #ece8ff; font-size: 17pt; }"
    ".sokari .estado { color: #9d95ff; font-size: 11pt; }";

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer u)
{
    /* Cerrar la ventana no cierra a Sokari: sigue escuchando. */
    gtk_widget_hide(w);
    if (!U.hide_notice_shown) {
        U.hide_notice_shown = true;
        app_notify("Sokari", "Sigo escuchando aunque cerraste la ventana. Para verme otra vez, ábreme de nuevo; para "
                             "cerrarme del todo, «Salir» en el menú.");
    }
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer u)
{
    if (e->keyval == GDK_KEY_F11 || (e->keyval == GDK_KEY_Escape && U.fullscreen)) {
        U.fullscreen = !U.fullscreen && e->keyval == GDK_KEY_F11;
        if (U.fullscreen) gtk_window_fullscreen(GTK_WINDOW(w));
        else gtk_window_unfullscreen(GTK_WINDOW(w));
        return TRUE;
    }
    return FALSE;
}

static void on_active_changed(GObject *w, GParamSpec *p, gpointer u)
{
    g_atomic_int_set(&g_own_active, gtk_window_is_active(GTK_WINDOW(w)) ? 1 : 0);
}

static GtkWidget *label(const char *cls, bool wrap)
{
    GtkWidget *l = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    return l;
}

static void build_window(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);

    U.win = gtk_application_window_new(U.app);
    gtk_window_set_title(GTK_WINDOW(U.win), "Sokari");
    gtk_window_set_default_size(GTK_WINDOW(U.win), 520, 640);
    gtk_style_context_add_class(gtk_widget_get_style_context(U.win), "sokari");
    size_t n = 0;
    const void *png = res_data(IDR_ICON_PNG, &n);
    if (png) {
        GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
        if (gdk_pixbuf_loader_write(ld, png, n, NULL) && gdk_pixbuf_loader_close(ld, NULL))
            gtk_window_set_default_icon(gdk_pixbuf_loader_get_pixbuf(ld));
        g_object_unref(ld);
    }

    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(bar), "Sokari");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Hablarle ahora", "app.hablar");
    g_menu_append(menu, "Micrófono en silencio", "app.silencio");
    g_menu_append(menu, "Configuración", "app.configuracion");
    g_menu_append(menu, "Tus PCs", "app.malla");
    g_menu_append(menu, "Buscar actualización", "app.actualizar");
    g_menu_append(menu, "Salir", "app.salir");
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    g_object_unref(menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), mb);
    GtkWidget *talk = gtk_button_new_with_label("Hablar");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(talk), "app.hablar");
    gtk_style_context_add_class(gtk_widget_get_style_context(talk), "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), talk);
    gtk_window_set_titlebar(GTK_WINDOW(U.win), bar);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "fondo");
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    U.area = gtk_drawing_area_new();
    gtk_widget_set_size_request(U.area, 220, 220);
    gtk_widget_set_vexpand(U.area, TRUE);
    gtk_widget_add_events(U.area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(U.area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(U.area, "button-press-event", G_CALLBACK(on_sphere_click), NULL);
    gtk_widget_add_tick_callback(U.area, on_tick, NULL, NULL);
    gtk_box_pack_start(GTK_BOX(box), U.area, TRUE, TRUE, 0);
    U.sub_sokari = label("ella", true);
    U.sub_user = label("tu", true);
    U.status = label("estado", false);
    gtk_box_pack_start(GTK_BOX(box), U.sub_sokari, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), U.sub_user, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), U.status, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(U.win), box);
    g_signal_connect(U.win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(U.win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(U.win, "notify::is-active", G_CALLBACK(on_active_changed), NULL);
    U.cur = SPHERE_IDLE;
    refresh_status();
    gtk_widget_show_all(box);
    bool subs = subtitles_on();
    gtk_widget_set_visible(U.sub_user, subs);
    gtk_widget_set_visible(U.sub_sokari, subs);
}

/* Lo primero que hace la Sokari que se queda corriendo. */
static void startup(bool from_autostart)
{
    g_atomic_int_set(&g_active, 1);
    g_application_hold(G_APPLICATION(U.app));
    g_action_map_add_action_entries(G_ACTION_MAP(U.app), ACTIONS, G_N_ELEMENTS(ACTIONS), NULL);
    GAction *mute = g_action_map_lookup_action(G_ACTION_MAP(U.app), "silencio");
    g_simple_action_set_state(G_SIMPLE_ACTION(mute), g_variant_new_boolean(config_mic_muted()));
    const char *accels_talk[] = {"<Primary>h", NULL};
    gtk_application_set_accels_for_action(U.app, "app.hablar", accels_talk);
    build_window();
    tray_init();
    autostart_refresh();
    /* La malla escucha desde que abres Sokari, aunque la voz no arranque. */
    voice_mesh_start();
    gnome_extension_enable();
    linux_hotkey_ensure();
    AppConfig cfg = config_snapshot();
    speaker_set_device(cfg.output_name);
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    LaunchKind kind = launch_kind(has_key(), from_autostart, false);
    if (kind != LAUNCH_DIRECT || !from_autostart) show_window();
    if (kind == LAUNCH_FIRST_RUN) settings_open(true);
    else start_voice(false);
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer u)
{
    int argc = 0;
    gchar **argv = g_application_command_line_get_arguments(cl, &argc);
    bool talk = false, autostart = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hablar")) talk = true;
        else if (!strcmp(argv[i], "--autostart")) autostart = true;
    }
    g_strfreev(argv);
    if (!U.win) startup(autostart);
    else if (!talk && !autostart) show_window();
    if (talk) request_talk();
    return 0;
}

int ui_run(int argc, char **argv)
{
    /* GTK no cambia el formato de los números (JSON y demás leen con punto):
       solo los textos y los mensajes van en tu idioma. */
    gtk_disable_setlocale();
    setlocale(LC_CTYPE, "");
    setlocale(LC_MESSAGES, "");
    U.app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(U.app, "command-line", G_CALLBACK(on_command_line), NULL);
    int rc = g_application_run(G_APPLICATION(U.app), argc, argv);
    g_object_unref(U.app);
    U.app = NULL;
    return rc;
}

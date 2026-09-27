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
#include "linux/settings_linux.h"
#include "log.h"
#include "mesh.h"
#include "resource.h"
#include "resources.h"
#include "skills.h"
#include "face.h"
#include "sphere.h"
#include "update.h"
#include "util.h"
#include "voice.h"

#define APP_ID "io.github.podselxd.Sokari"
#define SPHERE_MAX 560 /* más grande se ve igual y cuesta más dibujarla */

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *area, *status, *sub_user, *sub_sokari;
    AppIndicator *tray;
    GMenu *menu; /* el mismo menú arriba (bandeja) y en ☰ */
    SphereRenderer *sr;
    cairo_surface_t *surf;
    int base, style, want_style;
    gint64 last_us, start_us;
    SphereParams cur;
    double angle, voice_t, env, pulse_env, pulse_peak;
    float voice, pulse; /* los del cuadro que sigue */
    char *status_text; /* lo que dijo app_status; "" = el de cada estado */
    bool voice_started, talk_pending, hide_notice_shown, fullscreen;
    /* Un clic en la esfera la calla o le habla; arrastrarla mueve la ventana. */
    bool pressed;
    double press_x, press_y;
    /* El modo de pantalla que tiene la ventana (DisplayMode) y el que pide
       Configuración; «Aparecer solo cuando le hablas». */
    int mode, want_mode;
    bool only_talking, old_extension_told;
    int last_state;
    guint hide_timer, where_timer, where_tries;
    int where_x, where_y;
    GtkWidget *popup; /* el menú del clic derecho */
    /* Entrar y salir de la pantalla: la ventana se esconde cuando la esfera
       terminó de irse (on_tick). fresh: se acaba de mostrar, el reloj
       arranca de nuevo. */
    SphereAppear ap;
    int anim;
    bool fresh;
    /* La cara (beta): su pose y los colores del cuadro que sigue. */
    Face *face;
    SphereFace pose;
    SphereParams draw;
    bool face_on, face_symbols;
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

static void follow_state(int st);

static gboolean on_state_idle(gpointer u)
{
    refresh_status();
    follow_state(g_atomic_int_get(&g_state));
    return G_SOURCE_REMOVE;
}

void ui_post_state(int st)
{
    g_atomic_int_set(&g_state, st);
    g_idle_add(on_state_idle, NULL);
}

typedef struct {
    int kind; /* 0 subtítulo tuyo, 1 de Sokari, 2 estado, 3 aviso, 4 aviso con botón */
    char *a, *b;
    char *id, *button, *action; /* el aviso con botón */
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
    } else if ((p->kind == 3 || p->kind == 4) && U.app) {
        GNotification *n = g_notification_new(p->a);
        g_notification_set_body(n, p->b);
        if (p->kind == 4) {
            g_notification_add_button(n, p->button, p->action);
            g_notification_set_default_action(n, p->action);
        }
        /* Con nombre, uno nuevo reemplaza al anterior en vez de juntarse
           (el número sobre el ícono del dock no crece). */
        g_application_send_notification(G_APPLICATION(U.app), p->kind == 4 ? p->id : "aviso", n);
        g_object_unref(n);
    }
    free(p->a);
    free(p->b);
    free(p->id);
    free(p->button);
    free(p->action);
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

void ui_post_notify_button(const char *id, const char *title, const char *text, const char *button,
                           const char *action)
{
    Post *p = xcalloc(1, sizeof *p);
    p->kind = 4;
    p->a = xstrdup(title ? title : "Sokari");
    p->b = xstrdup(text ? text : "");
    p->id = xstrdup(id);
    p->button = xstrdup(button);
    p->action = xstrdup(action);
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
    if (U.fresh) {
        U.fresh = false;
        dt = 0;
    }
    if (sphere_appear_step(&U.ap, dt)) {
        gtk_widget_hide(U.win);
        return G_SOURCE_CONTINUE;
    }
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
    U.face_on = sphere_style_is_face((SphereStyle)U.want_style);
    if (U.face_on) {
        if (!U.face) U.face = face_create((unsigned)g_get_monotonic_time());
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.activity = (FaceActivity)st; /* mismo orden que JvState */
        in.voice = U.voice;
        in.pulse = U.pulse;
        in.affect = affect_get();
        in.cue = affect_last_cue(&in.cue_seq);
        in.level = FACE_LEVEL_HIGH; /* siempre «mucho» */
        in.symbols = U.face_symbols;
        face_step(U.face, dt, &in, &U.pose);
        face_sphere_colors(&in.affect, &U.cur, &U.draw);
    }
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
    sphere_set_presence(U.sr, U.ap.anim, U.ap.presence);
    bool face = U.face_on && sphere_style_is_face((SphereStyle)style);
    sphere_set_face(U.sr, face ? &U.pose : NULL);
    /* Flotante: sin fondo. El alfa sale de la intensidad, ya multiplicado
       (como lo quiere cairo): solo se ve la esfera, sin el cuadro negro. */
    bool flo = U.mode == DISPLAY_WINDOWED_BORDERLESS;
    sphere_render(U.sr, t, U.angle, U.voice_t, face ? &U.draw : &U.cur, U.voice, U.pulse, (SphereStyle)style,
                  (uint32_t *)cairo_image_surface_get_data(U.surf), cairo_image_surface_get_stride(U.surf) / 4, flo);
    cairo_surface_mark_dirty(U.surf);

    if (flo) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0.02, 0.016, 0.047);
        cairo_paint(cr);
    }
    /* Del tamaño de siempre; lo que sobra del lienzo es para las ondas. */
    double dst = (double)side * canvas / base;
    double scale = dst / canvas;
    /* Deslizarse: sube desde abajo del borde de la ventana. */
    double off = U.ap.anim == SPHERE_ANIM_SLIDE && U.ap.presence < 1.0f
                     ? sphere_slide_offset(U.ap.presence) * (H + dst) / 2
                     : 0;
    cairo_save(cr);
    cairo_translate(cr, (W - dst) / 2, (H - dst) / 2 + off);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, U.surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}

/* ------------------------------------------- la esfera flotante en GNOME --- */

/* En Wayland la ventana no puede ponerse encima de todo ni moverse sola: se
   lo pide a la extensión de Sokari, que solo toca ventanas de Sokari. */
static bool own_window(const char *action, int x, int y, int *out_x, int *out_y)
{
    bool ok = false;
    GnomeStatus st = gnome_own_window("Sokari", action, x, y, out_x, out_y, &ok);
    if (st == GN_NO_EXTENSION && !U.old_extension_told && gnome_desktop()) {
        U.old_extension_told = true;
        app_notify("Sokari", "Para que la esfera flotante quede encima de todo y recuerde dónde la dejaste, cierra "
                             "sesión y vuelve a entrar una vez (así GNOME carga la extensión nueva de Sokari).");
    }
    return st == GN_OK && ok;
}

/* La flotante: encima de todo y donde la dejaste la última vez. Pantalla
   completa: encima de todo. */
static gboolean place_window(gpointer u)
{
    if (!U.win || !gtk_widget_get_visible(U.win)) return G_SOURCE_REMOVE;
    if (U.mode == DISPLAY_FULLSCREEN) own_window("above", 0, 0, NULL, NULL);
    if (U.mode != DISPLAY_WINDOWED_BORDERLESS) return G_SOURCE_REMOVE;
    AppConfig c = config_snapshot();
    int x = c.orb_x, y = c.orb_y;
    SecureZeroMemory(c.groq_api_key, strlen(c.groq_api_key));
    config_free(&c);
    if (x >= 0 && y >= 0) own_window("move", x, y, NULL, NULL);
    own_window("above", 0, 0, NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* Dónde quedó: se guarda para la próxima vez. */
static void remember_place(void)
{
    if (!U.win || U.mode != DISPLAY_WINDOWED_BORDERLESS || !gtk_widget_get_visible(U.win)) return;
    int x = 0, y = 0;
    if (own_window("where", 0, 0, &x, &y)) config_set_orb_pos(x, y);
}

/* Después de arrastrarla: GNOME la mueve por su cuenta, así que se pregunta
   dónde va hasta que deja de moverse. */
static gboolean where_tick(gpointer u)
{
    int x = 0, y = 0;
    bool ok = U.win && own_window("where", 0, 0, &x, &y);
    if (ok && x == U.where_x && y == U.where_y) {
        config_set_orb_pos(x, y);
        U.where_timer = 0;
        return G_SOURCE_REMOVE;
    }
    U.where_x = x;
    U.where_y = y;
    if (!ok || ++U.where_tries > 30) {
        U.where_timer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void watch_place(void)
{
    if (U.mode != DISPLAY_WINDOWED_BORDERLESS) return;
    U.where_tries = 0;
    U.where_x = U.where_y = -1;
    if (!U.where_timer) U.where_timer = g_timeout_add(700, where_tick, NULL);
}

/* Solo el círculo de la esfera recibe clics: lo transparente de alrededor
   los deja pasar a lo que está abajo. */
static void update_input_shape(void)
{
    if (!U.win || !U.area || !gtk_widget_get_realized(U.win)) return;
    if (U.mode != DISPLAY_WINDOWED_BORDERLESS) {
        gtk_widget_input_shape_combine_region(U.win, NULL);
        return;
    }
    int ax = 0, ay = 0;
    gtk_widget_translate_coordinates(U.area, U.win, 0, 0, &ax, &ay);
    int W = gtk_widget_get_allocated_width(U.area), H = gtk_widget_get_allocated_height(U.area);
    double r = (W < H ? W : H) * 0.47, cx = ax + W / 2.0, cy = ay + H / 2.0;
    cairo_region_t *reg = cairo_region_create();
    for (int y = (int)(cy - r); y < (int)(cy + r); y += 4) {
        double dy = y + 2 - cy, half = sqrt(fmax(0, r * r - dy * dy));
        cairo_rectangle_int_t row = {(int)(cx - half), y, (int)(2 * half) + 1, 4};
        cairo_region_union_rectangle(reg, &row);
    }
    gtk_widget_input_shape_combine_region(U.win, reg);
    cairo_region_destroy(reg);
}

static void on_area_allocate(GtkWidget *w, GdkRectangle *a, gpointer u)
{
    update_input_shape();
}

/* Soltar sin haberla arrastrado es un clic: la calla si está hablando o
   pensando, y si no, le habla. Si se arrastra más de 6 px, mueve la ventana
   (en Wayland una app no puede moverse sola: se lo pide a GNOME). */
static void popup_menu(GdkEvent *e);

static gboolean on_sphere_press(GtkWidget *w, GdkEventButton *e, gpointer u)
{
    if (e->type == GDK_BUTTON_PRESS && e->button == 3) {
        popup_menu((GdkEvent *)e);
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS || e->button != 1) return FALSE;
    U.pressed = true;
    U.press_x = e->x_root;
    U.press_y = e->y_root;
    return TRUE;
}

static gboolean on_sphere_motion(GtkWidget *w, GdkEventMotion *e, gpointer u)
{
    if (!U.pressed || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
    double dx = e->x_root - U.press_x, dy = e->y_root - U.press_y;
    if (dx * dx + dy * dy < 36) return TRUE;
    U.pressed = false;
    gtk_window_begin_move_drag(GTK_WINDOW(U.win), 1, (gint)e->x_root, (gint)e->y_root, e->time);
    watch_place();
    return TRUE;
}

static gboolean on_sphere_release(GtkWidget *w, GdkEventButton *e, gpointer u)
{
    if (e->button != 1 || !U.pressed) return FALSE;
    U.pressed = false;
    int st = g_atomic_int_get(&g_state);
    if (st == JV_SPEAKING || st == JV_THINKING) voice_skip();
    else if (U.voice_started) voice_trigger();
    return TRUE;
}

/* El fondo (alrededor de la esfera y los subtítulos) también mueve la ventana. */
static gboolean on_background_press(GtkWidget *w, GdkEventButton *e, gpointer u)
{
    if (e->type == GDK_BUTTON_PRESS && e->button == 3) {
        popup_menu((GdkEvent *)e);
        return TRUE;
    }
    if (e->type != GDK_BUTTON_PRESS || e->button != 1) return FALSE;
    gtk_window_begin_move_drag(GTK_WINDOW(U.win), 1, (gint)e->x_root, (gint)e->y_root, e->time);
    watch_place();
    return TRUE;
}

/* -------------------------------------------------------- arrancar voz --- */

static bool subtitles_on(void)
{
    AppConfig cfg = config_snapshot();
    bool on = cfg.subtitles;
    U.want_style = cfg.sphere_style;
    U.anim = cfg.appear_anim;
    U.face_symbols = cfg.face_symbols;
    U.want_mode = cfg.display_mode >= 0 && cfg.display_mode < DISPLAY_MODE_COUNT ? cfg.display_mode : DISPLAY_WINDOWED;
    U.only_talking = cfg.show_only_talking;
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

/* La Configuración está en settings_linux.c; esto es lo que le pide a la ventana. */

void ui_linux_preview_appear(int anim)
{
    /* Con la ventana a la vista, la esfera sale y vuelve; si estaba cerrada,
       se asoma y se va. */
    if (!U.win || anim < 0 || anim >= SPHERE_ANIM_NONE) return;
    bool shown = gtk_widget_get_visible(U.win) && U.ap.dir >= 0;
    sphere_appear_test(&U.ap, (SphereAnim)anim, shown);
    if (!shown && !gtk_widget_get_visible(U.win)) {
        U.fresh = true;
        gtk_widget_show(U.win);
    }
}

static gboolean apply_mode_idle(gpointer u);

void ui_linux_settings_saved(bool first_run)
{
    gtk_widget_set_visible(U.sub_user, subtitles_on());
    gtk_widget_set_visible(U.sub_sokari, subtitles_on());
    /* Después de que se cierre Configuración: puede tocar rehacer la ventana. */
    if (U.want_mode != U.mode) g_idle_add(apply_mode_idle, NULL);
    if (U.voice_started) voice_settings_changed();
    else start_voice(first_run);
}

void ui_linux_settings_cancelled(bool first_run)
{
    if (first_run && !has_key())
        app_notify("Sokari", "Sin tu API key de Groq todavía no puedo contestarte. Abre Configuración cuando la tengas.");
}

static void settings_open(bool first_run)
{
    settings_linux_open(GTK_WINDOW(U.win), first_run, SET_ACCOUNT);
}

/* ------------------------------------------------------------- acciones --- */

static void show_window(void)
{
    if (!U.win) return;
    /* Oculta: entra desde fuera. Yéndose (o en Probar): regresa desde donde iba. */
    bool hidden = !gtk_widget_get_visible(U.win);
    if (hidden || sphere_appear_moving(&U.ap) || U.ap.presence < 1.0f)
        sphere_appear_enter(&U.ap, (SphereAnim)U.anim, hidden);
    if (hidden) U.fresh = true;
    gtk_widget_show(U.win);
    switch (U.mode) {
    case DISPLAY_WINDOWED_BORDERLESS:
        /* La flotante no se roba el teclado: solo aparece, encima de todo.
           GNOME la acomoda al mostrarla; se le pide su lugar un par de veces
           por si la acomodó después. */
        if (hidden) {
            g_timeout_add(250, place_window, NULL);
            g_timeout_add(900, place_window, NULL);
        }
        break;
    case DISPLAY_FULLSCREEN:
    case DISPLAY_FULLSCREEN_BORDERLESS:
        U.fullscreen = true;
        gtk_window_fullscreen(GTK_WINDOW(U.win));
        gtk_window_present(GTK_WINDOW(U.win));
        if (hidden) g_timeout_add(250, place_window, NULL);
        break;
    default: gtk_window_present(GTK_WINDOW(U.win));
    }
}

/* Se va con su animación y la ventana se esconde al terminar (on_tick). */
static void hide_window(void)
{
    if (!U.win || !gtk_widget_get_visible(U.win)) return;
    remember_place();
    if (sphere_appear_leave(&U.ap, (SphereAnim)U.anim)) gtk_widget_hide(U.win);
}

/* «Aparecer solo cuando le hablas»: aparece al hablarle y, un momento
   después de terminar, se va. Con Configuración abierta se queda. */
static gboolean hide_after_talk(gpointer u)
{
    U.hide_timer = 0;
    if (g_atomic_int_get(&g_state) == JV_IDLE && !settings_linux_is_open()) hide_window();
    return G_SOURCE_REMOVE;
}

static void follow_state(int st)
{
    int prev = U.last_state;
    U.last_state = st;
    if (!U.only_talking || U.mode == DISPLAY_MINIMIZED || !U.win) return;
    if (st != JV_IDLE && prev == JV_IDLE) {
        if (U.hide_timer) g_source_remove(U.hide_timer);
        U.hide_timer = 0;
        if (!gtk_widget_get_visible(U.win) || U.ap.dir < 0) show_window();
    } else if (st == JV_IDLE && prev != JV_IDLE && !U.hide_timer) {
        U.hide_timer = g_timeout_add(2500, hide_after_talk, NULL);
    }
}

static GMenuModel *app_menu(void);

/* Sokari va a abrir o a usar otra ventana: en pantalla completa (encima de
   todo) se aparta para que se vea. */
static gboolean on_yield_idle(gpointer u)
{
    if (U.mode == DISPLAY_FULLSCREEN) hide_window();
    return G_SOURCE_REMOVE;
}

void ui_post_yield(void)
{
    g_idle_add(on_yield_idle, NULL);
}

/* El clic derecho: el mismo menú de arriba y de ☰ (en la flotante no hay barra). */
static void popup_menu(GdkEvent *e)
{
    if (!U.popup) {
        U.popup = gtk_menu_new_from_model(app_menu());
        gtk_widget_insert_action_group(U.popup, "app", G_ACTION_GROUP(U.app));
        gtk_menu_attach_to_widget(GTK_MENU(U.popup), U.area, NULL);
    }
    gtk_menu_popup_at_pointer(GTK_MENU(U.popup), e);
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
    settings_linux_open(GTK_WINDOW(U.win), false, SET_DEVICES);
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer u)
{
    log_msg("Sokari: salir desde la ventana.");
    remember_place();
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

/* Reabre Sokari cuando esta ya se cerró (la instalada es la nueva). */
static void restart_sokari(void)
{
    char pid[16];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *argv[] = {"/bin/sh", "-c", "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; exec sokari", "sh", pid,
                    NULL};
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL))
        g_action_group_activate_action(G_ACTION_GROUP(U.app), "salir", NULL);
}

static void on_update_response(GtkDialog *d, gint response, gpointer u)
{
    gtk_widget_destroy(GTK_WIDGET(d));
    if (response == 1) restart_sokari();
}

static void update_done(GObject *src, GAsyncResult *res, gpointer data)
{
    char *msg = g_task_propagate_pointer(G_TASK(res), NULL);
    bool installed = linux_update_installed();
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(U.win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                                          installed ? GTK_BUTTONS_NONE : GTK_BUTTONS_OK, "%s",
                                          msg ? msg : "No pude revisar.");
    if (installed) gtk_dialog_add_buttons(GTK_DIALOG(m), "Después", 0, "Reiniciar Sokari", 1, NULL);
    g_signal_connect(m, "response", G_CALLBACK(on_update_response), NULL);
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
    {"actualizar", act_update, NULL, NULL, NULL, {0}}, {"version", NULL, NULL, NULL, NULL, {0}},
};

/* El menú, uno solo: el del ícono de arriba y el de ☰ salen de aquí. */
static GMenu *menu_new(void)
{
    GMenu *menu = g_menu_new(), *sec = g_menu_new();
    g_menu_append(sec, "Mostrar Sokari", "app.mostrar");
    g_menu_append(sec, "Hablarle ahora", "app.hablar");
    g_menu_append(sec, "Micrófono en silencio", "app.silencio");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    sec = g_menu_new();
    g_menu_append(sec, "Configuración", "app.configuracion");
    g_menu_append(sec, "Tus PCs", "app.malla");
    g_menu_append(sec, LINUX_UPDATE_LABEL, "app.actualizar");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    sec = g_menu_new();
    g_menu_append(sec, "Sokari " SOKARI_VERSION, "app.version"); /* apagada: solo dice la versión */
    g_menu_append(sec, "Salir", "app.salir");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    return menu;
}

static GMenuModel *app_menu(void)
{
    if (!U.menu) U.menu = menu_new();
    return G_MENU_MODEL(U.menu);
}

static GtkWidget *tray_menu_new(void)
{
    GtkWidget *menu = gtk_menu_new_from_model(app_menu());
    if (U.app) gtk_widget_insert_action_group(menu, "app", G_ACTION_GROUP(U.app));
    return menu;
}

/* ------------------------------------------------------ el ícono de arriba --- */

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
    GtkWidget *menu = tray_menu_new();
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
    ".sokari .estado { color: #9d95ff; font-size: 11pt; }"
    /* La esfera flotante: sin fondo; las letras con sombra para leerse encima de lo que sea. */
    "window.sokari.flotante, window.sokari.flotante .fondo { background-color: transparent; }"
    ".sokari.flotante .ella, .sokari.flotante .tu { text-shadow: 0 1px 3px rgba(0,0,0,0.95), 0 0 8px rgba(0,0,0,0.8); }";

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer u)
{
    /* Cerrar la ventana no cierra a Sokari: sigue escuchando. La esfera se va
       con su animación y la ventana se esconde al terminar (on_tick). */
    hide_window();
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

/* La barra de arriba: Hablar, el título, el menú y cerrar. */
static GtkWidget *titlebar_new(void)
{
    GtkWidget *bar = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(bar), "Sokari");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), app_menu());
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), mb);
    GtkWidget *talk = gtk_button_new_with_label("Hablar");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(talk), "app.hablar");
    gtk_style_context_add_class(gtk_widget_get_style_context(talk), "suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), talk);
    /* Mostrarla: en GTK 3 nace escondida y sin ella la ventana se queda sin
       barra (sin Hablar, sin el menú, sin la X y sin poder moverla). */
    gtk_widget_show_all(bar);
    return bar;
}

/* Los renglones del menú, "texto|acción" uno por línea (heap), secciones incluidas. */
static void menu_lines(GMenuModel *m, GString *out)
{
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        GMenuModel *sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sec) {
            menu_lines(sec, out);
            g_object_unref(sec);
            continue;
        }
        char *label = NULL, *action = NULL;
        g_menu_model_get_item_attribute(m, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        g_menu_model_get_item_attribute(m, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
        g_string_append_printf(out, "%s|%s\n", label ? label : "", action ? action : "");
        g_free(label);
        g_free(action);
    }
}

/* Para las pruebas (necesita pantalla): lo que falla en la barra y en los
   menús, o NULL. */
char *ui_window_problems(void)
{
    GtkWidget *bar = titlebar_new();
    g_object_ref_sink(bar);
    char *problem = NULL;
    bool talk = false;
    GtkWidget *menu_button = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(bar));
    for (GList *k = kids; k; k = k->next) {
        const char *action =
            GTK_IS_ACTIONABLE(k->data) ? gtk_actionable_get_action_name(GTK_ACTIONABLE(k->data)) : NULL;
        if (action && !strcmp(action, "app.hablar")) talk = gtk_widget_get_visible(k->data);
        if (GTK_IS_MENU_BUTTON(k->data) && gtk_widget_get_visible(k->data)) menu_button = k->data;
    }
    g_list_free(kids);
    GString *lines = g_string_new(NULL);
    menu_lines(app_menu(), lines);
    /* Lo que tiene que estar, igual arriba y en ☰. */
    static const char *const NEED[] = {"Mostrar Sokari|app.mostrar", "Hablarle ahora|app.hablar",
                                       "Micrófono en silencio|app.silencio", "Configuración|app.configuracion",
                                       "Tus PCs|app.malla", LINUX_UPDATE_LABEL "|app.actualizar",
                                       "Sokari " SOKARI_VERSION "|app.version", "Salir|app.salir"};
    const char *missing = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(NEED) && !missing; i++) {
        char *line = g_strdup_printf("%s\n", NEED[i]);
        if (!strstr(lines->str, line)) missing = NEED[i];
        g_free(line);
    }
    GtkWidget *tray = tray_menu_new();
    g_object_ref_sink(tray);
    int tray_items = 0, model_items = 0;
    kids = gtk_container_get_children(GTK_CONTAINER(tray));
    for (GList *k = kids; k; k = k->next)
        if (!GTK_IS_SEPARATOR_MENU_ITEM(k->data)) tray_items++;
    g_list_free(kids);
    for (const char *c = lines->str; *c; c++) model_items += *c == '\n';
    char *notice = linux_update_notice("v9.9.9");
    if (!gtk_widget_get_visible(bar))
        problem = xstrdup("la barra de arriba nace escondida (sin ella no hay Hablar, ni menú, ni X, ni cómo moverla)");
    else if (!talk) problem = xstrdup("no se ve el botón Hablar");
    else if (!menu_button) problem = xstrdup("no se ve el botón del menú");
    else if (!gtk_header_bar_get_show_close_button(GTK_HEADER_BAR(bar))) problem = xstrdup("no tiene la X para cerrar");
    else if (gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(menu_button)) != app_menu())
        problem = xstrdup("el menú ☰ no es el mismo que el de arriba");
    else if (missing) problem = str_printf("al menú le falta «%s»", missing);
    else if (tray_items != model_items)
        problem = str_printf("el menú de arriba tiene %d renglones y ☰ %d", tray_items, model_items);
    else if (!strstr(notice, "«" LINUX_UPDATE_LABEL "»"))
        problem = xstrdup("el aviso de versión nueva no nombra el botón que sí existe");
    free(notice);
    g_string_free(lines, TRUE);
    gtk_widget_destroy(tray);
    g_object_unref(tray);
    gtk_widget_destroy(bar);
    g_object_unref(bar);
    return problem;
}

/* La ventana según el modo: la flotante es solo la esfera (sin marco, sin
   fondo); las demás, la ventana de siempre con su barra. */
static void build_window(void)
{
    static bool styled;
    sphere_appear_init(&U.ap);
    if (!styled) {
        styled = true;
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, CSS, -1, NULL);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    }
    bool flo = U.mode == DISPLAY_WINDOWED_BORDERLESS;

    U.win = gtk_application_window_new(U.app);
    gtk_window_set_title(GTK_WINDOW(U.win), "Sokari");
    gtk_window_set_default_size(GTK_WINDOW(U.win), flo ? 300 : 520, flo ? 360 : 640);
    gtk_style_context_add_class(gtk_widget_get_style_context(U.win), "sokari");
    if (flo) {
        gtk_style_context_add_class(gtk_widget_get_style_context(U.win), "flotante");
        gtk_window_set_decorated(GTK_WINDOW(U.win), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(U.win), FALSE);
        gtk_widget_set_app_paintable(U.win, TRUE);
        /* Con alfa: sin esto, en X11 lo transparente sale negro. */
        GdkVisual *rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(U.win));
        if (rgba) gtk_widget_set_visual(U.win, rgba);
        /* En X11 alcanza con esto; en Wayland lo hace la extensión (place_floating). */
        gtk_window_set_keep_above(GTK_WINDOW(U.win), TRUE);
        gtk_window_stick(GTK_WINDOW(U.win));
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(U.win), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(U.win), TRUE);
        gtk_window_set_focus_on_map(GTK_WINDOW(U.win), FALSE);
    }
    size_t n = 0;
    const void *png = res_data(IDR_ICON_PNG, &n);
    if (png) {
        GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
        if (gdk_pixbuf_loader_write(ld, png, n, NULL) && gdk_pixbuf_loader_close(ld, NULL))
            gtk_window_set_default_icon(gdk_pixbuf_loader_get_pixbuf(ld));
        g_object_unref(ld);
    }

    if (!flo) gtk_window_set_titlebar(GTK_WINDOW(U.win), titlebar_new());

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, flo ? 2 : 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "fondo");
    gtk_container_set_border_width(GTK_CONTAINER(box), flo ? 0 : 16);
    U.area = gtk_drawing_area_new();
    gtk_widget_set_size_request(U.area, flo ? 280 : 220, flo ? 280 : 220);
    gtk_widget_set_vexpand(U.area, TRUE);
    gtk_widget_add_events(U.area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(U.area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(U.area, "button-press-event", G_CALLBACK(on_sphere_press), NULL);
    g_signal_connect(U.area, "motion-notify-event", G_CALLBACK(on_sphere_motion), NULL);
    g_signal_connect(U.area, "button-release-event", G_CALLBACK(on_sphere_release), NULL);
    gtk_widget_add_tick_callback(U.area, on_tick, NULL, NULL);
    g_signal_connect(U.area, "size-allocate", G_CALLBACK(on_area_allocate), NULL);
    gtk_box_pack_start(GTK_BOX(box), U.area, TRUE, TRUE, 0);
    U.sub_sokari = label("ella", true);
    U.sub_user = label("tu", true);
    U.status = label("estado", false);
    gtk_box_pack_start(GTK_BOX(box), U.sub_sokari, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), U.sub_user, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), U.status, FALSE, FALSE, 4);
    GtkWidget *back = gtk_event_box_new();
    /* Sin ventana propia que pintar: solo recibe los clics del fondo. */
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(back), FALSE);
    gtk_container_add(GTK_CONTAINER(back), box);
    g_signal_connect(back, "button-press-event", G_CALLBACK(on_background_press), NULL);
    gtk_container_add(GTK_CONTAINER(U.win), back);
    g_signal_connect(U.win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(U.win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(U.win, "notify::is-active", G_CALLBACK(on_active_changed), NULL);
    U.cur = SPHERE_IDLE;
    refresh_status();
    gtk_widget_show_all(back);
    bool subs = subtitles_on();
    gtk_widget_set_visible(U.sub_user, subs);
    gtk_widget_set_visible(U.sub_sokari, subs);
    /* En la flotante, el estado («Di Hey Sokari…») estorba: solo la esfera. */
    gtk_widget_set_no_show_all(U.status, flo);
    gtk_widget_set_visible(U.status, !flo);
}

/* Otro modo de pantalla: a la flotante (o de ella) se rehace la ventana; los
   demás solo cambian de pantalla completa o se minimizan. */
static gboolean apply_mode_idle(gpointer u)
{
    if (!U.win || U.want_mode == U.mode) return G_SOURCE_REMOVE;
    bool was_flo = U.mode == DISPLAY_WINDOWED_BORDERLESS, flo = U.want_mode == DISPLAY_WINDOWED_BORDERLESS;
    if (was_flo != flo) {
        remember_place();
        if (U.where_timer) g_source_remove(U.where_timer);
        U.where_timer = 0;
        if (U.popup) gtk_widget_destroy(U.popup);
        U.popup = NULL;
        GtkWidget *old = U.win;
        U.win = NULL;
        gtk_widget_destroy(old);
        U.mode = U.want_mode;
        build_window();
        show_window();
        return G_SOURCE_REMOVE;
    }
    U.mode = U.want_mode;
    U.fullscreen = U.mode == DISPLAY_FULLSCREEN || U.mode == DISPLAY_FULLSCREEN_BORDERLESS;
    if (U.fullscreen) {
        gtk_window_fullscreen(GTK_WINDOW(U.win));
        g_timeout_add(250, place_window, NULL);
    } else {
        own_window("normal", 0, 0, NULL, NULL);
        gtk_window_unfullscreen(GTK_WINDOW(U.win));
        if (U.mode == DISPLAY_MINIMIZED) gtk_window_iconify(GTK_WINDOW(U.win));
    }
    return G_SOURCE_REMOVE;
}

/* Lo primero que hace la Sokari que se queda corriendo. */
static void startup(bool from_autostart)
{
    g_atomic_int_set(&g_active, 1);
    g_application_hold(G_APPLICATION(U.app));
    g_action_map_add_action_entries(G_ACTION_MAP(U.app), ACTIONS, G_N_ELEMENTS(ACTIONS), NULL);
    GAction *version = g_action_map_lookup_action(G_ACTION_MAP(U.app), "version");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(version), FALSE);
    GAction *mute = g_action_map_lookup_action(G_ACTION_MAP(U.app), "silencio");
    g_simple_action_set_state(G_SIMPLE_ACTION(mute), g_variant_new_boolean(config_mic_muted()));
    const char *accels_talk[] = {"<Primary>h", NULL};
    gtk_application_set_accels_for_action(U.app, "app.hablar", accels_talk);
    subtitles_on();
    U.mode = U.want_mode;
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
    if (U.mode == DISPLAY_MINIMIZED) gtk_window_iconify(GTK_WINDOW(U.win));
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

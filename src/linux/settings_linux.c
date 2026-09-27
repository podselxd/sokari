/* La Configuración de Sokari en Linux, con las mismas secciones que en
   Windows (Cuenta, Pantalla, Voz y audio, General, Skills, Tus PCs, IA de
   respaldo) y una barra al lado para ir de una a otra. Lo que tarda (revisar
   la malla, probar una PC, buscar actualizaciones) corre en otro hilo; si la
   cierras antes de que termine, el resultado se tira. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "autostart.h"
#include "config.h"
#include "linux/settings_linux.h"
#include "log.h"
#include "memory.h"
#include "mesh.h"
#include "skills.h"
#include "sphere.h"
#include "tts.h"
#include "update.h"
#include "util.h"
#include "voice.h"

#define MAX_PCS 8

static const char *const PAGE_IDS[SET_PAGES] = {"cuenta", "pantalla", "voz", "general", "skills", "pcs", "ia"};
static const char *const PAGE_TITLES[SET_PAGES] = {"Cuenta",  "Pantalla", "Voz y audio",   "General",
                                                   "Skills",  "Tus PCs",  "IA de respaldo"};
static const char *const APPEAR_NAMES[SPHERE_ANIM_COUNT] = {"Materializarse", "Deslizarse", "Zoom", "Ninguna"};
static const char *const END_NAMES[3] = {"Poco", "Normal", "Más"};
static const char *const BACKUP_NAMES[4] = {"NVIDIA (build.nvidia.com, gratis con límite)",
                                            "DeepSeek (de pago, muy barata)", "OpenRouter (modelos «:free»)",
                                            "GLM de Z.ai (tiene uno gratis)"};
static const char *const BACKUP_URLS[4] = {"https://build.nvidia.com", "https://platform.deepseek.com/api_keys",
                                           "https://openrouter.ai/keys", "https://z.ai"};
static const char *const SOUND_EXTS[3] = {"mp3", "wav", "ogg"};

enum { JOB_DETECT, JOB_DIAGNOSE, JOB_FIREWALL, JOB_PROBE, JOB_UPDATE };

typedef struct {
    GtkWidget *dialog, *stack, *status;
    bool first_run, closed;
    int busy; /* trabajos en otro hilo que todavía no terminan */
    /* Cuenta */
    GtkWidget *key, *name, *profile_pw, *stop;
    /* Pantalla */
    GtkWidget *style, *anim, *subtitles, *face_level, *face_symbols;
    /* Voz y audio */
    GtkWidget *volume, *voice, *mic, *out, *sensitivity, *end_silence, *duck;
    TtsVoice *voices;
    int nvoices;
    /* General */
    GtkWidget *autostart, *full, *sound, *update;
    /* Skills */
    GtkWidget *skills[16], *city, *mine;
    /* Tus PCs */
    GtkWidget *secret, *listening, *pcs, *mesh_buttons, *mesh_result;
    /* IA de respaldo */
    GtkWidget *backup[4], *order;
} Form;

typedef struct {
    Form *f;
    int kind;
    char *name, *host; /* Probar: qué PC */
    char *text;        /* lo que salió */
} Job;

static Form *g_form; /* la que está abierta */

/* ------------------------------------------------------------ piezas --- */

static void set_status(Form *f, const char *text)
{
    gtk_label_set_text(GTK_LABEL(f->status), text ? text : "");
}

static GtkWidget *text_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
    return l;
}

/* Una explicación chica y más tenue, debajo de lo que explica. */
static GtkWidget *help_label(const char *text)
{
    GtkWidget *l = text_label(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    PangoAttrList *a = pango_attr_list_new();
    pango_attr_list_insert(a, pango_attr_scale_new(0.9));
    gtk_label_set_attributes(GTK_LABEL(l), a);
    pango_attr_list_unref(a);
    return l;
}

static GtkWidget *page_grid(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 10);
    gtk_grid_set_column_spacing(GTK_GRID(g), 14);
    gtk_container_set_border_width(GTK_CONTAINER(g), 18);
    return g;
}

static GtkWidget *add_row(GtkWidget *grid, int row, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return w;
}

/* A todo lo ancho. */
static GtkWidget *add_wide(GtkWidget *grid, int row, GtkWidget *w)
{
    gtk_grid_attach(GTK_GRID(grid), w, 0, row, 2, 1);
    return w;
}

static GtkWidget *check(const char *label, bool on)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    return c;
}

static GtkWidget *button_row(void)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    return b;
}

static GtkWidget *add_button(GtkWidget *row, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, data);
    gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    return b;
}

static void on_reveal(GtkEntry *e, GtkEntryIconPosition pos, GdkEvent *ev, gpointer u)
{
    bool show = !gtk_entry_get_visibility(e);
    gtk_entry_set_visibility(e, show);
    gtk_entry_set_icon_from_icon_name(e, GTK_ENTRY_ICON_SECONDARY, show ? "view-conceal-symbolic" : "view-reveal-symbolic");
}

/* Un campo con puntitos (API keys, contraseñas) y un ojo para verlo. */
static GtkWidget *secret_entry(const char *value, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(e), GTK_ENTRY_ICON_SECONDARY, "view-reveal-symbolic");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(e), GTK_ENTRY_ICON_SECONDARY, "Mostrar u ocultar");
    g_signal_connect(e, "icon-press", G_CALLBACK(on_reveal), NULL);
    if (value) gtk_entry_set_text(GTK_ENTRY(e), value);
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    return e;
}

static GtkWidget *text_entry(const char *value, const char *placeholder)
{
    GtkWidget *e = gtk_entry_new();
    if (value) gtk_entry_set_text(GTK_ENTRY(e), value);
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    return e;
}

static GtkWidget *choice(const char *const *names, int n, int active)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), active >= 0 && active < n ? active : 0);
    return c;
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

static char *entry_text(GtkWidget *e)
{
    return str_trim(gtk_entry_get_text(GTK_ENTRY(e)));
}

static void open_uri(const char *uri)
{
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        log_msg("No pude abrir %s: %s", uri, err ? err->message : "?");
        g_clear_error(&err);
    }
}

/* Abre una carpeta o un archivo con la app de tu escritorio. */
static void open_path(const wchar_t *path)
{
    char *p = wide_to_utf8(path);
    GFile *f = g_file_new_for_path(p);
    char *uri = g_file_get_uri(f);
    open_uri(uri);
    g_free(uri);
    g_object_unref(f);
    free(p);
}

/* ------------------------------------------------ trabajos en otro hilo --- */

static void form_free(Form *f)
{
    tts_free_voices(f->voices, f->nvoices);
    free(f);
}

static void set_busy(Form *f, bool busy)
{
    gtk_widget_set_sensitive(f->mesh_buttons, !busy);
    gtk_widget_set_sensitive(f->pcs, !busy);
    gtk_widget_set_sensitive(f->update, !busy);
}

static void fill_pcs(Form *f);

static void job_work(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    Job *j = data;
    switch (j->kind) {
    case JOB_DETECT:
        j->text = mesh_detect_devices();
        break;
    case JOB_DIAGNOSE:
        j->text = mesh_diagnose();
        break;
    case JOB_FIREWALL:
        j->text = xstrdup(mesh_allow_firewall() ? "Listo: la malla puede recibir órdenes de tu red de Tailscale."
                                                : "No pude cambiar el firewall (¿cancelaste la contraseña?).");
        break;
    case JOB_PROBE:
        j->text = mesh_probe_report(j->name, j->host);
        break;
    default:
        j->text = update_check_now();
        break;
    }
    g_task_return_boolean(t, TRUE);
}

static void job_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Job *j = data;
    Form *f = j->f;
    f->busy--;
    if (f->closed) {
        if (!f->busy) form_free(f);
    } else {
        set_busy(f, f->busy > 0);
        if (j->kind == JOB_UPDATE) {
            set_status(f, j->text && *j->text ? j->text : "No pude revisar si hay versión nueva.");
        } else {
            gtk_label_set_text(GTK_LABEL(f->mesh_result), j->text ? j->text : "");
            set_status(f, NULL);
            fill_pcs(f);
        }
    }
    free(j->text);
    free(j->name);
    free(j->host);
    free(j);
}

static void run_job(Form *f, int kind, const char *name, const char *host, const char *busy_text)
{
    Job *j = xcalloc(1, sizeof *j);
    j->f = f;
    j->kind = kind;
    j->name = name ? xstrdup(name) : NULL;
    j->host = host ? xstrdup(host) : NULL;
    f->busy++;
    set_busy(f, true);
    set_status(f, busy_text);
    GTask *t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_work);
    g_object_unref(t);
}

/* ------------------------------------------------------------- Cuenta --- */

static void on_reset_passwords(GtkButton *b, gpointer u)
{
    Form *f = u;
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(f->dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_YES_NO, "¿Restablecer las contraseñas?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(m), "Esto quita la contraseña de TODOS los perfiles guardados en esta PC (el tuyo y el de "
                               "cualquier otra persona que haya protegido el suyo). Solo se puede hacer desde aquí, "
                               "nunca por voz.");
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    if (r != GTK_RESPONSE_YES) return;
    int n = reset_all_profile_passwords();
    char *msg = n ? str_printf("Se quitaron %d contraseña(s).", n) : xstrdup("No había ninguna contraseña puesta.");
    set_status(f, msg);
    free(msg);
}

static GtkWidget *page_account(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    f->key = add_row(g, r++, "API key de Groq", secret_entry(cfg->groq_api_key, "gsk_…"));
    gtk_entry_set_activates_default(GTK_ENTRY(f->key), TRUE);
    GtkWidget *link = gtk_link_button_new_with_label("https://console.groq.com/keys",
                                                     "Consíguela gratis en console.groq.com →");
    gtk_widget_set_halign(link, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g), link, 1, r++, 1, 1);
    f->name = add_row(g, r++, "Tu nombre", text_entry(cfg->user_name, NULL));
    gtk_grid_attach(GTK_GRID(g), help_label("Para que Sokari sepa con quién habla desde que arranca."), 1, r++, 1, 1);
    f->profile_pw = add_row(g, r++, "Contraseña de tu perfil (opcional)",
                            secret_entry(NULL, "Déjala vacía para no cambiarla"));
    gtk_grid_attach(GTK_GRID(g),
                    help_label("Protege tus datos guardados si otras personas usan Sokari en esta PC."), 1, r++, 1, 1);
    GtkWidget *reset = gtk_button_new_with_label("¿Olvidaste una contraseña de perfil? Restablecer todas");
    gtk_button_set_relief(GTK_BUTTON(reset), GTK_RELIEF_NONE);
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_passwords), f);
    gtk_grid_attach(GTK_GRID(g), reset, 1, r++, 1, 1);
    f->stop = add_row(g, r++, "Palabra de apagado (opcional)", secret_entry(cfg->stop_word, NULL));
    gtk_grid_attach(GTK_GRID(g),
                    help_label("Si la dices, Sokari se apaga al instante. Se revisa en tu PC: nunca se le manda a la IA."),
                    1, r++, 1, 1);
    return g;
}

/* ----------------------------------------------------------- Pantalla --- */

/* Probar: con la animación elegida, aunque no esté guardada. */
static void on_try_anim(GtkButton *b, gpointer combo)
{
    ui_linux_preview_appear(gtk_combo_box_get_active(GTK_COMBO_BOX(combo)));
}

/* Con «Ninguna» no hay nada que probar. */
static void on_anim_changed(GtkComboBox *c, gpointer button)
{
    gtk_widget_set_sensitive(GTK_WIDGET(button), gtk_combo_box_get_active(c) < SPHERE_ANIM_NONE);
}

/* «Qué tanto se le nota» y los símbolos solo cuentan con una cara. */
static void on_style_changed(GtkComboBox *c, gpointer data)
{
    Form *f = data;
    bool face = gtk_combo_box_get_active(c) >= SPHERE_STYLE_FACE_EYES;
    gtk_widget_set_sensitive(f->face_level, face);
    gtk_widget_set_sensitive(f->face_symbols, face);
}

static GtkWidget *page_display(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    static const char *const STYLES[SPHERE_STYLE_COUNT] = {"Halo de puntos", "Líneas (beta)", "Cara: solo ojos (beta)",
                                                           "Cara: ojos y boca (beta)", "Cara: de puntos (beta)"};
    int style = cfg->sphere_style >= 0 && cfg->sphere_style < SPHERE_STYLE_COUNT ? cfg->sphere_style : 0;
    f->style = add_row(g, r++, "Estilo", choice(STYLES, SPHERE_STYLE_COUNT, style));
    static const char *const LEVELS[] = {"Poco", "Normal", "Mucho"};
    int level = cfg->face_level >= 0 && cfg->face_level <= 2 ? cfg->face_level : 1;
    f->face_level = add_row(g, r++, "Qué tanto se le nota", choice(LEVELS, 3, level));
    f->face_symbols = add_wide(g, r++, check("Símbolos en la cara (lágrima, destellos, «?»…)", cfg->face_symbols));
    add_wide(g, r++, help_label("La cara expresa el estado de Sokari, no sentimientos. Con cara, le pide a la IA una "
                                "etiqueta con la emoción de cada respuesta (unos 70 tokens más)."));
    g_signal_connect(f->style, "changed", G_CALLBACK(on_style_changed), f);
    on_style_changed(GTK_COMBO_BOX(f->style), f);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    f->anim = choice(APPEAR_NAMES, SPHERE_ANIM_COUNT, cfg->appear_anim);
    GtkWidget *try_anim = gtk_button_new_with_label("Probar");
    gtk_widget_set_tooltip_text(try_anim, "La esfera sale y vuelve a entrar con la animación elegida.");
    g_signal_connect(try_anim, "clicked", G_CALLBACK(on_try_anim), f->anim);
    g_signal_connect(f->anim, "changed", G_CALLBACK(on_anim_changed), try_anim);
    on_anim_changed(GTK_COMBO_BOX(f->anim), try_anim);
    gtk_box_pack_start(GTK_BOX(row), f->anim, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), try_anim, FALSE, FALSE, 0);
    add_row(g, r++, "Al aparecer y desaparecer", row);
    f->subtitles = add_wide(g, r++, check("Mostrar lo que dices y lo que contesto", cfg->subtitles));
    add_wide(g, r++, help_label("F11 en la ventana de Sokari: pantalla completa (Esc la regresa)."));
    return g;
}

/* -------------------------------------------------------- Voz y audio --- */

/* La voz elegida: "" es Automática (la mejor que haya; se pasa sola a Piper
   cuando termina de bajarla). */
static const char *chosen_voice(Form *f)
{
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(f->voice)) - 1;
    return i >= 0 && i < f->nvoices ? f->voices[i].id : "";
}

static void on_try_voice(GtkButton *b, gpointer u)
{
    Form *f = u;
    if (voice_running()) {
        voice_preview(chosen_voice(f));
        set_status(f, "Escucha: así suena esa voz. Si te gusta, dale a Guardar.");
    } else {
        set_status(f, "La voz se prueba con Sokari iniciado.");
    }
}

static GtkWidget *page_audio(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    f->volume = add_row(g, r++, "Volumen de mi voz", gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5));
    gtk_range_set_value(GTK_RANGE(f->volume), cfg->volume);
    f->nvoices = tts_list_voices(&f->voices);
    f->voice = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(f->voice), "Automática (la mejor que haya)");
    int active = 0;
    for (int i = 0; i < f->nvoices; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(f->voice), f->voices[i].name);
        if (cfg->voice && !strcmp(cfg->voice, f->voices[i].id)) active = i + 1;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(f->voice), active);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(row), f->voice, TRUE, TRUE, 0);
    add_button(row, "Probar", G_CALLBACK(on_try_voice), f);
    add_row(g, r++, "Voz", row);
    f->mic = add_row(g, r++, "Micrófono", device_combo(true, cfg->mic_name));
    f->out = add_row(g, r++, "Por dónde hablo", device_combo(false, cfg->output_name));
    f->sensitivity =
        add_row(g, r++, "Sensibilidad de «Hey Sokari»", gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1));
    gtk_range_set_value(GTK_RANGE(f->sensitivity), cfg->wake_sensitivity);
    gtk_grid_attach(GTK_GRID(g),
                    help_label("Más alta: te escucha aunque lo digas bajito o lejos, pero puede activarse solo con "
                               "ruido."),
                    1, r++, 1, 1);
    f->end_silence = add_row(g, r++, "Cuánto espero cuando te callas", choice(END_NAMES, 3, cfg->end_silence));
    f->duck = add_wide(g, r++, check("Bajar el volumen de la PC mientras te escucho", cfg->duck));
    return g;
}

/* ------------------------------------------------------------ General --- */

/* El sonido que suena al activarse: uno tuyo (activacion.mp3/.wav/.ogg en
   la carpeta de sonidos) o el de Sokari. */
static char *sound_text(void)
{
    for (int i = 0; i < 3; i++) {
        char *name = str_printf("activacion.%s", SOUND_EXTS[i]);
        wchar_t *wn = utf8_to_wide(name), *p = path_join(g_paths.sounds_dir, wn);
        bool have = file_exists(p);
        free(wn);
        free(p);
        if (have) {
            char *t = str_printf("Uno tuyo (%s).", name);
            free(name);
            return t;
        }
        free(name);
    }
    return xstrdup("El de Sokari.");
}

static void refresh_sound(Form *f)
{
    char *t = sound_text();
    gtk_label_set_text(GTK_LABEL(f->sound), t);
    free(t);
}

static void remove_sounds(const char *except)
{
    for (int i = 0; i < 3; i++) {
        if (except && !strcmp(except, SOUND_EXTS[i])) continue;
        char *name = str_printf("activacion.%s", SOUND_EXTS[i]);
        wchar_t *wn = utf8_to_wide(name), *p = path_join(g_paths.sounds_dir, wn);
        DeleteFileW(p);
        free(wn);
        free(p);
        free(name);
    }
}

static void on_pick_sound(GtkButton *b, gpointer u)
{
    Form *f = u;
    GtkFileChooserNative *fc = gtk_file_chooser_native_new("Elige el sonido de activación", GTK_WINDOW(f->dialog),
                                                           GTK_FILE_CHOOSER_ACTION_OPEN, "Usar", "Cancelar");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Audio (mp3, wav, ogg)");
    for (int i = 0; i < 3; i++) {
        char *pat = str_printf("*.%s", SOUND_EXTS[i]);
        gtk_file_filter_add_pattern(filter, pat);
        free(pat);
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), filter);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
        char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
        const char *dot = file ? strrchr(file, '.') : NULL;
        char *ext = str_lower(dot ? dot + 1 : "");
        int k = -1;
        for (int i = 0; i < 3; i++)
            if (!strcmp(ext, SOUND_EXTS[i])) k = i;
        bool ok = false;
        if (k >= 0) {
            ensure_dir(g_paths.sounds_dir);
            char *dir = wide_to_utf8(g_paths.sounds_dir), *dst_path = str_printf("%s/activacion.%s", dir, ext);
            GFile *src = g_file_new_for_path(file), *dst = g_file_new_for_path(dst_path);
            ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
            g_object_unref(src);
            g_object_unref(dst);
            free(dir);
            free(dst_path);
            if (ok) remove_sounds(ext);
        }
        set_status(f, ok ? "Listo, Sokari va a usar ese sonido al activarse."
                         : k < 0 ? "Ese archivo no es mp3, wav ni ogg." : "No pude copiar ese archivo.");
        free(ext);
        g_free(file);
        refresh_sound(f);
    }
    g_object_unref(fc);
}

static void on_clear_sound(GtkButton *b, gpointer u)
{
    Form *f = u;
    remove_sounds(NULL);
    refresh_sound(f);
    set_status(f, "Listo: vuelve a sonar el de Sokari.");
}

static void on_open_folder(GtkButton *b, gpointer u)
{
    ensure_dir(g_paths.local_dir);
    open_path(g_paths.local_dir);
}

static void on_check_update(GtkButton *b, gpointer u)
{
    run_job(u, JOB_UPDATE, NULL, NULL, "Buscando actualizaciones…");
}

static GtkWidget *page_general(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    f->autostart = add_wide(g, r++, check("Abrir Sokari al iniciar tu sesión", autostart_is_enabled()));
    add_wide(g, r++, help_label("Atajo: Ctrl+Alt+J para hablarle sin decir «Hey Sokari»."));
    f->full = add_wide(g, r++, check("Acceso completo (menos borrar): no te pregunta nada", cfg->full_access));
    add_wide(g, r++,
             help_label("Prendido, hace todo sin preguntarte: mover archivos, mandar mensajes, subir archivos, guardar "
                        "datos. Solo pide un «sí» antes de borrar. Riesgo: si lee una página con instrucciones "
                        "escondidas, podría obedecerlas sin avisarte. Apagado, pregunta antes de acciones delicadas "
                        "cuando leyó algo de afuera."));
    f->sound = add_row(g, r++, "Sonido de activación", text_label(""));
    refresh_sound(f);
    GtkWidget *row = button_row();
    add_button(row, "Elegir archivo…", G_CALLBACK(on_pick_sound), f);
    add_button(row, "Usar el de Sokari", G_CALLBACK(on_clear_sound), f);
    gtk_grid_attach(GTK_GRID(g), row, 1, r++, 1, 1);
    row = button_row();
    add_button(row, "Abrir carpeta de Sokari", G_CALLBACK(on_open_folder), f);
    f->update = add_button(row, "Buscar actualizaciones", G_CALLBACK(on_check_update), f);
    gtk_widget_set_margin_top(row, 8);
    add_wide(g, r++, row);
    return g;
}

/* ------------------------------------------------------------- Skills --- */

static char *skills_text(void)
{
    int n;
    char *names = skills_user_summary(&n);
    char *t = n ? str_printf("Tus skills (%d): %s. Cada una es un archivo de texto en la carpeta de skills.", n, names)
                : xstrdup("Todavía no tienes skills tuyas. Crea una con «Nueva skill» o diciéndole «crea una rutina "
                          "que abra Spotify cuando diga modo estudio».");
    free(names);
    return t;
}

static void on_open_skills(GtkButton *b, gpointer u)
{
    wchar_t *dir = skills_user_dir();
    ensure_dir(dir);
    open_path(dir);
    free(dir);
}

static void on_new_skill(GtkButton *b, gpointer u)
{
    Form *f = u;
    wchar_t *path = skills_new_template();
    if (path) open_path(path);
    else set_status(f, "No pude crear la skill nueva.");
    free(path);
    char *t = skills_text();
    gtk_label_set_text(GTK_LABEL(f->mine), t);
    free(t);
}

static GtkWidget *page_skills(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    add_wide(g, r++,
             text_label("Contestan en tu PC, sin gastar nada de IA (0 tokens). Lo que no entienden se lo pasan a la "
                        "IA."));
    int n = skills_count() < 16 ? skills_count() : 16, half = (n + 1) / 2;
    for (int i = 0; i < n; i++) {
        const SkillInfo *info = skills_get(i);
        f->skills[i] = check(info->name, config_skill_enabled(info->id));
        gtk_widget_set_tooltip_text(f->skills[i], info->example);
        gtk_grid_attach(GTK_GRID(g), f->skills[i], i / half, r + i % half, 1, 1);
    }
    r += half;
    f->city = add_row(g, r++, "Tu ciudad (para el clima)", text_entry(cfg->city, "Chihuahua"));
    char *st = skills_text();
    f->mine = add_wide(g, r++, text_label(st));
    free(st);
    gtk_widget_set_margin_top(f->mine, 8);
    GtkWidget *row = button_row();
    add_button(row, "Abrir carpeta de skills", G_CALLBACK(on_open_skills), f);
    add_button(row, "Nueva skill", G_CALLBACK(on_new_skill), f);
    add_wide(g, r++, row);
    return g;
}

/* ------------------------------------------------------------ Tus PCs --- */

static void on_mesh_job(GtkButton *b, gpointer u)
{
    static const char *const BUSY[] = {"Buscando tus PCs en Tailscale…",
                                       "Revisando la malla paso a paso (tarda unos segundos)…",
                                       "Tu escritorio te va a pedir tu contraseña para el firewall…"};
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "kind"));
    run_job(u, kind, NULL, NULL, BUSY[kind]);
}

static void on_probe(GtkButton *b, gpointer u)
{
    const char *name = g_object_get_data(G_OBJECT(b), "name"), *host = g_object_get_data(G_OBJECT(b), "host");
    char *busy = str_printf("Probando %s…", name);
    run_job(u, JOB_PROBE, name, host, busy);
    free(busy);
}

static void on_remove(GtkButton *b, gpointer u)
{
    Form *f = u;
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    char *msg = str_printf("Quité %s de tus PCs.", name);
    mesh_device_remove(name);
    set_status(f, msg);
    free(msg);
    fill_pcs(f);
}

static void on_copy_secret(GtkButton *b, gpointer u)
{
    Form *f = u;
    char *secret = config_mesh_secret(true);
    gtk_entry_set_text(GTK_ENTRY(f->secret), secret);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), secret, -1);
    SecureZeroMemory(secret, strlen(secret));
    free(secret);
    set_status(f, "Secreto copiado. Pégalo en este mismo campo en tu otra PC. No lo compartas con nadie.");
}

static void on_install_tailscale(GtkButton *b, gpointer u)
{
    open_uri("https://tailscale.com/download/linux");
    set_status(u, "Te abrí la página de Tailscale para Linux: instálalo y entra con la misma cuenta en cada PC.");
}

/* Si esta PC recibe órdenes y la lista de tus PCs, cada una con Probar y Quitar. */
static void fill_pcs(Form *f)
{
    char *ip = mesh_listening_ip();
    char *s = ip ? str_printf("✓ Esta PC recibe órdenes de tus otras PCs (en %s).", ip)
                 : xstrdup("Esta PC todavía no recibe órdenes: se activa sola cuando Tailscale se conecta.");
    gtk_label_set_text(GTK_LABEL(f->listening), s);
    free(s);
    free(ip);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(f->pcs));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(GTK_WIDGET(k->data));
    g_list_free(kids);
    MeshDevice *d;
    int n = mesh_devices(&d);
    if (!n)
        gtk_box_pack_start(GTK_BOX(f->pcs),
                           help_label("Todavía no tienes ninguna. Con Tailscale conectado en las dos PCs dale a "
                                      "«Detectar mis PCs», o dile a Sokari: «registra mi laptop en 100.x.y.z»."),
                           FALSE, FALSE, 0);
    for (int i = 0; i < n && i < MAX_PCS; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        char *t = str_printf("%s  ·  %s", d[i].name, d[i].host);
        GtkWidget *l = gtk_label_new(t);
        free(t);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
        const char *labels[] = {"Probar", "Quitar"};
        GCallback cbs[] = {G_CALLBACK(on_probe), G_CALLBACK(on_remove)};
        for (int k = 0; k < 2; k++) {
            GtkWidget *b = gtk_button_new_with_label(labels[k]);
            g_object_set_data_full(G_OBJECT(b), "name", g_strdup(d[i].name), g_free);
            g_object_set_data_full(G_OBJECT(b), "host", g_strdup(d[i].host), g_free);
            g_signal_connect(b, "clicked", cbs[k], f);
            gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(f->pcs), row, FALSE, FALSE, 0);
    }
    mesh_devices_free(d, n);
    gtk_widget_show_all(f->pcs);
}

static GtkWidget *page_devices(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    add_wide(g, r++,
             text_label("Tailscale conecta tus PCs para mandarles órdenes («dile a mi laptop que…»). Instálalo en cada "
                        "PC y entra con la misma cuenta."));
    f->listening = add_wide(g, r++, text_label(""));
    f->mesh_buttons = button_row();
    add_button(f->mesh_buttons, "Instalar Tailscale", G_CALLBACK(on_install_tailscale), f);
    GtkWidget *fw = add_button(f->mesh_buttons, "Permitir en el firewall", G_CALLBACK(on_mesh_job), f);
    g_object_set_data(G_OBJECT(fw), "kind", GINT_TO_POINTER(JOB_FIREWALL));
    GtkWidget *diag = add_button(f->mesh_buttons, "Revisar la malla", G_CALLBACK(on_mesh_job), f);
    g_object_set_data(G_OBJECT(diag), "kind", GINT_TO_POINTER(JOB_DIAGNOSE));
    GtkWidget *detect = add_button(f->mesh_buttons, "Detectar mis PCs", G_CALLBACK(on_mesh_job), f);
    g_object_set_data(G_OBJECT(detect), "kind", GINT_TO_POINTER(JOB_DETECT));
    add_wide(g, r++, f->mesh_buttons);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    f->secret = secret_entry(cfg->mesh_secret, NULL);
    gtk_box_pack_start(GTK_BOX(row), f->secret, TRUE, TRUE, 0);
    add_button(row, "Copiar secreto", G_CALLBACK(on_copy_secret), f);
    add_row(g, r++, "Secreto", row);
    gtk_grid_attach(GTK_GRID(g), help_label("Solo si tus PCs usan cuentas distintas de Tailscale."), 1, r++, 1, 1);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Tus PCs</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_set_margin_top(title, 6);
    add_wide(g, r++, title);
    f->pcs = add_wide(g, r++, gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    add_wide(g, r++, help_label("Para mandarle algo, dile a Sokari: «dile a mi laptop que abra Spotify»."));
    f->mesh_result = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(f->mesh_result), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(f->mesh_result), TRUE);
    gtk_label_set_xalign(GTK_LABEL(f->mesh_result), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(f->mesh_result), 70);
    add_wide(g, r++, f->mesh_result);
    fill_pcs(f);
    return g;
}

/* ------------------------------------------------------ IA de respaldo --- */

static GtkWidget *page_ai(Form *f, const AppConfig *cfg)
{
    GtkWidget *g = page_grid();
    int r = 0;
    add_wide(g, r++,
             text_label("Cuando se te acaba el cupo gratis de Groq, Sokari sigue con estas, en orden. Todas son "
                        "opcionales: pega solo las que tengas. Tu voz se sigue pasando a texto con Groq."));
    const char *keys[4] = {cfg->nvidia_key, cfg->deepseek_key, cfg->openrouter_key, cfg->glm_key};
    for (int i = 0; i < 4; i++) {
        GtkWidget *l = gtk_label_new(BACKUP_NAMES[i]);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_margin_top(l, 4);
        add_wide(g, r++, l);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        f->backup[i] = secret_entry(keys[i], NULL);
        gtk_box_pack_start(GTK_BOX(row), f->backup[i], TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_link_button_new_with_label(BACKUP_URLS[i], "Sacar key →"), FALSE, FALSE, 0);
        add_wide(g, r++, row);
    }
    f->order = add_row(g, r++, "Orden (de izquierda a derecha)", text_entry(cfg->ai_order, DEFAULT_AI_ORDER));
    gtk_widget_set_margin_top(f->order, 6);
    add_wide(g, r++,
             help_label("Ojo: en OpenRouter, muchos modelos gratis solo funcionan si permites que usen tus mensajes "
                        "para entrenar; DeepSeek guarda los datos en China. Sokari les manda tus frases y lo que "
                        "recuerda de ti."));
    return g;
}

/* ------------------------------------------------------------ Guardar --- */

static void show_page(Form *f, SettingsPage page)
{
    gtk_stack_set_visible_child_name(GTK_STACK(f->stack), PAGE_IDS[page]);
}

static void set_str(char **field, char *value)
{
    free(*field);
    *field = value;
}

/* Lo guarda todo; false (y no se cierra) si falta la API key o el secreto no sirve. */
static bool save(Form *f)
{
    char *key = entry_text(f->key);
    if (!*key) {
        free(key);
        show_page(f, SET_ACCOUNT);
        gtk_widget_grab_focus(f->key);
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(f->dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                              "Me falta tu API key de Groq.");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(m), "Sácala gratis en console.groq.com/keys y pégala aquí (empieza con gsk_).");
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        return false;
    }
    AppConfig cfg = config_snapshot();
    char *secret = entry_text(f->secret);
    bool secret_changed = *secret && strcmp(secret, cfg.mesh_secret);
    const char *bad = secret_changed ? config_secret_problem(secret) : NULL;
    if (bad) {
        /* Una IP pegada donde va el secreto: no se guarda nada y se dice por qué. */
        SecureZeroMemory(key, strlen(key));
        free(key);
        free(secret);
        SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
        config_free(&cfg);
        show_page(f, SET_DEVICES);
        gtk_widget_grab_focus(f->secret);
        set_status(f, bad);
        return false;
    }
    if (*secret) {
        set_str(&cfg.mesh_secret, secret);
        if (secret_changed) log_msg("Secreto de malla cambiado desde Configuración.");
    } else {
        free(secret);
    }
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    set_str(&cfg.groq_api_key, key);
    set_str(&cfg.user_name, entry_text(f->name));
    set_str(&cfg.stop_word, entry_text(f->stop));
    char **backup[4] = {&cfg.nvidia_key, &cfg.deepseek_key, &cfg.openrouter_key, &cfg.glm_key};
    for (int k = 0; k < 4; k++) {
        SecureZeroMemory(*backup[k], strlen(*backup[k]));
        set_str(backup[k], entry_text(f->backup[k]));
    }
    set_str(&cfg.ai_order, config_clean_ai_order(gtk_entry_get_text(GTK_ENTRY(f->order))));
    int style = gtk_combo_box_get_active(GTK_COMBO_BOX(f->style));
    cfg.sphere_style = style >= 0 && style < SPHERE_STYLE_COUNT ? style : 0;
    int level = gtk_combo_box_get_active(GTK_COMBO_BOX(f->face_level));
    cfg.face_level = level >= 0 && level <= 2 ? level : 1;
    cfg.face_symbols = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->face_symbols));
    int anim = gtk_combo_box_get_active(GTK_COMBO_BOX(f->anim));
    cfg.appear_anim = anim >= 0 && anim < SPHERE_ANIM_COUNT ? anim : 0;
    cfg.subtitles = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->subtitles));
    cfg.volume = (int)gtk_range_get_value(GTK_RANGE(f->volume));
    set_str(&cfg.voice, xstrdup(chosen_voice(f)));
    const char *mic = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->mic));
    set_str(&cfg.mic_name, xstrdup(mic ? mic : ""));
    const char *out = gtk_combo_box_get_active_id(GTK_COMBO_BOX(f->out));
    set_str(&cfg.output_name, xstrdup(out ? out : ""));
    cfg.wake_sensitivity = (int)gtk_range_get_value(GTK_RANGE(f->sensitivity));
    int end = gtk_combo_box_get_active(GTK_COMBO_BOX(f->end_silence));
    cfg.end_silence = end >= 0 && end < 3 ? end : 1;
    cfg.duck = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->duck));
    cfg.full_access = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->full));
    cfg.autostart = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->autostart));
    set_str(&cfg.city, entry_text(f->city));
    StrBuf off;
    sb_init(&off);
    for (int i = 0; i < skills_count() && i < 16; i++)
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->skills[i])))
            sb_appendf(&off, "%s%s", off.len ? "," : "", skills_get(i)->id);
    set_str(&cfg.skills_off, off.data ? sb_steal(&off) : xstrdup(""));
    sb_free(&off);
    config_apply(&cfg);
    bool saved = config_save();
    autostart_set(cfg.autostart);
    speaker_set_device(cfg.output_name);
    const char *pw = gtk_entry_get_text(GTK_ENTRY(f->profile_pw));
    if (*pw) set_profile_password(cfg.user_name, pw);
    gtk_entry_set_text(GTK_ENTRY(f->profile_pw), "");
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    for (int k = 0; k < 4; k++) SecureZeroMemory(*backup[k], strlen(*backup[k]));
    config_free(&cfg);
    if (!saved) log_msg("Configuración: no pude guardarla.");
    if (secret_changed)
        app_notify("Sokari", "El secreto nuevo tiene que ser el mismo en tus otras PCs (si usan la misma cuenta de "
                             "Tailscale, ni hace falta).");
    ui_linux_settings_saved(f->first_run);
    return true;
}

static void on_response(GtkDialog *d, int response, gpointer u)
{
    Form *f = u;
    if (response == GTK_RESPONSE_ACCEPT) {
        if (!save(f)) return;
    } else {
        ui_linux_settings_cancelled(f->first_run);
    }
    g_form = NULL;
    f->closed = true;
    gtk_widget_destroy(f->dialog);
    if (!f->busy) form_free(f);
}

void settings_linux_open(GtkWindow *parent, bool first_run, SettingsPage page)
{
    if (page < 0 || page >= SET_PAGES) page = SET_ACCOUNT;
    if (g_form) {
        show_page(g_form, page);
        gtk_window_present(GTK_WINDOW(g_form->dialog));
        return;
    }
    Form *f = xcalloc(1, sizeof *f);
    f->first_run = first_run;
    g_form = f;
    f->dialog = gtk_dialog_new_with_buttons("Configuración de Sokari", parent,
                                            GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR, "Cancelar",
                                            GTK_RESPONSE_CANCEL, first_run ? "Empezar" : "Guardar", GTK_RESPONSE_ACCEPT,
                                            NULL);
    gtk_window_set_default_size(GTK_WINDOW(f->dialog), 820, 620);
    gtk_dialog_set_default_response(GTK_DIALOG(f->dialog), GTK_RESPONSE_ACCEPT);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(f->dialog));
    gtk_box_set_spacing(GTK_BOX(content), 0);
    if (first_run) {
        GtkWidget *hi = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hi), "<b>Para empezar, pega tu API key de Groq.</b> Es gratis: sácala en "
                                            "<a href=\"https://console.groq.com/keys\">console.groq.com/keys</a>. "
                                            "Todo lo demás lo puedes cambiar después.");
        gtk_label_set_line_wrap(GTK_LABEL(hi), TRUE);
        gtk_label_set_xalign(GTK_LABEL(hi), 0);
        gtk_container_set_border_width(GTK_CONTAINER(content), 0);
        gtk_widget_set_margin_start(hi, 18);
        gtk_widget_set_margin_end(hi, 18);
        gtk_widget_set_margin_top(hi, 12);
        gtk_widget_set_margin_bottom(hi, 6);
        gtk_box_pack_start(GTK_BOX(content), hi, FALSE, FALSE, 0);
    }
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    f->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(f->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    GtkWidget *side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(f->stack));
    gtk_widget_set_size_request(side, 180, -1);
    gtk_box_pack_start(GTK_BOX(body), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), f->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), body, TRUE, TRUE, 0);
    f->status = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(f->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(f->status), 0);
    gtk_label_set_selectable(GTK_LABEL(f->status), TRUE);
    gtk_widget_set_margin_start(f->status, 18);
    gtk_widget_set_margin_end(f->status, 18);
    gtk_widget_set_margin_top(f->status, 6);
    gtk_widget_set_margin_bottom(f->status, 6);
    gtk_box_pack_start(GTK_BOX(content), f->status, FALSE, FALSE, 0);

    AppConfig cfg = config_snapshot();
    GtkWidget *(*const build[SET_PAGES])(Form *, const AppConfig *) = {
        page_account, page_display, page_audio, page_general, page_skills, page_devices, page_ai};
    for (int i = 0; i < SET_PAGES; i++) {
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), build[i](f, &cfg));
        gtk_stack_add_titled(GTK_STACK(f->stack), scroll, PAGE_IDS[i], PAGE_TITLES[i]);
    }
    SecureZeroMemory(cfg.groq_api_key, strlen(cfg.groq_api_key));
    config_free(&cfg);
    g_signal_connect(f->dialog, "response", G_CALLBACK(on_response), f);
    gtk_widget_show_all(f->dialog);
    show_page(f, page);
}

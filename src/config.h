#ifndef SOKARI_CONFIG_H
#define SOKARI_CONFIG_H

#include <stdbool.h>
#include <wchar.h>

#define SOKARI_VERSION "2.7.3"
#define SOKARI_VERSION_W L"2.7.3"
#define GITHUB_REPO "podselxd/sokari"

typedef enum {
    DISPLAY_FULLSCREEN = 0,
    DISPLAY_FULLSCREEN_BORDERLESS = 1,
    DISPLAY_WINDOWED_BORDERLESS = 2, /* la esfera flotante */
    DISPLAY_WINDOWED = 3,
    DISPLAY_MINIMIZED = 4, /* como DISPLAY_WINDOWED, pero arranca minimizada */
    DISPLAY_MODE_COUNT
} DisplayMode;

typedef struct {
    char *groq_api_key;
    char *user_name;
    char *stop_word;
    char *mesh_secret;
    char *voice;
    char *mic_name;
    char *output_name; /* salida de audio; "" = la predeterminada de Windows */
    char *fw_asked;    /* el exe para el que ya se pidió abrir el firewall de la malla */
    /* IA de respaldo (opcionales) y en qué orden se usan: "groq,nvidia,deepseek,openrouter,glm". */
    char *nvidia_key, *deepseek_key, *openrouter_key, *glm_key;
    char *ai_order;
    char *skills_off; /* skills locales apagadas: "clima,chistes" */
    char *city;       /* tu ciudad, para el clima */
    int display_mode;
    int resolution; /* 0 = automática, si no el alto en píxeles (720, 1080, 1440, 2160) */
    int volume;     /* 0-100, volumen de la voz de Sokari (no el del sistema) */
    int wake_sensitivity; /* 0-100 */
    int sphere_style;     /* SphereStyle: 0 halo de puntos, 1 líneas (beta), 2-4 las caras (beta); de fábrica, 2 */
    int appear_anim;      /* al aparecer y desaparecer (SphereAnim): 0 materializarse, 1 deslizarse, 2 zoom, 3 nada */
    int end_silence;      /* cuánto esperar callado para terminar tu orden: 0 corta, 1 normal, 2 larga */
    int orb_x, orb_y;     /* posición de la ventana flotante; -1 = centrada */
    int win_x, win_y, win_w, win_h; /* modo Ventana; win_w <= 0 = tamaño y lugar por defecto */
    bool subtitles;
    bool autostart;
    bool mic_muted;
    bool show_only_talking; /* la esfera se esconde cuando no le hablas */
    bool full_access;       /* acceso completo: no pide permiso para nada, salvo antes de borrar */
    bool mexa;              /* habla como mexicano ("háblame como mexa"); entenderlo, siempre */
    bool duck;              /* baja el volumen de la PC mientras te escucha */
    bool barge_in;          /* hablarle encima la calla (sin decir «Hey Sokari») */
    bool desk_move;         /* ¿Puede Sokari moverse por el escritorio? (esfera flotante) */
    int desk_after;         /* empieza después de tantos segundos sin hablarle: 2, 5, 10 o 30 */
    bool screen_view;       /* Puede ver mi pantalla (capturas solo cuando se lo pides) */
    bool face_symbols;      /* la cara con símbolos: lágrima, destellos, gota de sudor, «?», enojo, «…» */
} AppConfig;

typedef struct {
    wchar_t *local_dir;  /* %LOCALAPPDATA%\Sokari: config, log, dispositivos */
    wchar_t *memory_dir; /* <OneDrive o perfil>\Desktop\Sokari: memoria, datos, perfiles */
    /* Las de la versión anterior, que se quedaron como respaldo con tu API key
       y tu memoria adentro: solo se usan para protegerlas también. */
    wchar_t *legacy_local_dir;
    wchar_t *legacy_memory_dir;
    wchar_t *config_file;
    wchar_t *log_file;
    wchar_t *sounds_dir;
    wchar_t *update_dir;
} AppPaths;

extern AppPaths g_paths;

void paths_init(void);

void config_load(void);
bool config_save(void);
void config_migrate_legacy(void);

AppConfig config_snapshot(void);
void config_free(AppConfig *c);
void config_apply(const AppConfig *c);

char *config_api_key(void);
/* La key de una IA de respaldo ("nvidia", "deepseek", "openrouter", "glm"); "" si no hay. */
char *config_provider_key(const char *provider);
char *config_ai_order(void);
#define DEFAULT_AI_ORDER "groq,nvidia,deepseek,openrouter,glm"
char *config_stop_word(void);
/* ¿Está prendida esta skill local ("clima", "hora"…)? Todas vienen prendidas. */
bool config_skill_enabled(const char *id);
/* Tu ciudad para el clima ("" si no la has dicho) y guardarla. */
char *config_city(void);
void config_set_city(const char *city);
char *config_user_name(void);
char *config_mesh_secret(bool create);
/* Por qué ese texto no sirve como secreto de malla (una IP, el nombre de una
   PC, muy corto), en palabras para el usuario; NULL si sirve. */
const char *config_secret_problem(const char *s);
char *config_fw_asked(void);
void config_set_fw_asked(const char *exe);
int config_volume(void);
float config_wake_threshold(void);
bool config_mic_muted(void);
bool config_show_only_talking(void);
bool config_full_access(void);
bool config_mexa(void);
int config_end_silence(void);
bool config_duck(void);
bool config_barge_in(void);
bool config_desk_move(void);
int config_desk_after(void);
bool config_screen_view(void);
/* Los segundos que se ofrecen en «Empieza después de…». */
extern const int DESK_AFTER_CHOICES[4];
void config_set_mexa(bool on);
void config_set_full_access(bool on);
void config_set_mic_muted(bool muted);
void config_set_orb_pos(int x, int y);
void config_set_window_rect(int x, int y, int w, int h);
void config_set_display_mode(int mode);
void config_set_output(const char *name);

const char *display_mode_key(int mode);
/* El orden de las IA como lo escribiste ("NVIDIA, groq; glm"): solo las que
   conoce, sin repetir, en minúsculas; vacío o nada conocido es el de fábrica (heap). */
char *config_clean_ai_order(const char *text);
/* "puntos", "lineas", "cara_ojos", "cara_boca" o "cara_puntos" (SphereStyle); lo que no entiende es 0. */
const char *sphere_style_key(int style);
int sphere_style_from_key(const char *key);
/* ¿La esfera es una de las caras? Entonces a la IA se le pide la etiqueta de afecto. */
bool config_face(void);
/* "materializar", "deslizar", "zoom" o "ninguna"; lo que no entiende es 0 (materializarse). */
const char *appear_anim_key(int anim);
int appear_anim_from_key(const char *key);

#endif

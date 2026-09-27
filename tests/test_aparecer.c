/* Entrar y salir de la pantalla: las tres animaciones de la esfera
   (Materializarse, Deslizarse, Zoom) y Ninguna, su reloj (entrar, salir,
   regresar a medio camino, Probar) y la opción en config.env. Sin ventanas:
   la esfera se dibuja en memoria. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "sphere.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

#define SIZE 320

static void draw(SphereRenderer *r, SphereStyle style, bool premul, uint32_t *px)
{
    sphere_render(r, 3.0, 0.45, 3.0, &SPHERE_IDLE, 0, 0, style, px, sphere_size(r), premul);
}

/* Cuánta luz hay y a qué distancia del centro, en promedio. */
typedef struct {
    double light, radius;
} Light;

static Light light_of(const uint32_t *px, int n)
{
    double sum = 0, dist = 0, c = n / 2.0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            uint32_t p = px[(size_t)y * n + x];
            double v = ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255);
            sum += v;
            dist += v * hypot(x + 0.5 - c, y + 0.5 - c);
        }
    return (Light){sum, sum > 0 ? dist / sum : 0};
}

static const char *const NAMES[SPHERE_ANIM_COUNT] = {"materializarse", "deslizarse", "zoom", "ninguna"};

static void test_dibujo(void)
{
    printf("-- cómo se dibuja --\n");
    SphereRenderer *fresh = sphere_create(SIZE), *r = sphere_create(SIZE);
    int n = sphere_size(r);
    size_t bytes = sizeof(uint32_t) * (size_t)n * n;
    uint32_t *base = malloc(bytes), *px = malloc(bytes);

    /* En 1 (quieta en pantalla) nada cambia: ni un píxel. */
    bool same = true, empty = true, none_ignores = true;
    for (int st = 0; st < 2; st++)
        for (int pm = 0; pm < 2; pm++) {
            draw(fresh, (SphereStyle)st, pm, base);
            for (int a = 0; a < SPHERE_ANIM_COUNT; a++) {
                sphere_set_presence(r, (SphereAnim)a, 1.0f);
                draw(r, (SphereStyle)st, pm, px);
                if (memcmp(px, base, bytes)) {
                    same = false;
                    printf("      distinta: %s, %s\n", NAMES[a], st ? "líneas" : "puntos");
                }
                sphere_set_presence(r, (SphereAnim)a, 0.0f);
                draw(r, (SphereStyle)st, pm, px);
                if (a == SPHERE_ANIM_NONE) {
                    none_ignores = none_ignores && !memcmp(px, base, bytes);
                    continue;
                }
                uint32_t want = pm ? 0u : 0xFF000000u;
                for (size_t i = 0; i < (size_t)n * n; i++)
                    if (px[i] != want) {
                        empty = false;
                        printf("      no quedó vacía: %s, %s\n", NAMES[a], st ? "líneas" : "puntos");
                        break;
                    }
            }
        }
    check(same, "en pantalla, las 4 opciones dibujan la esfera de siempre, píxel por píxel (puntos y líneas)");
    check(empty, "fuera de la pantalla no se dibuja nada (transparente en la flotante, negro en las demás)");
    check(none_ignores, "«Ninguna» no se anima: siempre la esfera completa");

    draw(fresh, SPHERE_STYLE_DOTS, false, base);
    Light full = light_of(base, n);

    sphere_set_presence(r, SPHERE_ANIM_MATERIALIZE, 0.5f);
    draw(r, SPHERE_STYLE_DOTS, false, px);
    Light half = light_of(px, n);
    printf("      materializarse a la mitad: luz %.0f%%, distancia %.0f%%\n", 100 * half.light / full.light,
           100 * half.radius / full.radius);
    check(half.radius > full.radius * 1.15 && half.light < full.light,
          "materializarse a la mitad: los puntos vienen de afuera (más lejos del centro) y todavía tenues");
    sphere_set_presence(r, SPHERE_ANIM_MATERIALIZE, 0.25f);
    draw(r, SPHERE_STYLE_DOTS, false, px);
    Light early = light_of(px, n);
    check(early.light < half.light && half.light < full.light, "materializarse: se va juntando la luz poco a poco");

    draw(fresh, SPHERE_STYLE_LINES, false, base);
    Light full_lines = light_of(base, n);
    sphere_set_presence(r, SPHERE_ANIM_MATERIALIZE, 0.5f);
    draw(r, SPHERE_STYLE_LINES, false, px);
    Light half_lines = light_of(px, n);
    check(half_lines.radius > full_lines.radius * 1.1,
          "materializarse con líneas (beta): los tramos también llegan de afuera");

    sphere_set_presence(r, SPHERE_ANIM_ZOOM, 0.1f);
    draw(r, SPHERE_STYLE_DOTS, false, px);
    Light tiny = light_of(px, n);
    printf("      zoom al empezar: distancia %.0f%%\n", 100 * tiny.radius / full.radius);
    check(tiny.radius < full.radius * 0.6, "zoom al empezar: la esfera es chiquita, en el centro");

    sphere_set_presence(r, SPHERE_ANIM_SLIDE, 0.2f);
    draw(r, SPHERE_STYLE_DOTS, false, px);
    Light slide = light_of(px, n);
    check(slide.light < full.light * 0.75 && fabs(slide.radius - full.radius) < full.radius * 0.05,
          "deslizarse: en su lienzo solo se atenúa (lo que se mueve es la ventana)");

    free(base);
    free(px);
    sphere_destroy(fresh);
    sphere_destroy(r);

    check(fabsf(sphere_slide_offset(0.0f) - 1.0f) < 1e-4f && fabsf(sphere_slide_offset(1.0f)) < 1e-4f,
          "deslizarse: empieza fuera de la pantalla (1) y termina en su lugar (0)");
    float lowest = 1;
    bool down = true;
    for (int i = 1; i <= 100; i++) {
        float o = sphere_slide_offset(i / 100.0f);
        /* Sube hasta el 58% (ahí está lo más alto del rebote) y luego se acomoda. */
        if (i <= 55 && o > sphere_slide_offset((i - 1) / 100.0f)) down = false;
        if (o < lowest) lowest = o;
    }
    check(down, "deslizarse: sube sin regresarse");
    check(lowest < -0.03f && lowest > -0.15f, "deslizarse: se pasa un poco de su lugar antes de acomodarse (el rebote)");
}

/* Corre el reloj de a 1/60 de segundo; cuenta cuántas veces dijo "ya se fue". */
static int run(SphereAppear *a, double seconds)
{
    int gone = 0;
    for (int i = 0; i < (int)lround(seconds * 60); i++) gone += sphere_appear_step(a, 1.0 / 60);
    return gone;
}

static void test_reloj(void)
{
    printf("-- el reloj --\n");
    SphereAppear a;
    sphere_appear_init(&a);
    check(a.presence == 1.0f && !sphere_appear_moving(&a), "al empezar: en pantalla y quieta");

    sphere_appear_enter(&a, SPHERE_ANIM_MATERIALIZE, true);
    check(a.presence == 0.0f && sphere_appear_moving(&a), "entrar desde fuera: empieza en 0");
    int gone = run(&a, 0.5);
    check(fabsf(a.presence - 0.5f) < 0.02f, "a medio segundo va a la mitad (entra en 1 segundo)");
    gone += run(&a, 0.55);
    check(a.presence == 1.0f && !sphere_appear_moving(&a) && !gone, "al segundo ya entró, sin «ya se fue»");

    check(!sphere_appear_leave(&a, SPHERE_ANIM_ZOOM), "salir con animación: la ventana no se esconde todavía");
    gone = run(&a, 0.4);
    check(!gone && fabsf(a.presence - 0.5f) < 0.03f, "a 0.4 segundos va a la mitad (sale en 0.8)");
    gone = run(&a, 0.5);
    check(gone == 1 && a.presence == 0.0f, "al terminar de salir avisa una sola vez: ahí se esconde la ventana");
    check(run(&a, 1.0) == 0, "y ya no vuelve a avisar");

    /* A medio camino regresa sin brincos. */
    sphere_appear_enter(&a, SPHERE_ANIM_MATERIALIZE, true);
    run(&a, 1.1);
    sphere_appear_leave(&a, SPHERE_ANIM_MATERIALIZE);
    run(&a, 0.4);
    float mid = a.presence;
    sphere_appear_enter(&a, SPHERE_ANIM_MATERIALIZE, false);
    check(a.presence == mid && a.dir == 1, "le hablas mientras se va: regresa desde donde iba, sin brincos");
    gone = run(&a, 0.6);
    check(!gone && a.presence == 1.0f, "y termina de entrar; nunca se escondió");

    sphere_appear_enter(&a, SPHERE_ANIM_NONE, true);
    check(a.presence == 1.0f && !sphere_appear_moving(&a), "«Ninguna»: aparece de golpe");
    check(sphere_appear_leave(&a, SPHERE_ANIM_NONE), "«Ninguna»: se esconde de golpe");

    /* Probar con la esfera en pantalla: sale, espera un momento y vuelve. */
    sphere_appear_init(&a);
    sphere_appear_test(&a, SPHERE_ANIM_SLIDE, true);
    float lowest = 1;
    gone = 0;
    for (int i = 0; i < 60 * 3; i++) {
        gone += sphere_appear_step(&a, 1.0 / 60);
        if (a.presence < lowest) lowest = a.presence;
    }
    check(lowest == 0.0f && a.presence == 1.0f && !gone && !sphere_appear_moving(&a),
          "Probar en pantalla: sale, regresa y la ventana nunca se esconde");

    /* Probar con la esfera oculta: se asoma, se queda un segundo y se va. */
    sphere_appear_init(&a);
    sphere_appear_leave(&a, SPHERE_ANIM_ZOOM);
    run(&a, 1.0);
    sphere_appear_test(&a, SPHERE_ANIM_ZOOM, false);
    gone = run(&a, 1.5);
    check(!gone && a.presence == 1.0f, "Probar con la esfera oculta: entra y se queda un momento");
    gone += run(&a, 1.5);
    check(gone == 1 && a.presence == 0.0f, "…y se va: avisa una vez para volver a esconder la ventana");

    sphere_appear_init(&a);
    sphere_appear_test(&a, SPHERE_ANIM_NONE, true);
    check(!sphere_appear_moving(&a) && a.presence == 1.0f, "Probar con «Ninguna»: no hace nada");
}

static void test_config(const wchar_t *dir)
{
    printf("-- la opción --\n");
    check(!strcmp(appear_anim_key(0), "materializar") && !strcmp(appear_anim_key(1), "deslizar") &&
              !strcmp(appear_anim_key(2), "zoom") && !strcmp(appear_anim_key(3), "ninguna"),
          "los nombres en config.env");
    check(appear_anim_from_key("zoom") == 2 && appear_anim_from_key("girar") == 0 && appear_anim_from_key(NULL) == 0,
          "un valor que no conoce es Materializarse");

    DeleteFileW(g_paths.config_file);
    config_load();
    AppConfig c = config_snapshot();
    check(c.appear_anim == SPHERE_ANIM_MATERIALIZE, "sin configurar (también al actualizar): Materializarse");
    config_free(&c);

    const char *text = "SOKARI_ANIMATION=deslizar\n";
    write_file_atomic(g_paths.config_file, text, strlen(text));
    config_load();
    c = config_snapshot();
    check(c.appear_anim == SPHERE_ANIM_SLIDE, "lee SOKARI_ANIMATION=deslizar");
    c.appear_anim = SPHERE_ANIM_NONE;
    config_apply(&c);
    config_free(&c);
    char *saved = read_file_all(g_paths.config_file, NULL);
    check(saved && strstr(saved, "SOKARI_ANIMATION=ninguna\n"), "la guarda como SOKARI_ANIMATION=ninguna");
    free(saved);
    config_load();
    c = config_snapshot();
    check(c.appear_anim == SPHERE_ANIM_NONE, "y la vuelve a leer igual");
    config_free(&c);
    (void)dir;
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    /* En una carpeta temporal: nunca se toca tu config.env. */
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_aparecer");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");

    test_dibujo();
    test_reloj();
    test_config(dir);

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

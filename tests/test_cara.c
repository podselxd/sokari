/* La cara (beta): el dibujo (sphere.c) y la lógica (face.c). El halo y las
   líneas no cambian ni un píxel; las caras se dibujan donde deben; los gestos
   salen de lo que pasa y la pose nunca se sale de sus límites. Sin ventanas:
   todo se dibuja en memoria. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "config.h"
#include "face.h"
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

static void draw(SphereRenderer *r, SphereStyle style, uint32_t *px)
{
    sphere_render(r, 3.0, 0.45, 3.0, &SPHERE_IDLE, 0, 0, style, px, SIZE, false);
}

/* Qué tan brillante es un cuadrito de 5x5 alrededor de (x, y). */
static double bright(const uint32_t *px, int cx, int cy)
{
    double s = 0;
    for (int y = cy - 2; y <= cy + 2; y++)
        for (int x = cx - 2; x <= cx + 2; x++) {
            uint32_t p = px[y * SIZE + x];
            s += ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255);
        }
    return s / (25 * 3);
}

/* Dónde queda (u, v) de la cara en el lienzo, con la cara quieta. */
static void spot(float u, float v, int *x, int *y)
{
    float R = 360.0f * (SIZE / 960.0f) * 0.80f;
    *x = (int)lroundf(SIZE / 2.0f + u * R);
    *y = (int)lroundf(SIZE / 2.0f + v * R);
}

static void test_dibujo(void)
{
    printf("-- el dibujo --\n");
    uint32_t *a = xmalloc(sizeof(uint32_t) * SIZE * SIZE), *b = xmalloc(sizeof(uint32_t) * SIZE * SIZE);
    SphereRenderer *r1 = sphere_create(SIZE), *r2 = sphere_create(SIZE);
    SphereFace f;
    sphere_face_neutral(&f);
    f.fx = 0.2f;
    f.scale = 1.2f;
    f.nsym = 1;
    f.sym[0] = (SphereSymbol){SPHERE_SYM_TEAR, -0.5f, 0.3f, 1, 1, 0};
    sphere_set_face(r2, &f);
    bool same = true;
    for (int st = SPHERE_STYLE_DOTS; st <= SPHERE_STYLE_LINES; st++) {
        draw(r1, (SphereStyle)st, a);
        draw(r2, (SphereStyle)st, b);
        same &= !memcmp(a, b, sizeof(uint32_t) * SIZE * SIZE);
    }
    check(same, "el halo de puntos y las líneas salen idénticos aunque haya una cara puesta");

    /* Solo ojos: luz en los ojos, apagados al parpadear. */
    sphere_face_neutral(&f);
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    draw(r1, SPHERE_STYLE_FACE_EYES, a); /* sin sphere_set_face: el halo solo */
    int lx, ly, rx, ry, cx, cy;
    spot(-0.32f, -0.04f, &lx, &ly);
    spot(0.32f, -0.04f, &rx, &ry);
    spot(0, -0.04f, &cx, &cy);
    char msg[200];
    snprintf(msg, sizeof msg, "solo ojos: los dos ojos brillan (%.0f y %.0f) y entre ellos no (%.0f)", bright(b, lx, ly),
             bright(b, rx, ry), bright(b, cx, cy));
    check(bright(b, lx, ly) > 180 && bright(b, rx, ry) > 180 && bright(b, cx, cy) < 120, msg);
    check(bright(a, lx, ly) < 120, "sin sphere_set_face, un estilo de cara se ve como el halo solo");
    f.blink[0] = f.blink[1] = 1;
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    check(bright(b, lx, ly - 12) < 120 && bright(b, rx, ry - 12) < 120, "al parpadear, los ojos se cierran");

    /* Ojos y boca: la boca brilla abajo; sonriendo, el centro de la boca baja. */
    sphere_face_neutral(&f);
    f.smile = 1;
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_MOUTH, b);
    int mx, my, mx2, my2;
    spot(0, 0.3f + 0.15f, &mx, &my);
    spot(0, 0.3f - 0.08f, &mx2, &my2);
    snprintf(msg, sizeof msg, "ojos y boca: sonriendo, la boca baja en medio (%.0f abajo, %.0f arriba)", bright(b, mx, my),
             bright(b, mx2, my2));
    check(bright(b, mx, my) > 150 && bright(b, mx2, my2) < 120, msg);

    /* De puntos: los puntos de los ojos se prenden. */
    sphere_face_neutral(&f);
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_DOTS, b);
    draw(r1, SPHERE_STYLE_FACE_DOTS, a);
    int dx, dy;
    spot(-0.36f, -0.2f, &dx, &dy);
    double on = 0, off = 0;
    for (int y = dy - 8; y <= dy + 8; y++)
        for (int x = dx - 8; x <= dx + 8; x++) {
            on += b[y * SIZE + x] & 255;
            off += a[y * SIZE + x] & 255;
        }
    snprintf(msg, sizeof msg, "de puntos: donde van los ojos hay mucha más luz (%.0f contra %.0f)", on, off);
    check(on > off * 2, msg);

    /* El color: con alegría el resplandor es amarillo. */
    sphere_face_neutral(&f);
    f.color[0] = 255, f.color[1] = 240, f.color[2] = 106;
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    uint32_t p = b[(ly + 26) * SIZE + lx];
    printf("      junto al ojo: R %u G %u B %u\n", (p >> 16) & 255, (p >> 8) & 255, p & 255);
    check(((p >> 16) & 255) > (p & 255) + 20 && ((p >> 8) & 255) > (p & 255) + 20,
          "con el color de la alegría, el resplandor de los ojos es amarillo");

    /* Un símbolo: la lágrima brilla en su lugar, y sin símbolos no está. */
    sphere_face_neutral(&f);
    f.nsym = 1;
    f.sym[0] = (SphereSymbol){SPHERE_SYM_TEAR, -0.55f, 0.4f, 1, 1, 0};
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    int tx, ty;
    spot(-0.55f, 0.4f, &tx, &ty);
    double with = bright(b, tx, ty);
    f.nsym = 0;
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    snprintf(msg, sizeof msg, "la lágrima se dibuja donde va (%.0f; sin ella %.0f)", with, bright(b, tx, ty));
    check(with > bright(b, tx, ty) + 60, msg);

    /* Se mueve con la pose: la cara corrida a la derecha. */
    sphere_face_neutral(&f);
    f.fx = 0.2f;
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_EYES, b);
    int sx, sy;
    spot(0.32f + 0.2f, -0.04f, &sx, &sy);
    check(bright(b, sx, sy) > 180 && bright(b, lx, ly) < 180, "la cara se mueve con la pose (fx)");

    /* Nada de NaN ni cosas raras con una pose loca. */
    sphere_face_neutral(&f);
    f.scale = 50, f.sx = -3, f.sy = 0, f.fx = 9, f.tilt = 99, f.eye_h = 0, f.blink[0] = 7, f.nsym = 99;
    for (int i = 0; i < SPHERE_MAX_SYMBOLS; i++) f.sym[i] = (SphereSymbol){(SphereSymbolKind)(i % 6), 9, -9, 50, 3, 0};
    sphere_set_face(r2, &f);
    draw(r2, SPHERE_STYLE_FACE_MOUTH, b);
    check(true, "una pose loca no truena el dibujo");

    sphere_destroy(r1);
    sphere_destroy(r2);
    free(a);
    free(b);
}

/* ------------------------------------------------------------------ face.c */

static AffectState pure(AffectKind k)
{
    AffectState a;
    memset(&a, 0, sizeof a);
    a.weights[k] = 1;
    a.dominant = k;
    a.secondary = k == AFF_NEUTRAL ? AFF_JOY : AFF_NEUTRAL;
    return a;
}

static FaceInput input(AffectKind k)
{
    FaceInput in;
    memset(&in, 0, sizeof in);
    in.affect = pure(k);
    in.level = FACE_LEVEL_NORMAL;
    in.symbols = true;
    return in;
}

static void run(Face *f, FaceInput *in, double seconds, SphereFace *out)
{
    for (double t = 0; t < seconds; t += 0.02) face_step(f, 0.02, in, out);
}

static void test_poses(void)
{
    printf("-- la forma sale de la mezcla de emociones --\n");
    SphereFace o;
    Face *f = face_create(1);
    FaceInput in = input(AFF_JOY);
    run(f, &in, 3, &o);
    check(o.happy > 0.9f && o.smile > 0.8f, "alegría pura: ojos «^ ^» y sonrisa");
    face_destroy(f);
    f = face_create(1);
    in = input(AFF_SADNESS);
    run(f, &in, 3, &o);
    check(o.lid_tilt < -0.9f && o.smile < -0.8f && o.scale < 0.9f, "tristeza pura: párpados caídos, boca triste y se encoge");
    face_destroy(f);
    f = face_create(1);
    in = input(AFF_ANGER);
    run(f, &in, 3, &o);
    check(o.lid_tilt > 1.0f && o.scale > 1.1f, "furia pura: párpados en V y se infla");
    face_destroy(f);

    /* mitad y mitad: en medio */
    f = face_create(1);
    in = input(AFF_NEUTRAL);
    in.affect.weights[AFF_NEUTRAL] = 0.5f;
    in.affect.weights[AFF_JOY] = 0.5f;
    run(f, &in, 3, &o);
    check(o.happy > 0.4f && o.happy < 0.6f, "mitad alegría, mitad neutral: a medias");
    face_destroy(f);

    /* «Qué tanto se le nota» */
    float tilt[FACE_LEVEL_COUNT];
    for (int l = 0; l < FACE_LEVEL_COUNT; l++) {
        f = face_create(1);
        in = input(AFF_SADNESS);
        in.level = (FaceLevel)l;
        run(f, &in, 3, &o);
        tilt[l] = o.lid_tilt;
        face_destroy(f);
    }
    char msg[160];
    snprintf(msg, sizeof msg, "poco, normal, mucho: los párpados de tristeza %.2f, %.2f, %.2f", tilt[0], tilt[1], tilt[2]);
    check(tilt[0] > tilt[1] && tilt[1] > tilt[2] && fabsf(tilt[0] + 0.5f) < 0.05f, msg);

    SphereParams p;
    AffectState joy = pure(AFF_JOY), neu = pure(AFF_NEUTRAL);
    face_sphere_colors(&joy, &SPHERE_IDLE, &p);
    check(p.high[0] > 240 && p.high[1] > 220 && p.high[2] < 130, "con alegría la esfera se pone amarilla");
    face_sphere_colors(&neu, &SPHERE_IDLE, &p);
    check(!memcmp(p.high, SPHERE_IDLE.high, sizeof p.high) && !memcmp(p.low, SPHERE_IDLE.low, sizeof p.low),
          "neutral: los colores de siempre");
}

static void test_gestos(void)
{
    printf("-- los gestos salen de lo que pasa --\n");
    SphereFace o;
    Face *f = face_create(3);
    FaceInput in = input(AFF_NEUTRAL);
    run(f, &in, 0.5, &o);
    in.cue = AFF_CUE_GREETING;
    in.cue_seq = 1;
    float lmax = 0, rmax = 0;
    for (double t = 0; t < 1.2; t += 0.02) {
        face_step(f, 0.02, &in, &o);
        lmax = fmaxf(lmax, o.blink[0]);
        if (t > 0.35 && t < 0.8) rmax = fmaxf(rmax, o.blink[1]);
    }
    check(lmax > 0.9f && rmax < 0.5f, "un saludo: guiña un ojo (el otro sigue abierto)");

    in.cue = AFF_CUE_FAIL;
    in.cue_seq = 2;
    float fxmin = 0, fxmax = 0;
    for (double t = 0; t < 1.2; t += 0.02) {
        face_step(f, 0.02, &in, &o);
        fxmin = fminf(fxmin, o.fx);
        fxmax = fmaxf(fxmax, o.fx);
    }
    check(fxmin < -0.1f && fxmax > 0.08f, "algo falló: niega (de un lado a otro)");

    in.cue = AFF_CUE_DELICATE;
    in.cue_seq = 3;
    bool q = false;
    for (double t = 0; t < 1.5; t += 0.02) {
        face_step(f, 0.02, &in, &o);
        for (int i = 0; i < o.nsym; i++) q |= o.sym[i].kind == SPHERE_SYM_QUESTION && o.sym[i].size > 0.5f;
    }
    check(q && face_gesture(f) == FACE_G_DOUBT, "pide un «sí»: ladea la cabeza con un «?»");

    /* cambiar a temor: tiembla */
    run(f, &in, 2.5, &o);
    in.affect = pure(AFF_FEAR);
    face_step(f, 0.02, &in, &o);
    check(face_gesture(f) == FACE_G_TREMBLE, "cuando entra el temor, tiembla");
    in.affect = pure(AFF_JOY);
    face_step(f, 0.02, &in, &o);
    float fymin = 0;
    for (double t = 0; t < 1; t += 0.02) {
        face_step(f, 0.02, &in, &o);
        fymin = fminf(fymin, o.fy);
    }
    check(face_gesture(f) == FACE_G_BOUNCE && fymin < -0.2f, "cuando entra la alegría, rebota");

    /* repetir el mismo aviso (mismo número) no repite el gesto */
    run(f, &in, 3, &o);
    face_step(f, 0.02, &in, &o);
    check(face_gesture(f) == FACE_G_NONE, "el mismo aviso otra vez no repite el gesto");
    face_destroy(f);

    /* sola: parpadea (a veces dos veces) y, callada, mira alrededor */
    f = face_create(5);
    in = input(AFF_NEUTRAL);
    int blinks = 0, looks = 0;
    bool closed = false;
    for (double t = 0; t < 70; t += 0.02) {
        face_step(f, 0.02, &in, &o);
        if (o.blink[0] > 0.9f && !closed) blinks++;
        closed = o.blink[0] > 0.9f;
        if (face_gesture(f) == FACE_G_LOOK) looks++;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "en 70 s sola: parpadeó %d veces y miró alrededor", blinks);
    check(blinks >= 10 && blinks <= 40 && looks > 0, msg);

    /* pensando: mira arriba y salen los puntitos */
    in.activity = FACE_THINKING;
    run(f, &in, 1.5, &o);
    bool dots = false;
    for (int i = 0; i < o.nsym; i++) dots |= o.sym[i].kind == SPHERE_SYM_DOTS;
    check(o.gaze_y < -0.08f && dots, "pensando: mira arriba y salen los «…»");
    in.symbols = false;
    face_step(f, 0.02, &in, &o);
    check(o.nsym == 0, "con los símbolos apagados no hay ninguno");
    /* hablando: la boca va con la voz */
    in.activity = FACE_SPEAKING;
    in.pulse = 0.9f;
    run(f, &in, 1, &o);
    check(o.talk > 0.5f, "hablando: la boca se abre con cada sílaba");
    face_destroy(f);
}

static unsigned g_seed = 99;
static float rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 16777216.0f;
}

static bool finite_pose(const SphereFace *o)
{
    const float *v = (const float *)o;
    size_t n = offsetof(SphereFace, nsym) / sizeof(float);
    for (size_t i = 0; i < n; i++)
        if (!isfinite(v[i])) return false;
    return o->nsym >= 0 && o->nsym <= SPHERE_MAX_SYMBOLS && o->scale >= 0.6f && o->scale <= 1.35f &&
           fabsf(o->fx) <= 0.5f && fabsf(o->fy) <= 0.5f && o->blink[0] >= 0 && o->blink[0] <= 1 &&
           o->happy >= 0 && o->happy <= 1;
}

static void test_limites(void)
{
    printf("-- la pose nunca se sale de sus límites --\n");
    Face *f = face_create(11);
    SphereFace o;
    FaceInput in = input(AFF_NEUTRAL);
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        if (rnd() < 0.05f) {
            float sum = 0;
            for (int k = 0; k < AFF_COUNT; k++) sum += in.affect.weights[k] = rnd();
            for (int k = 0; k < AFF_COUNT; k++) in.affect.weights[k] /= sum;
            in.affect.dominant = (AffectKind)(rnd() * AFF_COUNT);
        }
        if (rnd() < 0.02f) in.cue = (AffectCue)(rnd() * 9), in.cue_seq++;
        if (rnd() < 0.01f) in.activity = (FaceActivity)(rnd() * 4);
        if (rnd() < 0.01f) in.level = (FaceLevel)(rnd() * 3);
        in.voice = rnd();
        in.pulse = rnd();
        in.symbols = rnd() < 0.9f;
        if (rnd() < 0.001f) in.affect.weights[0] = NAN;
        double dt = rnd() < 0.99f ? rnd() * 0.05 : rnd() * 5;
        face_step(f, dt, &in, &o);
        if (!finite_pose(&o)) bad++;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "20 000 cuadros al azar (con NaN): 0 poses fuera de rango (hubo %d)", bad);
    check(bad == 0, msg);
    face_destroy(f);
}

static void test_config(const wchar_t *dir)
{
    printf("-- en config.env --\n");
    const char *text = "SOKARI_SPHERE_STYLE=cara_ojos\nSOKARI_FACE_LEVEL=mucho\nSOKARI_FACE_SYMBOLS=0\n";
    write_file_atomic(g_paths.config_file, text, strlen(text));
    config_load();
    AppConfig c = config_snapshot();
    check(c.sphere_style == SPHERE_STYLE_FACE_EYES && c.face_level == 2 && !c.face_symbols,
          "lee la cara, «mucho» y sin símbolos");
    c.face_level = 0;
    c.face_symbols = true;
    config_apply(&c);
    config_free(&c);
    char *saved = read_file_all(g_paths.config_file, NULL);
    check(saved && strstr(saved, "SOKARI_FACE_LEVEL=poco\n") && strstr(saved, "SOKARI_FACE_SYMBOLS=1\n") &&
              strstr(saved, "SOKARI_SPHERE_STYLE=cara_ojos\n"),
          "y lo guarda igual");
    free(saved);
    DeleteFileW(g_paths.config_file);
    config_load();
    c = config_snapshot();
    check(c.sphere_style == SPHERE_STYLE_DOTS && c.face_level == 1 && c.face_symbols,
          "de fábrica: el halo de puntos; con cara, «normal» y con símbolos");
    config_free(&c);
    (void)dir;
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_cara");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");

    test_dibujo();
    test_poses();
    test_gestos();
    test_limites();
    test_config(dir);

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

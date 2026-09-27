#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"
#include "util.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* La forma de la cara en cada emoción; la del cuadro es su mezcla con los pesos. */
typedef struct {
    float eye_w, eye_h, lid_top, lid_tilt, lid_bot, happy, asym, round, eye_dy, shade;
    float smile, mouth_open, mouth_w, wave, mouth_o, mouth_asym, glow;
} Shape;

#define SHAPE_N (int)(sizeof(Shape) / sizeof(float))

static const Shape POSE[AFF_COUNT] = {
    [AFF_NEUTRAL] = {.eye_w = 1, .eye_h = 1, .round = 4, .smile = 0.15f, .mouth_w = 1, .glow = 1},
    [AFF_JOY] = {.eye_w = 1.12f, .eye_h = 0.95f, .happy = 1, .round = 4, .eye_dy = -0.03f, .smile = 1,
                 .mouth_open = 0.3f, .mouth_w = 1.3f, .glow = 1.35f},
    [AFF_FEAR] = {.eye_w = 1.25f, .eye_h = 1.4f, .lid_tilt = -0.35f, .round = 2.3f, .eye_dy = -0.04f, .smile = -0.2f,
                  .mouth_w = 1, .wave = 0.3f, .mouth_o = 0.85f, .glow = 1.2f},
    [AFF_ANGER] = {.eye_w = 1.05f, .eye_h = 0.85f, .lid_top = 0.33f, .lid_tilt = 1.15f, .lid_bot = 0.1f, .round = 4,
                   .smile = -0.7f, .mouth_open = 0.12f, .mouth_w = 0.85f, .glow = 1.5f},
    [AFF_DISGUST] = {.eye_w = 1, .eye_h = 0.9f, .lid_top = 0.14f, .lid_bot = 0.24f, .asym = 0.45f, .round = 4,
                     .smile = -0.45f, .mouth_w = 0.9f, .wave = 0.6f, .mouth_asym = 0.9f, .glow = 0.9f},
    [AFF_SADNESS] = {.eye_w = 0.95f, .eye_h = 0.8f, .lid_top = 0.4f, .lid_tilt = -1, .round = 4, .eye_dy = 0.05f,
                     .smile = -0.95f, .mouth_w = 0.8f, .glow = 0.7f},
};

/* Los colores (inspirados en películas de emociones, solo el color): el de
   neutral es el de siempre y lo pone quien llama. */
static const float HIGH[AFF_COUNT][3] = {
    {0, 0, 0}, {255, 240, 106}, {217, 194, 255}, {255, 128, 56}, {168, 240, 92}, {108, 196, 255},
};
static const float LOW[AFF_COUNT][3] = {
    {0, 0, 0}, {242, 169, 0}, {122, 76, 217}, {214, 26, 26}, {37, 154, 58}, {29, 79, 216},
};

static const float LEVEL[FACE_LEVEL_COUNT] = {0.5f, 1.0f, 1.5f};

static const struct {
    const char *name;
    float dur;
} GESTURES[FACE_G_COUNT] = {
    [FACE_G_NONE] = {"ninguno", 0},        [FACE_G_NOD] = {"asiente", 1.0f},
    [FACE_G_SHAKE] = {"niega", 1.15f},     [FACE_G_BOUNCE] = {"rebota", 1.55f},
    [FACE_G_TREMBLE] = {"tiembla", 1.8f},  [FACE_G_SHRINK] = {"se encoge", 1.5f},
    [FACE_G_INFLATE] = {"se infla", 0.9f}, [FACE_G_RECOIL] = {"se aparta", 1.3f},
    [FACE_G_DOUBT] = {"duda", 2.0f},       [FACE_G_BLINK2] = {"parpadeo doble", 0.9f},
    [FACE_G_LOOK] = {"mira alrededor", 2.4f}, [FACE_G_WINK] = {"guiño", 1.1f},
};

typedef struct {
    FaceGesture g;
    double t0;
    float amp;
} Active;

struct Face {
    double t;
    unsigned rng;
    Active cur, prev; /* el gesto de ahora y el anterior, que se apaga en 0.2 s */
    double cur_until;
    bool started;
    unsigned cue_seq;
    AffectKind dominant;
    double next_blink, blink_at, blink2_at;
    double idle_since, next_look, next_saccade;
    float saccade_x, saccade_y, gaze_x, gaze_y;
    float act[4]; /* cuánto de cada actividad (se acercan suave) */
    double think_t0;
};

typedef struct {
    float fx, fy, tilt, scale, stretch; /* sy = 1 + stretch, sx = 1 - 0.9 stretch */
    float gx, gy;
    float blink[2];
    float eye_k; /* multiplica el tamaño de los ojos */
    float lid_top, asym, happy, mouth_open, mouth_asym, smile;
    float glow; /* multiplica */
    float follow;
    int nsym;
    SphereSymbol sym[3];
} GOut;

static float clampf(float x, float lo, float hi)
{
    if (isnan(x)) x = 0;
    return x < lo ? lo : x > hi ? hi : x;
}

static float smooth(float x)
{
    x = clampf(x, 0, 1);
    return x * x * (3 - 2 * x);
}

/* Cuadros clave: (tiempo, valor) con curvas suaves entre ellos. */
static float kf(float t, const float *keys, int n)
{
    if (t <= keys[0]) return keys[1];
    for (int i = 0; i + 1 < n; i++) {
        float t0 = keys[2 * i], v0 = keys[2 * i + 1], t1 = keys[2 * i + 2], v1 = keys[2 * i + 3];
        if (t <= t1) return v0 + (v1 - v0) * smooth((t - t0) / (t1 - t0));
    }
    return keys[2 * n - 1];
}
#define KF(t, ...) kf((t), (const float[]){__VA_ARGS__}, (int)(sizeof((const float[]){__VA_ARGS__}) / sizeof(float) / 2))

/* Se pasa y regresa: aparece un símbolo. */
static float pop(float t, float t0)
{
    if (t < t0) return 0;
    float x = clampf((t - t0) / 0.32f, 0, 1) - 1, s = 2.6f;
    return x * x * ((s + 1) * x + s) + 1;
}

static float blinkf(float t, float at, float close, float hold, float open)
{
    if (t < at || t > at + close + hold + open) return 0;
    if (t < at + close) return smooth((t - at) / close);
    if (t < at + close + hold) return 1;
    return 1 - smooth((t - at - close - hold) / open);
}

static float jitter(float t, float amp, float f1, float f2, float ph)
{
    return amp * (0.6f * sinf(2 * (float)M_PI * f1 * t + ph) + 0.4f * sinf(2 * (float)M_PI * f2 * t + 1.3f + ph));
}

static void add_sym(GOut *o, SphereSymbolKind k, float x, float y, float size, float alpha, float rot)
{
    if (o->nsym >= 3 || size <= 0.01f || alpha <= 0.01f) return;
    o->sym[o->nsym++] = (SphereSymbol){k, x, y, size, alpha, rot};
}

static void spark(GOut *o, float t, float t0, float x, float y)
{
    if (t < t0) return;
    add_sym(o, SPHERE_SYM_SPARK, x, y, pop(t, t0) * (0.75f + 0.25f * sinf(2 * (float)M_PI * 3 * (t - t0))),
            1 - smooth((t - t0 - 0.9f) / 0.3f), 0.6f * (t - t0));
}

/* Un gesto t segundos después de empezar (lo que le suma a la pose). */
static void gesture_eval(FaceGesture g, float t, GOut *o)
{
    memset(o, 0, sizeof *o);
    o->eye_k = 1;
    o->glow = 1;
    o->follow = 0.55f;
    switch (g) {
    case FACE_G_NOD: {
        o->fy = KF(t, 0, 0, 0.12f, -0.05f, 0.3f, 0.17f, 0.46f, -0.02f, 0.62f, 0.13f, 0.8f, -0.01f, 0.95f, 0);
        o->stretch = -0.5f * fmaxf(o->fy, 0);
        float b = 0.35f * smooth((o->fy - 0.08f) / 0.08f);
        o->blink[0] = o->blink[1] = b;
        o->happy = 0.4f * smooth(t / 0.2f) * (1 - smooth((t - 0.8f) / 0.2f));
        break;
    }
    case FACE_G_SHAKE:
        o->fx = KF(t, 0, 0, 0.1f, 0.04f, 0.28f, -0.17f, 0.5f, 0.15f, 0.7f, -0.1f, 0.88f, 0.05f, 1.05f, 0);
        o->tilt = -0.7f * o->fx;
        o->gx = 0.4f * o->fx;
        o->gy = 0.03f * smooth(t / 0.2f) * (1 - smooth((t - 0.9f) / 0.2f));
        break;
    case FACE_G_BOUNCE:
        o->fy = KF(t, 0, 0, 0.18f, 0.07f, 0.45f, -0.3f, 0.72f, 0, 0.8f, 0.04f, 1.05f, -0.14f, 1.3f, 0, 1.38f, 0.02f, 1.5f, 0);
        o->stretch = KF(t, 0, 0, 0.18f, -0.2f, 0.3f, 0.16f, 0.45f, 0, 0.62f, 0.1f, 0.74f, -0.18f, 0.86f, 0.02f, 0.95f,
                        0.08f, 1.28f, -0.1f, 1.4f, 0);
        o->follow = 1;
        o->mouth_open = 0.2f;
        spark(o, t, 0.4f, -0.85f, -0.55f);
        spark(o, t, 0.55f, 0.8f, -0.65f);
        spark(o, t, 1.05f, 0.95f, 0.2f);
        break;
    case FACE_G_TREMBLE: {
        o->eye_k = KF(t, 0, 1, 0.08f, 1.28f, 0.2f, 1.12f, 1.5f, 1.12f, 1.8f, 1);
        o->scale = KF(t, 0, 0, 0.1f, -0.15f, 0.3f, -0.1f, 1.5f, -0.1f, 1.8f, 0);
        o->fy = KF(t, 0, 0, 0.1f, -0.07f, 0.3f, -0.03f, 1.5f, -0.03f, 1.8f, 0);
        float on = smooth((t - 0.25f) / 0.05f) * (1 - smooth((t - 1.4f) / 0.3f));
        o->fx += jitter(t, 0.045f, 19, 27, 0) * on;
        o->fy += jitter(t, 0.018f, 23, 29, 0.7f) * on;
        o->gx = KF(t, 0, 0, 0.6f, 0, 0.65f, -0.11f, 1.0f, -0.11f, 1.05f, 0.11f, 1.4f, 0.11f, 1.45f, 0);
        o->follow = 0.9f;
        break;
    }
    case FACE_G_SHRINK: /* un suspiro: sube, baja de más y se acomoda (la postura sigue) */
        o->fy = KF(t, 0, 0, 0.4f, -0.05f, 0.9f, 0.06f, 1.5f, 0);
        o->scale = KF(t, 0, 0, 0.4f, 0.05f, 0.9f, -0.07f, 1.5f, 0);
        o->stretch = KF(t, 0, 0, 0.4f, 0.05f, 0.9f, -0.05f, 1.5f, 0);
        o->follow = 1;
        break;
    case FACE_G_INFLATE: /* se encoge tantito y se infla de más */
        o->scale = KF(t, 0, 0, 0.15f, -0.08f, 0.42f, 0.1f, 0.9f, 0);
        o->follow = 1;
        o->glow = 1 + 0.4f * smooth(t / 0.4f) * (1 - smooth((t - 0.5f) / 0.4f));
        break;
    case FACE_G_RECOIL: {
        o->scale = KF(t, 0, 0, 0.25f, -0.08f, 1.0f, -0.05f, 1.3f, 0);
        o->fx = KF(t, 0, 0, 0.25f, 0.06f, 1.0f, 0.04f, 1.3f, 0);
        float sh = t > 0.5f && t < 0.9f ? sinf((float)M_PI * (t - 0.5f) / 0.4f) : 0;
        o->fx += jitter(t, 0.022f, 23, 31, 0) * sh;
        o->follow = 0.9f;
        break;
    }
    case FACE_G_DOUBT: {
        o->tilt = KF(t, 0, 0, 0.12f, -0.06f, 0.42f, 0.5f, 0.55f, 0.44f, 1.6f, 0.44f, 2.0f, 0);
        float f = smooth(t / 0.42f) * (1 - smooth((t - 1.6f) / 0.4f));
        o->asym = 0.85f * f;
        o->mouth_asym = 0.8f * f;
        o->smile = -0.15f * f;
        o->gx = -0.06f * f;
        o->gy = -0.07f * f;
        add_sym(o, SPHERE_SYM_QUESTION, 0.74f, -0.8f, pop(t, 0.45f), 1 - smooth((t - 1.6f) / 0.3f), 0);
        break;
    }
    case FACE_G_BLINK2: {
        float b = fmaxf(blinkf(t, 0.2f, 0.05f, 0.03f, 0.09f), blinkf(t, 0.42f, 0.06f, 0.05f, 0.14f));
        o->blink[0] = o->blink[1] = b;
        o->stretch = -0.04f * b;
        o->fy = 0.015f * b;
        break;
    }
    case FACE_G_LOOK: {
        o->gx = KF(t, 0, 0, 0.15f, 0, 0.22f, -0.15f, 0.95f, -0.15f, 1.02f, 0.15f, 1.7f, 0.15f, 1.78f, 0);
        o->fx = KF(t, 0, 0, 0.2f, 0, 0.5f, -0.1f, 1.0f, -0.1f, 1.35f, 0.1f, 1.75f, 0.1f, 2.1f, 0);
        o->tilt = -0.5f * o->fx;
        float b = blinkf(t, 0.98f, 0.06f, 0.04f, 0.11f);
        o->blink[0] = o->blink[1] = b;
        break;
    }
    case FACE_G_WINK: {
        o->tilt = KF(t, 0, 0, 0.1f, -0.04f, 0.35f, 0.22f, 0.48f, 0.18f, 0.9f, 0.18f, 1.1f, 0);
        o->fy = KF(t, 0, 0, 0.1f, 0.03f, 0.3f, -0.08f, 0.5f, 0, 0.56f, 0.015f, 0.65f, 0);
        o->stretch = KF(t, 0, 0, 0.1f, -0.1f, 0.25f, 0.08f, 0.5f, -0.06f, 0.62f, 0);
        o->follow = 1;
        o->blink[0] = smooth((t - 0.3f) / 0.06f) * (1 - smooth((t - 0.85f) / 0.1f));
        o->happy = 0.6f * smooth(t / 0.2f) * (1 - smooth((t - 0.9f) / 0.2f));
        spark(o, t, 0.4f, -0.62f, -0.42f);
        break;
    }
    default:
        break;
    }
}

static float rnd(Face *f)
{
    f->rng = f->rng * 1664525u + 1013904223u;
    return (float)(f->rng >> 8) / 16777216.0f;
}

Face *face_create(unsigned seed)
{
    Face *f = xcalloc(1, sizeof *f);
    f->rng = seed ? seed : 0x5eed;
    f->next_blink = 1.5 + 2.5 * rnd(f);
    f->blink_at = f->blink2_at = -10;
    f->next_look = 25 + 20 * rnd(f);
    f->next_saccade = 3;
    return f;
}

void face_destroy(Face *f)
{
    free(f);
}

const char *face_gesture_name(FaceGesture g)
{
    return g >= 0 && g < FACE_G_COUNT ? GESTURES[g].name : "";
}

FaceGesture face_gesture(const Face *f)
{
    return f->t < f->cur_until ? f->cur.g : FACE_G_NONE;
}

static void play(Face *f, FaceGesture g, float amp)
{
    if (g <= FACE_G_NONE || g >= FACE_G_COUNT) return;
    if (f->t < f->cur_until) f->prev = f->cur; /* el de antes se apaga suave */
    else f->prev.g = FACE_G_NONE;
    f->cur = (Active){g, f->t, amp};
    f->cur_until = f->t + GESTURES[g].dur;
}

void face_play(Face *f, FaceGesture g)
{
    play(f, g, 1);
}

static void mix(GOut *acc, const GOut *g, float k)
{
    acc->fx += g->fx * k, acc->fy += g->fy * k, acc->tilt += g->tilt * k, acc->scale += g->scale * k;
    acc->stretch += g->stretch * k, acc->gx += g->gx * k, acc->gy += g->gy * k;
    for (int i = 0; i < 2; i++) acc->blink[i] = fmaxf(acc->blink[i], g->blink[i] * fminf(k, 1));
    acc->eye_k *= 1 + (g->eye_k - 1) * k;
    acc->lid_top += g->lid_top * k, acc->asym += g->asym * k, acc->happy += g->happy * k;
    acc->mouth_open += g->mouth_open * k, acc->mouth_asym += g->mouth_asym * k, acc->smile += g->smile * k;
    acc->glow *= 1 + (g->glow - 1) * k;
    for (int i = 0; i < g->nsym && acc->nsym < 3; i++) acc->sym[acc->nsym++] = g->sym[i];
}

/* Lo que acaba de pasar decide el gesto. */
static void react(Face *f, const FaceInput *in)
{
    if (!f->started) {
        f->started = true;
        f->cue_seq = in->cue_seq;
        f->dominant = in->affect.dominant;
        return;
    }
    if (in->cue_seq != f->cue_seq) {
        f->cue_seq = in->cue_seq;
        switch (in->cue) {
        case AFF_CUE_THANKS: play(f, FACE_G_NOD, 1); break;
        case AFF_CUE_GREETING: play(f, FACE_G_WINK, 1); break;
        case AFF_CUE_JOKE: play(f, FACE_G_BOUNCE, 1); break;
        case AFF_CUE_DONE: play(f, FACE_G_NOD, 0.7f); break;
        case AFF_CUE_DELICATE: play(f, FACE_G_DOUBT, 1); break;
        case AFF_CUE_FAIL: play(f, FACE_G_SHAKE, 1); break;
        case AFF_CUE_ERROR: play(f, FACE_G_SHAKE, 0.8f); break;
        default: break;
        }
    }
    if (in->affect.dominant != f->dominant) {
        f->dominant = in->affect.dominant;
        /* Si recién rebotó o asintió por lo mismo (un chiste, un gracias), no rebota otra vez. */
        bool busy = f->t < f->cur_until && f->t - f->cur.t0 < 0.6;
        static const FaceGesture ON_ENTER[AFF_COUNT] = {
            [AFF_JOY] = FACE_G_BOUNCE,   [AFF_FEAR] = FACE_G_TREMBLE,  [AFF_ANGER] = FACE_G_INFLATE,
            [AFF_DISGUST] = FACE_G_RECOIL, [AFF_SADNESS] = FACE_G_SHRINK,
        };
        FaceGesture g = f->dominant >= 0 && f->dominant < AFF_COUNT ? ON_ENTER[f->dominant] : FACE_G_NONE;
        if (g && !(busy && g == FACE_G_BOUNCE && (f->cur.g == FACE_G_BOUNCE || f->cur.g == FACE_G_NOD))) play(f, g, 1);
    }
}

void face_sphere_colors(const AffectState *a, const SphereParams *neutral, SphereParams *out)
{
    *out = *neutral;
    for (int c = 0; c < 3; c++) {
        float lo = 0, hi = 0;
        for (int k = 0; k < AFF_COUNT; k++) {
            float w = clampf(a->weights[k], 0, 1);
            lo += w * (k == AFF_NEUTRAL ? neutral->low[c] : LOW[k][c]);
            hi += w * (k == AFF_NEUTRAL ? neutral->high[c] : HIGH[k][c]);
        }
        out->low[c] = clampf(lo, 0, 255);
        out->high[c] = clampf(hi, 0, 255);
    }
}

void face_step(Face *f, double dt, const FaceInput *in, SphereFace *out)
{
    if (!(dt > 0)) dt = 0;
    if (dt > 0.25) dt = 0.25;
    f->t += dt;
    float t = (float)f->t, L = LEVEL[in->level >= 0 && in->level < FACE_LEVEL_COUNT ? in->level : FACE_LEVEL_NORMAL];
    const AffectState *a = &in->affect;
    float w[AFF_COUNT];
    for (int k = 0; k < AFF_COUNT; k++) w[k] = clampf(a->weights[k], 0, 1);
    react(f, in);

    /* La forma: la mezcla de las poses, más o menos marcada según el nivel. */
    Shape s;
    float *sv = (float *)&s;
    const float *nv = (const float *)&POSE[AFF_NEUTRAL];
    for (int i = 0; i < SHAPE_N; i++) {
        float m = 0;
        for (int k = 0; k < AFF_COUNT; k++) m += w[k] * ((const float *)&POSE[k])[i];
        sv[i] = nv[i] + (m - nv[i]) * L;
    }

    /* Las actividades se prenden y apagan suave. */
    int act = in->activity >= FACE_IDLE && in->activity <= FACE_SPEAKING ? (int)in->activity : FACE_IDLE;
    float ka = 1 - expf(-(float)dt / 0.25f);
    for (int i = 0; i < 4; i++) f->act[i] += ((i == act ? 1.0f : 0.0f) - f->act[i]) * ka;
    if (act == FACE_THINKING && f->act[FACE_THINKING] < 0.05f) f->think_t0 = f->t;
    if (act != FACE_IDLE) f->idle_since = f->t;
    float al = f->act[FACE_LISTENING], at = f->act[FACE_THINKING], as = f->act[FACE_SPEAKING];

    GOut g;
    memset(&g, 0, sizeof g);
    g.eye_k = g.glow = 1;
    /* Lo que se sostiene mientras dura la emoción (va con los pesos). */
    float pfx = 0, pfy = 0, pscale = 0;
    pfy += -0.03f * w[AFF_JOY] + 0.15f * w[AFF_SADNESS] - 0.03f * w[AFF_FEAR];
    pfy += 0.02f * w[AFF_JOY] * sinf(2 * (float)M_PI * 1.2f * t);
    pscale += 0.18f * w[AFF_ANGER] - 0.22f * w[AFF_SADNESS] - 0.1f * w[AFF_FEAR] - 0.1f * w[AFF_DISGUST];
    pfx += 0.06f * w[AFF_DISGUST] + jitter(t, 0.012f, 19, 27, 0) * w[AFF_ANGER] + jitter(t, 0.012f, 21, 29, 0.4f) * w[AFF_FEAR];
    g.tilt += -0.14f * w[AFF_DISGUST];
    g.gy += 0.07f * w[AFF_SADNESS] + 0.02f * w[AFF_DISGUST];
    g.gx += -0.05f * w[AFF_DISGUST];
    g.lid_top += 0.15f * w[AFF_SADNESS];
    g.glow *= 1 + 0.2f * w[AFF_ANGER] * sinf(2 * (float)M_PI * 4 * t);

    /* Escuchando: se inclina y abre los ojos. Pensando: mira arriba. Hablando: la boca. */
    g.scale += 0.06f * al;
    g.tilt += 0.13f * al + 0.12f * at;
    g.eye_k *= 1 + 0.12f * al;
    g.stretch += 0.035f * in->voice * al;
    g.fy += 0.02f * in->voice * al - 0.03f * at + 0.012f * in->pulse * as;
    g.gx += (0.12f + 0.02f * sinf(2.2f * t)) * at;
    g.gy += -0.14f * at;
    g.lid_top += 0.18f * at;
    g.asym += -0.5f * at;
    /* Siempre respira (menos al hablar). */
    float br = sinf(2 * (float)M_PI * 0.4f * t);
    g.stretch += 0.03f * br * (1 - 0.5f * as);
    g.fy += -0.01f * br;
    g.glow *= 1 + 0.12f * br;

    /* Callada un rato: mira alrededor de vez en cuando; y siempre mueve un
       poco los ojos. */
    if (act == FACE_IDLE && f->t - f->idle_since > 12 && f->t >= f->next_look && f->t >= f->cur_until) {
        play(f, FACE_G_LOOK, 1);
        f->next_look = f->t + 25 + 20 * rnd(f);
    }
    if (f->t >= f->next_saccade) {
        f->saccade_x = (rnd(f) - 0.5f) * 0.06f;
        f->saccade_y = (rnd(f) - 0.5f) * 0.04f;
        f->next_saccade = f->t + 2.5 + 3 * rnd(f);
    }
    float kg = 1 - expf(-(float)dt / 0.06f);
    f->gaze_x += (f->saccade_x - f->gaze_x) * kg;
    f->gaze_y += (f->saccade_y - f->gaze_y) * kg;
    g.gx += f->gaze_x * (1 - at);
    g.gy += f->gaze_y * (1 - at);

    /* El gesto de ahora (y el anterior, que se apaga), con su envolvente. */
    GOut ge, gl;
    GOut sum;
    memset(&sum, 0, sizeof sum);
    sum.eye_k = sum.glow = 1;
    float lag_fx = 0, lag_fy = 0;
    const Active *list[2] = {&f->cur, &f->prev};
    for (int i = 0; i < 2; i++) {
        const Active *A = list[i];
        if (A->g == FACE_G_NONE) continue;
        float tg = (float)(f->t - A->t0), dur = GESTURES[A->g].dur;
        if (tg > dur) continue;
        float env = i == 0 ? 1.0f : 1 - smooth((float)(f->t - f->cur.t0) / 0.2f);
        if (env <= 0) continue;
        gesture_eval(A->g, tg, &ge);
        gesture_eval(A->g, fmaxf(0, tg - 0.07f), &gl); /* la esfera la sigue con retraso */
        float k = env * A->amp * L;
        mix(&sum, &ge, k);
        /* los parpadeos del gesto no se escalan con el nivel */
        for (int e = 0; e < 2; e++) sum.blink[e] = fmaxf(sum.blink[e], ge.blink[e] * env);
        lag_fx += gl.fx * k * gl.follow;
        lag_fy += gl.fy * k * gl.follow;
    }

    /* Parpadea sola, a veces dos veces seguidas. */
    if (f->t >= f->next_blink) {
        f->blink_at = f->t;
        f->blink2_at = rnd(f) < 0.18f ? f->t + 0.22 : -10;
        f->next_blink = f->t + 2.5 + 3.5 * rnd(f);
    }
    float bl = fmaxf(blinkf((float)(f->t - f->blink_at), 0, 0.06f, 0.04f, 0.11f),
                     blinkf((float)(f->t - f->blink2_at), 0, 0.06f, 0.04f, 0.11f));

    /* ---- la pose ---- */
    sphere_face_neutral(out);
    float ek = g.eye_k * sum.eye_k;
    out->eye_w = clampf(s.eye_w * ek, 0.5f, 1.8f);
    out->eye_h = clampf(s.eye_h * ek, 0.4f, 2.0f);
    out->lid_top = clampf(s.lid_top + (g.lid_top + sum.lid_top) * L, 0, 0.8f);
    out->lid_bot = clampf(s.lid_bot, 0, 0.7f);
    out->lid_tilt = clampf(s.lid_tilt, -1.6f, 1.6f);
    out->happy = clampf(s.happy + sum.happy, 0, 1);
    out->asym = clampf(s.asym + g.asym * L + sum.asym, -1, 1.2f);
    out->round = clampf(s.round, 2, 4);
    out->eye_dy = clampf(s.eye_dy, -0.1f, 0.1f);
    out->shade = clampf(s.shade, 0, 0.9f);
    out->blink[0] = clampf(fmaxf(bl, fmaxf(g.blink[0], sum.blink[0])), 0, 1);
    out->blink[1] = clampf(fmaxf(bl, fmaxf(g.blink[1], sum.blink[1])), 0, 1);
    out->gaze_x = clampf(g.gx * L + sum.gx, -0.2f, 0.2f);
    out->gaze_y = clampf(g.gy * L + sum.gy, -0.2f, 0.2f);
    out->smile = clampf(s.smile + sum.smile, -1.2f, 1.2f);
    out->mouth_open = clampf(s.mouth_open + sum.mouth_open, 0, 1);
    out->mouth_w = clampf(s.mouth_w, 0.6f, 1.5f);
    out->wave = clampf(s.wave, 0, 1);
    out->mouth_o = clampf(s.mouth_o, 0, 1);
    out->mouth_asym = clampf(s.mouth_asym + sum.mouth_asym, -1.2f, 1.2f);
    out->talk = clampf(in->pulse * as, 0, 1);
    out->fx = clampf((pfx + g.fx) * L + sum.fx, -0.5f, 0.5f);
    out->fy = clampf((pfy + g.fy) * L + sum.fy, -0.5f, 0.5f);
    out->tilt = clampf(g.tilt * L + sum.tilt, -0.7f, 0.7f);
    out->scale = clampf(1 + (pscale + g.scale) * L + sum.scale, 0.65f, 1.3f);
    float st = clampf(g.stretch * L + sum.stretch, -0.3f, 0.3f);
    out->sy = 1 + st;
    out->sx = 1 - 0.9f * st;
    /* La esfera: la postura entera, y el gesto con retraso según qué tanto la jala. */
    out->sphere_dx = clampf((pfx + g.fx) * L + lag_fx, -0.5f, 0.5f);
    out->sphere_dy = clampf((pfy + g.fy) * L + lag_fy, -0.5f, 0.5f);
    out->glow = clampf(s.glow * g.glow * sum.glow, 0.4f, 2.2f);
    for (int c = 0; c < 3; c++) {
        float col = 0;
        for (int k = 0; k < AFF_COUNT; k++) col += w[k] * (k == AFF_NEUTRAL ? SPHERE_IDLE.high[c] : HIGH[k][c]);
        out->color[c] = clampf(col, 0, 255);
    }

    /* ---- los símbolos ---- */
    out->nsym = 0;
    if (!in->symbols) return;
    for (int i = 0; i < sum.nsym && out->nsym < SPHERE_MAX_SYMBOLS; i++) out->sym[out->nsym++] = sum.sym[i];
    if (at > 0.3f && out->nsym < SPHERE_MAX_SYMBOLS) {
        float n = fmodf((float)(f->t - f->think_t0) * 2.5f, 4.2f);
        out->sym[out->nsym++] = (SphereSymbol){SPHERE_SYM_DOTS, 0.55f, -0.72f, fminf(3, n), at, 0};
    }
    /* lágrima que cae cada tanto, gota de sudor, la marca de enojo que late */
    if (w[AFF_SADNESS] > 0.45f && out->nsym < SPHERE_MAX_SYMBOLS) {
        float c = fmodf(t, 3.5f), v = 0.18f + 0.44f * smooth(c / 1.8f);
        float al2 = smooth(c / 0.15f) * (1 - smooth((c - 1.6f) / 0.4f));
        out->sym[out->nsym++] = (SphereSymbol){SPHERE_SYM_TEAR, -0.52f, v, 1, al2 * smooth((w[AFF_SADNESS] - 0.45f) / 0.15f), 0};
    }
    if (w[AFF_FEAR] > 0.45f && out->nsym < SPHERE_MAX_SYMBOLS) {
        float c = fmodf(t, 3.0f), v = -0.52f + 0.22f * smooth(c / 2.2f);
        float al2 = smooth(c / 0.2f) * (1 - smooth((c - 2.4f) / 0.5f));
        out->sym[out->nsym++] = (SphereSymbol){SPHERE_SYM_SWEAT, 0.66f, v, 1, al2 * smooth((w[AFF_FEAR] - 0.45f) / 0.15f), 0};
    }
    if (w[AFF_ANGER] > 0.45f && out->nsym < SPHERE_MAX_SYMBOLS)
        out->sym[out->nsym++] = (SphereSymbol){SPHERE_SYM_ANGER, 0.62f, -0.64f, 1 + 0.12f * sinf(2 * (float)M_PI * 3 * t),
                                               smooth((w[AFF_ANGER] - 0.45f) / 0.15f), 0};
}

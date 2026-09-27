#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* candado y reloj (en Linux, src/linux/include/windows.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "config.h"
#include "log.h"
#include "third_party/cJSON.h"
#include "util.h"

/* Los puntos de referencia (§11): el centro de cada campo. */
static const float REF[AFF_COUNT][2] = {
    {0.00f, 0.00f}, {0.80f, 0.45f}, {-0.35f, 0.85f}, {-0.75f, 0.60f}, {-0.85f, -0.10f}, {-0.55f, -0.75f},
};
static const char *const NAMES[AFF_COUNT] = {"neutral", "alegría", "temor", "furia", "desagrado", "tristeza"};

#define SIGMA 0.35f      /* el ancho de cada campo */
#define ENTER 0.60f      /* histéresis: para entrar, la nueva pesa esto… */
#define MARGIN 0.12f     /* …y le saca esto a la de ahora */
#define EXIT 0.45f       /* la de ahora se suelta si pesa menos que esto */
#define SUSTAIN 0.25     /* segundos que el cambio tiene que sostenerse */
#define MIN_DWELL 1.2    /* segundos mínimos en una dominante */
#define OMEGA 3.0f       /* el resorte (amortiguamiento crítico): llega en ~1.6 s */
#define VMAX 0.9f        /* unidades por segundo */
#define AMAX 3.0f        /* unidades por segundo² */
#define STEP 0.02        /* segundos por paso */
#define TAU_TARGET 8.0   /* el objetivo regresa a la línea base */
#define TAU_FOLLOW 600.0 /* el ánimo sigue a la emoción (10 min) */
#define TAU_RELAX 14400.0 /* el ánimo regresa al temperamento (4 h) */
#define MOOD_MAX 0.6f
#define TRAIL_STEP 0.25 /* las métricas: una muestra cada cuarto de segundo… */
#define TRAIL_N 480     /* …de los últimos 2 minutos */
#define HABIT_WINDOW 30.0
#define MAX_TOOLS 24

struct AffectEngine {
    AffectTemperament temper;
    double t;
    float pv, pa, uv, ua; /* el punto y su velocidad */
    float tv, ta;         /* el objetivo */
    float mv, ma;         /* el ánimo */
    float salience;       /* qué tan importante fue lo último (se desvanece) */
    double hold_until;    /* el objetivo se sostiene hasta aquí antes de regresar */
    float w[AFF_COUNT];
    AffectKind dominant, secondary;
    int candidate;
    double candidate_since, dominant_since;
    int switches;
    unsigned reasons;
    /* habituación: el mismo estímulo seguido pesa menos */
    char last_cause[64];
    float last_sv, last_sa;
    int repeats;
    double last_cause_t;
    /* lo esperado de cada herramienta */
    struct {
        char name[40];
        float ok, n;
    } tools[MAX_TOOLS];
    int ntools;
    /* las métricas */
    float trail_v[TRAIL_N], trail_speed[TRAIL_N];
    int trail_head, trail_count;
    double trail_next;
    double peak_t;
    float peak_dev;
    bool in_peak;
    float recovery;
    /* la trayectoria */
    AffectTraceFn trace;
    void *trace_ctx;
    long long trace_id;
    double sample_t;
    float sample_i;
    char cause_now[64];
};

static float clampf(float x, float lo, float hi)
{
    if (isnan(x)) x = 0;
    return x < lo ? lo : x > hi ? hi : x;
}

static void copy_cause(char *dst, size_t n, const char *cause)
{
    snprintf(dst, n, "%s", cause ? cause : "");
}

const char *affect_name(AffectKind k)
{
    return k >= 0 && k < AFF_COUNT ? NAMES[k] : NAMES[AFF_NEUTRAL];
}

void affect_reference(AffectKind k, float *valence, float *arousal)
{
    if (k < 0 || k >= AFF_COUNT) k = AFF_NEUTRAL;
    if (valence) *valence = REF[k][0];
    if (arousal) *arousal = REF[k][1];
}

const char *affect_reason_name(unsigned bit)
{
    static const char *const R[AFF_R_COUNT] = {"sorpresa",   "novedad",          "sin control",  "incertidumbre",
                                               "trivial",    "repetido",         "tope de velocidad",
                                               "tope de aceleración", "histéresis", "cambio de dominante",
                                               "regresa a la base",   "ánimo en su tope", "la IA"};
    for (int i = 0; i < AFF_R_COUNT; i++)
        if (bit == 1u << i) return R[i];
    return "";
}

/* ------------------------------------------------------------------ el motor */

static void mix_weights(float v, float a, float out[AFF_COUNT])
{
    float sum = 0;
    for (int k = 0; k < AFF_COUNT; k++) {
        float dv = v - REF[k][0], da = a - REF[k][1];
        out[k] = expf(-(dv * dv + da * da) / (2 * SIGMA * SIGMA));
        sum += out[k];
    }
    for (int k = 0; k < AFF_COUNT; k++) out[k] = sum > 0 ? out[k] / sum : k == AFF_NEUTRAL;
}

static float intensity_of(const AffectEngine *e)
{
    /* §10: la distancia al centro, pero no sola: lo importante o sorpresivo se nota más. */
    return clampf(hypotf(e->pv, e->pa) * (0.8f + 0.3f * e->salience), 0, 1);
}

AffectState affect_engine_state(const AffectEngine *e)
{
    AffectState s;
    memset(&s, 0, sizeof s);
    s.valence = e->pv;
    s.arousal = e->pa;
    s.intensity = intensity_of(e);
    memcpy(s.weights, e->w, sizeof s.weights);
    s.dominant = e->dominant;
    s.secondary = e->secondary;
    s.mood_valence = e->mv;
    s.mood_arousal = e->ma;
    s.velocity = hypotf(e->uv, e->ua);
    return s;
}

void affect_engine_target(const AffectEngine *e, float *valence, float *arousal)
{
    if (valence) *valence = e->tv;
    if (arousal) *arousal = e->ta;
}

AffectMetrics affect_engine_metrics(const AffectEngine *e)
{
    AffectMetrics m;
    memset(&m, 0, sizeof m);
    m.deviation = hypotf(e->pv - e->mv, e->pa - e->ma);
    m.recovery_time = e->recovery;
    if (e->trail_count > 1) {
        /* la tendencia: la pendiente (mínimos cuadrados) de los últimos 30 s */
        int n = e->trail_count < 120 ? e->trail_count : 120;
        if (n * TRAIL_STEP >= 5.0) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int i = 0; i < n; i++) {
                double x = i * TRAIL_STEP, y = e->trail_v[(e->trail_head + TRAIL_N - n + i) % TRAIL_N];
                sx += x, sy += y, sxx += x * x, sxy += x * y;
            }
            double den = n * sxx - sx * sx;
            if (den > 0) m.trend = (float)((n * sxy - sx * sy) / den * 60.0);
        }
        double sq = 0;
        for (int i = 0; i < e->trail_count; i++) sq += (double)e->trail_speed[i] * e->trail_speed[i];
        m.volatility = (float)sqrt(sq / e->trail_count);
    }
    return m;
}

static void emit(AffectEngine *e, const char *event)
{
    if (!e->trace) return;
    AffectTrace tr;
    memset(&tr, 0, sizeof tr);
    tr.id = ++e->trace_id;
    tr.event = event;
    tr.cause = e->cause_now;
    tr.state = affect_engine_state(e);
    tr.target_valence = e->tv;
    tr.target_arousal = e->ta;
    tr.trend = affect_engine_metrics(e).trend;
    tr.reasons = e->reasons;
    e->sample_t = e->t;
    e->sample_i = tr.state.intensity;
    e->trace(e->trace_ctx, &tr);
}

/* §13: una emoción entra si pesa ENTER (y le saca MARGIN a la de ahora); la de
   ahora sale si pesa menos que EXIT, y entonces queda la que le sigue si pesa
   al menos EXIT, o neutral. Neutral nunca sale sola. El cambio se tiene que
   sostener SUSTAIN y la dominante dura al menos MIN_DWELL. */
static void update_dominant(AffectEngine *e, double held)
{
    mix_weights(e->pv, e->pa, e->w);
    int d = e->dominant, c = -1, want = -1;
    for (int k = 0; k < AFF_COUNT; k++)
        if (k != d && (c < 0 || e->w[k] > e->w[c])) c = k;
    e->secondary = (AffectKind)c;
    if (e->w[c] >= ENTER && e->w[c] - e->w[d] >= MARGIN) want = c;
    else if (d != AFF_NEUTRAL && e->w[d] < EXIT) want = e->w[c] >= EXIT ? c : AFF_NEUTRAL;
    if (want < 0) {
        if (e->w[c] > e->w[d]) e->reasons |= AFF_R_HYSTERESIS;
        e->candidate = -1;
        return;
    }
    if (e->candidate != want) {
        e->candidate = want;
        e->candidate_since = e->t - held;
    }
    if (e->t - e->candidate_since + 1e-9 < SUSTAIN || e->t - e->dominant_since + 1e-9 < MIN_DWELL) {
        e->reasons |= AFF_R_HYSTERESIS;
        return;
    }
    e->dominant = (AffectKind)want;
    e->dominant_since = e->t;
    e->candidate = -1;
    e->switches++;
    e->reasons |= AFF_R_SWITCH;
    int s = -1;
    for (int k = 0; k < AFF_COUNT; k++)
        if (k != want && (s < 0 || e->w[k] > e->w[s])) s = k;
    e->secondary = (AffectKind)s;
    emit(e, "cambio");
}

static void push_trail(AffectEngine *e, float speed)
{
    e->trail_v[e->trail_head] = e->pv;
    e->trail_speed[e->trail_head] = speed;
    e->trail_head = (e->trail_head + 1) % TRAIL_N;
    if (e->trail_count < TRAIL_N) e->trail_count++;
}

static void track_recovery(AffectEngine *e)
{
    float dev = hypotf(e->pv - e->mv, e->pa - e->ma);
    if (dev > 0.3f) {
        if (!e->in_peak || dev > e->peak_dev) {
            e->peak_t = e->t;
            e->peak_dev = dev;
        }
        e->in_peak = true;
        e->recovery = -1;
    } else if (e->in_peak && dev < 0.1f) {
        e->in_peak = false;
        e->peak_dev = 0;
        e->recovery = (float)(e->t - e->peak_t);
    }
}

static void clamp_mood(AffectEngine *e)
{
    if (fabsf(e->mv) > MOOD_MAX || fabsf(e->ma) > MOOD_MAX) e->reasons |= AFF_R_MOOD_LIMIT;
    e->mv = clampf(e->mv, -MOOD_MAX, MOOD_MAX);
    e->ma = clampf(e->ma, -MOOD_MAX, MOOD_MAX);
}

static void step(AffectEngine *e, double dt)
{
    float h = (float)dt;
    /* el objetivo regresa a la línea base (después de sostenerse un momento) */
    if (e->t >= e->hold_until) {
        float k = 1 - expf(-h / (float)TAU_TARGET);
        e->tv += (e->mv - e->tv) * k;
        e->ta += (e->ma - e->ta) * k;
        if (fabsf(e->tv - e->mv) + fabsf(e->ta - e->ma) > 0.02f) e->reasons |= AFF_R_DECAY;
    }
    e->salience *= expf(-h / (float)TAU_TARGET);
    /* §12: resorte con amortiguamiento crítico, con aceleración y velocidad acotadas */
    float av = OMEGA * OMEGA * (e->tv - e->pv) - 2 * OMEGA * e->uv;
    float aa = OMEGA * OMEGA * (e->ta - e->pa) - 2 * OMEGA * e->ua;
    float am = hypotf(av, aa);
    if (am > AMAX) {
        av *= AMAX / am;
        aa *= AMAX / am;
        e->reasons |= AFF_R_ACCEL_CAP;
    }
    e->uv += av * h;
    e->ua += aa * h;
    float um = hypotf(e->uv, e->ua);
    if (um > VMAX) {
        e->uv *= VMAX / um;
        e->ua *= VMAX / um;
        um = VMAX;
        e->reasons |= AFF_R_SPEED_CAP;
    }
    e->pv += e->uv * h;
    e->pa += e->ua * h;
    if (fabsf(e->pv) > 1) {
        e->pv = clampf(e->pv, -1, 1);
        e->uv = 0;
    }
    if (fabsf(e->pa) > 1) {
        e->pa = clampf(e->pa, -1, 1);
        e->ua = 0;
    }
    /* el ánimo: sigue a la emoción muy despacio y regresa al temperamento */
    e->mv += (float)(((e->pv - e->mv) / TAU_FOLLOW + (e->temper.valence - e->mv) / TAU_RELAX) * dt);
    e->ma += (float)(((e->pa - e->ma) / TAU_FOLLOW + (e->temper.arousal - e->ma) / TAU_RELAX) * dt);
    clamp_mood(e);
    e->t += dt;
    update_dominant(e, 0);
    if (e->t >= e->trail_next) {
        push_trail(e, um);
        e->trail_next = e->t + TRAIL_STEP;
    }
    track_recovery(e);
    /* §14: muestreo adaptativo, solo cuando la intensidad cambió de a de veras */
    float i = intensity_of(e);
    if (fabsf(i - e->sample_i) >= 0.2f && e->t - e->sample_t >= 1.0) emit(e, "muestra");
}

static bool settled(const AffectEngine *e)
{
    return e->t >= e->hold_until && fabsf(e->pv - e->tv) + fabsf(e->pa - e->ta) < 2e-3f &&
           fabsf(e->uv) + fabsf(e->ua) < 2e-3f && fabsf(e->tv - e->mv) + fabsf(e->ta - e->ma) < 2e-3f &&
           e->candidate < 0;
}

/* Ya quieto, el tiempo solo regresa el ánimo al temperamento: se salta de golpe. */
static void jump(AffectEngine *e, double dt)
{
    float k = (float)exp(-dt / TAU_RELAX);
    e->mv = e->temper.valence + (e->mv - e->temper.valence) * k;
    e->ma = e->temper.arousal + (e->ma - e->temper.arousal) * k;
    clamp_mood(e);
    e->pv = e->tv = e->mv;
    e->pa = e->ta = e->ma;
    e->uv = e->ua = 0;
    e->salience *= (float)exp(-dt / TAU_TARGET);
    e->t += dt;
    update_dominant(e, dt);
    update_dominant(e, 0); /* por si tocaba cambiar: el cambio ya se sostuvo todo el salto */
    int n = (int)(dt / TRAIL_STEP);
    for (int i = 0; i < n && i < TRAIL_N; i++) push_trail(e, 0);
    e->trail_next = e->t + TRAIL_STEP;
    track_recovery(e);
}

void affect_engine_advance(AffectEngine *e, double t)
{
    while (t - e->t > 1e-9) {
        double left = t - e->t;
        if (left > 1.0 && settled(e)) {
            jump(e, left);
            break;
        }
        step(e, left < STEP ? left : STEP);
    }
}

AffectEngine *affect_engine_create(const AffectTemperament *temper)
{
    AffectEngine *e = xcalloc(1, sizeof *e);
    AffectTemperament def = {0, 0, 1.0f, 0.5f};
    e->temper = temper ? *temper : def;
    e->temper.valence = clampf(e->temper.valence, -MOOD_MAX, MOOD_MAX);
    e->temper.arousal = clampf(e->temper.arousal, -MOOD_MAX, MOOD_MAX);
    e->temper.sensitivity = clampf(e->temper.sensitivity, 0.2f, 2.0f);
    e->temper.stability = clampf(e->temper.stability, 0, 1);
    e->mv = e->tv = e->pv = e->temper.valence;
    e->ma = e->ta = e->pa = e->temper.arousal;
    e->candidate = -1;
    e->dominant_since = -MIN_DWELL;
    e->dominant = AFF_NEUTRAL;
    mix_weights(e->pv, e->pa, e->w);
    for (int k = 0; k < AFF_COUNT; k++)
        if (e->w[k] > e->w[e->dominant]) e->dominant = (AffectKind)k;
    e->secondary = e->dominant == AFF_NEUTRAL ? AFF_JOY : AFF_NEUTRAL;
    return e;
}

void affect_engine_destroy(AffectEngine *e)
{
    free(e);
}

void affect_engine_on_trace(AffectEngine *e, AffectTraceFn fn, void *ctx)
{
    e->trace = fn;
    e->trace_ctx = ctx;
}

unsigned affect_engine_take_reasons(AffectEngine *e)
{
    unsigned r = e->reasons;
    e->reasons = 0;
    return r;
}

int affect_engine_switches(const AffectEngine *e)
{
    return e->switches;
}

void affect_engine_set_mood(AffectEngine *e, float valence, float arousal, double elapsed_seconds)
{
    if (!(elapsed_seconds > 0)) elapsed_seconds = 0;
    float k = (float)exp(-elapsed_seconds / TAU_RELAX);
    e->mv = e->temper.valence + (clampf(valence, -MOOD_MAX, MOOD_MAX) - e->temper.valence) * k;
    e->ma = e->temper.arousal + (clampf(arousal, -MOOD_MAX, MOOD_MAX) - e->temper.arousal) * k;
    /* se arranca en calma, sobre la línea base */
    e->pv = e->tv = e->mv;
    e->pa = e->ta = e->ma;
    e->uv = e->ua = 0;
    mix_weights(e->pv, e->pa, e->w);
}

/* stimulus → appraisal → objetivo (§12); el paso acotado lo hace step(). */
static void apply_target(AffectEngine *e, float sv, float sa, float w, float salience, const char *cause)
{
    copy_cause(e->cause_now, sizeof e->cause_now, cause);
    /* habituación (§80): lo mismo, otra vez y pronto, pesa menos */
    if (cause && *cause && !strcmp(cause, e->last_cause) && e->t - e->last_cause_t < HABIT_WINDOW &&
        hypotf(sv - e->last_sv, sa - e->last_sa) < 0.3f) {
        e->repeats++;
        w /= 1 + 0.5f * (float)e->repeats;
        e->reasons |= AFF_R_REPEATED;
    } else {
        e->repeats = 0;
        copy_cause(e->last_cause, sizeof e->last_cause, cause);
    }
    e->last_cause_t = e->t;
    e->last_sv = sv;
    e->last_sa = sa;
    /* estabilidad (§83): lo que movería muy poco casi no mueve */
    float dist = hypotf(sv - e->tv, sa - e->ta);
    if (w * dist < 0.1f * e->temper.stability) {
        w *= 0.3f;
        e->reasons |= AFF_R_TRIVIAL;
    }
    w = clampf(w, 0, 0.95f);
    e->tv += (sv - e->tv) * w;
    e->ta += (sa - e->ta) * w;
    salience = clampf(salience, 0, 1);
    if (salience > e->salience) e->salience = salience;
    double hold = 1.5 + 3.0 * salience;
    if (e->t + hold > e->hold_until) e->hold_until = e->t + hold;
    emit(e, "estimulo");
}

void affect_engine_stimulus(AffectEngine *e, double t, const AffectStimulus *s)
{
    affect_engine_advance(e, t);
    float imp = clampf(s->importance, 0, 1), nov = clampf(s->novelty, 0, 1), sur = clampf(s->surprise, 0, 1);
    float ctl = clampf(s->control, -1, 1), cer = clampf(s->certainty, -1, 1);
    float sv = clampf(s->valence, -1, 1);
    /* la sorpresa, lo nuevo, la falta de control y la incertidumbre activan */
    float sa = clampf(clampf(s->arousal, -1, 1) + 0.45f * sur + 0.25f * nov + 0.3f * fmaxf(-ctl, 0) -
                          0.15f * fmaxf(ctl, 0) + 0.3f * fmaxf(-cer, 0),
                      -1, 1);
    if (sur > 0.3f) e->reasons |= AFF_R_SURPRISE;
    if (nov > 0.3f) e->reasons |= AFF_R_NOVELTY;
    if (ctl < -0.3f) e->reasons |= AFF_R_LOW_CONTROL;
    if (cer < -0.3f) e->reasons |= AFF_R_UNCERTAIN;
    float w = e->temper.sensitivity * (0.3f + 0.7f * imp) * (0.8f + 0.2f * fmaxf(nov, sur));
    apply_target(e, sv, sa, w, fmaxf(imp, fmaxf(nov, sur)), s->cause);
}

void affect_engine_emotions(AffectEngine *e, double t, const float amount[AFF_COUNT], const char *cause)
{
    affect_engine_advance(e, t);
    /* La etiqueta evalúa toda la situación: el objetivo se vuelve casi ese
       punto. Hasta una emoción baja queda a medio camino de su referencia
       (0.2 → 48 %, 0.5 → 68 %, 1 → 100 %) para que se reconozca. Con dos, manda
       la más fuerte y la otra solo la jala un poco: promediar puntos lejanos
       (tristeza + temor) caería en una tercera emoción (desagrado). */
    int k1 = -1, k2 = -1;
    for (int k = 0; k < AFF_COUNT; k++) {
        if (!(clampf(amount[k], 0, 1) > 0)) continue;
        if (k1 < 0 || amount[k] > amount[k1]) k2 = k1, k1 = k;
        else if (k2 < 0 || amount[k] > amount[k2]) k2 = k;
    }
    if (k1 < 0) return;
    float i1 = clampf(amount[k1], 0, 1), reach = 0.35f + 0.65f * i1;
    float sv = REF[k1][0] * reach, sa = REF[k1][1] * reach;
    if (k2 >= 0) {
        float i2 = clampf(amount[k2], 0, 1), r2 = 0.35f + 0.65f * i2, pull = 0.35f * i2 / i1;
        sv += (REF[k2][0] * r2 - sv) * pull;
        sa += (REF[k2][1] * r2 - sa) * pull;
    }
    e->reasons |= AFF_R_AI;
    apply_target(e, clampf(sv, -1, 1), clampf(sa, -1, 1), e->temper.sensitivity * (0.85f + 0.1f * i1),
                 0.5f + 0.5f * i1, cause);
}

void affect_engine_emotion(AffectEngine *e, double t, AffectKind k, float intensity, const char *cause)
{
    float amount[AFF_COUNT] = {0};
    if (k < 0 || k >= AFF_COUNT) return;
    amount[k] = intensity;
    affect_engine_emotions(e, t, amount, cause);
}

void affect_engine_outcome(AffectEngine *e, double t, float expected, bool ok, float importance, const char *cause)
{
    float sur = fabsf((ok ? 1.0f : 0.0f) - clampf(expected, 0, 1));
    AffectStimulus s;
    memset(&s, 0, sizeof s);
    s.importance = importance;
    s.surprise = sur;
    s.cause = cause;
    /* Salir bien es satisfacción (más si no se esperaba); fallar se va hacia
       la tristeza (con poca energía), no al enojo ni al desagrado. La
       activación ya descuenta lo que le suma la sorpresa. */
    if (ok) {
        s.valence = 0.35f + 0.35f * sur;
        s.arousal = 0.1f + 0.3f * sur - 0.45f * sur;
    } else {
        s.valence = -0.4f - 0.2f * sur;
        s.arousal = -0.75f + 0.1f * sur - 0.45f * sur;
    }
    affect_engine_stimulus(e, t, &s);
}

void affect_engine_event(AffectEngine *e, double t, AffectEvent ev)
{
    static const struct {
        float v, a, imp, ctl;
        const char *cause;
    } EV[AFF_EV_COUNT] = {
        [AFF_EV_THANKS] = {0.7f, 0.25f, 0.55f, 0, "gracias"},
        [AFF_EV_GREETING] = {0.5f, 0.3f, 0.35f, 0, "saludo"},
        [AFF_EV_JOKE] = {0.8f, 0.5f, 0.5f, 0, "chiste"},
        [AFF_EV_TASK_DONE] = {0.35f, 0.1f, 0.3f, 0, "resuelto sin IA"},
        [AFF_EV_DELICATE] = {-0.1f, 0.55f, 0.45f, -0.4f, "pide un sí"},
        [AFF_EV_NETWORK_ERROR] = {-0.4f, -0.75f, 0.5f, -0.3f, "sin conexión"},
        [AFF_EV_QUOTA] = {-0.3f, -0.6f, 0.35f, 0, "sin cupo"},
    };
    if (ev < 0 || ev >= AFF_EV_COUNT) return;
    AffectStimulus s;
    memset(&s, 0, sizeof s);
    s.valence = EV[ev].v;
    s.arousal = EV[ev].a;
    s.importance = EV[ev].imp;
    s.control = EV[ev].ctl;
    s.cause = EV[ev].cause;
    affect_engine_stimulus(e, t, &s);
}

void affect_engine_tool(AffectEngine *e, double t, const char *name, bool ok)
{
    char clean[40];
    size_t n = 0;
    for (const char *p = name ? name : ""; *p && n + 1 < sizeof clean; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') clean[n++] = *p;
    clean[n] = 0;
    int slot = -1;
    for (int i = 0; i < e->ntools; i++)
        if (!strcmp(e->tools[i].name, clean)) slot = i;
    if (slot < 0) {
        slot = e->ntools < MAX_TOOLS ? e->ntools++ : (int)(e->trace_id % MAX_TOOLS);
        copy_cause(e->tools[slot].name, sizeof e->tools[slot].name, clean);
        e->tools[slot].ok = e->tools[slot].n = 0;
    }
    /* lo esperado: 85 % al principio, y luego lo que de veras le ha pasado */
    float expected = (e->tools[slot].ok + 0.85f * 4) / (e->tools[slot].n + 4);
    char cause[64];
    snprintf(cause, sizeof cause, "%s %s", *clean ? clean : "herramienta", ok ? "salió" : "falló");
    affect_engine_outcome(e, t, expected, ok, 0.35f, cause);
    e->tools[slot].n += 1;
    if (ok) e->tools[slot].ok += 1;
}

/* ------------------------------------------------------------------ el de Sokari */

static SRWLOCK g_lock = SRWLOCK_INIT;
static AffectEngine *g_e;
static double g_t0, g_saved_t;
static float g_saved_v, g_saved_a;

static double now_s(void)
{
    return (double)GetTickCount64() / 1000.0 - g_t0;
}

static wchar_t *affect_path(const wchar_t *name)
{
    return g_paths.local_dir ? path_join(g_paths.local_dir, name) : NULL;
}

static void add_num(cJSON *o, const char *key, float x)
{
    cJSON_AddNumberToObject(o, key, round((double)x * 1000) / 1000);
}

/* La trayectoria en afecto.jsonl: un renglón por evento, y a los 256 KB se
   queda con la mitad más nueva. */
static void on_trace(void *ctx, const AffectTrace *tr)
{
    (void)ctx;
    wchar_t *path = affect_path(L"afecto.jsonl");
    if (!path) return;
    cJSON *o = cJSON_CreateObject();
    char *ts = local_iso_now();
    cJSON_AddStringToObject(o, "ts", ts);
    free(ts);
    cJSON_AddNumberToObject(o, "id", (double)tr->id);
    cJSON_AddStringToObject(o, "evento", tr->event);
    cJSON_AddStringToObject(o, "causa", tr->cause);
    add_num(o, "valencia", tr->state.valence);
    add_num(o, "activacion", tr->state.arousal);
    add_num(o, "intensidad", tr->state.intensity);
    cJSON_AddStringToObject(o, "dominante", affect_name(tr->state.dominant));
    cJSON_AddStringToObject(o, "secundaria", affect_name(tr->state.secondary));
    add_num(o, "objetivo_valencia", tr->target_valence);
    add_num(o, "objetivo_activacion", tr->target_arousal);
    add_num(o, "animo_valencia", tr->state.mood_valence);
    add_num(o, "animo_activacion", tr->state.mood_arousal);
    add_num(o, "velocidad", tr->state.velocity);
    add_num(o, "tendencia", tr->trend);
    cJSON *why = cJSON_AddArrayToObject(o, "motivos");
    for (int i = 0; i < AFF_R_COUNT; i++)
        if (tr->reasons & (1u << i)) cJSON_AddItemToArray(why, cJSON_CreateString(affect_reason_name(1u << i)));
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        char *line = str_printf("%s\n", json);
        ensure_dir(g_paths.local_dir);
        append_file(path, line, strlen(line));
        free(line);
        free(json);
    }
    if (file_size(path) > 256 * 1024) {
        size_t len = 0;
        char *all = read_file_all(path, &len);
        char *cut = all ? memchr(all + len / 2, '\n', len - len / 2) : NULL;
        if (cut) write_file_atomic(path, cut + 1, len - (size_t)(cut + 1 - all));
        free(all);
    }
    free(path);
}

static void save_locked(void)
{
    wchar_t *path = affect_path(L"afecto.json");
    if (!path) return;
    AffectState s = affect_engine_state(g_e);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "version", 1);
    add_num(o, "animo_valencia", s.mood_valence);
    add_num(o, "animo_activacion", s.mood_arousal);
    cJSON_AddNumberToObject(o, "guardado", floor(now_epoch()));
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    ensure_dir(g_paths.local_dir);
    if (txt && write_file_atomic(path, txt, strlen(txt))) {
        g_saved_v = s.mood_valence;
        g_saved_a = s.mood_arousal;
    }
    g_saved_t = g_e->t;
    free(txt);
    free(path);
}

static void load_mood(void)
{
    wchar_t *path = affect_path(L"afecto.json");
    char *txt = path ? read_file_all(path, NULL) : NULL;
    cJSON *o = txt ? cJSON_Parse(txt) : NULL;
    cJSON *v = cJSON_GetObjectItem(o, "animo_valencia"), *a = cJSON_GetObjectItem(o, "animo_activacion");
    cJSON *when = cJSON_GetObjectItem(o, "guardado");
    if (cJSON_IsNumber(v) && cJSON_IsNumber(a)) {
        double elapsed = cJSON_IsNumber(when) ? now_epoch() - when->valuedouble : 0;
        affect_engine_set_mood(g_e, (float)v->valuedouble, (float)a->valuedouble, elapsed);
        AffectState s = affect_engine_state(g_e);
        g_saved_v = s.mood_valence;
        g_saved_a = s.mood_arousal;
        log_msg("Ánimo de la sesión anterior: valencia %.2f, activación %.2f.", s.mood_valence, s.mood_arousal);
    }
    cJSON_Delete(o);
    free(txt);
    free(path);
}

/* Con el candado tomado: crea el motor la primera vez y lo avanza a ahora;
   cada 5 minutos guarda el ánimo si se movió. */
static AffectEngine *engine_locked(void)
{
    if (!g_e) {
        g_t0 = (double)GetTickCount64() / 1000.0;
        g_e = affect_engine_create(NULL);
        load_mood();
        affect_engine_on_trace(g_e, on_trace, NULL);
    }
    affect_engine_advance(g_e, now_s());
    if (g_e->t - g_saved_t >= 300 && fabsf(g_e->mv - g_saved_v) + fabsf(g_e->ma - g_saved_a) > 0.005f) save_locked();
    return g_e;
}

void affect_init(void)
{
    AcquireSRWLockExclusive(&g_lock);
    engine_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

void affect_stimulus(const AffectStimulus *s)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectEngine *e = engine_locked();
    affect_engine_stimulus(e, e->t, s);
    ReleaseSRWLockExclusive(&g_lock);
}

void affect_emotions(const float amount[AFF_COUNT], const char *cause)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectEngine *e = engine_locked();
    affect_engine_emotions(e, e->t, amount, cause);
    ReleaseSRWLockExclusive(&g_lock);
}

void affect_outcome(float expected, bool ok, float importance, const char *cause)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectEngine *e = engine_locked();
    affect_engine_outcome(e, e->t, expected, ok, importance, cause);
    ReleaseSRWLockExclusive(&g_lock);
}

void affect_event(AffectEvent ev)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectEngine *e = engine_locked();
    affect_engine_event(e, e->t, ev);
    ReleaseSRWLockExclusive(&g_lock);
}

void affect_tool(const char *name, bool ok)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectEngine *e = engine_locked();
    affect_engine_tool(e, e->t, name, ok);
    ReleaseSRWLockExclusive(&g_lock);
}

AffectState affect_get(void)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectState s = affect_engine_state(engine_locked());
    ReleaseSRWLockExclusive(&g_lock);
    return s;
}

void affect_target(float *valence, float *arousal)
{
    AcquireSRWLockExclusive(&g_lock);
    affect_engine_target(engine_locked(), valence, arousal);
    ReleaseSRWLockExclusive(&g_lock);
}

AffectMetrics affect_metrics(void)
{
    AcquireSRWLockExclusive(&g_lock);
    AffectMetrics m = affect_engine_metrics(engine_locked());
    ReleaseSRWLockExclusive(&g_lock);
    return m;
}

void affect_save(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_e) { /* si nada lo despertó en esta sesión, no hay nada nuevo que guardar */
        engine_locked();
        save_locked();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* ------------------------------------------------------------------ la etiqueta de la IA */

/* Una letra en minúsculas y sin acento (á → a, Ñ → n); *len = los bytes que ocupa. */
static char fold(const char *s, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    *len = 1;
    if (u[0] == 0xC3 && u[1]) {
        static const char MAP[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyps"
                                  "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
        *len = 2;
        return MAP[(u[1] - 0x80) & 63];
    }
    return (char)(u[0] >= 'A' && u[0] <= 'Z' ? u[0] + 32 : u[0]);
}

static bool is_letter(const char *s)
{
    unsigned char c = (unsigned char)*s;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == 0xC3 && s[1]);
}

/* La palabra que empieza en s, doblada (sin acentos, minúsculas); devuelve dónde termina. */
static const char *read_word(const char *s, char *out, size_t n)
{
    size_t k = 0;
    while (*s && is_letter(s)) {
        int len;
        char c = fold(s, &len);
        if (k + 1 < n) out[k++] = c;
        s += len;
    }
    out[k] = 0;
    return s;
}

static bool is_keyword(const char *w)
{
    return !strcmp(w, "afecto") || !strcmp(w, "afectos") || !strcmp(w, "emocion") || !strcmp(w, "emociones") ||
           !strcmp(w, "emotion") || !strcmp(w, "affect");
}

/* ¿w es el principio de una palabra clave (cortada al final de la respuesta)? */
static bool keyword_prefix(const char *w)
{
    static const char *const K[] = {"afecto", "emocion", "emotion", "affect"};
    size_t n = strlen(w);
    for (size_t i = 0; i < sizeof K / sizeof *K; i++)
        if (!strncmp(K[i], w, n)) return true;
    return false;
}

static int kind_of(const char *w)
{
    static const struct {
        const char *w;
        AffectKind k;
    } S[] = {
        {"neutral", AFF_NEUTRAL},    {"neutro", AFF_NEUTRAL},      {"neutra", AFF_NEUTRAL},
        {"calma", AFF_NEUTRAL},      {"tranquilo", AFF_NEUTRAL},   {"tranquila", AFF_NEUTRAL},
        {"sereno", AFF_NEUTRAL},     {"serena", AFF_NEUTRAL},      {"alegria", AFF_JOY},
        {"alegre", AFF_JOY},         {"feliz", AFF_JOY},           {"felicidad", AFF_JOY},
        {"contento", AFF_JOY},       {"contenta", AFF_JOY},        {"entusiasmo", AFF_JOY},
        {"joy", AFF_JOY},            {"happy", AFF_JOY},           {"happiness", AFF_JOY},
        {"temor", AFF_FEAR},         {"miedo", AFF_FEAR},          {"nervios", AFF_FEAR},
        {"nervioso", AFF_FEAR},      {"nerviosa", AFF_FEAR},       {"ansiedad", AFF_FEAR},
        {"preocupacion", AFF_FEAR},  {"preocupado", AFF_FEAR},     {"preocupada", AFF_FEAR},
        {"susto", AFF_FEAR},         {"fear", AFF_FEAR},           {"furia", AFF_ANGER},
        {"enojo", AFF_ANGER},        {"enojado", AFF_ANGER},       {"enojada", AFF_ANGER},
        {"ira", AFF_ANGER},          {"coraje", AFF_ANGER},        {"frustracion", AFF_ANGER},
        {"molestia", AFF_ANGER},     {"molesto", AFF_ANGER},       {"molesta", AFF_ANGER},
        {"anger", AFF_ANGER},        {"angry", AFF_ANGER},         {"desagrado", AFF_DISGUST},
        {"asco", AFF_DISGUST},       {"disgusto", AFF_DISGUST},    {"rechazo", AFF_DISGUST},
        {"disgust", AFF_DISGUST},    {"tristeza", AFF_SADNESS},    {"triste", AFF_SADNESS},
        {"pena", AFF_SADNESS},       {"melancolia", AFF_SADNESS},  {"decepcion", AFF_SADNESS},
        {"desanimo", AFF_SADNESS},   {"sad", AFF_SADNESS},         {"sadness", AFF_SADNESS},
    };
    for (size_t i = 0; i < sizeof S / sizeof *S; i++)
        if (!strcmp(w, S[i].w)) return S[i].k;
    return -1;
}

/* «alegría 0.6, temor 20%» → amount; devuelve cuántas entendió. */
static int parse_list(const char *s, const char *end, float amount[AFF_COUNT])
{
    int found = 0;
    while (s < end) {
        if (!is_letter(s)) {
            s++;
            continue;
        }
        char w[32];
        s = read_word(s, w, sizeof w);
        if (s > end) break;
        int k = kind_of(w);
        if (k < 0) continue; /* «y», «intensidad», una que no es de las 6… */
        while (s < end && (*s == ' ' || *s == ':' || *s == '=' || *s == '\t')) s++;
        float value = 0.5f;
        if (s < end && ((*s >= '0' && *s <= '9') || (*s == '.' && s + 1 < end && s[1] >= '0' && s[1] <= '9'))) {
            double x = 0, scale = 0;
            for (; s < end; s++) {
                if (*s >= '0' && *s <= '9') {
                    if (scale > 0) x += (*s - '0') * scale, scale /= 10;
                    else x = x * 10 + (*s - '0');
                } else if ((*s == '.' || *s == ',') && scale == 0 && s + 1 < end && s[1] >= '0' && s[1] <= '9') {
                    scale = 0.1;
                } else break;
            }
            while (s < end && *s == ' ') s++;
            if (s < end && *s == '%') x /= 100, s++;
            else if (end - s >= 4 && !strncmp(s, "/100", 4)) x /= 100, s += 4;
            else if (end - s >= 3 && !strncmp(s, "/10", 3)) x /= 10, s += 3;
            else if (x > 10) x /= 100;
            else if (x > 1) x /= 10;
            value = clampf((float)x, 0, 1);
        }
        amount[k] = value > amount[k] ? value : amount[k];
        found++;
    }
    return found;
}

static char closer_of(char c)
{
    return c == '[' ? ']' : c == '(' ? ')' : c == '{' ? '}' : '>';
}

char *affect_take_tags(const char *reply, float amount[AFF_COUNT], int *found)
{
    float local[AFF_COUNT];
    float *am = amount ? amount : local;
    for (int k = 0; k < AFF_COUNT; k++) am[k] = 0;
    int nfound = 0;
    bool removed = false;
    StrBuf out;
    sb_init(&out);
    sb_append(&out, "");
    const char *p = reply ? reply : "";
    while (*p) {
        bool line_start = p == reply || p[-1] == '\n';
        if (*p == '[' || *p == '(' || *p == '{' || *p == '<') {
            char close = closer_of(*p);
            const char *q = p + 1;
            while (*q == ' ' || *q == '*' || *q == '_') q++;
            char w[32];
            const char *after = read_word(q, w, sizeof w);
            if (*w && is_keyword(w)) {
                const char *end = strchr(after, close);
                removed = true;
                if (!end) break; /* cortada: se quita hasta el final y no cuenta */
                nfound += parse_list(after, end, am);
                p = end + 1;
                continue;
            }
            /* «[af», «(emo» o «[» al puro final: el principio de una etiqueta cortada */
            const char *rest = after;
            while (*rest == ' ' || *rest == '\n' || *rest == '\r') rest++;
            if (!*rest && keyword_prefix(w)) {
                removed = true;
                break;
            }
        } else if (line_start && is_letter(p)) {
            /* «afecto: alegría 0.6» en su propio renglón, sin corchetes (y
               corto, con una emoción de verdad: «Emoción: es un estado…» se queda) */
            char w[32];
            const char *after = read_word(p, w, sizeof w);
            const char *c = after;
            while (*c == ' ') c++;
            if (is_keyword(w) && *c == ':') {
                const char *end = strchr(c, '\n');
                if (!end) end = c + strlen(c);
                float line[AFF_COUNT] = {0};
                int n = end - c <= 60 ? parse_list(c + 1, end, line) : 0;
                if (n) {
                    for (int k = 0; k < AFF_COUNT; k++)
                        if (line[k] > am[k]) am[k] = line[k];
                    nfound += n;
                    removed = true;
                    p = *end ? end + 1 : end;
                    continue;
                }
            }
        }
        sb_append_char(&out, *p++);
    }
    if (found) *found = nfound;
    if (!removed) return sb_steal(&out);
    /* Lo que quedó: sin espacios dobles ni espacio antes de la puntuación. */
    char *raw = sb_steal(&out);
    StrBuf tidy;
    sb_init(&tidy);
    sb_append(&tidy, "");
    for (const char *s = raw; *s; s++) {
        if (*s == ' ' && (s[1] == ' ' || s[1] == '.' || s[1] == ',' || s[1] == '!' || s[1] == '?' || s[1] == '\n' ||
                          s[1] == 0 || tidy.len == 0 || tidy.data[tidy.len - 1] == '\n'))
            continue;
        sb_append_char(&tidy, *s);
    }
    free(raw);
    char *res = str_trim(tidy.data);
    sb_free(&tidy);
    return res;
}

const char *affect_prompt(void)
{
    return "Tu cara en la pantalla expresa emociones. Al final de CADA respuesta agrega una etiqueta con la "
           "emoción que va con lo que dices y con cómo salió lo que te pidieron, por ejemplo [afecto: alegría 0.6]. "
           "Usa alegría, tristeza, furia, desagrado, temor o neutral, con intensidad de 0 a 1 (puedes poner dos: "
           "[afecto: alegría 0.5, temor 0.2]). Nadie la oye ni la ve: no la menciones. Si nada destaca, "
           "[afecto: neutral 0.5].";
}

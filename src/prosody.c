/* La voz con emoción sobre el audio: el tono se cambia remuestreando (sube o
   baja el tono y también la duración) y la duración se corrige con WSOLA
   (encima trozos de 30 ms buscando dónde empalman igual, así no cambia el
   tono). Luego el temblor, el volumen y el silencio del final. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "prosody.h"
#include "util.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Cómo suena cada emoción al máximo (la de la frase es la mezcla con los
   pesos). Cambios chicos: más, y suena a caricatura. */
static const Prosody STYLE[AFF_COUNT] = {
    [AFF_NEUTRAL] = {1.0f, 0, 0, 0, 0},
    [AFF_JOY] = {1.10f, 0.8f, 1.0f, 0, -60},       /* más rápida, apenas más aguda (sin chillar), pausas cortas */
    [AFF_FEAR] = {1.10f, 1.2f, -0.5f, 1.0f, -50},  /* rápida, un poco aguda y temblorosa */
    [AFF_ANGER] = {1.05f, -0.6f, 2.5f, 0, -30},    /* firme y más fuerte */
    [AFF_DISGUST] = {0.94f, -1.0f, -1.0f, 0, 100}, /* un poco lenta y grave */
    [AFF_SADNESS] = {0.86f, -1.8f, -3.0f, 0, 240}, /* lenta, grave, bajita, pausas largas */
};

Prosody prosody_neutral(void)
{
    return STYLE[AFF_NEUTRAL];
}

bool prosody_is_neutral(const Prosody *p)
{
    return fabsf(p->tempo - 1) < 0.005f && fabsf(p->pitch) < 0.05f && fabsf(p->gain_db) < 0.1f && p->tremble < 0.02f &&
           fabsf(p->pause_ms) < 5;
}

Prosody prosody_for(const float weights[AFF_COUNT])
{
    /* Para la voz, la mezcla más marcada: los campos de las emociones se
       enciman y una furia con 27 % de temor ya temblaba. Se eleva al cuadrado,
       y lo que casi no pesa (menos de 8 %) no cuenta: en neutral, tu voz tal cual. */
    float sq[AFF_COUNT], sum = 0;
    for (int k = 0; k < AFF_COUNT; k++) {
        float w = weights[k] > 0 ? (weights[k] < 1 ? weights[k] : 1) : 0;
        sq[k] = w * w;
        sum += sq[k];
    }
    Prosody p = {1.0f, 0, 0, 0, 0};
    for (int k = 0; k < AFF_COUNT && sum > 0; k++) {
        float w = sq[k] / sum;
        if (k != AFF_NEUTRAL) w = (w - 0.08f) / 0.92f;
        if (!(w > 0)) continue;
        p.tempo += w * (STYLE[k].tempo - 1);
        p.pitch += w * STYLE[k].pitch;
        p.gain_db += w * STYLE[k].gain_db;
        p.tremble += w * STYLE[k].tremble;
        p.pause_ms += w * STYLE[k].pause_ms;
    }
    return p;
}

/* ------------------------------------------------------------ remuestrear */

static float sincf(float x)
{
    if (fabsf(x) < 1e-6f) return 1.0f;
    return sinf((float)M_PI * x) / ((float)M_PI * x);
}

#define PHASES 512
#define LOBES 8

/* Lee x a paso r (r > 1: más corto y más agudo). Sinc con ventana de Lanczos
   y el corte bajado a 1/r cuando se comprime, para no doblar agudos. */
static float *resample(const float *x, size_t n, float r, size_t *out_n)
{
    size_t m = (size_t)((double)n / r);
    float *y = xcalloc(m ? m : 1, sizeof *y);
    float c = r > 1 ? 1.0f / r : 1.0f; /* el corte */
    int half = (int)ceilf(LOBES / c), taps = 2 * half;
    float *tab = xmalloc(sizeof *tab * (size_t)PHASES * (size_t)taps);
    for (int ph = 0; ph < PHASES; ph++) {
        float frac = (float)ph / PHASES, sum = 0;
        float *row = tab + (size_t)ph * taps;
        for (int k = 0; k < taps; k++) {
            float t = (float)(k - half + 1) - frac; /* distancia al punto */
            float ct = c * t;
            row[k] = fabsf(ct) < LOBES ? c * sincf(ct) * sincf(ct / LOBES) : 0;
            sum += row[k];
        }
        for (int k = 0; k < taps; k++) row[k] /= sum; /* sin cambiar el volumen */
    }
    for (size_t j = 0; j < m; j++) {
        double pos = (double)j * r;
        long i0 = (long)floor(pos);
        int ph = (int)((pos - (double)i0) * PHASES);
        if (ph >= PHASES) ph = PHASES - 1;
        const float *row = tab + (size_t)ph * taps;
        float acc = 0;
        for (int k = 0; k < taps; k++) {
            long i = i0 + k - half + 1;
            if (i >= 0 && (size_t)i < n) acc += x[i] * row[k];
        }
        y[j] = acc;
    }
    free(tab);
    *out_n = m;
    return y;
}

/* ------------------------------------------------------------------ WSOLA */

/* Qué tan parecidos son a y b (n muestras, cada `step`), sobre la energía de b. */
static float match(const float *a, const float *b, int n, int step)
{
    float ab = 0, bb = 1e-9f;
    for (int i = 0; i < n; i += step) {
        ab += a[i] * b[i];
        bb += b[i] * b[i];
    }
    return ab / sqrtf(bb);
}

/* Estira el audio s veces (s > 1: más largo) sin cambiar el tono. */
static float *wsola(const float *x, size_t n, float s, int rate, size_t *out_n)
{
    int N = (int)(0.03f * rate) & ~1, hs = N / 2, tol = (int)(0.01f * rate);
    size_t want = (size_t)((double)n * s + 0.5);
    if (N < 16 || n < (size_t)N * 2) { /* muy corto: se queda como está */
        float *y = xmalloc(sizeof *y * (n ? n : 1));
        memcpy(y, x, sizeof *y * n);
        *out_n = n;
        return y;
    }
    /* la entrada con ceros a los lados para que ningún trozo se salga */
    size_t pad = (size_t)(N + tol);
    float *in = xcalloc(n + 2 * pad, sizeof *in);
    memcpy(in + pad, x, sizeof *x * n);
    size_t cap = want + (size_t)N * 2;
    float *out = xcalloc(cap, sizeof *out), *wsum = xcalloc(cap, sizeof *wsum);
    float *win = xmalloc(sizeof *win * (size_t)N);
    for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * (float)i / (float)N);
    double ha = hs / (double)s; /* cuánto se avanza en la entrada por trozo */
    long prev = 0;
    for (size_t k = 0;; k++) {
        size_t opos = k * (size_t)hs;
        if (opos >= want) break;
        long nominal = (long)llround((double)k * ha), best = nominal;
        if (k > 0) {
            /* lo que seguiría naturalmente al trozo anterior, contra los candidatos:
               primero de 4 en 4 (y cada 4 muestras) y luego fino alrededor */
            const float *nat = in + pad + prev + hs;
            float top = -1e30f;
            for (long d = -tol; d <= tol; d += 4) {
                float v = match(nat, in + pad + nominal + d, N, 4);
                if (v > top) top = v, best = nominal + d;
            }
            long center = best;
            top = -1e30f;
            for (long d = -3; d <= 3; d++) {
                long c = center + d;
                if (c < nominal - tol || c > nominal + tol) continue;
                float v = match(nat, in + pad + c, N, 1);
                if (v > top) top = v, best = c;
            }
        }
        if (best < -(long)tol) best = -(long)tol;
        if (best > (long)n + tol) best = (long)n + tol;
        const float *fr = in + pad + best;
        for (int i = 0; i < N && opos + (size_t)i < cap; i++) {
            out[opos + i] += fr[i] * win[i];
            wsum[opos + i] += win[i];
        }
        prev = best;
    }
    for (size_t i = 0; i < want; i++)
        if (wsum[i] > 1e-3f) out[i] /= wsum[i];
    free(in);
    free(win);
    free(wsum);
    *out_n = want;
    return out;
}

/* ---------------------------------------------------------------- temblor */

/* Cúbica (Catmull-Rom) entre muestras. */
static float at(const float *x, size_t n, double pos)
{
    long i = (long)floor(pos);
    float t = (float)(pos - (double)i), p[4];
    for (int k = 0; k < 4; k++) {
        long j = i - 1 + k;
        p[k] = j < 0 ? x[0] : (size_t)j >= n ? x[n - 1] : x[j];
    }
    return p[1] + 0.5f * t * (p[2] - p[0] + t * (2 * p[0] - 5 * p[1] + 4 * p[2] - p[3] + t * (3 * (p[1] - p[2]) + p[3] - p[0])));
}

/* La voz tiembla: el tono sube y baja un poco (vibrato, retrasando la señal
   de a poquito) y el volumen late, a unas 5-6 veces por segundo. */
static void tremble(float *x, size_t n, int rate, float amount)
{
    if (amount <= 0.01f || !n) return;
    if (amount > 1) amount = 1;
    float *src = xmalloc(sizeof *src * n);
    memcpy(src, x, sizeof *src * n);
    double ph = 0, ph2 = 0;
    for (size_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        double f = 5.4 + 0.8 * sin(2 * M_PI * 0.7 * t); /* no tan parejo: suena menos a máquina */
        ph += 2 * M_PI * f / rate;
        ph2 += 2 * M_PI * 6.3 / rate;
        /* ±3.5 % de tono: retraso de hasta D muestras con derivada D·π·f/rate */
        double D = amount * 0.035 * rate / (M_PI * 5.4);
        double delay = D * 0.5 * (1 - cos(ph));
        x[i] = at(src, n, (double)i - delay) * (float)(1 + 0.1 * amount * sin(ph2));
    }
    free(src);
}

/* ------------------------------------------------------ volumen y silencio */

/* Más fuerte sin tronar: arriba de 0.8 se dobla suave hacia 1. */
static void gain_limit(float *x, size_t n, float db)
{
    float g = powf(10.0f, db / 20.0f);
    const float T = 0.8f;
    for (size_t i = 0; i < n; i++) {
        float y = x[i] * g, a = fabsf(y);
        if (a > T) y = copysignf(T + (1 - T) * tanhf((a - T) / (1 - T)), y);
        x[i] = y;
    }
}

/* Cuánto silencio hay al final (muestras). */
static size_t tail_silence(const float *x, size_t n, int rate)
{
    float peak = 0;
    for (size_t i = 0; i < n; i++) peak = fmaxf(peak, fabsf(x[i]));
    float thr = fmaxf(0.004f, 0.03f * peak);
    size_t end = n;
    while (end > 0 && fabsf(x[end - 1]) <= thr) end--;
    return n - end;
}

static float *adjust_pause(float *x, size_t *n, int rate, float ms)
{
    size_t sil = tail_silence(x, *n, rate);
    long want = (long)sil + (long)(ms * (float)rate / 1000.0f);
    long keep = (long)(0.03f * rate), most = (long)sil + (long)(0.6f * rate);
    if (want < keep) want = sil < (size_t)keep ? (long)sil : keep;
    if (want > most) want = most;
    size_t m = *n - sil + (size_t)want;
    if (m == *n) return x;
    if (m < *n) {
        /* se corta dentro del silencio, con una bajadita de 5 ms para no tronar */
        size_t fade = (size_t)(0.005f * rate);
        for (size_t i = 0; i < fade && i < m; i++) x[m - 1 - i] *= (float)i / (float)fade;
        *n = m;
        return x;
    }
    x = xrealloc(x, sizeof *x * m);
    memset(x + *n, 0, sizeof *x * (m - *n));
    *n = m;
    return x;
}

/* ------------------------------------------------------------------ todo */

int16_t *prosody_apply(int16_t *pcm, size_t *n, int rate, const Prosody *p)
{
    /* menos de 20 ms no es una frase: se queda como está */
    if (!pcm || !p || rate <= 0 || *n < (size_t)rate / 50 || prosody_is_neutral(p)) return pcm;
    float tempo = p->tempo < 0.5f ? 0.5f : p->tempo > 2.0f ? 2.0f : p->tempo;
    float semis = p->pitch < -6 ? -6 : p->pitch > 6 ? 6 : p->pitch;
    size_t len = *n;
    float *x = xmalloc(sizeof *x * len);
    for (size_t i = 0; i < len; i++) x[i] = pcm[i] / 32768.0f;
    if (fabsf(semis) >= 0.05f || fabsf(tempo - 1) >= 0.005f) {
        /* el tono: remuestrear r veces (queda 1/r de largo y r veces más agudo)… */
        float r = powf(2.0f, semis / 12.0f);
        if (fabsf(semis) >= 0.05f) {
            size_t m;
            float *y = resample(x, len, r, &m);
            free(x);
            x = y, len = m;
        } else {
            r = 1;
        }
        /* …y el largo: de 1/r a 1/tempo del original */
        size_t m;
        float *y = wsola(x, len, r / tempo, rate, &m);
        free(x);
        x = y, len = m;
    }
    tremble(x, len, rate, p->tremble);
    if (fabsf(p->gain_db) >= 0.1f) gain_limit(x, len, p->gain_db);
    if (fabsf(p->pause_ms) >= 5) x = adjust_pause(x, &len, rate, p->pause_ms);
    if (!len) { /* no debería pasar: mejor como estaba que nada */
        free(x);
        return pcm;
    }
    int16_t *out = xrealloc(pcm, sizeof *out * len);
    for (size_t i = 0; i < len; i++) {
        float v = x[i] * 32767.0f;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
    }
    free(x);
    *n = len;
    return out;
}

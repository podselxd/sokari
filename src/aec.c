#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aec.h"
#include "fft.h"
#include "util.h"

#define B AEC_BLOCK
#define N (2 * AEC_BLOCK)
#define K (AEC_BLOCK + 1)
#define P AEC_PARTS
#define MIN_LEAK 0.005
#define RATE_HZ 16000.0

/* Media FFT (de 0 a N/2): lo demás es su espejo. */
typedef struct {
    float re[K], im[K];
} Spec;

struct Aec {
    Fft *fft;
    Spec X[P]; /* lo que sonaba, en frecuencia: X[(head + p) % P] es de hace p bloques */
    int head;
    Spec Wf[P], Wb[P]; /* el filtro que se usa y el de fondo, que aprende */
    float xprev[B];
    float power[K]; /* la potencia de lo que suena, promediada, por frecuencia */
    float prop[P];  /* cuánto aprende cada bloque del filtro: más el que tiene más eco */
    double Eh[K], Yh[K], Pey, Pyy;
    float leak; /* qué tanto del eco queda sin quitar (regresión de |E|² sobre |Y|²) */
    bool frozen; /* le hablas encima: la cuenta de cuánto se escapa no se mueve (ver aec_freeze) */
    bool adapted;
    double sum_adapt;
    double Davg1, Davg2, Dvar1, Dvar2; /* para decidir si el de fondo ya es mejor */
    unsigned count;
    float re[N], im[N];
};

Aec *aec_create(void)
{
    Aec *a = xcalloc(1, sizeof *a);
    a->fft = fft_create(N);
    a->Pey = a->Pyy = 1.0;
    a->leak = 1.0f;
    for (int p = 0; p < P; p++) a->prop[p] = 0.99f / P;
    return a;
}

void aec_destroy(Aec *a)
{
    if (!a) return;
    fft_destroy(a->fft);
    free(a);
}

float aec_leak(const Aec *a)
{
    return a->leak;
}

void aec_freeze(Aec *a, bool frozen)
{
    a->frozen = frozen;
}

void aec_copy(Aec *dst, const Aec *src)
{
    Fft *keep = dst->fft;
    memcpy(dst, src, sizeof *dst);
    dst->fft = keep;
}

/* [first, second] (NULL: ceros) a frecuencia. */
static void to_spec(Aec *a, const float *first, const float *second, Spec *out)
{
    for (int i = 0; i < B; i++) {
        a->re[i] = first ? first[i] : 0.0f;
        a->re[B + i] = second ? second[i] : 0.0f;
    }
    memset(a->im, 0, sizeof a->im);
    fft_run(a->fft, a->re, a->im, false);
    memcpy(out->re, a->re, sizeof out->re);
    memcpy(out->im, a->im, sizeof out->im);
}

/* De frecuencia al tiempo; first y second reciben cada mitad (NULL: no importa). */
static void from_spec(Aec *a, const Spec *s, float *first, float *second)
{
    a->re[0] = s->re[0];
    a->im[0] = 0;
    a->re[N / 2] = s->re[N / 2];
    a->im[N / 2] = 0;
    for (int k = 1; k < N / 2; k++) {
        a->re[k] = s->re[k];
        a->im[k] = s->im[k];
        a->re[N - k] = s->re[k];
        a->im[N - k] = -s->im[k];
    }
    fft_run(a->fft, a->re, a->im, true);
    const float scale = 1.0f / N;
    for (int i = 0; i < B; i++) {
        if (first) first[i] = a->re[i] * scale;
        if (second) second[i] = a->re[B + i] * scale;
    }
}

/* Lo que el filtro W dice que es el eco. */
static void filter_out(const Aec *a, const Spec *W, Spec *Y)
{
    memset(Y, 0, sizeof *Y);
    for (int p = 0; p < P; p++) {
        const Spec *x = &a->X[(a->head + p) % P], *w = &W[p];
        for (int k = 0; k < K; k++) {
            Y->re[k] += w->re[k] * x->re[k] - w->im[k] * x->im[k];
            Y->im[k] += w->re[k] * x->im[k] + w->im[k] * x->re[k];
        }
    }
}

/* Un bloque del filtro solo puede tener B muestras (lo demás sería eco que
   «da la vuelta»): se recorta en el tiempo. Uno por bloque, por turnos, más
   el primero siempre (el que más eco lleva). */
static void constrain(Aec *a, Spec *w)
{
    float t[B];
    from_spec(a, w, t, NULL);
    to_spec(a, t, NULL, w);
}

static void adjust_prop(Aec *a)
{
    float norm[P], max = 1.0f, sum = 1.0f;
    for (int p = 0; p < P; p++) {
        double t = 1.0;
        for (int k = 0; k < K; k++) t += (double)a->Wb[p].re[k] * a->Wb[p].re[k] + (double)a->Wb[p].im[k] * a->Wb[p].im[k];
        norm[p] = (float)sqrt(t);
        if (norm[p] > max) max = norm[p];
    }
    for (int p = 0; p < P; p++) {
        norm[p] += 0.1f * max;
        sum += norm[p];
    }
    for (int p = 0; p < P; p++) a->prop[p] = 0.99f * norm[p] / sum;
}

void aec_push_far(Aec *a, const float *ref)
{
    a->head = (a->head + P - 1) % P;
    to_spec(a, a->xprev, ref, &a->X[a->head]);
    memcpy(a->xprev, ref, sizeof a->xprev);
}

void aec_forget_far(Aec *a)
{
    memset(a->X, 0, sizeof a->X);
    memset(a->xprev, 0, sizeof a->xprev);
}

static double filter_energy(const Spec *W)
{
    double e = 0;
    for (int p = 0; p < P; p++)
        for (int k = 0; k < K; k++) e += (double)W[p].re[k] * W[p].re[k] + (double)W[p].im[k] * W[p].im[k];
    return e;
}

void aec_shift(Aec *a, int blocks)
{
    if (!blocks) return;
    const double before = filter_energy(a->Wf);
    Spec *both[2] = {a->Wf, a->Wb};
    for (int w = 0; w < 2; w++) {
        Spec *W = both[w];
        if (blocks > 0) {
            for (int p = 0; p < P; p++) {
                if (p + blocks < P) W[p] = W[p + blocks];
                else memset(&W[p], 0, sizeof W[p]);
            }
        } else {
            for (int p = P - 1; p >= 0; p--) {
                if (p + blocks >= 0) W[p] = W[p + blocks];
                else memset(&W[p], 0, sizeof W[p]);
            }
        }
    }
    aec_forget_far(a);
    a->Davg1 = a->Davg2 = a->Dvar1 = a->Dvar2 = 0;
    /* Si casi todo lo aprendido se salió (o no había aprendido nada), hay que
       aprenderlo otra vez: con el paso rápido del principio, porque el normal
       depende del eco que ya estima. */
    if (filter_energy(a->Wf) < 0.5 * before || a->leak > 0.5f) {
        a->adapted = false;
        a->sum_adapt = 0;
    }
}

/* Cuánto eco se escapa: cómo sube lo que queda (R, |E|²) cuando sube el eco
   (Y, |Y|²), frecuencia por frecuencia. Se promedia más rápido cuando lo que
   queda es sobre todo eco (sin que le hables encima). */
static float update_leak(const double *R, const double *Y, double See, double Syy, double *Eh, double *Yh,
                         double *sPey, double *sPyy, bool frozen)
{
    const double sa = B / RATE_HZ;
    double Pey = 0, Pyy = 0;
    for (int k = 0; k < K; k++) {
        const double eh = R[k] - Eh[k], yh = Y[k] - Yh[k];
        Pey += eh * yh;
        Pyy += yh * yh;
        Eh[k] = (1 - sa) * Eh[k] + sa * R[k];
        Yh[k] = (1 - sa) * Yh[k] + sa * Y[k];
    }
    Pyy = sqrt(Pyy);
    Pey = Pyy > 0 ? Pey / Pyy : 0;
    /* Lo que queda del eco no puede subir más que el eco mismo (ni bajar
       cuando él sube): si lo hace, es tu voz encima de la suya. Sin este
       tope, un solo cuadro de tu voz fuerte bastaba para inflar la cuenta. */
    if (Pey > Pyy) Pey = Pyy;
    if (Pey < 0) Pey = 0;
    double alpha = 0;
    if (See > 0 && !frozen) {
        double t = 2 * B / RATE_HZ * Syy;
        if (t > 0.5 * B / RATE_HZ * See) t = 0.5 * B / RATE_HZ * See;
        alpha = t / See;
    }
    *sPey = (1 - alpha) * *sPey + alpha * Pey;
    *sPyy = (1 - alpha) * *sPyy + alpha * Pyy;
    if (*sPyy < 1) *sPyy = 1;
    if (*sPey < MIN_LEAK * *sPyy) *sPey = MIN_LEAK * *sPyy;
    if (*sPey > *sPyy) *sPey = *sPyy;
    return (float)(*sPey / *sPyy);
}

void aec_process(Aec *a, const float *mic, const float *ref, float *out, AecStats *st)
{
    aec_push_far(a, ref);
    const Spec *X0 = &a->X[a->head];

    /* Lo que dice cada filtro que es el eco, y lo que queda al quitarlo. */
    Spec Y;
    float yf[B], yb[B], ef[B], eb[B];
    filter_out(a, a->Wf, &Y);
    from_spec(a, &Y, NULL, yf);
    filter_out(a, a->Wb, &Y);
    from_spec(a, &Y, NULL, yb);
    double Sxx = 0, Sdd = 0, Sff = 0, See = 0, Syy = 0, Sey = 0, Dbf = 10, Syf = 0;
    for (int i = 0; i < B; i++) {
        ef[i] = mic[i] - yf[i];
        eb[i] = mic[i] - yb[i];
        Sxx += (double)ref[i] * ref[i];
        Sdd += (double)mic[i] * mic[i];
        Sff += (double)ef[i] * ef[i];
        See += (double)eb[i] * eb[i];
        Syy += (double)yb[i] * yb[i];
        Sey += (double)eb[i] * yb[i];
        Dbf += (double)(yb[i] - yf[i]) * (yb[i] - yf[i]);
        Syf += (double)yf[i] * yf[i];
    }

    /* ¿El de fondo ya quita claramente más eco? Entonces es el que se usa.
       ¿Quita claramente menos (desaprendió con tu voz)? Vuelve al que se usa. */
    const double diff = Sff - See;
    a->Davg1 = 0.6 * a->Davg1 + 0.4 * diff;
    a->Davg2 = 0.85 * a->Davg2 + 0.15 * diff;
    a->Dvar1 = 0.36 * a->Dvar1 + 0.16 * Sff * Dbf;
    a->Dvar2 = 0.7225 * a->Dvar2 + 0.0225 * Sff * Dbf;
    if (diff * fabs(diff) > Sff * Dbf || a->Davg1 * fabs(a->Davg1) > 0.5 * a->Dvar1 ||
        a->Davg2 * fabs(a->Davg2) > 0.25 * a->Dvar2) {
        a->Davg1 = a->Davg2 = a->Dvar1 = a->Dvar2 = 0;
        memcpy(a->Wf, a->Wb, sizeof a->Wf);
        /* sin brincos: de lo que quitaba el anterior a lo del nuevo a lo largo del bloque */
        Sff = Syf = 0;
        for (int i = 0; i < B; i++) {
            const float t = 0.5f - 0.5f * cosf((float)M_PI * (i + 0.5f) / B);
            ef[i] = (1 - t) * ef[i] + t * eb[i];
            yf[i] = mic[i] - ef[i];
            Sff += (double)ef[i] * ef[i];
            Syf += (double)yf[i] * yf[i];
        }
    } else if (-diff * fabs(diff) > 4 * Sff * Dbf || -a->Davg1 * fabs(a->Davg1) > 4 * a->Dvar1 ||
               -a->Davg2 * fabs(a->Davg2) > 4 * a->Dvar2) {
        memcpy(a->Wb, a->Wf, sizeof a->Wb);
        memcpy(eb, ef, sizeof eb);
        memcpy(yb, yf, sizeof yb);
        See = Sff;
        Syy = Syf;
        Sey = 0;
        for (int i = 0; i < B; i++) Sey += (double)eb[i] * yb[i];
        a->Davg1 = a->Davg2 = a->Dvar1 = a->Dvar2 = 0;
    }

    /* Error y eco del de fondo, en frecuencia. */
    Spec E, Ys;
    to_spec(a, NULL, eb, &E);
    to_spec(a, NULL, yb, &Ys);
    double Rf[K], Yf[K];
    const float ss = 0.35f / P;
    for (int k = 0; k < K; k++) {
        Rf[k] = (double)E.re[k] * E.re[k] + (double)E.im[k] * E.im[k];
        Yf[k] = (double)Ys.re[k] * Ys.re[k] + (double)Ys.im[k] * Ys.im[k];
        const double xf = (double)X0->re[k] * X0->re[k] + (double)X0->im[k] * X0->im[k];
        a->power[k] = (float)((1 - ss) * a->power[k] + 1 + ss * xf);
    }

    a->leak = update_leak(Rf, Yf, See, Syy, a->Eh, a->Yh, &a->Pey, &a->Pyy, a->frozen);

    /* Qué parte de lo que queda es eco (y no tu voz): con eso se decide qué
       tanto aprender. */
    double RER = (0.0001 * Sxx + 3.0 * a->leak * Syy) / (See + 1);
    const double low = Sey * Sey / (1 + See * Syy);
    if (RER < low) RER = low;
    if (RER > 0.5) RER = 0.5;
    if (!a->adapted && a->sum_adapt > P && a->leak > 0.03f) a->adapted = true;
    float step[K];
    if (a->adapted) {
        for (int k = 0; k < K; k++) {
            double r = a->leak * Yf[k], e = Rf[k] + 1;
            if (r > 0.5 * e) r = 0.5 * e;
            r = 0.7 * r + 0.3 * RER * e;
            step[k] = (float)(r / (e * (a->power[k] + 10)));
        }
    } else {
        double rate = 0;
        if (Sxx > N * 1000.0 / 64) {
            double t = 0.25 * Sxx;
            if (t > 0.25 * See) t = 0.25 * See;
            rate = See > 0 ? t / See : 0;
        }
        for (int k = 0; k < K; k++) step[k] = (float)(rate / (a->power[k] + 10));
        a->sum_adapt += rate;
    }

    /* Aprende el de fondo (NLMS por frecuencia). */
    adjust_prop(a);
    for (int p = 0; p < P; p++) {
        const Spec *x = &a->X[(a->head + p) % P];
        Spec *w = &a->Wb[p];
        for (int k = 0; k < K; k++) {
            const float g = a->prop[p] * step[k];
            w->re[k] += g * (x->re[k] * E.re[k] + x->im[k] * E.im[k]);
            w->im[k] += g * (x->re[k] * E.im[k] - x->im[k] * E.re[k]);
        }
    }
    constrain(a, &a->Wb[0]);
    constrain(a, &a->Wb[1 + a->count % (P - 1)]);
    a->count++;

    if (st) {
        double cross = 0;
        for (int i = 0; i < B; i++) cross += (double)ef[i] * yf[i];
        st->mic = (float)Sdd;
        st->ref = (float)Sxx;
        st->echo = (float)Syf;
        st->out = (float)Sff;
        st->residual = (float)(a->leak * Syf);
        st->cross = (float)cross;
    }
    memcpy(out, ef, sizeof ef);
}

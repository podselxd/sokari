#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <time.h>
#endif

#include "aec.h"
#include "eco.h"
#include "fft.h"
#include "log.h"
#include "util.h"

#define FS MIC_RATE
#define BLOCKS (MIC_FRAME / AEC_BLOCK)
#define CLOCK_WIN 64          /* 5 s de cuadros del micrófono para su reloj */
#define MAX_SEGS 32
#define KEEP_SAMPLES (3 * FS) /* lo que sonó hace más de 3 s ya no se guarda */
#define LW 10000              /* para medir el retraso: 0.625 s de lo que se oyó */
#define LNEG 3200             /* de -200 ms (su voz se oye antes de lo que dice el reloj) */
#define LMAX (LNEG + 8000)    /* a +500 ms */
#define HIST (LW + LMAX)
#define EST_EVERY 8000 /* se vuelve a medir cada 0.5 s mientras suena algo */
#define EST_FFT 32768
#define EST_CONFIDENCE 8.0f
#define TALK_OVER 4.0f /* tu voz: 6 dB más fuerte que el eco que queda */
#define PRE_MARGIN (3 * AEC_BLOCK) /* 24 ms */
#define MISMATCH -0.25f /* más negativo: se está quitando un eco que ya no está ahí */
#define NO_DELAY INT32_MIN

_Static_assert(MIC_FRAME % AEC_BLOCK == 0, "un cuadro del micrófono son bloques enteros del cancelador");
_Static_assert(2 * LW + LMAX <= EST_FFT, "la correlación no da la vuelta");

typedef struct {
    unsigned id;
    int64_t start; /* en la cuenta del micrófono */
    size_t len;
    int rate;       /* la de lo que se mandó a la bocina (para cortarlo) */
    float *x;       /* a 16 kHz */
    int64_t cut_at; /* dónde iba el micrófono cuando se cortó (para reanudar) */
} Seg;

/* Lo comparten el micrófono, la bocina y el hilo de voz. */
static struct {
    SRWLOCK lock;
    int64_t offs[CLOCK_WIN]; /* µs en que se habría grabado la muestra 0 */
    int64_t when[CLOCK_WIN]; /* y cuándo llegó el cuadro que lo dijo */
    int noffs, ioffs;
    Seg segs[MAX_SEGS];
    int nsegs;
    unsigned next_id;
    volatile LONG reset;
} G = {SRWLOCK_INIT};

/* Solo el hilo de voz. */
static struct {
    Aec *aec;
    Aec *saved;                     /* cómo estaba antes del último cuadro */
    float last_ref[MIC_FRAME];      /* lo que sonaba en ese cuadro (ya alineado) */
    uint64_t next_pos;
    bool started, need_prime;
    int pre;   /* bloques que se atrasa lo que sonaba antes de restarlo */
    int delay; /* muestras (NO_DELAY: todavía no se sabe) */
    int cand;
    float dc_x, dc_y;
    float hfar[HIST], hmic[HIST]; /* lo último que sonó y lo que se oyó, alineados por la cuenta */
    int hfill, since;
    int quiet_frames;
    int talk_frames; /* cuadros que faltan con la cuenta del eco congelada (le hablaste encima) */
    Fft *fft;
    float *re1, *im1, *re2, *im2;
} V;

static uint64_t (*g_clock)(void);

static uint64_t now_us(void)
{
    if (g_clock) return g_clock();
#ifdef _WIN32
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

void eco_set_clock(uint64_t (*clock)(void))
{
    g_clock = clock;
}

/* ------------------------------------------------------ reloj y bocina --- */

void eco_mic_pushed(uint64_t total)
{
    const int64_t t = (int64_t)now_us(), off = t - (int64_t)(total * 1000000u / FS);
    AcquireSRWLockExclusive(&G.lock);
    G.offs[G.ioffs] = off;
    G.when[G.ioffs] = t;
    G.ioffs = (G.ioffs + 1) % CLOCK_WIN;
    if (G.noffs < CLOCK_WIN) G.noffs++;
    ReleaseSRWLockExclusive(&G.lock);
}

static void drop_segs(void)
{
    for (int i = 0; i < G.nsegs; i++) free(G.segs[i].x);
    G.nsegs = 0;
}

void eco_mic_reset(void)
{
    AcquireSRWLockExclusive(&G.lock);
    G.noffs = G.ioffs = 0;
    drop_segs();
    ReleaseSRWLockExclusive(&G.lock);
    InterlockedExchange(&G.reset, 1);
}

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* La muestra del micrófono que se está grabando ahora. El reloj de la
   tarjeta de sonido no va exacto con el de la PC (se adelanta o se atrasa un
   poco, en algunas mucho): se sigue esa diferencia con una recta sobre los
   últimos 5 s. Y de los cuadros, los que llegaron más pronto (los que menos
   se retrasaron en entregarse) son los que mejor dicen cuándo se grabaron;
   pero no el primero de todos: al arrancar, algunos servidores de sonido
   entregan de golpe audio que ya tenían. Un octavo de los más pronto. */
static int64_t mic_now_locked(void)
{
    const int n = G.noffs;
    const int64_t now = (int64_t)now_us(), t0 = G.when[(G.ioffs + CLOCK_WIN - 1) % CLOCK_WIN];
    double slope = 0, mt = 0, mo = 0;
    for (int i = 0; i < n; i++) {
        mt += (double)(G.when[i] - t0);
        mo += (double)(G.offs[i] - G.offs[0]);
    }
    mt /= n;
    mo /= n;
    if (n >= 8) {
        double stt = 0, sto = 0;
        for (int i = 0; i < n; i++) {
            const double dt = (double)(G.when[i] - t0) - mt, dof = (double)(G.offs[i] - G.offs[0]) - mo;
            stt += dt * dt;
            sto += dt * dof;
        }
        if (stt > 0) slope = sto / stt;
        if (slope > 0.01) slope = 0.01; /* más de 1 % sería otra cosa */
        if (slope < -0.01) slope = -0.01;
    }
    double res[CLOCK_WIN];
    for (int i = 0; i < n; i++)
        res[i] = (double)(G.offs[i] - G.offs[0]) - mo - slope * ((double)(G.when[i] - t0) - mt);
    qsort(res, (size_t)n, sizeof *res, cmp_double);
    const double off = (double)G.offs[0] + mo + slope * ((double)(now - t0) - mt) + res[n / 8];
    return (int64_t)(((double)now - off) * FS / 1e6);
}

/* A 16 kHz, con el volumen y el tope que le pone speaker_play: un filtro
   pasa bajas (sinc con ventana de Blackman, 32 muestras) en 256 fases. */
#define RS_HALF 16
#define RS_PHASES 256
static float *to_16k(const int16_t *pcm, size_t n, int rate, float gain, size_t *out_n)
{
    float *in = xmalloc(sizeof(float) * n);
    for (size_t i = 0; i < n; i++) {
        float v = (float)pcm[i] * gain;
        in[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    if (rate == FS) {
        *out_n = n;
        return in;
    }
    const double ratio = (double)FS / rate, fc = 0.45 * (ratio < 1 ? ratio : 1.0);
    float(*table)[2 * RS_HALF] = xmalloc(sizeof(float) * (RS_PHASES + 1) * 2 * RS_HALF); /* suenan dos cosas a la vez: cada una la suya */
    for (int ph = 0; ph <= RS_PHASES; ph++) {
        const double frac = (double)ph / RS_PHASES;
        for (int k = -RS_HALF + 1; k <= RS_HALF; k++) {
            const double x = frac - k;
            const double s = fabs(x) < 1e-9 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            const double w = fabs(x) >= RS_HALF ? 0
                                                : 0.42 + 0.5 * cos(M_PI * x / RS_HALF) + 0.08 * cos(2 * M_PI * x / RS_HALF);
            table[ph][k + RS_HALF - 1] = (float)(s * w);
        }
    }
    const size_t m = (size_t)floor((double)n * ratio);
    float *out = xmalloc(sizeof(float) * (m ? m : 1));
    for (size_t j = 0; j < m; j++) {
        const double t = (double)j / ratio;
        long i0 = (long)floor(t);
        const int ph = (int)lround((t - i0) * RS_PHASES);
        const float *h = table[ph];
        double acc = 0;
        for (int k = -RS_HALF + 1; k <= RS_HALF; k++) {
            const long i = i0 + k;
            if (i >= 0 && (size_t)i < n) acc += in[i] * h[k + RS_HALF - 1];
        }
        out[j] = (float)acc;
    }
    free(table);
    free(in);
    *out_n = m;
    return out;
}

static Seg *find_seg(unsigned id)
{
    for (int i = 0; id && i < G.nsegs; i++)
        if (G.segs[i].id == id) return &G.segs[i];
    return NULL;
}

/* Cómo sigue la cuenta: -1 empieza donde diga el reloj; 0 sigue a prev
   después de gap muestras (a rate); 1 sigue a prev después de lo que haya
   durado la pausa según el reloj. */
static unsigned play_add(int how, unsigned prev, size_t gap, const int16_t *pcm, size_t n, int rate, float gain)
{
    if (!pcm || !n || rate <= 0) return 0;
    size_t len;
    float *x = to_16k(pcm, n, rate, gain, &len);
    unsigned id = 0;
    AcquireSRWLockExclusive(&G.lock);
    if (G.noffs && len) { /* sin micrófono abierto no hay a qué restarle nada */
        const int64_t now = mic_now_locked();
        int64_t start = now;
        const Seg *p = how >= 0 ? find_seg(prev) : NULL;
        if (p && how == 0) start = p->start + (int64_t)p->len + (int64_t)((double)gap * FS / rate + 0.5);
        if (p && how == 1) start = p->start + (int64_t)p->len + (now - p->cut_at);
        if (G.nsegs == MAX_SEGS) {
            free(G.segs[0].x);
            memmove(&G.segs[0], &G.segs[1], sizeof(Seg) * (MAX_SEGS - 1));
            G.nsegs--;
        }
        id = ++G.next_id ? G.next_id : ++G.next_id;
        G.segs[G.nsegs++] = (Seg){id, start, len, rate, x, 0};
        x = NULL;
    }
    ReleaseSRWLockExclusive(&G.lock);
    free(x);
    return id;
}

unsigned eco_play_begin(const int16_t *pcm, size_t n, int rate, float gain)
{
    return play_add(-1, 0, 0, pcm, n, rate, gain);
}

unsigned eco_play_continue(unsigned prev, size_t gap, const int16_t *pcm, size_t n, int rate, float gain)
{
    return play_add(0, prev, gap, pcm, n, rate, gain);
}

unsigned eco_play_resume(unsigned prev, const int16_t *pcm, size_t n, int rate, float gain)
{
    return play_add(1, prev, 0, pcm, n, rate, gain);
}

void eco_play_cut(unsigned id, size_t played)
{
    if (!id) return;
    AcquireSRWLockExclusive(&G.lock);
    Seg *s = find_seg(id);
    if (s) {
        const size_t keep = (size_t)((double)played * FS / s->rate + 0.5);
        if (keep < s->len) s->len = keep;
        if (G.noffs) s->cut_at = mic_now_locked();
    }
    ReleaseSRWLockExclusive(&G.lock);
}

/* Lo que sonaba en [from, from + n) de la cuenta del micrófono. */
static void far_at(int64_t from, int n, float *out)
{
    memset(out, 0, sizeof(float) * (size_t)n);
    for (int i = 0; i < G.nsegs; i++) {
        const Seg *s = &G.segs[i];
        int64_t a = from > s->start ? from : s->start;
        int64_t b = from + n < s->start + (int64_t)s->len ? from + n : s->start + (int64_t)s->len;
        for (int64_t t = a; t < b; t++) out[t - from] += s->x[t - s->start];
    }
}

static void drop_old(int64_t pos)
{
    int w = 0;
    for (int i = 0; i < G.nsegs; i++) {
        if (G.segs[i].start + (int64_t)G.segs[i].len < pos - KEEP_SAMPLES) free(G.segs[i].x);
        else G.segs[w++] = G.segs[i];
    }
    G.nsegs = w;
}

/* --------------------------------------------------- medir el retraso --- */

/* Correlación cruzada con PHAT (solo la forma del espectro, no su volumen)
   entre lo que sonó y lo que se oyó, de 250 Hz a 3.8 kHz, para retrasos de
   -0.2 a 0.5 s (hfar va LNEG muestras adelantado). Devuelve el retraso
   (muestras) o NO_DELAY si no se ve claro. */
static int measure_delay(void)
{
    int active = 0, chunks = HIST / 320;
    for (int c = 0; c < chunks; c++) {
        double e = 0;
        for (int i = 0; i < 320; i++) e += (double)V.hfar[c * 320 + i] * V.hfar[c * 320 + i];
        if (e / 320 > 1e4) active++; /* más de 100 de 32768: está sonando algo */
    }
    if (active * 10 < chunks * 3) return NO_DELAY;
    if (!V.fft) {
        V.fft = fft_create(EST_FFT);
        V.re1 = xmalloc(sizeof(float) * EST_FFT);
        V.im1 = xmalloc(sizeof(float) * EST_FFT);
        V.re2 = xmalloc(sizeof(float) * EST_FFT);
        V.im2 = xmalloc(sizeof(float) * EST_FFT);
    }
    memset(V.re1, 0, sizeof(float) * EST_FFT);
    memset(V.im1, 0, sizeof(float) * EST_FFT);
    memset(V.re2, 0, sizeof(float) * EST_FFT);
    memset(V.im2, 0, sizeof(float) * EST_FFT);
    memcpy(V.re1, V.hfar, sizeof(float) * HIST);
    memcpy(V.re2, V.hmic + LMAX, sizeof(float) * LW);
    fft_run(V.fft, V.re1, V.im1, false);
    fft_run(V.fft, V.re2, V.im2, false);
    for (int k = 0; k < EST_FFT; k++) {
        const int kk = k <= EST_FFT / 2 ? k : EST_FFT - k;
        const double hz = (double)kk * FS / EST_FFT;
        if (hz < 250 || hz > 3800) {
            V.re1[k] = V.im1[k] = 0;
            continue;
        }
        /* conj(mic) · lo que sonó */
        const double gr = (double)V.re2[k] * V.re1[k] + (double)V.im2[k] * V.im1[k];
        const double gi = (double)V.re2[k] * V.im1[k] - (double)V.im2[k] * V.re1[k];
        const double w = pow(sqrt(gr * gr + gi * gi) + 1e-9, 0.8);
        V.re1[k] = (float)(gr / w);
        V.im1[k] = (float)(gi / w);
    }
    fft_run(V.fft, V.re1, V.im1, true);
    int best = 0;
    double sum2 = 0;
    for (int k = 0; k <= LMAX; k++) {
        sum2 += (double)V.re1[k] * V.re1[k];
        if (V.re1[k] > V.re1[best]) best = k;
    }
    const double rms = sqrt(sum2 / (LMAX + 1));
    if (rms <= 0 || V.re1[best] / rms < EST_CONFIDENCE) return NO_DELAY;
    return LMAX - LNEG - best;
}

static void set_delay(int d)
{
    V.delay = d;
    /* Con margen: lo que llega antes que el sonido directo (el filtro de la
       salida de audio, un rebote medido de más) todavía se puede restar. Si
       sale negativo, lo que sonaba se toma adelantado: se sabe desde antes
       todo lo que va a decir. */
    int pre = (int)floor((double)(d - PRE_MARGIN) / AEC_BLOCK);
    if (pre != V.pre) {
        aec_shift(V.aec, pre - V.pre);
        V.pre = pre;
        V.need_prime = true;
    }
    log_msg("Eco: mi voz llega al micrófono %d ms después de lo que marca el reloj.", d * 1000 / FS);
}

static void track_delay(const float *ref, const float *mic)
{
    memmove(V.hfar, V.hfar + MIC_FRAME, sizeof(float) * (HIST - MIC_FRAME));
    memmove(V.hmic, V.hmic + MIC_FRAME, sizeof(float) * (HIST - MIC_FRAME));
    memcpy(V.hfar + HIST - MIC_FRAME, ref, sizeof(float) * MIC_FRAME);
    memcpy(V.hmic + HIST - MIC_FRAME, mic, sizeof(float) * MIC_FRAME);
    if (V.hfill < HIST) V.hfill += MIC_FRAME;
    V.since += MIC_FRAME;
    if (V.hfill < HIST || V.since < EST_EVERY) return;
    V.since = 0;
    const int d = measure_delay();
    if (d == NO_DELAY) return;
    if (V.delay != NO_DELAY && abs(d - V.delay) <= AEC_BLOCK / 8) { /* lo mismo que ya se sabía */
        V.cand = NO_DELAY;
        return;
    }
    /* Un cambio se cree cuando dos medidas seguidas coinciden. */
    if (V.cand != NO_DELAY && abs(d - V.cand) <= AEC_BLOCK / 8) {
        set_delay(d);
        V.cand = NO_DELAY;
    } else {
        V.cand = d;
    }
}

int eco_delay_ms(void)
{
    return !V.aec || V.delay == NO_DELAY ? ECO_NO_DELAY : V.delay * 1000 / FS;
}

/* ------------------------------------------------------- el hilo de voz --- */

static void start_over(void)
{
    aec_destroy(V.aec);
    aec_destroy(V.saved);
    V.aec = aec_create();
    V.saved = aec_create();
    V.started = V.need_prime = false;
    V.pre = 0;
    V.delay = V.cand = NO_DELAY;
    V.hfill = V.since = 0;
    V.talk_frames = 0;
}

void eco_frame(uint64_t pos, const int16_t in[MIC_FRAME], int16_t out[MIC_FRAME], EcoInfo *info)
{
    if (InterlockedExchange(&G.reset, 0) || !V.aec) start_over();
    const bool jump = !V.started || pos != V.next_pos;
    const int64_t p = (int64_t)pos, from = p - (int64_t)V.pre * AEC_BLOCK;
    float far_aec[MIC_FRAME], far_ahead[MIC_FRAME], prime[AEC_PARTS * AEC_BLOCK];
    const bool prime_now = jump || V.need_prime;
    AcquireSRWLockExclusive(&G.lock);
    drop_old(p);
    far_at(from, MIC_FRAME, far_aec);
    far_at(p + LNEG, MIC_FRAME, far_ahead);
    if (prime_now) far_at(from - AEC_PARTS * AEC_BLOCK, AEC_PARTS * AEC_BLOCK, prime);
    ReleaseSRWLockExclusive(&G.lock);
    if (prime_now) {
        aec_forget_far(V.aec);
        for (int b = 0; b < AEC_PARTS; b++) aec_push_far(V.aec, prime + b * AEC_BLOCK);
        V.need_prime = false;
    }
    if (jump) {
        V.hfill = V.since = 0;
        V.dc_x = V.dc_y = 0;
        V.quiet_frames = 0;
    }
    V.started = true;
    V.next_pos = pos + MIC_FRAME;

    /* Sin la corriente directa del micrófono (algunos traen un desfase). */
    float m[MIC_FRAME], o[MIC_FRAME];
    for (int i = 0; i < MIC_FRAME; i++) {
        const float x = in[i];
        V.dc_y = x - V.dc_x + 0.995f * V.dc_y;
        V.dc_x = x;
        m[i] = V.dc_y;
    }
    /* Cómo estaba antes de este cuadro: si resulta ser tu voz encima de la
       suya (eco_talk_feed), se deshace lo que aprendió de él. */
    aec_copy(V.saved, V.aec);
    memcpy(V.last_ref, far_aec, sizeof far_aec);
    double mic = 0, fare = 0, oute = 0, res = 0, echo = 0, cross = 0;
    for (int b = 0; b < BLOCKS; b++) {
        AecStats st;
        aec_process(V.aec, m + b * AEC_BLOCK, far_aec + b * AEC_BLOCK, o + b * AEC_BLOCK, &st);
        mic += st.mic;
        fare += st.ref;
        oute += st.out;
        res += st.residual;
        echo += st.echo;
        cross += st.cross;
    }
    for (int i = 0; i < MIC_FRAME; i++) {
        const float v = roundf(o[i]);
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (V.talk_frames && !--V.talk_frames) aec_freeze(V.aec, false);
    /* Lo que sonaba ya alineado (el eco llega de 0 a 192 ms después): 3
       cuadros sin nada y ya no queda eco. */
    V.quiet_frames = fare / MIC_FRAME > 1e3 ? 0 : V.quiet_frames + 1;
    if (info) {
        info->playing = fare / MIC_FRAME > 1e3;
        info->quiet = V.quiet_frames >= 3;
        info->leak = aec_leak(V.aec);
        info->learned = info->leak < 0.15f;
        info->mic = (float)(mic / MIC_FRAME);
        info->out = (float)(oute / MIC_FRAME);
        info->residual = (float)(res / MIC_FRAME);
        info->mismatch = oute > 0 && echo > 0 ? (float)(cross / sqrt(oute * echo)) : 0.0f;
    }
    track_delay(far_ahead, m);
}

bool eco_talk_frame(bool voice, const EcoInfo *info)
{
    if (!voice) return false;
    if (info->quiet) return true;
    return info->learned && info->out > TALK_OVER * info->residual && info->mismatch > MISMATCH;
}

#define TALK_FREEZE_FRAMES 25 /* 2 s */

bool eco_talk_feed(EcoTalk *t, bool voice, const EcoInfo *info)
{
    const bool talk = eco_talk_frame(voice, info);
    /* Tu voz encima de la suya: lo que aprendió de este cuadro se deshace (era
       tu voz, no su eco) y la cuenta de cuánto eco se escapa no se mueve
       mientras hablas y 2 s después. */
    if (talk && V.aec) {
        if (!V.talk_frames) {
            aec_copy(V.aec, V.saved);
            for (int b = 0; b < BLOCKS; b++) aec_push_far(V.aec, V.last_ref + b * AEC_BLOCK);
        }
        aec_freeze(V.aec, true);
        V.talk_frames = TALK_FREEZE_FRAMES;
    }
    t->hist = ((t->hist << 1) | (talk ? 1u : 0u)) & 0xFu;
    int n = 0;
    for (unsigned h = t->hist; h; h >>= 1) n += (int)(h & 1u);
    return n >= 3;
}

void eco_reset(void)
{
    AcquireSRWLockExclusive(&G.lock);
    G.noffs = G.ioffs = 0;
    drop_segs();
    ReleaseSRWLockExclusive(&G.lock);
    InterlockedExchange(&G.reset, 0);
    start_over();
}

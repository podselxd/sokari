/* Quitarle al micrófono la voz de Sokari (aec.c, eco.c), en un cuarto
   simulado: su voz sale por la bocina con retraso, rebota en el cuarto, a
   veces la bocina satura, el micrófono entrega sus cuadros con retraso y
   brincos, y tú le hablas encima. Se mide cuánto eco quita, que mida bien
   con cuánto retraso llega, que no desaprenda cuando le hablas encima, que
   note que le hablas encima (y nunca con solo su eco) y que «Hey Sokari» se
   oiga encima de su voz. Las voces son las de tests/datos. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aec.h"
#include "eco.h"
#include "log.h"
#include "resource.h"
#include "resources.h"
#include "util.h"
#include "vad.h"
#include "wakeword.h"

#define FS 16000

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

/* ------------------------------------------------------------ las voces --- */

static int16_t *load_wav16k(const wchar_t *path, size_t *samples)
{
    size_t len;
    char *data = read_file_all(path, &len);
    if (!data || len < 44) {
        free(data);
        return NULL;
    }
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t sz;
        memcpy(&sz, data + off + 4, 4);
        if (!memcmp(data + off, "data", 4)) {
            if (off + 8 + sz > len) sz = (uint32_t)(len - off - 8);
            int16_t *pcm = malloc(sz);
            memcpy(pcm, data + off + 8, sz);
            *samples = sz / 2;
            free(data);
            return pcm;
        }
        off += 8 + sz + (sz & 1);
    }
    free(data);
    return NULL;
}

static int16_t *datos(const wchar_t *name, size_t *n)
{
    wchar_t *dir = exe_dir(), *rel = path_join(L"../../tests/datos", name), *path = path_join(dir, rel);
    int16_t *pcm = load_wav16k(path, n);
    free(dir);
    free(rel);
    free(path);
    return pcm;
}

static uint32_t g_rng = 12345;

static float frand(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)((g_rng >> 8) & 0xFFFFFF) / 16777216.0f;
}

static float grand(void)
{
    float u = frand() + 1e-7f, v = frand();
    return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

static double rms(const float *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return n ? sqrt(s / n) : 0;
}

/* ------------------------------------------------------------- el cuarto --- */

static uint64_t g_now;
static uint64_t fake_clock(void)
{
    return g_now;
}

typedef struct {
    int in_ms;     /* lo que tarda el micrófono en entregar cada cuadro */
    int jitter_ms; /* y de más, al azar */
    float rt60;    /* reverberación (s) */
    float gain;    /* amplitud del eco respecto a lo que sonó */
    float drive;   /* 0: bocina lineal; más: satura */
    float noise;   /* ruido del cuarto (RMS) */
    float reverb;  /* energía de la reverberación respecto al sonido directo (0: 0.25, -6 dB) */
} Room;

typedef struct {
    const int16_t *pcm; /* lo que se manda a la bocina */
    size_t n;
    int rate;
    const float *sound; /* cómo suena de verdad, a 16 kHz */
    size_t nsound;
    double at_s;  /* cuándo se manda */
    int out_ms;   /* y lo que tarda en sonar */
    size_t cut;   /* se corta después de tantas muestras (a rate); 0: no */
} Play;

typedef struct {
    Room room;
    Play plays[2];
    int nplays;
    const float *user; /* tu voz */
    size_t nnear;
    double near_at[3];
    int nnear_at;
    float near_gain;
    double total_s;
    /* lo que resulta */
    size_t n;
    float *echo, *nearsig, *mic;
    int16_t *mic16, *out16;
    EcoInfo *info;
    bool *talk; /* ¿se notaba que le hablas encima? (cuadro a cuadro, como en voice.c) */
    int nframes;
} Scene;

/* La respuesta del cuarto: el sonido directo (la bocina está cerca del
   micrófono), un par de rebotes cercanos (la mesa, la pantalla) y la
   reverberación del cuarto, que se apaga en rt60 y trae `reverb` de la
   energía del sonido directo. */
static float *room_ir(const Room *r, int *len)
{
    int n = (int)(0.15f * FS);
    float *h = calloc((size_t)n, sizeof *h);
    h[0] = 1.0f;
    h[37] += 0.4f;  /* la mesa */
    h[101] -= 0.3f; /* la pantalla */
    double tail = 0;
    float *t = calloc((size_t)n, sizeof *t);
    for (int k = 60; k < n; k++) {
        t[k] = expf(-6.9f * k / (r->rt60 * FS)) * grand();
        tail += (double)t[k] * t[k];
    }
    const double want = r->reverb > 0 ? r->reverb : 0.25;
    for (int k = 60; k < n; k++) h[k] += (float)(t[k] * sqrt(want / tail));
    free(t);
    double e = 0;
    for (int k = 0; k < n; k++) e += (double)h[k] * h[k];
    for (int k = 0; k < n; k++) h[k] = (float)(h[k] * r->gain / sqrt(e));
    *len = n;
    return h;
}

static void scene_run(Scene *s)
{
    s->n = (size_t)(s->total_s * FS) / MIC_FRAME * MIC_FRAME;
    s->echo = calloc(s->n, sizeof(float));
    s->nearsig = calloc(s->n, sizeof(float));
    s->mic = calloc(s->n, sizeof(float));
    s->mic16 = calloc(s->n, sizeof(int16_t));
    s->out16 = calloc(s->n, sizeof(int16_t));
    s->nframes = (int)(s->n / MIC_FRAME);
    s->info = calloc((size_t)s->nframes, sizeof(EcoInfo));
    s->talk = calloc((size_t)s->nframes, sizeof(bool));
    int hl;
    float *h = room_ir(&s->room, &hl);
    for (int p = 0; p < s->nplays; p++) {
        const Play *pl = &s->plays[p];
        size_t nsound = pl->nsound;
        if (pl->cut) nsound = (size_t)((double)pl->cut * FS / pl->rate);
        const long place = lround((pl->at_s + pl->out_ms / 1000.0) * FS);
        for (size_t i = 0; i < nsound; i++) {
            float x = pl->sound[i];
            if (s->room.drive > 0) x = 32767.0f * tanhf(s->room.drive * x / 32767.0f) / s->room.drive;
            for (int k = 0; k < hl; k++) {
                const long t = place + (long)i + k;
                if (t >= 0 && (size_t)t < s->n) s->echo[t] += h[k] * x;
            }
        }
    }
    free(h);
    for (int j = 0; j < s->nnear_at; j++) {
        const long place = lround(s->near_at[j] * FS);
        for (size_t i = 0; i < s->nnear; i++)
            if ((size_t)place + i < s->n) s->nearsig[place + i] += s->near_gain * s->user[i];
    }
    for (size_t i = 0; i < s->n; i++) {
        float v = s->echo[i] + s->nearsig[i] + s->room.noise * grand();
        s->mic[i] = v;
        s->mic16[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lroundf(v));
    }

    /* El tiempo pasa: el micrófono entrega cuadros, se manda (y se corta) lo
       que suena, y el hilo de voz procesa cada cuadro en cuanto llega. */
    eco_reset();
    eco_set_clock(fake_clock);
    Listener *l = listener_create();
    EcoTalk t = {0};
    unsigned ids[2] = {0, 0};
    bool begun[2] = {false, false}, cut[2] = {false, false};
    for (int f = 0; f < s->nframes; f++) {
        const uint64_t push = (uint64_t)(f + 1) * 80000u + (uint64_t)s->room.in_ms * 1000u +
                              (uint64_t)(frand() * s->room.jitter_ms * 1000);
        for (int p = 0; p < s->nplays; p++) {
            const Play *pl = &s->plays[p];
            const uint64_t at = (uint64_t)(pl->at_s * 1e6);
            if (!begun[p] && at < push) {
                g_now = at;
                ids[p] = eco_play_begin(pl->pcm, pl->n, pl->rate, 1.0f);
                begun[p] = true;
            }
            const uint64_t cut_at = at + (uint64_t)((double)pl->cut * 1e6 / pl->rate);
            if (begun[p] && pl->cut && !cut[p] && cut_at < push) {
                g_now = cut_at;
                eco_play_cut(ids[p], pl->cut);
                cut[p] = true;
            }
        }
        g_now = push;
        eco_mic_pushed((uint64_t)(f + 1) * MIC_FRAME);
        eco_frame((uint64_t)f * MIC_FRAME, s->mic16 + (size_t)f * MIC_FRAME, s->out16 + (size_t)f * MIC_FRAME,
                  &s->info[f]);
        /* Lo mismo que hace voice.c con cada cuadro: el detector de voz sobre
           lo que queda y si eso cuenta como que le hablas encima. */
        bool voice = listener_feed(l, s->out16 + (size_t)f * MIC_FRAME, 150);
        s->talk[f] = eco_talk_feed(&t, voice, &s->info[f]);
    }
    eco_set_clock(NULL);
    listener_destroy(l);
}

static void scene_free(Scene *s)
{
    free(s->echo);
    free(s->nearsig);
    free(s->mic);
    free(s->mic16);
    free(s->out16);
    free(s->info);
    free(s->talk);
}

/* Cuánto eco quitó entre a y b (s), donde no hablas tú. */
static double erle(const Scene *s, double a, double b)
{
    size_t i0 = (size_t)(a * FS), i1 = (size_t)(b * FS);
    if (i1 > s->n) i1 = s->n;
    double in = 0, out = 0;
    for (size_t i = i0; i < i1; i++) {
        in += (double)s->mic16[i] * s->mic16[i];
        out += (double)s->out16[i] * s->out16[i];
    }
    return 10 * log10((in + 1) / (out + 1));
}

/* Con ECO_TRAZA=1: cuánto eco quita segundo a segundo. */
static void trace(const Scene *s)
{
    if (!getenv("ECO_TRAZA")) return;
    printf("      por segundo:");
    for (int k = 0; k + 1 <= (int)(s->n / FS); k++) printf(" %.0f", erle(s, k, k + 1));
    printf("\n");
}

/* ¿Cuándo tu voz suena de verdad? (cuadros donde tu voz sola pasa de 300 RMS) */
static bool near_active(const Scene *s, int f)
{
    return rms(s->nearsig + (size_t)f * MIC_FRAME, MIC_FRAME) > 300;
}

/* Los avisos de que le hablas encima: cuántos hubo, cuántos sin que
   hablaras (false_alarms) y en qué cuadro, los que sí fueron tuyos (hasta
   nfirst). */
static int talk_onsets(const Scene *s, int *first, int nfirst, double skip_s, int *false_alarms)
{
    int onsets = 0, fa = 0, got = 0;
    bool in = false;
    for (int f = 0; f < s->nframes; f++) {
        bool on = s->talk[f];
        if (on && !in && f * 0.08 >= skip_s) {
            onsets++;
            /* ¿había voz tuya hace poco (0.5 s) o ahora? */
            bool yours = false;
            for (int g = f - 6; g <= f; g++)
                if (g >= 0 && near_active(s, g)) yours = true;
            if (!yours) fa++;
            else if (got < nfirst) first[got++] = f;
        }
        in = on;
    }
    if (false_alarms) *false_alarms = fa;
    return onsets;
}

/* -------------------------------------------------------- las pruebas --- */

static float *g_far16;
static size_t g_nfar;
static int16_t *g_far_pcm;
static float *g_near;
static size_t g_nnear;

static Scene base_scene(double total_s)
{
    Scene s = {0};
    s.room = (Room){.in_ms = 10, .jitter_ms = 5, .rt60 = 0.3f, .gain = 0.5f, .drive = 0, .noise = 10};
    s.plays[0] = (Play){g_far_pcm, g_nfar, FS, g_far16, g_nfar, 0.5, 60, 0};
    s.nplays = 1;
    s.total_s = total_s;
    return s;
}

static void test_room(void)
{
    printf("-- un cuarto normal (su voz 60 ms después de mandarla, rebotes de 0.3 s) --\n");
    Scene s = base_scene(12);
    scene_run(&s);
    trace(&s);
    int d = eco_delay_ms();
    printf("      retraso medido: %d ms (de verdad: 70) · eco quitado: %.1f dB\n", d, erle(&s, 4, 11.5));
    check(d >= 68 && d <= 72, "mide con cuánto retraso le llega su voz (±2 ms)");
    check(erle(&s, 4, 11.5) >= 20, "le quita al micrófono 20 dB o más de su voz");
    int fa;
    talk_onsets(&s, NULL, 0, 0, &fa);
    check(fa == 0, "con solo su eco nunca cree que le hablas encima");
    scene_free(&s);

    printf("-- retraso grande (bocina Bluetooth: 250 ms) --\n");
    s = base_scene(12);
    s.plays[0].out_ms = 230;
    s.room.in_ms = 20;
    scene_run(&s);
    trace(&s);
    d = eco_delay_ms();
    printf("      retraso medido: %d ms (de verdad: 250) · eco quitado: %.1f dB\n", d, erle(&s, 5, 11.5));
    check(d >= 248 && d <= 252, "mide un retraso de 250 ms");
    check(erle(&s, 5, 11.5) >= 18, "y también quita su voz");
    talk_onsets(&s, NULL, 0, 0, &fa);
    check(fa == 0, "sin creer que le hablas encima");
    scene_free(&s);

    printf("-- bocina que satura (volumen alto en una bocina chica) --\n");
    s = base_scene(12);
    s.room.drive = 3.0f;
    s.room.gain = 0.8f;
    scene_run(&s);
    printf("      eco quitado: %.1f dB\n", erle(&s, 4, 11.5));
    check(erle(&s, 4, 11.5) >= 8, "quita menos (lo que satura no se puede restar), pero quita");
    talk_onsets(&s, NULL, 0, 0, &fa);
    check(fa == 0, "y tampoco cree que le hablas encima con lo que no pudo quitar");
    scene_free(&s);

    printf("-- su voz a 24 kHz (como la de Windows) --\n");
    /* A 24 kHz con el mismo filtro con que se oiría (sinc con ventana). */
    size_t n24 = g_nfar * 3 / 2;
    int16_t *p24 = malloc(n24 * sizeof *p24);
    for (size_t j = 0; j < n24; j++) {
        double t = j * 2.0 / 3.0, acc = 0;
        long i0 = (long)floor(t);
        for (long i = i0 - 15; i <= i0 + 16; i++) {
            if (i < 0 || (size_t)i >= g_nfar) continue;
            double x = t - i, sc = fabs(x) < 1e-9 ? 1 : sin(M_PI * x) / (M_PI * x);
            double w = 0.42 + 0.5 * cos(M_PI * x / 16) + 0.08 * cos(2 * M_PI * x / 16);
            acc += g_far16[i] * sc * w;
        }
        p24[j] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : lround(acc));
    }
    s = base_scene(12);
    s.plays[0].pcm = p24;
    s.plays[0].n = n24;
    s.plays[0].rate = 24000;
    scene_run(&s);
    printf("      eco quitado: %.1f dB\n", erle(&s, 4, 11.5));
    check(erle(&s, 4, 11.5) >= 18, "la pasa a 16 kHz bien: quita su voz igual");
    scene_free(&s);
    free(p24);
}

/* De cada vez que hablaste, ¿hubo un aviso en los 560 ms desde que empezó
   a sonar tu voz? */
static int caught_in_time(const Scene *s, const int *onsets, int n)
{
    int caught = 0;
    for (int j = 0; j < s->nnear_at; j++) {
        int start = -1;
        for (int f = (int)(s->near_at[j] / 0.08); f < s->nframes && start < 0; f++)
            if (near_active(s, f)) start = f;
        for (int k = 0; k < n; k++)
            if (start >= 0 && onsets[k] >= start && onsets[k] - start <= 7) {
                caught++;
                break;
            }
    }
    return caught;
}

/* Le hablas encima a `below` dB debajo de su eco, en ese cuarto: cuántas de
   2 veces se nota (en 600 ms o menos) y cuántos avisos falsos. */
static void talk_case(Room room, double below, int *caught, int *fa, double *e)
{
    Scene probe = base_scene(16);
    probe.room = room;
    scene_run(&probe);
    double echo_rms = rms(probe.echo + 2 * FS, 2 * FS);
    scene_free(&probe);
    Scene s = base_scene(16);
    s.room = room;
    s.user = g_near;
    s.nnear = g_nnear;
    s.near_at[0] = 5.0;
    s.near_at[1] = 11.0;
    s.nnear_at = 2;
    s.near_gain = (float)(echo_rms * pow(10, -below / 20) / rms(g_near, g_nnear));
    scene_run(&s);
    int first[16];
    int got = talk_onsets(&s, first, 16, 0, fa) - *fa;
    *caught = caught_in_time(&s, first, got < 16 ? got : 16);
    *e = erle(&s, 9.3, 11);
    scene_free(&s);
}

static void test_harder(void)
{
    printf("-- más difícil --\n");
    Room normal = {.in_ms = 10, .jitter_ms = 5, .rt60 = 0.3f, .gain = 0.5f, .noise = 10};
    int caught, fa;
    double e;
    Room reverb = normal;
    reverb.rt60 = 0.6f;
    reverb.reverb = 1.0f;
    talk_case(reverb, 6, &caught, &fa, &e);
    printf("      cuarto con mucho eco (tanta reverberación como sonido directo): %.1f dB quitados, se notó %d de 2, "
           "%d falsos\n", e, caught, fa);
    check(e >= 12 && caught == 2 && fa == 0, "en un cuarto con mucho eco también");
    talk_case(normal, 12, &caught, &fa, &e);
    printf("      le hablas quedito (su eco 12 dB más fuerte): se notó %d de 2, %d falsos\n", caught, fa);
    check(caught >= 1 && fa == 0, "hablándole quedito, por lo menos una de dos (y nunca en falso)");
    /* Fuerte y largo, sin que ella se pause (como con «Callarme con solo
       hablarme encima» apagado): la segunda vez se tiene que notar igual. */
    talk_case(normal, -10, &caught, &fa, &e);
    printf("      le hablas fuerte (10 dB más que su eco), dos veces: se notó %d de 2, %d falsos, %.1f dB quitados "
           "después\n",
           caught, fa, e);
    check(caught == 2 && fa == 0, "hablándole fuerte, las dos veces (lo de la primera no estorba a la segunda)");
    Room fan = normal;
    fan.noise = 150;
    talk_case(fan, 6, &caught, &fa, &e);
    printf("      con un ventilador (ruido de 150): %.1f dB quitados, se notó %d de 2, %d falsos\n", e, caught, fa);
    check(caught == 2 && fa == 0, "con ruido de ventilador también");
    Room loud = normal;
    loud.gain = 1.2f;
    loud.drive = 2.0f;
    talk_case(loud, 6, &caught, &fa, &e);
    printf("      bocina fuerte que satura: %.1f dB quitados, se notó %d de 2, %d falsos\n", e, caught, fa);
    check(fa == 0, "bocina fuerte que satura: nunca en falso");
}

static void test_talk(WakeWord *ww)
{
    printf("-- le hablas encima (su eco 6 dB más fuerte que tu voz) --\n");
    Scene s = base_scene(16);
    s.user = g_near;
    s.nnear = g_nnear;
    s.near_at[0] = 5.0;
    s.near_at[1] = 11.0;
    s.nnear_at = 2;
    /* Tu voz 6 dB abajo de su eco. */
    Scene probe = base_scene(16);
    scene_run(&probe);
    double echo_rms = rms(probe.echo + 2 * FS, 2 * FS);
    scene_free(&probe);
    s.near_gain = (float)(echo_rms * 0.5 / rms(g_near, g_nnear));
    scene_run(&s);
    int first[16], fa;
    int onsets = talk_onsets(&s, first, 16, 0, &fa);
    int yours = onsets - fa < 16 ? onsets - fa : 16;
    /* Cuánto tarda en notarse cada vez (desde que empieza a sonar tu voz). */
    int late[2] = {-1, -1};
    for (int j = 0; j < 2; j++) {
        int start = -1;
        for (int f = (int)(s.near_at[j] / 0.08); f < s.nframes && start < 0; f++)
            if (near_active(&s, f)) start = f;
        for (int k = 0; k < yours && late[j] < 0; k++)
            if (first[k] >= start) late[j] = (first[k] - start) * 80;
    }
    printf("      avisos: %d (falsos: %d) · se nota a los %d ms y a los %d ms de que empiezas\n", onsets, fa, late[0],
           late[1]);
    check(late[0] >= 0 && late[0] <= 400, "la primera vez se nota a los 400 ms o antes");
    check(late[1] >= 0 && late[1] <= 400, "la segunda también");
    check(fa == 0, "ningún aviso sin que hables");
    double before = erle(&s, 3, 5), after = erle(&s, 9.2, 11);
    printf("      eco quitado antes de que hablaras: %.1f dB · después: %.1f dB\n", before, after);
    check(after >= before - 3, "no desaprende su eco por tu voz");
    /* Lo que queda de ti: tu voz contra lo que queda de su eco. */
    double keep = 0, err = 0;
    for (size_t i = (size_t)(11.0 * FS); i < (size_t)(14.0 * FS) && i < s.n; i++) {
        keep += (double)s.nearsig[i] * s.nearsig[i];
        double e = s.out16[i] - s.nearsig[i];
        err += e * e;
    }
    printf("      tu voz queda %.1f dB arriba de lo que queda de su eco (en el micrófono: %.1f dB)\n",
           10 * log10(keep / (err + 1)), 20 * log10(0.5));
    check(10 * log10(keep / (err + 1)) >= 10, "tu voz queda clara (10 dB arriba de su eco o más)");

    if (ww) {
        float best_out = 0, best_mic = 0;
        ww_reset(ww);
        for (int f = 0; f < s.nframes; f++) {
            float sc = ww_process(ww, s.out16 + (size_t)f * MIC_FRAME);
            if (f * 0.08 >= 10.5 && sc > best_out) best_out = sc;
        }
        ww_reset(ww);
        for (int f = 0; f < s.nframes; f++) {
            float sc = ww_process(ww, s.mic16 + (size_t)f * MIC_FRAME);
            if (f * 0.08 >= 10.5 && sc > best_mic) best_mic = sc;
        }
        printf("      «Hey Sokari» encima de su voz: %.3f sin su eco (sin quitarlo: %.3f; umbral 0.4)\n", best_out,
               best_mic);
        check(best_out > 0.4f, "«Hey Sokari» se detecta encima de su voz");
    }
    scene_free(&s);

    printf("-- le hablas cuando no suena nada --\n");
    s = base_scene(8);
    s.nplays = 0;
    s.user = g_near;
    s.nnear = g_nnear;
    s.near_at[0] = 2.0;
    s.nnear_at = 1;
    s.near_gain = 1.0f;
    scene_run(&s);
    onsets = talk_onsets(&s, first, 1, 0, &fa);
    printf("      eco quitado (no hay): %.2f dB · avisos: %d\n", erle(&s, 0, 8), onsets);
    check(fabs(erle(&s, 0, 8)) < 0.5, "tu voz pasa igual");
    check(onsets == 1 && fa == 0, "y se nota que le hablas");
    scene_free(&s);
}

static void test_cut(void)
{
    printf("-- se corta a media frase --\n");
    Scene s = base_scene(10);
    s.plays[0].cut = (size_t)(3.5 * FS);
    scene_run(&s);
    /* Después del corte (y su eco) solo queda el ruido del cuarto: nada de
       «restar» algo que ya no sonó. */
    size_t a = (size_t)(4.6 * FS), b = (size_t)(9.5 * FS);
    double out = 0;
    for (size_t i = a; i < b; i++) out += (double)s.out16[i] * s.out16[i];
    out = sqrt(out / (b - a));
    printf("      después del corte queda %.1f RMS (el ruido del cuarto: %.1f)\n", out, s.room.noise);
    check(out < 2.5 * s.room.noise, "no inventa eco después del corte");
    scene_free(&s);

    printf("-- cambia el retraso (otra bocina a media plática) --\n");
    s = base_scene(20);
    s.plays[1] = s.plays[0];
    s.plays[1].at_s = 10.0;
    s.plays[1].out_ms = 160;
    s.plays[0].cut = (size_t)(8.5 * FS);
    s.nplays = 2;
    scene_run(&s);
    int d = eco_delay_ms();
    printf("      retraso al final: %d ms (de verdad: 170) · eco quitado al final: %.1f dB\n", d, erle(&s, 14, 19.5));
    check(d >= 168 && d <= 172, "se da cuenta del retraso nuevo");
    check(erle(&s, 14, 19.5) >= 15, "y vuelve a quitar su voz");
    int fa;
    talk_onsets(&s, NULL, 0, 0, &fa);
    printf("      avisos (nadie le habla): %d\n", fa);
    check(fa == 0, "y mientras se ajusta no cree que le hablas encima");
    scene_free(&s);
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    if (getenv("ECO_TRAZA")) log_to_console(1);
    size_t n1, n2, n3;
    int16_t *a = datos(L"hey_safari.wav", &n1), *b = datos(L"oye_socorro.wav", &n2), *c = datos(L"hey_sokari.wav", &n3);
    if (!a || !b || !c) {
        printf("FALLA no encontré las voces de tests/datos\n");
        return 1;
    }
    /* Lo que dice Sokari: los pedazos con voz de dos grabaciones, de corrido
       con pausas cortas como entre frases (200 ms), hasta 20 s. */
    g_nfar = 20 * FS;
    g_far16 = calloc(g_nfar, sizeof(float));
    g_far_pcm = calloc(g_nfar, sizeof(int16_t));
    size_t w = 0;
    for (int rep = 0; w < g_nfar; rep++) {
        const int16_t *src = rep % 2 ? b : a;
        size_t ns = rep % 2 ? n2 : n1;
        for (size_t f = 0; f + 320 <= ns && w < g_nfar; f += 320) {
            double e = 0;
            for (int i = 0; i < 320; i++) e += (double)src[f + i] * src[f + i];
            if (sqrt(e / 320) < 300) continue;
            for (int i = 0; i < 320 && w < g_nfar; i++, w++) g_far_pcm[w] = src[f + i];
        }
        w += FS / 5;
    }
    for (size_t i = 0; i < g_nfar; i++) g_far16[i] = g_far_pcm[i];
    g_nnear = n3;
    g_near = malloc(n3 * sizeof(float));
    for (size_t i = 0; i < n3; i++) g_near[i] = c[i];
    free(a);
    free(b);
    free(c);

    size_t blen = 0, wlen = 0;
    const void *blob = res_data(IDR_WAKEWORD, &blen), *word = res_data(IDR_HEY_SOKARI, &wlen);
    WakeWord *ww = word ? ww_create(blob, blen, word, wlen) : NULL;
    if (!ww) printf("(sin el modelo de \"Hey Sokari\": se omite esa prueba)\n");

    test_room();
    test_talk(ww);
    test_harder();
    test_cut();

    ww_destroy(ww);
    free(g_far16);
    free(g_far_pcm);
    free(g_near);
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

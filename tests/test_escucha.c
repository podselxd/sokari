/* Que te escuche solo mientras hablas: con voz de verdad (las grabaciones de
   tests/datos pegadas como una frase) y ruidos de mentira encima (ruido rosa
   y blanco, el zumbido de un aparato, silencio), revisa que la orden empiece
   con tu voz, termine poco después de que te callas aunque siga el ruido, se
   corte a los 30 s, y que el puro ruido no se mande a transcribir. Nada de
   micrófono: todo es audio armado aquí. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "util.h"
#include "vad.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static int16_t *load_wav16k(const wchar_t *name, size_t *samples)
{
    wchar_t *dir = exe_dir(), *rel = path_join(L"../../tests/datos", name), *path = path_join(dir, rel);
    size_t len;
    char *data = read_file_all(path, &len);
    free(dir);
    free(rel);
    free(path);
    if (!data || len < 44) return NULL;
    for (size_t off = 12; off + 8 <= len;) {
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

static double rms(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return n ? sqrt(s / (double)n) : 0;
}

/* Una "frase": cada grabación recortada a donde hay voz, con 0.25 s entre
   una y otra (como pausas entre palabras). */
static int16_t *sentence(size_t *n)
{
    const wchar_t *names[] = {L"hey_sokari.wav", L"oye_socorro.wav", L"hey_safari.wav"};
    int16_t *out = calloc(16000 * 20, sizeof *out);
    size_t len = 0;
    for (int k = 0; k < 3; k++) {
        size_t m;
        int16_t *w = load_wav16k(names[k], &m);
        if (!w) continue;
        long first = -1, last = -1;
        for (size_t i = 0; i + 320 <= m; i += 320)
            if (rms(w + i, 320) > 400) {
                if (first < 0) first = (long)i;
                last = (long)(i + 320);
            }
        if (first >= 0) {
            first = first > 1600 ? first - 1600 : 0;
            last = last + 1600 < (long)m ? last + 1600 : (long)m;
            if (len) len += 4000;
            memcpy(out + len, w + first, sizeof *out * (size_t)(last - first));
            len += (size_t)(last - first);
        }
        free(w);
    }
    *n = len;
    return out;
}

typedef enum { N_SILENCE, N_PINK, N_WHITE, N_HUM, N_TALK } Noise;

/* Una tele (o alguien platicando) atrás: otra voz, con sus pausas. */
static int16_t *g_talk;
static size_t g_ntalk;
static double g_talk_rms;

static double g_b0, g_b1, g_b2;
static double noise_sample(Noise k, long i)
{
    double r = rand() / (double)RAND_MAX * 2 - 1, t = i / 16000.0;
    switch (k) {
    case N_PINK:
        g_b0 = 0.99765 * g_b0 + r * 0.0990460;
        g_b1 = 0.96300 * g_b1 + r * 0.2965164;
        g_b2 = 0.57000 * g_b2 + r * 1.0526913;
        return (g_b0 + g_b1 + g_b2 + r * 0.1848) / 3.0;
    case N_WHITE: return r;
    case N_HUM: return 0.6 * sin(2 * M_PI * 60 * t) + 0.25 * sin(2 * M_PI * 120 * t) + 0.15 * sin(2 * M_PI * 180 * t);
    case N_TALK: return g_ntalk ? g_talk[(size_t)(i + 5000) % g_ntalk] / g_talk_rms : 0;
    default: return 0;
    }
}

typedef struct {
    bool heard;            /* empezó a grabar */
    double after_s;        /* cuánto después de que te callas terminó */
    double sent_s;         /* cuánto audio se mandaría a transcribir (0: nada) */
    double recorded_s;     /* cuánto duró la orden */
} Result;

/* 6 s de fondo (Sokari esperando "Hey Sokari" y aprendiendo el ruido),
   luego la orden: speech (repetida reps veces, con pausa pause_s entre
   repeticiones) a snr_db sobre el ruido, y 12 s más de ruido. */
/* Cómo cambia tu volumen durante la orden (1 = igual). */
typedef enum { V_SAME, V_SHOUT, V_BANG, V_DROP } Volume;
static Volume g_volume;
static double g_drop = 0.3; /* V_DROP: cuánto baja (0.3 = 10 dB) */

static double volume_at(long s, long len)
{
    switch (g_volume) {
    case V_SHOUT: return s < 8000 ? 5.0 : 1.0;   /* «¡Sokari!» gritado (+14 dB), luego normal */
    case V_BANG: return s < 4000 ? 10.0 : 1.0;   /* un golpe o tos muy fuerte (+20 dB) al empezar */
    case V_DROP: return s < len / 2 ? 1.0 : g_drop; /* te volteas: la otra mitad más bajito */
    default: return 1.0;
    }
}

static Result run(Noise noise, double snr_db, int reps, double pause_s, int end_frames)
{
    srand(7);
    g_b0 = g_b1 = g_b2 = 0;
    size_t ns;
    int16_t *one = sentence(&ns);
    double speech_rms = rms(one, ns);
    long gap = (long)(pause_s * 16000);
    long speech_len = (long)(ns * (size_t)reps) + gap * (reps - 1);
    long pre = 16000 * 6, lead = 4000, post = 16000 * 12, total = pre + lead + speech_len + post;
    double acc = 0;
    for (long i = 0; i < 16000; i++) {
        double v = noise_sample(noise, i);
        acc += v * v;
    }
    double gain = noise == N_SILENCE ? 0 : speech_rms / pow(10, snr_db / 20) / sqrt(acc / 16000);
    int16_t *x = malloc(sizeof *x * (size_t)total);
    for (long i = 0; i < total; i++) {
        double v = noise_sample(noise, i) * gain + (rand() / (double)RAND_MAX * 2 - 1) * 30;
        long s = i - pre - lead;
        if (s >= 0 && s < speech_len) {
            long in = s % ((long)ns + gap);
            if (in < (long)ns) v += one[in] * volume_at(s, speech_len);
        }
        x[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(one);

    Listener *l = listener_create();
    const float silence = 90; /* el umbral calibrado de tu log */
    Result r = {0};
    long i = 0;
    for (; i + MIC_FRAME <= pre; i += MIC_FRAME) {
        listener_feed(l, x + i, silence);
        listener_learn(l, x + i); /* como Sokari esperando "Hey Sokari" */
    }
    Recording rec;
    rec_begin(&rec, end_frames);
    rec_set_background(&rec, listener_background(l));
    for (; i + MIC_FRAME <= total; i += MIC_FRAME)
        if (rec_feed(&rec, x + i, listener_feed(l, x + i, silence))) break;
    long end = i + MIC_FRAME, speech_end = pre + lead + speech_len;
    r.heard = rec.heard;
    r.after_s = (double)(end - speech_end) / 16000;
    r.recorded_s = (double)rec.frames * MIC_FRAME / 16000;
    size_t n;
    int16_t *out = rec_take(&rec, &n, NULL);
    r.sent_s = out ? (double)n / 16000 : 0;
    free(out);
    rec_free(&rec);
    listener_destroy(l);
    free(x);
    return r;
}

/* Cuánto de la frase es voz de verdad (sin los márgenes ni las pausas). */
static double speech_core(void)
{
    size_t ns;
    int16_t *one = sentence(&ns);
    int voiced = 0;
    for (size_t i = 0; i + 320 <= ns; i += 320) voiced += rms(one + i, 320) > 400;
    free(one);
    return voiced * 0.02;
}

static void test_escucha(void)
{
    size_t ns;
    free(sentence(&ns));
    /* La tele: otras voces (cualquier voz sirve) hablando de corrido. */
    size_t n1, n2;
    int16_t *a = load_wav16k(L"oye_socorro.wav", &n1), *b = load_wav16k(L"hey_safari.wav", &n2);
    if (a && b) {
        g_talk = malloc(sizeof *g_talk * (n1 + n2));
        g_ntalk = 0;
        for (int k = 0; k < 2; k++) {
            const int16_t *src = k ? b : a;
            size_t ns2 = k ? n2 : n1;
            for (size_t f = 0; f + 320 <= ns2; f += 320)
                if (rms(src + f, 320) > 300) {
                    memcpy(g_talk + g_ntalk, src + f, sizeof *g_talk * 320);
                    g_ntalk += 320;
                }
        }
        g_talk_rms = rms(g_talk, g_ntalk);
    }
    free(a);
    free(b);
    double speech = (double)ns / 16000, core = speech_core();
    printf("-- tu voz, con y sin ruido (frase de %.1f s, %.1f s de voz) --\n", speech, core);
    static const struct {
        Noise noise;
        double snr;
        const char *name;
    } CASES[] = {{N_SILENCE, 0, "en silencio"},
                 {N_PINK, 10, "con ruido rosa 10 dB abajo de tu voz"},
                 {N_PINK, 6, "con ruido rosa 6 dB abajo"},
                 {N_WHITE, 6, "con ruido blanco 6 dB abajo"},
                 {N_HUM, 6, "con el zumbido de un aparato 6 dB abajo"},
                 {N_TALK, 12, "con una tele hablando 12 dB abajo (dijiste «adiós» y ya)"}};
    for (size_t k = 0; k < sizeof CASES / sizeof *CASES; k++) {
        Result r = run(CASES[k].noise, CASES[k].snr, 1, 0, end_silence_frames(END_NORMAL));
        char what[200];
        snprintf(what, sizeof what, "%s: termina %.1f s después de que te callas y manda %.1f s", CASES[k].name,
                 r.after_s, r.sent_s);
        /* Con la tele, lo que se manda trae un poco de ella al principio (lo de
           antes de que empezaras): lo que importa es que termina. */
        const double extra = CASES[k].noise == N_TALK ? 1.4 : 0.8;
        check(r.heard && r.after_s > 0 && r.after_s < 1.2 && r.sent_s >= core && r.sent_s < speech + extra, what);
    }

    /* Que la tele no cuente no debe cortarte: gritar, un golpe o bajar la voz
       a media orden (aunque sea larga) nunca terminan la orden antes. */
    static const struct {
        Volume volume;
        double drop;
        int reps;
        Noise noise;
        double snr;
        const char *name;
    } VOLUMES[] = {{V_SHOUT, 1, 1, N_PINK, 20, "gritas «¡Sokari!» y sigues normal"},
                   {V_BANG, 1, 1, N_PINK, 20, "un golpe o una tos muy fuerte al empezar"},
                   {V_DROP, 0.3, 1, N_PINK, 20, "te volteas y la mitad la dices 10 dB más bajito"},
                   {V_DROP, 0.3, 3, N_PINK, 20, "lo mismo en una orden de 11 s"},
                   {V_DROP, 0.25, 4, N_PINK, 20, "y 12 dB más bajito en una de 15 s"},
                   {V_SAME, 1, 4, N_TALK, 12, "con la tele atrás, una orden de 15 s"}};
    for (size_t k = 0; k < sizeof VOLUMES / sizeof *VOLUMES; k++) {
        g_volume = VOLUMES[k].volume;
        g_drop = VOLUMES[k].drop;
        Result r = run(VOLUMES[k].noise, VOLUMES[k].snr, VOLUMES[k].reps, 0.25, end_silence_frames(END_NORMAL));
        g_volume = V_SAME;
        char what[200];
        snprintf(what, sizeof what, "%s: no te corta (termina %.1f s después de que te callas, manda %.1f s)",
                 VOLUMES[k].name, r.after_s, r.sent_s);
        const double limit = VOLUMES[k].noise == N_TALK ? 2.0 : 1.2;
        check(r.heard && r.after_s > 0 && r.after_s < limit && r.sent_s >= VOLUMES[k].reps * core, what);
    }

    printf("-- el puro ruido no es una orden --\n");
    static const struct {
        Noise noise;
        double snr;
        const char *name;
    } NOISES[] = {{N_PINK, 6, "ruido rosa fuerte"}, {N_WHITE, 6, "ruido blanco fuerte"}, {N_HUM, 6, "zumbido fuerte"}};
    for (size_t k = 0; k < sizeof NOISES / sizeof *NOISES; k++) {
        /* Mismo ruido, pero la frase nunca llega: reps = 0. */
        Result r = run(NOISES[k].noise, NOISES[k].snr, 0, 0, end_silence_frames(END_NORMAL));
        char what[200];
        snprintf(what, sizeof what, "%s sin que hables: no manda nada a transcribir (esperó %.1f s)", NOISES[k].name,
                 r.recorded_s > 0 ? r.recorded_s : 5.0);
        check(!r.heard && r.sent_s == 0, what);
    }

    printf("-- largo y corto --\n");
    Result r = run(N_PINK, 10, 12, 0.25, end_silence_frames(END_NORMAL));
    char what[200];
    snprintf(what, sizeof what, "si hablas más de 30 s seguidos, corta a los 30 (grabó %.1f s)", r.recorded_s);
    check(r.heard && r.recorded_s <= 30.1 && r.recorded_s >= 29.5, what);
    r = run(N_SILENCE, 0, 5, 0.25, end_silence_frames(END_NORMAL));
    snprintf(what, sizeof what, "si hablas 18 s, no espera a los 30: termina %.1f s después", r.after_s);
    check(r.heard && r.after_s > 0 && r.after_s < 1.2, what);

    Result corta = run(N_SILENCE, 0, 1, 0, end_silence_frames(END_SHORT));
    Result larga = run(N_SILENCE, 0, 1, 0, end_silence_frames(END_LONG));
    Result normal = run(N_SILENCE, 0, 1, 0, end_silence_frames(END_NORMAL));
    snprintf(what, sizeof what, "«Poco», «Normal» y «Más»: termina %.1f, %.1f y %.1f s después", corta.after_s,
             normal.after_s, larga.after_s);
    check(corta.after_s < normal.after_s && normal.after_s < larga.after_s && larga.after_s < 1.6, what);

    /* Con «Más» (1.2 s), una pausa de 0.7 s en medio no termina la orden,
       pero se manda acortada. */
    Result pausa = run(N_SILENCE, 0, 2, 0.7, end_silence_frames(END_LONG));
    Result junta = run(N_SILENCE, 0, 2, 0.25, end_silence_frames(END_LONG));
    snprintf(what, sizeof what,
             "una pausa larga en medio no corta la orden y se manda acortada (grabó %.1f s, manda %.1f s; sin la "
             "pausa manda %.1f s)",
             pausa.recorded_s, pausa.sent_s, junta.sent_s);
    check(pausa.heard && pausa.sent_s >= 2 * core && pausa.sent_s - junta.sent_s < 0.35 &&
              pausa.recorded_s - pausa.sent_s > 0.3,
          what);
}

static void test_bajar_volumen(void)
{
    printf("-- bajar el volumen mientras escucha --\n");
    DuckState d = system_duck(0.3f);
    printf("      (esta máquina: %s)\n", d.before < 0 ? "sin salida de audio o en silencio" : "lo bajó y lo regresa");
    system_unduck(d);
    check(true, "bajar y regresar el volumen no truena, haya o no salida de audio");
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    test_escucha();
    test_bajar_volumen();
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

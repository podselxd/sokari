/* El eco de verdad, con el servidor de sonido: Sokari habla por las bocinas
   y el micrófono es lo que suena en ellas (el «monitor» de las bocinas), así
   su voz llega al micrófono con los retrasos reales del servidor. Como en
   voice.c: cada cuadro pasa por el cancelador de eco y, si se nota que le
   hablas encima, se pausa (speaker_hold) hasta que te callas y sigue. Tu voz
   suena por otro lado (no pasa por speaker_play: Sokari no la conoce), dos
   veces. Se revisa que mida el retraso, que le quite su voz al micrófono
   (antes y después de las pausas, sin volver a aprender), que en pausa no
   «reste» lo que ya no sonó, y que las dos veces que le hablas se noten, sin
   avisos cuando solo habla ella. Sin servidor de sonido o sin las bocinas de
   prueba de la CI (compilar.yml), lo dice y no lo cuenta. */
#include <windows.h>

#include <math.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "eco.h"
#include "linux/proc.h"
#include "log.h"
#include "util.h"
#include "vad.h"

#define FS 16000
#define MAX_FRAMES 600

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

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

/* Lo de cada cuadro que llegó mientras hablaba. */
typedef struct {
    uint64_t t0;
    int nframes;
    double mic[MAX_FRAMES], out[MAX_FRAMES];
    uint64_t at[MAX_FRAMES]; /* ms desde que empezó a sonar */
    bool onset[MAX_FRAMES];  /* aquí se notó que le hablas */
    bool held[MAX_FRAMES];   /* en pausa */
    /* una pausa a propósito, para probar pausar y seguir */
    uint64_t hold_at_ms, hold_ms;
    bool holding, hold_done;
    /* la pausa porque le hablaste, hasta que te callas */
    bool talk_hold;
    int heard, silent;
    Listener *l;
    EcoTalk t;
} Run;

static void take_frames(Run *r)
{
    int16_t f[MIC_FRAME], clean[MIC_FRAME];
    uint64_t pos;
    while (r->nframes < MAX_FRAMES && mic_read_nowait_pos(f, &pos)) {
        EcoInfo info;
        eco_frame(pos, f, clean, &info);
        int k = r->nframes++;
        double m = 0, o = 0;
        for (int i = 0; i < MIC_FRAME; i++) {
            m += (double)f[i] * f[i];
            o += (double)clean[i] * clean[i];
        }
        r->mic[k] = m / MIC_FRAME;
        r->out[k] = o / MIC_FRAME;
        r->at[k] = GetTickCount64() - r->t0;
        r->held[k] = r->holding || r->talk_hold;
        bool voice = listener_feed(r->l, clean, 150);
        if (r->talk_hold) {
            /* Como talk_heard() en voice.c: te oye hasta que te callas (0.64 s)
               y, como si no hubiera entendido nada, sigue. */
            if (voice) {
                r->heard++;
                r->silent = 0;
            } else if (++r->silent >= 8) {
                r->talk_hold = false;
                r->t.hist = 0;
                speaker_hold(false);
            }
            continue;
        }
        if (eco_talk_feed(&r->t, voice, &info) && !r->holding) {
            r->onset[k] = true;
            r->talk_hold = true;
            r->heard = r->silent = 0;
            speaker_hold(true);
        }
    }
}

static bool cb(float level, void *ctx)
{
    Run *r = ctx;
    take_frames(r);
    uint64_t now = GetTickCount64() - r->t0;
    if (!r->hold_done && !r->holding && !r->talk_hold && now >= r->hold_at_ms) {
        speaker_hold(true);
        r->holding = true;
    } else if (r->holding && now >= r->hold_at_ms + r->hold_ms) {
        speaker_hold(false);
        r->holding = false;
        r->hold_done = true;
    }
    return true;
}

/* Tu voz, por otro lado (no pasa por speaker_play: Sokari no la conoce),
   dos veces. */
typedef struct {
    const int16_t *pcm;
    size_t n;
    uint64_t t0, at_ms[2], started[2];
} Other;

static DWORD WINAPI other_voice(LPVOID arg)
{
    Other *o = arg;
    pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = FS, .channels = 1};
    pa_buffer_attr ba = {.maxlength = (uint32_t)-1, .tlength = FS * 2 / 10, .prebuf = (uint32_t)-1,
                         .minreq = (uint32_t)-1, .fragsize = (uint32_t)-1};
    int err = 0;
    pa_simple *s = pa_simple_new(NULL, "prueba", PA_STREAM_PLAYBACK, NULL, "Tu voz", &ss, NULL, &ba, &err);
    if (!s) return 0;
    for (int i = 0; i < 2; i++) {
        while (GetTickCount64() < o->t0 + o->at_ms[i]) Sleep(5);
        o->started[i] = GetTickCount64() - o->t0;
        pa_simple_write(s, o->pcm, o->n * sizeof(int16_t), &err);
        pa_simple_drain(s, &err);
    }
    pa_simple_free(s);
    return 0;
}

static double db(double a, double b)
{
    return 10 * log10((a + 1) / (b + 1));
}

int wmain(void)
{
    size_t n1, n2, n3;
    int16_t *a = datos(L"hey_safari.wav", &n1), *b = datos(L"oye_socorro.wav", &n2), *you = datos(L"hey_sokari.wav", &n3);
    if (!a || !b || !you) {
        printf("FALLA no encontré las voces de tests/datos\n");
        return 1;
    }
    /* El micrófono: el monitor de las bocinas de prueba, puesto de
       predeterminado mientras dura la prueba. */
    int code = -1;
    const char *get_src[] = {"pactl", "get-default-source", NULL};
    char *before = proc_run(get_src, NULL, 0, 5000, 4096, NULL, &code);
    const char *list[] = {"pactl", "list", "short", "sources", NULL};
    char *sources = code == 0 ? proc_run(list, NULL, 0, 5000, 65536, NULL, &code) : NULL;
    bool have = sources && strstr(sources, "bocinas.monitor");
    free(sources);
    const char *set_mon[] = {"pactl", "set-default-source", "bocinas.monitor", NULL};
    if (have) free(proc_run(set_mon, NULL, 0, 5000, 0, NULL, &code));
    if (!have || code != 0 || !mic_start("")) {
        printf("      (sin las bocinas de prueba y su monitor: no se prueba el eco de verdad)\n");
        printf("\n0/0 pruebas pasaron\n");
        free(before);
        return 0;
    }
    /* Lo que dice: los pedazos con voz de dos grabaciones, 13 s. */
    size_t nfar = 13 * FS, w = 0;
    int16_t *say = calloc(nfar, sizeof *say);
    for (int rep = 0; w < nfar; rep++) {
        const int16_t *src = rep % 2 ? b : a;
        size_t ns = rep % 2 ? n2 : n1;
        for (size_t f = 0; f + 320 <= ns && w < nfar; f += 320) {
            double e = 0;
            for (int i = 0; i < 320; i++) e += (double)src[f + i] * src[f + i];
            if (sqrt(e / 320) < 300) continue;
            for (int i = 0; i < 320 && w < nfar; i++, w++) say[w] = (int16_t)(src[f + i] / 2);
        }
        w += FS / 5;
    }
    /* Tu voz sin el silencio de antes y de después. */
    size_t y0 = 0, y1 = n3;
    while (y0 + 320 <= n3 && fabs((double)you[y0 + 160]) < 300) y0 += 160;
    while (y1 > y0 + 320 && fabs((double)you[y1 - 160]) < 300) y1 -= 160;

    /* Que el micrófono ya esté dando audio a su ritmo: el reloj del eco sale
       de ahí (el servidor de prueba tarda en arrancarlo). */
    uint64_t tm = GetTickCount64();
    int16_t first[MIC_FRAME];
    while (!mic_read(first, 200) && GetTickCount64() - tm < 5000) {
    }
    Sleep(500);
    mic_flush();

    printf("-- Sokari habla y se oye a sí misma; le hablas encima dos veces --\n");
    Run *r = calloc(1, sizeof *r);
    r->l = listener_create();
    r->hold_at_ms = 5300;
    r->hold_ms = 600;
    r->t0 = GetTickCount64();
    Other other = {you + y0, y1 - y0, r->t0, {6500, 10000}, {0, 0}};
    HANDLE th = CreateThread(NULL, 0, other_voice, &other, 0, NULL);
    bool played = speaker_play(say, nfar, FS, 1.0f, cb, r);
    uint64_t took = GetTickCount64() - r->t0;
    uint64_t end = GetTickCount64() + 1500;
    while (GetTickCount64() < end) {
        take_frames(r);
        Sleep(40);
    }
    WaitForSingleObject(th, 15000);
    CloseHandle(th);
    int d = eco_delay_ms();
    printf("      sonó %llu ms (lo suyo: %d ms, más las pausas) · retraso medido: %d ms · tu voz a los %llu y %llu ms\n",
           (unsigned long long)took, (int)(nfar * 1000 / FS), d, (unsigned long long)other.started[0],
           (unsigned long long)other.started[1]);
    check(played, "habla completo, con las pausas en medio");
    check(took + 300 >= nfar * 1000 / FS + r->hold_ms, "las pausas sí detuvieron lo que decía (y siguió donde iba)");
    check(d != ECO_NO_DELAY, "mide con cuánto retraso se oye a sí misma");

    /* ¿Cuándo sonaba tu voz? (con margen para lo que tarda en llegar) */
    bool yours[MAX_FRAMES] = {false};
    for (int k = 0; k < r->nframes; k++)
        for (int i = 0; i < 2; i++)
            if (other.started[i] && r->at[k] + 100 >= other.started[i] &&
                r->at[k] <= other.started[i] + (y1 - y0) * 1000 / FS + 800)
                yours[k] = true;
    /* Eco quitado donde solo habla ella. Aprende en unos 4 s (mide el
       retraso y se va afinando: ~12 dB al segundo, ~20 a los 3): de los 2 a
       los 3 s ya debe quitar bastante; de los 4 s a la primera pausa y
       después de la primera vez que le hablaste (sin volver a aprender), lo
       de siempre. */
    double m0 = 0, o0 = 0, m1 = 0, o1 = 0, m2 = 0, o2 = 0, hold_out = 0;
    int nh = 0, after_resume = -1;
    for (int k = 1; k < r->nframes; k++)
        if (r->held[k - 1] && !r->held[k] && other.started[0] && r->at[k] > other.started[0]) {
            after_resume = k;
            break;
        }
    for (int k = 0; k < r->nframes; k++) {
        if (r->held[k] || yours[k]) continue;
        if (r->at[k] >= 2000 && r->at[k] < 3000) {
            m0 += r->mic[k];
            o0 += r->out[k];
        }
        if (r->at[k] >= 4000 && r->at[k] < r->hold_at_ms) {
            m1 += r->mic[k];
            o1 += r->out[k];
        }
        if (after_resume >= 0 && k >= after_resume + 6 && other.started[1] && r->at[k] + 200 < other.started[1]) {
            m2 += r->mic[k];
            o2 += r->out[k];
        }
    }
    for (int k = 0; k < r->nframes; k++)
        if (r->held[k] && k >= 4 && r->held[k - 4] && !yours[k]) {
            hold_out += r->out[k];
            nh++;
        }
    printf("      eco quitado de los 2 a los 3 s: %.1f dB · ya que aprendió: %.1f dB · después de que le hablaste: "
           "%.1f dB · en pausa quedó %.1f RMS\n",
           db(m0, o0), db(m1, o1), db(m2, o2), nh ? sqrt(hold_out / nh) : -1.0);
    check(m0 > 0 && db(m0, o0) >= 10, "aprende rápido: a los 2 s ya le quita a su voz 10 dB o más");
    check(m1 > 0 && db(m1, o1) >= 18, "ya que aprendió, le quita 18 dB o más");
    check(m2 > 0 && db(m2, o2) >= 18, "y después de que le hablaste encima también (sin volver a aprender)");
    check(nh > 0 && sqrt(hold_out / nh) < 100, "en pausa no «resta» lo que ya no sonó");

    int caught = 0, fa = 0;
    long long late[2] = {-1, -1};
    for (int k = 0; k < r->nframes; k++) {
        if (!r->onset[k]) continue;
        if (!yours[k]) {
            fa++;
            continue;
        }
        for (int i = 0; i < 2; i++)
            if (late[i] < 0 && other.started[i] && r->at[k] + 100 >= other.started[i] &&
                r->at[k] < other.started[i] + 1500) {
                late[i] = (long long)r->at[k] - (long long)other.started[i];
                caught++;
                break;
            }
    }
    printf("      le hablaste 2 veces: se notó %d (a los %lld y %lld ms) · avisos sin que hablaras: %d\n", caught, late[0],
           late[1], fa);
    check(fa == 0, "sin avisos mientras solo habla ella");
    check(caught == 2, "las dos veces que le hablas encima se notan (y se pausa)");

    mic_stop();
    if (before) { /* el micrófono predeterminado, como estaba */
        before[strcspn(before, "\r\n")] = 0;
        const char *restore[] = {"pactl", "set-default-source", before, NULL};
        free(proc_run(restore, NULL, 0, 5000, 0, NULL, &code));
        free(before);
    }
    listener_destroy(r->l);
    free(r);
    free(say);
    free(a);
    free(b);
    free(you);
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

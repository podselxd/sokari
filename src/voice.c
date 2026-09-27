/* Hilo de voz: micrófono -> "Hey Sokari" -> grabación del comando -> Groq ->
   herramientas -> respuesta hablada. Más un hilo aparte para la síntesis
   (SAPI) que va generando oración por oración mientras se reproduce la
   anterior, así Sokari empieza a hablar enseguida aunque la respuesta sea
   larga. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "app.h"
#include "audio.h"
#include "config.h"
#include "groq.h"
#include "log.h"
#include "memory.h"
#include "mesh.h"
#include "resource.h"
#include "resources.h"
#include "skills.h"
#include "sounds.h"
#include "tts.h"
#include "util.h"
#include "vad.h"
#include "voice.h"
#include "wakeword.h"

#define LOOKAHEAD_FRAMES 4 /* 320 ms después de "Hey Sokari" para ver si sigues hablando */
/* 400 ms hasta el cuadro donde se detectó el nombre: el detector avisa hasta
   ~1/3 s después de que terminas de decir "Sokari", y para entonces la orden
   ya pudo empezar. Si se cuela el final del nombre, no pasa nada. */
#define WAKE_HISTORY_FRAMES 5
#define REMINDER_CHECK_FRAMES (20 * MIC_RATE / MIC_FRAME)
/* Los temporizadores se revisan cada medio segundo (están en memoria). */
#define TIMER_CHECK_FRAMES (MIC_RATE / 2 / MIC_FRAME)

typedef struct Chunk {
    int16_t *pcm;
    size_t n;
    struct Chunk *next;
} Chunk;

static struct {
    HANDLE thread;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    char **sentences;
    int nsent, next_sent;
    bool job_active, cancel, synth_done, quit;
    Chunk *head, *tail;
    char *pending_voice;
    bool list_request, list_ready;
    TtsVoice *list_result;
    int list_count;
} S;

static HANDLE g_thread;
static HANDLE g_quit;
static HANDLE g_trigger;
/* Clic en la esfera: callarla. Se limpia al empezar cada turno para que un
   clic viejo no corte la siguiente respuesta. */
static HANDLE g_skip;
static volatile LONG g_settings_dirty;
static volatile LONG g_test_audio;
static SRWLOCK g_preview_lock = SRWLOCK_INIT;
static char *g_preview_voice;
static WakeWord *g_ww;
static int g_silence = 300;
/* Qué es voz y qué es ruido: aprende el ruido de tu cuarto con todo lo que
   oye mientras espera "Hey Sokari" (solo lo usa el hilo de voz). */
static Listener *g_listener;
/* Cuándo empezó a sonar la primera frase de la última respuesta. */
static uint64_t g_first_audio_at;
static Conversation *g_conv;
static Conversation *g_mesh_conv;
static char *g_mic_name;

static int16_t *g_sim;
static size_t g_sim_n, g_sim_pos;
static bool g_sim_mode;

/* ------------------------------------------------------------ síntesis --- */

static void free_chunks(void)
{
    while (S.head) {
        Chunk *c = S.head;
        S.head = c->next;
        free(c->pcm);
        free(c);
    }
    S.tail = NULL;
}

static DWORD WINAPI speech_worker(LPVOID arg)
{
    char *voice = arg;
    tts_init(voice);
    free(voice);
    EnterCriticalSection(&S.lock);
    while (!S.quit) {
        if (S.pending_voice) {
            char *v = S.pending_voice;
            S.pending_voice = NULL;
            LeaveCriticalSection(&S.lock);
            if (*v) tts_set_voice(v);
            free(v);
            EnterCriticalSection(&S.lock);
            continue;
        }
        if (S.list_request) {
            S.list_request = false;
            LeaveCriticalSection(&S.lock);
            TtsVoice *list;
            int n = tts_list_voices(&list);
            EnterCriticalSection(&S.lock);
            S.list_result = list;
            S.list_count = n;
            S.list_ready = true;
            WakeAllConditionVariable(&S.cv);
            continue;
        }
        if (S.job_active && !S.cancel && S.next_sent < S.nsent) {
            char *sentence = xstrdup(S.sentences[S.next_sent++]);
            LeaveCriticalSection(&S.lock);
            size_t n = 0;
            int16_t *pcm = tts_synthesize(sentence, &n);
            free(sentence);
            EnterCriticalSection(&S.lock);
            if (pcm && !S.cancel) {
                Chunk *c = xcalloc(1, sizeof *c);
                c->pcm = pcm;
                c->n = n;
                if (S.tail) S.tail->next = c;
                else S.head = c;
                S.tail = c;
            } else {
                free(pcm);
            }
            if (S.next_sent >= S.nsent) S.synth_done = true;
            WakeAllConditionVariable(&S.cv);
            continue;
        }
        if (S.job_active && !S.synth_done) {
            S.synth_done = true;
            WakeAllConditionVariable(&S.cv);
        }
        SleepConditionVariableCS(&S.cv, &S.lock, INFINITE);
    }
    LeaveCriticalSection(&S.lock);
    tts_shutdown();
    return 0;
}

int voice_list_voices(TtsVoice **out)
{
    *out = NULL;
    if (!S.thread) return 0;
    EnterCriticalSection(&S.lock);
    S.list_ready = false;
    S.list_request = true;
    WakeAllConditionVariable(&S.cv);
    while (!S.list_ready && !S.quit)
        if (!SleepConditionVariableCS(&S.cv, &S.lock, 3000)) break;
    int n = S.list_ready ? S.list_count : 0;
    *out = S.list_ready ? S.list_result : NULL;
    S.list_result = NULL;
    S.list_ready = false;
    LeaveCriticalSection(&S.lock);
    return n;
}

/* Parte la respuesta en oraciones para sintetizar la primera mientras se
   escucha nada todavía; las muy cortas se juntan con la siguiente para que
   la entonación no quede entrecortada. */
static int split_sentences(const char *text, char ***out)
{
    int cap = 8, n = 0;
    char **list = xmalloc(sizeof(char *) * (size_t)cap);
    StrBuf cur;
    sb_init(&cur);
    for (const char *p = text;; p++) {
        bool end = !*p;
        if (!end) sb_append_char(&cur, *p);
        bool boundary = end || *p == '\n' ||
                        ((*p == '.' || *p == '!' || *p == '?' || *p == ';' || *p == ':') &&
                         (p[1] == ' ' || p[1] == '\n' || !p[1]));
        if (boundary && (end || cur.len >= 40)) {
            char *t = str_trim(cur.data);
            if (*t) {
                if (n == cap) {
                    cap *= 2;
                    list = xrealloc(list, sizeof(char *) * (size_t)cap);
                }
                list[n++] = t;
            } else {
                free(t);
            }
            cur.len = 0;
            cur.data[0] = 0;
        }
        if (end) break;
    }
    sb_free(&cur);
    *out = list;
    return n;
}

/* Cómo la pararon (mientras piensa o mientras habla). */
typedef enum { STOP_NONE, STOP_CLICK, STOP_TRIGGER, STOP_WAKE } StopKind;

typedef struct {
    bool allow_interrupt;
    StopKind stop;
} PlayCtx;

static bool input_read_nowait(int16_t *f);

/* Mientras habla, la callan "Hey Sokari", el atajo o un clic en la esfera.
   Antes bastaba cualquier ruido un poco más fuerte que el silencio del
   cuarto, y se cortaba a media frase casi siempre. */
static bool play_cb(float level, void *ctx)
{
    PlayCtx *pc = ctx;
    app_set_level(level);
    if (WaitForSingleObject(g_quit, 0) == WAIT_OBJECT_0) return false;
    if (!pc->allow_interrupt) return true;
    if (WaitForSingleObject(g_skip, 0) == WAIT_OBJECT_0) {
        log_msg("Callada con un clic en la esfera.");
        pc->stop = STOP_CLICK;
        return false;
    }
    if (WaitForSingleObject(g_trigger, 0) == WAIT_OBJECT_0) {
        log_msg("Interrumpido con el atajo.");
        pc->stop = STOP_TRIGGER;
        return false;
    }
    int16_t f[MIC_FRAME];
    while (input_read_nowait(f)) {
        if (g_ww && ww_process(g_ww, f) > config_wake_threshold()) {
            log_msg("Interrumpido: dijiste \"Hey Sokari\".");
            ww_reset(g_ww);
            pc->stop = STOP_WAKE;
            return false;
        }
    }
    return true;
}

static void input_flush(void);

/* Todo lo que dice se puede saltar (también «Sokari en línea», la prueba de
   audio y la muestra de voz). Devuelve cómo la pararon (STOP_NONE: lo dijo
   todo). */
static StopKind speak(const char *text)
{
    const bool allow_interrupt = true;
    if (str_is_blank(text)) return STOP_NONE;
    if (app_get_state() == JV_THINKING && WaitForSingleObject(g_skip, 0) == WAIT_OBJECT_0) {
        log_msg("No lo digo: le diste clic a la esfera mientras pensaba («%s»).", text);
        return STOP_CLICK;
    }
    if (!g_sim_mode) sound_activation_stop();
    if (g_ww) ww_reset(g_ww);
    log_msg("Sokari: %s", text);
    app_subtitle(false, text);
    app_set_state(JV_SPEAKING);
    char *clean = tts_clean_text(text);
    char **sentences;
    int n = split_sentences(clean, &sentences);
    free(clean);

    EnterCriticalSection(&S.lock);
    free_chunks();
    S.sentences = sentences;
    S.nsent = n;
    S.next_sent = 0;
    S.cancel = false;
    S.synth_done = n == 0;
    S.job_active = true;
    WakeAllConditionVariable(&S.cv);
    LeaveCriticalSection(&S.lock);

    PlayCtx ctx = {.allow_interrupt = allow_interrupt && !g_sim_mode, .stop = STOP_NONE};
    bool cut = false;
    for (;;) {
        EnterCriticalSection(&S.lock);
        while (!S.head && !S.synth_done && !S.quit) SleepConditionVariableCS(&S.cv, &S.lock, 200);
        Chunk *c = S.head;
        if (c) {
            S.head = c->next;
            if (!S.head) S.tail = NULL;
        }
        LeaveCriticalSection(&S.lock);
        if (!c) break;
        bool finished = true;
        if (!g_first_audio_at) g_first_audio_at = GetTickCount64();
        if (!g_sim_mode) finished = speaker_play(c->pcm, c->n, TTS_RATE, volume_to_gain(config_volume()), play_cb, &ctx);
        free(c->pcm);
        free(c);
        if (!finished) {
            cut = true;
            EnterCriticalSection(&S.lock);
            S.cancel = true;
            LeaveCriticalSection(&S.lock);
            break;
        }
    }
    EnterCriticalSection(&S.lock);
    while (S.job_active && !S.synth_done && S.cancel) SleepConditionVariableCS(&S.cv, &S.lock, 100);
    free_chunks();
    for (int i = 0; i < S.nsent; i++) free(S.sentences[i]);
    free(S.sentences);
    S.sentences = NULL;
    S.nsent = S.next_sent = 0;
    S.job_active = false;
    LeaveCriticalSection(&S.lock);
    app_set_level(0);
    /* Si lo callaste con "Hey Sokari", lo que dices justo después es tu orden:
       no se tira. */
    if (!cut) input_flush();
    return cut ? (ctx.stop != STOP_NONE ? ctx.stop : STOP_CLICK) : STOP_NONE;
}

/* -------------------------------------------------------------- entrada --- */

static bool input_read(int16_t *f, unsigned timeout_ms)
{
    if (g_sim_mode) {
        if (g_sim_pos + MIC_FRAME > g_sim_n) {
            SetEvent(g_quit);
            return false;
        }
        memcpy(f, g_sim + g_sim_pos, sizeof(int16_t) * MIC_FRAME);
        g_sim_pos += MIC_FRAME;
        return true;
    }
    return mic_read(f, timeout_ms);
}

static bool input_read_nowait(int16_t *f)
{
    return g_sim_mode ? false : mic_read_nowait(f);
}

static void input_flush(void)
{
    if (!g_sim_mode) mic_flush();
}

void voice_set_input_wav(const wchar_t *path)
{
    size_t len;
    char *d = read_file_all(path, &len);
    if (!d) return;
    for (size_t off = 12; off + 8 <= len;) {
        uint32_t sz;
        memcpy(&sz, d + off + 4, 4);
        if (!memcmp(d + off, "data", 4)) {
            if (off + 8 + sz > len) sz = (uint32_t)(len - off - 8);
            g_sim = xmalloc(sz);
            memcpy(g_sim, d + off + 8, sz);
            g_sim_n = sz / 2;
            g_sim_mode = true;
            break;
        }
        off += 8 + sz + (sz & 1);
    }
    free(d);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Mide el ruido de fondo real al arrancar. Percentil 10 (no el promedio) para
   que una tos o un portazo durante la medición no arruinen el número; y cuanto
   más veces corrió, más corta la medición y más pesa el historial. */
static void calibrate(void)
{
    int prev = 0, runs = 0;
    bool has = load_calibration(&prev, &runs);
    if (!has) runs = 0;
    double duration = fmax(0.4, 2.0 - 0.15 * runs);
    int frames = (int)(duration * MIC_RATE / MIC_FRAME);
    if (frames < 1) frames = 1;
    if (frames > 64) frames = 64;
    float energies[64];
    int got = 0;
    int16_t f[MIC_FRAME];
    input_flush();
    while (got < frames && input_read(f, 2000)) energies[got++] = frame_energy(f, MIC_FRAME);
    if (!got) return;
    qsort(energies, (size_t)got, sizeof(float), cmp_float);
    double pos = 0.1 * (got - 1);
    int lo = (int)pos;
    double noise = energies[lo] + (lo + 1 < got ? (energies[lo + 1] - energies[lo]) * (pos - lo) : 0);
    int fresh = (int)(noise * 2.2);
    if (fresh < 90) fresh = 90;
    g_silence = has ? (int)lround((fresh + (double)prev * runs) / (runs + 1)) : fresh;
    if (!g_sim_mode) save_calibration(g_silence, runs + 1);
    log_msg("Umbral de silencio ajustado a tu ambiente: %d", g_silence);
}

/* seed: audio que ya se sabe que es el principio de la orden (lo que dijiste
   de corrido después de "Hey Sokari"); con él ya no se espera a que hables. */
/* Tu orden: empieza cuando hablas, termina cuando te callas (aunque siga el
   ruido de fondo) o a los 30 s, y regresa solo tu voz. Sin voz, NULL: no se
   manda nada a transcribir. Mientras escucha, baja el volumen de la PC. */
static int16_t *record_command(const int16_t *seed, int nseed, size_t *out_n)
{
    Recording rec;
    rec_begin(&rec, end_silence_frames((EndSilence)config_end_silence()));
    rec_seed(&rec, seed, nseed);
    DuckState duck = {-1, -1};
    if (!g_sim_mode && config_duck()) duck = system_duck(0.3f);
    int16_t f[MIC_FRAME];
    for (;;) {
        if (WaitForSingleObject(g_quit, 0) == WAIT_OBJECT_0 || !input_read(f, 2000)) break;
        float lvl = frame_energy(f, MIC_FRAME) / 3000.0f;
        app_set_level(lvl > 1 ? 1 : lvl);
        if (rec_feed(&rec, f, listener_feed(g_listener, f, (float)g_silence))) break;
    }
    system_unduck(duck);
    app_set_level(0);
    float voice_s = 0;
    int16_t *buf = rec_take(&rec, out_n, &voice_s);
    float heard_s = (float)rec.frames * MIC_FRAME / MIC_RATE;
    if (!buf && rec.heard) log_msg("Oí %.1f s, pero casi nada era voz (ruido): no lo mando a transcribir.", heard_s);
    else if (buf) log_msg("Te escuché %.1f s (voz: %.1f s; mando %.1f s).", heard_s, voice_s, (double)*out_n / MIC_RATE);
    rec_free(&rec);
    return buf;
}

/* ------------------------------------------------------- conversación --- */

/* ---- pensar sin dejar de oír: «Hey Sokari», el atajo o un clic la paran ---- */

/* Lo que piensa (pasar tu voz a texto y el agente) va en otro hilo; este
   sigue oyendo. Si la paras, ese hilo termina lo que tenía en curso (una
   llamada a la IA no se puede cortar a la mitad), pero ya no hace ninguna
   acción ni dice nada. El último en soltar el trabajo lo libera. */
typedef struct {
    int16_t *audio;
    size_t n;
    volatile LONG cancel, refs;
    HANDLE done;
    char *text;
    GroqError err;
    TurnResult r;
    TurnStats ts;
    uint64_t t_text, t_reply;
} ThinkJob;

static void think_job_release(ThinkJob *j)
{
    if (InterlockedDecrement(&j->refs)) return;
    free(j->audio);
    free(j->text);
    groq_error_free(&j->err);
    free(j->r.reply);
    CloseHandle(j->done);
    free(j);
}

static DWORD WINAPI think_thread(LPVOID arg)
{
    ThinkJob *j = arg;
    j->text = groq_transcribe(j->audio, j->n, MIC_RATE, &j->err);
    if (j->text && *j->text && !InterlockedCompareExchange(&j->cancel, 0, 0)) {
        log_msg("Tú: %s", j->text);
        app_subtitle(true, j->text);
        j->t_text = GetTickCount64();
        turn_stats_reset();
        state_lock();
        conv_set_cancel(g_conv, &j->cancel);
        j->r = agent_process(g_conv, j->text);
        conv_set_cancel(g_conv, NULL);
        state_unlock();
        j->ts = turn_stats_get();
        j->t_reply = GetTickCount64();
    }
    SetEvent(j->done);
    think_job_release(j);
    return 0;
}

/* Espera a que termine de pensar sin dejar de oír. STOP_NONE: terminó. */
static StopKind think_wait(ThinkJob *j)
{
    for (;;) {
        if (WaitForSingleObject(j->done, 30) == WAIT_OBJECT_0) return STOP_NONE;
        if (WaitForSingleObject(g_quit, 0) == WAIT_OBJECT_0) return STOP_CLICK;
        if (WaitForSingleObject(g_skip, 0) == WAIT_OBJECT_0) {
            log_msg("Callada con un clic en la esfera mientras pensaba.");
            return STOP_CLICK;
        }
        if (WaitForSingleObject(g_trigger, 0) == WAIT_OBJECT_0) {
            log_msg("Interrumpida con el atajo mientras pensaba.");
            return STOP_TRIGGER;
        }
        if (g_sim_mode) continue; /* en las pruebas el audio simulado es la siguiente orden */
        int16_t f[MIC_FRAME];
        while (input_read_nowait(f)) {
            if (g_ww && ww_process(g_ww, f) > config_wake_threshold()) {
                log_msg("Interrumpida: dijiste \"Hey Sokari\" mientras pensaba.");
                ww_reset(g_ww);
                return STOP_WAKE;
            }
        }
    }
}

/* La paraste con «Hey Sokari» o el atajo: te escucha ya para la orden nueva
   (sin repetir el nombre). Con un clic solo se calla. */
static bool after_stop(StopKind stop)
{
    if (stop == STOP_WAKE || stop == STOP_TRIGGER) {
        if (!g_sim_mode) sound_activation();
        return true;
    }
    return false;
}

static bool handle_turn(const int16_t *audio, size_t n)
{
    if (n < (size_t)(MIC_RATE * 3 / 10)) return true;
    /* Un clic o un Ctrl+Alt+J de mientras te escuchaba no cuentan: callar es
       para lo que diga desde aquí. */
    ResetEvent(g_skip);
    ResetEvent(g_trigger);
    uint64_t t_quiet = GetTickCount64(); /* te acabas de callar */
    app_set_state(JV_THINKING);
    app_status("Escuchando lo que dijiste…");
    ThinkJob *j = xcalloc(1, sizeof *j);
    j->audio = xmalloc(n * sizeof *audio);
    memcpy(j->audio, audio, n * sizeof *audio);
    j->n = n;
    j->refs = 2;
    j->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, think_thread, j, 0, NULL);
    if (th) {
        CloseHandle(th);
    } else {
        think_thread(j); /* sin hilo: se piensa aquí mismo */
    }
    StopKind stop = think_wait(j);
    if (stop != STOP_NONE) {
        InterlockedExchange(&j->cancel, 1);
        app_status("");
        think_job_release(j);
        return after_stop(stop);
    }
    app_status("");
    if (!j->text) {
        log_msg("Error transcribiendo: %s", j->err.detail ? j->err.detail : "?");
        speak(j->err.status == GROQ_AUTH_ERROR ? "Tu API key de Groq no es válida. Revísala en Configuración."
                                               : "No pude transcribir el audio.");
        think_job_release(j);
        return true;
    }
    if (!*j->text) {
        think_job_release(j);
        return true;
    }
    TurnResult r = j->r;
    TurnStats ts = j->ts;
    uint64_t t_text = j->t_text, t_reply = j->t_reply;
    g_first_audio_at = 0;
    /* En la ventana: si esta respuesta gastó IA o salió de tu PC. */
    if (r.reply) {
        char *st = ts.calls ? str_printf("Contestó la IA (%d tokens)", ts.tokens_in + ts.tokens_out)
                            : xstrdup("Contesté sin IA: 0 tokens");
        app_status(st);
        free(st);
    }
    StopKind cut = r.reply ? speak(r.reply) : STOP_NONE;
    /* Dónde se va el tiempo de cada respuesta, para saber qué arreglar. */
    uint64_t t_audio = g_first_audio_at ? g_first_audio_at : t_reply;
    if (ts.calls)
        log_msg("Tiempos: voz a texto %.1f s · pensar %.1f s (%d llamada%s a la IA, %d tokens) · empezar a hablar %.1f s · "
                "total %.1f s desde que te callaste.",
                (double)(t_text - t_quiet) / 1000, (double)(t_reply - t_text) / 1000, ts.calls, ts.calls == 1 ? "" : "s",
                ts.tokens_in + ts.tokens_out, (double)(t_audio - t_reply) / 1000, (double)(t_audio - t_quiet) / 1000);
    else
        log_msg("Tiempos: voz a texto %.1f s · sin IA · empezar a hablar %.1f s · total %.1f s desde que te callaste.",
                (double)(t_text - t_quiet) / 1000, (double)(t_audio - t_reply) / 1000, (double)(t_audio - t_quiet) / 1000);
    bool shutdown = r.shutdown, keep = r.keep_going;
    think_job_release(j);
    if (shutdown) {
        app_request_quit();
        return false;
    }
    if (cut == STOP_WAKE || cut == STOP_TRIGGER) return after_stop(cut);
    return keep;
}

/* "Hey Sokari abre Spotify" de corrido: si justo después del nombre sigues
   hablando, no suena el tono ni se tira ese audio (es el principio de la
   orden). Devuelve cuántos cuadros hay en seed (los últimos antes de la
   detección más los que siguen), o 0 si hiciste pausa. */
static int continued_speech(const int16_t *history, int nhist, int16_t seed[][MIC_FRAME])
{
    memcpy(seed[0], history, sizeof(int16_t) * MIC_FRAME * (size_t)nhist);
    int n = nhist, loud = 0;
    for (int i = 0; i < LOOKAHEAD_FRAMES; i++) {
        if (!input_read(seed[n], 1000)) break;
        /* El primero no cuenta: puede ser la cola de "Sokari" o el eco del cuarto. */
        bool voice = listener_feed(g_listener, seed[n], (float)g_silence);
        if (i > 0 && voice) loud++;
        n++;
    }
    return loud >= 2 ? n : 0;
}

/* history: los últimos cuadros hasta el que activó "Hey Sokari" (nhist = 0 si
   fue con el atajo). */
static void conversation(const int16_t *history, int nhist)
{
    app_set_state(JV_LISTENING);
    int16_t seed[WAKE_HISTORY_FRAMES + LOOKAHEAD_FRAMES][MIC_FRAME];
    int nseed = nhist ? continued_speech(history, nhist, seed) : 0;
    if (nseed) {
        log_msg("Seguiste hablando después del nombre: tomo la orden sin tono.");
    } else {
        if (!g_sim_mode) sound_activation();
        input_flush();
    }
    conv_new_session(g_conv);
    for (bool first = true;; first = false) {
        app_set_state(JV_LISTENING);
        size_t n;
        int16_t *audio = first && nseed ? record_command(seed[0], nseed, &n) : record_command(NULL, 0, &n);
        bool cont = audio && n >= (size_t)(MIC_RATE * 3 / 10) && handle_turn(audio, n);
        free(audio);
        if (!cont || WaitForSingleObject(g_quit, 0) == WAIT_OBJECT_0) break;
    }
    app_set_state(JV_IDLE);
    log_msg("Escuchando \"Hey Sokari\"...");
}

static void announce_due_reminders(void)
{
    ResetEvent(g_skip);
    state_lock();
    DueReminder *due;
    int n = reminders_take_due(&due);
    state_unlock();
    for (int i = 0; i < n; i++) {
        char *who = strcmp(due[i].profile, DEFAULT_PROFILE) ? profile_display_name(due[i].profile) : NULL;
        char *msg;
        if (due[i].alarm) {
            /* Una alarma: suena antes de decirse. */
            msg = who ? str_printf("%s: %s", who, due[i].texto) : xstrdup(due[i].texto);
            for (int k = 0; k < 3; k++) {
                sound_chime();
                Sleep(700);
            }
        } else {
            msg = who ? str_printf("%s, te quería recordar: %s", who, due[i].texto)
                      : str_printf("Te quería recordar: %s", due[i].texto);
        }
        app_notify(due[i].alarm ? "Alarma" : "Recordatorio", due[i].texto);
        speak(msg);
        free(msg);
        free(who);
    }
    due_reminders_free(due, n);
    if (n) app_set_state(JV_IDLE);
}

static void announce_due_timers(void)
{
    int n;
    char **due = skills_due_timers(&n);
    if (!n) return;
    ResetEvent(g_skip);
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            sound_chime();
            Sleep(600);
        }
        app_notify("Temporizador", due[i]);
        speak(due[i]);
        free(due[i]);
    }
    free(due);
    app_set_state(JV_IDLE);
}

static char *mesh_handle(const char *cmd, const char *origen)
{
    if (!state_try_lock(8000)) return NULL;
    if (!g_mesh_conv) {
        g_mesh_conv = conv_create(false);
        conv_set_remote(g_mesh_conv, true);
    }
    log_msg("Malla (%s): %s", origen, cmd);
    /* Aviso en esta PC: así sabes que la orden sí llegó. */
    char *title = str_printf("Orden desde %s", origen);
    app_notify(title, cmd);
    free(title);
    /* Cada orden por la malla es una conversación aparte: lo que otra orden
       leyó de afuera ya no cuenta (ni sirve para esconder instrucciones). */
    conv_new_session(g_mesh_conv);
    TurnResult r = agent_process(g_mesh_conv, cmd);
    state_unlock();
    if (r.shutdown) app_request_quit();
    return r.reply ? r.reply : xstrdup("Listo.");
}

/* La voz se cambia en el hilo de síntesis, antes de la siguiente frase. */
static void set_speech_voice(const char *id)
{
    EnterCriticalSection(&S.lock);
    free(S.pending_voice);
    S.pending_voice = xstrdup(id);
    WakeAllConditionVariable(&S.cv);
    LeaveCriticalSection(&S.lock);
}

static char *take_preview(void)
{
    AcquireSRWLockExclusive(&g_preview_lock);
    char *v = g_preview_voice;
    g_preview_voice = NULL;
    ReleaseSRWLockExclusive(&g_preview_lock);
    return v;
}

static void apply_settings(void)
{
    AppConfig c = config_snapshot();
    set_speech_voice(c.voice);
    if (!g_sim_mode && strcmp(c.mic_name, g_mic_name ? g_mic_name : "")) {
        free(g_mic_name);
        g_mic_name = xstrdup(c.mic_name);
        if (!mic_restart(g_mic_name)) mic_restart("");
    }
    config_free(&c);
}

static DWORD WINAPI voice_main(LPVOID arg)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    AppConfig cfg = config_snapshot();
    g_mic_name = xstrdup(cfg.mic_name);

    InitializeCriticalSection(&S.lock);
    InitializeConditionVariable(&S.cv);
    S.thread = CreateThread(NULL, 0, speech_worker, xstrdup(cfg.voice), 0, NULL);

    size_t blen = 0, wlen = 0;
    const void *blob = res_data(IDR_WAKEWORD, &blen);
    const void *word = res_data(IDR_HEY_SOKARI, &wlen);
    WakeWord *ww = word ? ww_create(blob, blen, word, wlen) : NULL;
    g_ww = ww;
    if (!word) {
        log_msg("Este exe no trae el modelo de \"Hey Sokari\": solo se le habla con Ctrl+Alt+J.");
        app_notify("Sokari", "Todavía no tengo mi palabra \"Hey Sokari\". Por ahora háblame con Ctrl+Alt+J.");
    } else if (!ww) {
        app_notify("Sokari", "No pude cargar el detector de \"Hey Sokari\". Usa Ctrl+Alt+J para hablarle.");
    }

    if (!g_sim_mode && !mic_start(g_mic_name) && !mic_start("")) {
        app_notify("Sokari", "No encontré ningún micrófono. Conecta uno y vuelve a abrir Sokari.");
    }
    state_lock();
    memory_identify_on_start(cfg.user_name);
    g_conv = conv_create(true);
    state_unlock();
    config_free(&cfg);

    g_listener = listener_create();
    log_msg("Calibrando nivel de silencio, no hace falta que digas nada...");
    calibrate();

    speak("Sokari en línea.");
    app_set_state(JV_IDLE);
    log_msg(ww ? "Listo. Di \"Hey Sokari\" (o Ctrl+Alt+J) para hablarle." : "Listo. Ctrl+Alt+J para hablarle.");

    int16_t f[MIC_FRAME];
    int16_t hist[WAKE_HISTORY_FRAMES][MIC_FRAME];
    int nhist = 0;
    int frame_count = 0;
    uint64_t last_data = GetTickCount64();
    while (WaitForSingleObject(g_quit, 0) != WAIT_OBJECT_0) {
        if (InterlockedExchange(&g_settings_dirty, 0)) apply_settings();
        if (InterlockedExchange(&g_test_audio, 0)) {
            sound_chime();
            ResetEvent(g_skip); /* un clic viejo no corta la prueba */
            speak("Así me escuchas por esta salida de audio.");
            app_set_state(JV_IDLE);
            if (ww) ww_reset(ww);
            nhist = 0;
            continue;
        }
        char *preview = take_preview();
        if (preview) {
            AppConfig c = config_snapshot();
            set_speech_voice(preview);
            ResetEvent(g_skip);
            speak("Hola, soy Sokari. Así sueno con esta voz.");
            set_speech_voice(c.voice);
            config_free(&c);
            free(preview);
            app_set_state(JV_IDLE);
            if (ww) ww_reset(ww);
            nhist = 0;
            continue;
        }
        bool triggered = WaitForSingleObject(g_trigger, 0) == WAIT_OBJECT_0;
        bool got = input_read(f, 500);
        if (!got) {
            if (!g_sim_mode && GetTickCount64() - last_data > 5000) {
                log_msg("El micrófono dejó de mandar audio; lo reabro.");
                if (!mic_restart(g_mic_name)) mic_restart("");
                last_data = GetTickCount64();
            }
            if (!triggered) continue;
        } else {
            last_data = GetTickCount64();
            listener_feed(g_listener, f, (float)g_silence); /* aprende el ruido de tu cuarto */
            if (nhist == WAKE_HISTORY_FRAMES) {
                memmove(hist[0], hist[1], sizeof hist[0] * (WAKE_HISTORY_FRAMES - 1));
                nhist--;
            }
            memcpy(hist[nhist++], f, sizeof f);
        }
        if (++frame_count % REMINDER_CHECK_FRAMES == 0) announce_due_reminders();
        if (frame_count % TIMER_CHECK_FRAMES == 0) announce_due_timers();
        if (config_mic_muted()) {
            if (triggered) app_notify("Sokari", "El micrófono está silenciado (actívalo desde el ícono de la bandeja).");
            continue;
        }
        float score = (got && ww) ? ww_process(ww, f) : 0.0f;
        if (score > config_wake_threshold() || triggered) {
            log_msg(triggered ? "Activado con el atajo." : "Wake word detectada (%.2f).", score);
            if (ww) ww_reset(ww);
            conversation(hist[0], triggered ? 0 : nhist);
            if (ww) ww_reset(ww);
            input_flush();
            nhist = 0;
        }
    }

    g_ww = NULL;
    EnterCriticalSection(&S.lock);
    S.quit = true;
    S.cancel = true;
    WakeAllConditionVariable(&S.cv);
    LeaveCriticalSection(&S.lock);
    WaitForSingleObject(S.thread, 3000);
    if (!g_sim_mode) mic_stop();
    ww_destroy(ww);
    listener_destroy(g_listener);
    g_listener = NULL;
    CoUninitialize();
    return 0;
}

/* La malla escucha desde que abres Sokari, aunque todavía no le hayas dado
   «Iniciar» (antes, una PC que se quedaba en Inicio nunca recibía órdenes). */
void voice_mesh_start(void)
{
    agent_init();
    mesh_start(mesh_handle);
}

void voice_mesh_stop(void)
{
    mesh_stop();
}

bool voice_start(void)
{
    g_quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_trigger = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_skip = CreateEventW(NULL, FALSE, FALSE, NULL);
    agent_init();
    g_thread = CreateThread(NULL, 1 << 22, voice_main, NULL, 0, NULL);
    return g_thread != NULL;
}

void voice_stop(void)
{
    if (!g_thread) return;
    SetEvent(g_quit);
    WaitForSingleObject(g_thread, 8000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

bool voice_wait(unsigned ms)
{
    return g_thread && WaitForSingleObject(g_thread, ms) == WAIT_OBJECT_0;
}

void voice_skip(void)
{
    if (g_skip) SetEvent(g_skip);
}

void voice_trigger(void)
{
    if (g_trigger) SetEvent(g_trigger);
}

void voice_settings_changed(void)
{
    InterlockedExchange(&g_settings_dirty, 1);
}

bool voice_running(void)
{
    return g_thread != NULL;
}

void voice_test_audio(void)
{
    InterlockedExchange(&g_test_audio, 1);
}

void voice_preview(const char *voice_id)
{
    AcquireSRWLockExclusive(&g_preview_lock);
    free(g_preview_voice);
    g_preview_voice = xstrdup(voice_id ? voice_id : "");
    ReleaseSRWLockExclusive(&g_preview_lock);
}

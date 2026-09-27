/* audio.h en Linux, con PulseAudio (en Ubuntu y Fedora lo atiende PipeWire).
   El micrófono lo drena un hilo propio a un buffer circular, como en Windows;
   la voz sale en pedazos de 40 ms para poder cortarla al instante y mover la
   esfera al compás; y "bajar el volumen mientras te escucho" baja la salida
   predeterminada y la regresa, salvo que tú le hayas movido. */
#include <windows.h>

#include <math.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "eco.h"
#include "linux/linux.h"
#include "log.h"
#include "util.h"

#define RING_SAMPLES (MIC_RATE * 30)
#define PLAY_CHUNK_MS 40
#define PLAY_AHEAD_MS 120

/* ------------------------------------ consultas al servidor de sonido --- */

/* Las listas y el volumen se piden con la API asíncrona y un bucle propio que
   se corre hasta que la consulta termina (o pasan 3 s). */
typedef struct {
    bool done, ok;
    int n, cap;
    char **names, **descs;
    char *default_sink;
    pa_cvolume vol;
    bool have_vol;
    int mute;
} PaQuery;

static void add_dev(PaQuery *q, const char *name, const char *desc)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 8;
        q->names = xrealloc(q->names, sizeof(char *) * (size_t)q->cap);
        q->descs = xrealloc(q->descs, sizeof(char *) * (size_t)q->cap);
    }
    q->names[q->n] = xstrdup(name ? name : "");
    q->descs[q->n] = xstrdup(desc && *desc ? desc : name ? name : "");
    q->n++;
}

static void on_sink(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    PaQuery *q = u;
    if (eol) {
        q->done = q->ok = true;
        return;
    }
    add_dev(q, i->name, i->description);
}

static void on_source(pa_context *c, const pa_source_info *i, int eol, void *u)
{
    PaQuery *q = u;
    if (eol) {
        q->done = q->ok = true;
        return;
    }
    /* Los "monitor" son lo que suena por una salida: no son micrófonos. */
    if (i->monitor_of_sink == PA_INVALID_INDEX) add_dev(q, i->name, i->description);
}

static void on_server(pa_context *c, const pa_server_info *i, void *u)
{
    PaQuery *q = u;
    q->default_sink = i && i->default_sink_name ? xstrdup(i->default_sink_name) : NULL;
    q->done = q->ok = true;
}

static void on_sink_volume(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    PaQuery *q = u;
    if (eol) {
        q->done = true;
        return;
    }
    q->vol = i->volume;
    q->mute = i->mute;
    q->have_vol = q->ok = true;
}

static void on_success(pa_context *c, int success, void *u)
{
    PaQuery *q = u;
    q->ok = success != 0;
    q->done = true;
}

typedef enum { Q_SINKS, Q_SOURCES, Q_SERVER, Q_SINK_VOLUME, Q_SET_SINK_VOLUME, Q_SET_SINK_MUTE } QueryKind;

/* name: el sink para leer o poner el volumen. */
static bool pa_query(QueryKind kind, const char *name, PaQuery *q)
{
    pa_mainloop *ml = pa_mainloop_new();
    if (!ml) return false;
    pa_context *ctx = pa_context_new(pa_mainloop_get_api(ml), "Sokari");
    bool ok = false;
    if (ctx && pa_context_connect(ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0) {
        uint64_t until = GetTickCount64() + 3000;
        pa_context_state_t st;
        while ((st = pa_context_get_state(ctx)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(st) && GetTickCount64() < until)
            pa_mainloop_iterate(ml, 0, NULL), Sleep(2);
        if (st == PA_CONTEXT_READY) {
            pa_operation *op = NULL;
            switch (kind) {
            case Q_SINKS: op = pa_context_get_sink_info_list(ctx, on_sink, q); break;
            case Q_SOURCES: op = pa_context_get_source_info_list(ctx, on_source, q); break;
            case Q_SERVER: op = pa_context_get_server_info(ctx, on_server, q); break;
            case Q_SINK_VOLUME: op = pa_context_get_sink_info_by_name(ctx, name, on_sink_volume, q); break;
            case Q_SET_SINK_VOLUME: op = pa_context_set_sink_volume_by_name(ctx, name, &q->vol, on_success, q); break;
            case Q_SET_SINK_MUTE: op = pa_context_set_sink_mute_by_name(ctx, name, q->mute, on_success, q); break;
            }
            while (op && !q->done && GetTickCount64() < until) pa_mainloop_iterate(ml, 0, NULL), Sleep(2);
            if (op) pa_operation_unref(op);
            ok = q->done && q->ok;
        }
        pa_context_disconnect(ctx);
    }
    if (ctx) pa_context_unref(ctx);
    pa_mainloop_free(ml);
    return ok;
}

static void query_free(PaQuery *q)
{
    for (int i = 0; i < q->n; i++) {
        free(q->names[i]);
        free(q->descs[i]);
    }
    free(q->names);
    free(q->descs);
    free(q->default_sink);
    memset(q, 0, sizeof *q);
}

/* El nombre interno ("alsa_output.…") de la salida o micrófono que en
   Configuración se ve así ("Audio interno Estéreo analógico"). NULL: el
   predeterminado. */
static char *device_by_desc(bool sinks, const char *desc)
{
    if (!desc || !*desc) return NULL;
    PaQuery q = {0};
    char *r = NULL;
    if (pa_query(sinks ? Q_SINKS : Q_SOURCES, NULL, &q))
        for (int i = 0; i < q.n && !r; i++)
            if (!strcmp(q.descs[i], desc) || !strcmp(q.names[i], desc)) r = xstrdup(q.names[i]);
    query_free(&q);
    return r;
}

static int list_devices(bool sinks, char ***names_out)
{
    PaQuery q = {0};
    pa_query(sinks ? Q_SINKS : Q_SOURCES, NULL, &q);
    char **out = xcalloc((size_t)q.n + 1, sizeof(char *));
    for (int i = 0; i < q.n; i++) out[i] = xstrdup(q.descs[i]);
    int n = q.n;
    query_free(&q);
    *names_out = out;
    return n;
}

int mic_list_devices(char ***names_out)
{
    return list_devices(false, names_out);
}

int speaker_list_devices(char ***names_out)
{
    return list_devices(true, names_out);
}

void free_string_list(char **list, int n)
{
    if (!list) return;
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

/* --------------------------------------------------------- micrófono --- */

static pa_simple *g_rec;
static HANDLE g_thread;
static volatile LONG g_running;
static int16_t *g_ring;
static size_t g_ring_start, g_ring_count;
static uint64_t g_pushed; /* muestras que ha dado el micrófono desde que se abrió */
static CRITICAL_SECTION g_ring_lock;
static CONDITION_VARIABLE g_ring_cv;
static bool g_ring_init;
static volatile LONG g_level_milli;
static volatile LONG64 g_last_data_ms;

static void ring_push(const int16_t *s, size_t n)
{
    EnterCriticalSection(&g_ring_lock);
    for (size_t i = 0; i < n; i++) {
        size_t pos = (g_ring_start + g_ring_count) % RING_SAMPLES;
        g_ring[pos] = s[i];
        if (g_ring_count < RING_SAMPLES) g_ring_count++;
        else g_ring_start = (g_ring_start + 1) % RING_SAMPLES; /* lleno: se pierde lo más viejo */
    }
    g_pushed += n;
    uint64_t total = g_pushed;
    WakeAllConditionVariable(&g_ring_cv);
    LeaveCriticalSection(&g_ring_lock);
    eco_mic_pushed(total);
}

static bool ring_pop(int16_t *out, size_t n)
{
    if (g_ring_count < n) return false;
    for (size_t i = 0; i < n; i++) out[i] = g_ring[(g_ring_start + i) % RING_SAMPLES];
    g_ring_start = (g_ring_start + n) % RING_SAMPLES;
    g_ring_count -= n;
    return true;
}

static DWORD WINAPI capture_thread(LPVOID arg)
{
    int16_t buf[MIC_FRAME];
    bool warned = false;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        int err = 0;
        if (pa_simple_read(g_rec, buf, sizeof buf, &err) < 0) {
            if (!warned) log_msg("El micrófono dejó de dar audio: %s", pa_strerror(err));
            warned = true;
            Sleep(100);
            continue;
        }
        ring_push(buf, MIC_FRAME);
        float lvl = frame_energy(buf, MIC_FRAME) / 3000.0f;
        if (lvl > 1) lvl = 1;
        InterlockedExchange(&g_level_milli, (LONG)(lvl * 1000));
        InterlockedExchange64(&g_last_data_ms, (LONG64)GetTickCount64());
    }
    return 0;
}

bool mic_start(const char *device_name)
{
    if (!g_ring_init) {
        InitializeCriticalSection(&g_ring_lock);
        InitializeConditionVariable(&g_ring_cv);
        g_ring = xmalloc(sizeof(int16_t) * RING_SAMPLES);
        g_ring_init = true;
    }
    g_ring_start = g_ring_count = 0;
    g_pushed = 0;
    eco_mic_reset();
    pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = MIC_RATE, .channels = 1};
    /* Pedazos de 80 ms: lo que el hilo de voz lee de una vez. */
    pa_buffer_attr ba = {.maxlength = (uint32_t)-1, .fragsize = MIC_FRAME * 2};
    char *dev = device_by_desc(false, device_name);
    int err = 0;
    g_rec = pa_simple_new(NULL, "Sokari", PA_STREAM_RECORD, dev, "Te escucho", &ss, NULL, &ba, &err);
    if (!g_rec && dev) g_rec = pa_simple_new(NULL, "Sokari", PA_STREAM_RECORD, NULL, "Te escucho", &ss, NULL, &ba, &err);
    free(dev);
    if (!g_rec) {
        log_msg("No pude abrir el micrófono: %s", pa_strerror(err));
        return false;
    }
    InterlockedExchange(&g_running, 1);
    InterlockedExchange64(&g_last_data_ms, (LONG64)GetTickCount64());
    g_thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    return g_thread != NULL;
}

void mic_stop(void)
{
    if (!g_rec) return;
    InterlockedExchange(&g_running, 0);
    WaitForSingleObject(g_thread, 2000); /* a lo más un pedazo de 80 ms */
    CloseHandle(g_thread);
    g_thread = NULL;
    pa_simple_free(g_rec);
    g_rec = NULL;
    EnterCriticalSection(&g_ring_lock);
    WakeAllConditionVariable(&g_ring_cv);
    LeaveCriticalSection(&g_ring_lock);
}

bool mic_restart(const char *device_name)
{
    mic_stop();
    return mic_start(device_name);
}

bool mic_read(int16_t out[MIC_FRAME], unsigned timeout_ms)
{
    /* Sin micrófono abierto se espera igual el plazo: si no, el hilo de voz
       daría vueltas sin parar hasta que conectes uno. */
    if (!g_ring_init || !g_rec) {
        Sleep(timeout_ms < 200 ? timeout_ms : 200);
        return false;
    }
    uint64_t deadline = GetTickCount64() + timeout_ms;
    EnterCriticalSection(&g_ring_lock);
    while (g_ring_count < MIC_FRAME) {
        uint64_t now = GetTickCount64();
        if (now >= deadline || !g_rec) {
            LeaveCriticalSection(&g_ring_lock);
            return false;
        }
        SleepConditionVariableCS(&g_ring_cv, &g_ring_lock, (DWORD)(deadline - now));
    }
    bool ok = ring_pop(out, MIC_FRAME);
    LeaveCriticalSection(&g_ring_lock);
    return ok;
}

bool mic_read_nowait(int16_t out[MIC_FRAME])
{
    if (!g_ring_init) return false;
    EnterCriticalSection(&g_ring_lock);
    bool ok = ring_pop(out, MIC_FRAME);
    LeaveCriticalSection(&g_ring_lock);
    return ok;
}

bool mic_read_nowait_pos(int16_t out[MIC_FRAME], uint64_t *pos)
{
    if (!g_ring_init) return false;
    EnterCriticalSection(&g_ring_lock);
    *pos = g_pushed - g_ring_count;
    bool ok = ring_pop(out, MIC_FRAME);
    LeaveCriticalSection(&g_ring_lock);
    return ok;
}

void mic_flush(void)
{
    if (!g_ring_init) return;
    EnterCriticalSection(&g_ring_lock);
    g_ring_start = g_ring_count = 0;
    LeaveCriticalSection(&g_ring_lock);
}

float mic_level(void)
{
    if (GetTickCount64() - (uint64_t)InterlockedCompareExchange64(&g_last_data_ms, 0, 0) > 500) return 0;
    return (float)InterlockedCompareExchange(&g_level_milli, 0, 0) / 1000.0f;
}

/* ----------------------------------------------------------- bocinas --- */

static char *g_out_name;
static SRWLOCK g_out_lock = SRWLOCK_INIT;

void speaker_set_device(const char *name)
{
    AcquireSRWLockExclusive(&g_out_lock);
    free(g_out_name);
    g_out_name = name && *name ? xstrdup(name) : NULL;
    ReleaseSRWLockExclusive(&g_out_lock);
}

/* El nivel del pedazo que suena ahora (no el último que se mandó), para que
   la esfera se mueva con lo que se oye. */
static float now_playing(pa_simple *s, size_t written, int rate, size_t chunk, const float *levels, size_t nchunks)
{
    int err;
    pa_usec_t lat = pa_simple_get_latency(s, &err);
    size_t behind = (size_t)((double)lat * rate / 1e6);
    size_t at = written > behind ? written - behind : 0;
    return at / chunk < nchunks ? levels[at / chunk] : 0.0f;
}

static volatile LONG g_hold;

void speaker_hold(bool hold)
{
    InterlockedExchange(&g_hold, hold ? 1 : 0);
}

/* Cuántas de las que se escribieron ya sonaron. */
static size_t played_samples(pa_simple *s, size_t written, int rate)
{
    int err;
    pa_usec_t lat = pa_simple_get_latency(s, &err);
    size_t behind = (size_t)((double)lat * rate / 1e6);
    return written > behind ? written - behind : 0;
}

bool speaker_play(const int16_t *pcm, size_t samples, int rate, float gain, PlayCallback cb, void *ctx)
{
    pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = (uint32_t)rate, .channels = 1};
    /* Poco audio adelantado: cortar (un clic en la esfera) se oye al momento. */
    pa_buffer_attr ba = {.maxlength = (uint32_t)-1,
                         .tlength = (uint32_t)(rate * 2 * PLAY_AHEAD_MS / 1000),
                         .prebuf = (uint32_t)-1,
                         .minreq = (uint32_t)-1,
                         .fragsize = (uint32_t)-1};
    AcquireSRWLockShared(&g_out_lock);
    char *want = g_out_name ? xstrdup(g_out_name) : NULL;
    ReleaseSRWLockShared(&g_out_lock);
    /* Se busca por nombre en cada reproducción: los audífonos van y vienen. */
    char *dev = device_by_desc(true, want);
    free(want);
    int err = 0;
    pa_simple *s = pa_simple_new(NULL, "Sokari", PA_STREAM_PLAYBACK, dev, "Voz de Sokari", &ss, NULL, &ba, &err);
    if (!s && dev) s = pa_simple_new(NULL, "Sokari", PA_STREAM_PLAYBACK, NULL, "Voz de Sokari", &ss, NULL, &ba, &err);
    free(dev);
    if (!s) {
        log_msg("No pude abrir la salida de audio: %s", pa_strerror(err));
        return false;
    }
    size_t chunk = (size_t)rate * PLAY_CHUNK_MS / 1000;
    size_t nchunks = (samples + chunk - 1) / chunk;
    float *levels = xcalloc(nchunks + 1, sizeof(float));
    for (size_t c = 0; c < nchunks; c++) {
        size_t pos = c * chunk, n = samples - pos < chunk ? samples - pos : chunk;
        double sum = 0;
        for (size_t k = 0; k < n; k++) sum += fabs((float)pcm[pos + k] * gain);
        float lvl = (float)(sum / (double)n / 4000.0);
        levels[c] = lvl > 1 ? 1 : lvl;
    }
    int16_t *buf = xmalloc(sizeof(int16_t) * chunk);
    bool stopped = false;
    /* written: hasta dónde se mandó de lo suyo; silent: el silencio que se
       mandó en pausas. Lo que va a sonar va también al eco (eco.h). */
    size_t written = 0, seg_base = 0, silent = 0;
    unsigned eco_id = eco_play_begin(pcm, samples, rate, gain);
    if (cb) InterlockedExchange(&g_hold, 0); /* una pausa vieja no cuenta */
    for (;;) {
        if (cb && InterlockedCompareExchange(&g_hold, 0, 0)) {
            /* En pausa: lo que ya se mandó termina de sonar y luego silencio,
               sin dejar de mandar audio (así la salida no se reinicia y la
               cuenta del eco sigue exacta). */
            eco_play_cut(eco_id, written - seg_base);
            size_t gap = 0;
            memset(buf, 0, sizeof(int16_t) * chunk);
            while (!stopped && InterlockedCompareExchange(&g_hold, 0, 0)) {
                if (pa_simple_write(s, buf, chunk * sizeof(int16_t), &err) < 0) stopped = true;
                gap += chunk;
                if (!stopped && !cb(0.0f, ctx)) stopped = true;
            }
            silent += gap;
            if (stopped) break;
            seg_base = written;
            if (written < samples) eco_id = eco_play_continue(eco_id, gap, pcm + written, samples - written, rate, gain);
            continue;
        }
        if (written >= samples) break;
        size_t n = samples - written < chunk ? samples - written : chunk;
        for (size_t k = 0; k < n; k++) {
            float v = (float)pcm[written + k] * gain;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[k] = (int16_t)v;
        }
        if (pa_simple_write(s, buf, n * sizeof(int16_t), &err) < 0) {
            log_msg("Se cortó la salida de audio: %s", pa_strerror(err));
            stopped = true;
            break;
        }
        written += n;
        if (cb && !cb(now_playing(s, written + silent, rate, chunk, levels, nchunks), ctx)) stopped = true;
        if (stopped) break;
    }
    /* Lo que falta por sonar (lo del buffer más la latencia de la salida, que
       nunca llega a cero) también se puede cortar; si se pausa aquí, ya no
       falta nada de lo suyo por mandar: solo se espera. */
    if (!stopped) {
        int e2;
        pa_usec_t lat = pa_simple_get_latency(s, &e2);
        uint64_t until = GetTickCount64() + lat / 1000;
        while (!stopped && (GetTickCount64() < until || (cb && InterlockedCompareExchange(&g_hold, 0, 0)))) {
            if (cb && !cb(now_playing(s, written + silent, rate, chunk, levels, nchunks), ctx)) stopped = true;
            else Sleep(20);
        }
    }
    if (stopped) {
        /* De lo suyo, lo que alcanzó a sonar (lo demás se tira). */
        size_t at = played_samples(s, written + silent, rate);
        at = at > silent ? at - silent : 0;
        if (at > written) at = written;
        eco_play_cut(eco_id, at > seg_base ? at - seg_base : 0);
        pa_simple_flush(s, &err);
    } else {
        pa_simple_drain(s, &err);
    }
    pa_simple_free(s);
    free(buf);
    free(levels);
    if (cb) {
        InterlockedExchange(&g_hold, 0);
        cb(0, ctx);
    }
    return !stopped;
}

/* ----------------------------------------- bajar el volumen mientras te escucho --- */

/* El canal más alto: al bajar y al regresar se escala la salida completa, así
   el balance izquierda/derecha se queda como estaba. */
static float cvolume_level(const pa_cvolume *v)
{
    return (float)pa_cvolume_max(v) / (float)PA_VOLUME_NORM;
}

DuckState system_duck(float factor)
{
    DuckState d = {-1, -1};
    PaQuery srv = {0};
    if (!pa_query(Q_SERVER, NULL, &srv) || !srv.default_sink) {
        query_free(&srv);
        return d;
    }
    PaQuery q = {0};
    if (pa_query(Q_SINK_VOLUME, srv.default_sink, &q) && q.have_vol) {
        float before = cvolume_level(&q.vol);
        /* En silencio o casi: no hay nada que bajar. */
        if (before > 0.02f) {
            PaQuery set = {0};
            set.vol = q.vol;
            pa_cvolume_scale(&set.vol, (pa_volume_t)(pa_cvolume_max(&q.vol) * factor));
            if (pa_query(Q_SET_SINK_VOLUME, srv.default_sink, &set)) {
                d.before = before;
                d.ducked = cvolume_level(&set.vol);
            }
            query_free(&set);
        }
    }
    query_free(&q);
    query_free(&srv);
    return d;
}

void system_unduck(DuckState d)
{
    if (d.before < 0) return;
    PaQuery srv = {0};
    if (!pa_query(Q_SERVER, NULL, &srv) || !srv.default_sink) {
        query_free(&srv);
        return;
    }
    PaQuery q = {0};
    /* Si mientras tanto le moviste al volumen, se queda como lo dejaste. */
    if (pa_query(Q_SINK_VOLUME, srv.default_sink, &q) && q.have_vol && fabsf(cvolume_level(&q.vol) - d.ducked) < 0.02f) {
        PaQuery set = {0};
        set.vol = q.vol;
        pa_cvolume_scale(&set.vol, (pa_volume_t)(d.before * (float)PA_VOLUME_NORM));
        pa_query(Q_SET_SINK_VOLUME, srv.default_sink, &set);
        query_free(&set);
    }
    query_free(&q);
    query_free(&srv);
}

/* ------------------------------------------ el volumen de la PC (control_media) --- */

static int percent_of(const pa_cvolume *v)
{
    return (int)((double)pa_cvolume_max(v) * 100.0 / (double)PA_VOLUME_NORM + 0.5);
}

bool system_volume_get(int *percent, bool *muted)
{
    PaQuery q = {0};
    bool ok = pa_query(Q_SINK_VOLUME, "@DEFAULT_SINK@", &q) && q.have_vol;
    if (ok) {
        if (percent) *percent = percent_of(&q.vol);
        if (muted) *muted = q.mute != 0;
    }
    query_free(&q);
    return ok;
}

bool system_volume_set(int percent)
{
    PaQuery q = {0};
    bool ok = false;
    if (pa_query(Q_SINK_VOLUME, "@DEFAULT_SINK@", &q) && q.have_vol) {
        PaQuery set = {0};
        set.vol = q.vol;
        /* Igual en todas las bocinas, sin cambiar el balance. */
        pa_cvolume_scale(&set.vol, (pa_volume_t)((double)PA_VOLUME_NORM * percent / 100.0 + 0.5));
        ok = pa_query(Q_SET_SINK_VOLUME, "@DEFAULT_SINK@", &set);
        query_free(&set);
        /* Como en Windows: si lo subes, deja de estar en silencio. */
        if (ok && percent > 0 && q.mute) system_mute_set(false);
    }
    query_free(&q);
    return ok;
}

bool system_mute_set(bool mute)
{
    PaQuery q = {0};
    q.mute = mute;
    bool ok = pa_query(Q_SET_SINK_MUTE, "@DEFAULT_SINK@", &q);
    query_free(&q);
    return ok;
}

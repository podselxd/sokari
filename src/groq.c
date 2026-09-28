#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "groq.h"
#include "http.h"
#include "log.h"
#include "util.h"

#define GROQ_BASE "https://api.groq.com/openai/v1"

static const char *STT_MODELS[] = {"whisper-large-v3-turbo", "whisper-large-v3"};

/* Frases que Whisper inventa sobre silencio/ruido (aprendidas de subtítulos
   de YouTube). La versión anterior las tomaba como si el usuario las hubiera
   dicho — de ahí los "¡Gracias!" fantasma en el log. */
static const char *HALLUCINATIONS[] = {
    "amara.org", "gracias por ver", "suscríbete", "suscribete", "subtítulos realizados", "subtitulos realizados",
    "subtítulos por la comunidad", "¡gracias por ver", "no olvides suscribirte", "thank you for watching",
};

void groq_error_free(GroqError *e)
{
    if (!e) return;
    free(e->detail);
    e->detail = NULL;
}

static void set_error(GroqError *err, GroqStatus st, const HttpResponse *r, const char *detail)
{
    if (!err) return;
    free(err->detail);
    err->status = st;
    err->http_status = r ? r->status : 0;
    err->retry_after = r ? r->retry_after : -1;
    err->detail = xstrdup(detail ? detail : "");
}

static GroqStatus classify(const HttpResponse *r)
{
    if (r->status == 0) return GROQ_NETWORK_ERROR;
    if (r->status == 200) return GROQ_OK;
    if (r->status == 401 || r->status == 403) return GROQ_AUTH_ERROR;
    if (r->status == 429) return GROQ_RATE_LIMITED;
    if (r->status >= 500) return GROQ_SERVER_ERROR;
    return GROQ_BAD_RESPONSE;
}

static char *error_detail(const HttpResponse *r)
{
    if (r->error) return xstrdup(r->error);
    cJSON *j = cJSON_Parse(r->body);
    char *msg = NULL;
    if (j) {
        cJSON *e = cJSON_GetObjectItem(j, "error");
        cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
        if (cJSON_IsString(m)) msg = xstrdup(m->valuestring);
        cJSON_Delete(j);
    }
    if (!msg) msg = str_printf("HTTP %d", r->status);
    return msg;
}

unsigned char *wav_encode(const int16_t *pcm, size_t samples, int sample_rate, size_t *out_len)
{
    size_t data_len = samples * 2;
    size_t total = 44 + data_len;
    unsigned char *w = xmalloc(total);
    uint32_t v32;
    uint16_t v16;
    memcpy(w, "RIFF", 4);
    v32 = (uint32_t)(36 + data_len);
    memcpy(w + 4, &v32, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    v32 = 16;
    memcpy(w + 16, &v32, 4);
    v16 = 1;
    memcpy(w + 20, &v16, 2);
    memcpy(w + 22, &v16, 2);
    v32 = (uint32_t)sample_rate;
    memcpy(w + 24, &v32, 4);
    v32 = (uint32_t)sample_rate * 2;
    memcpy(w + 28, &v32, 4);
    v16 = 2;
    memcpy(w + 32, &v16, 2);
    v16 = 16;
    memcpy(w + 34, &v16, 2);
    memcpy(w + 36, "data", 4);
    v32 = (uint32_t)data_len;
    memcpy(w + 40, &v32, 4);
    memcpy(w + 44, pcm, data_len);
    *out_len = total;
    return w;
}

static char *auth_header(const char *extra)
{
    char *key = config_api_key();
    char *h = str_printf("Authorization: Bearer %s\r\n%s", key, extra ? extra : "");
    SecureZeroMemory(key, strlen(key));
    free(key);
    return h;
}

static bool is_hallucination(const char *text)
{
    char *low = str_lower(text);
    bool bad = false;
    for (size_t i = 0; i < sizeof HALLUCINATIONS / sizeof *HALLUCINATIONS && !bad; i++)
        bad = strstr(low, HALLUCINATIONS[i]) != NULL;
    free(low);
    return bad;
}

/* verbose_json trae por segmento la probabilidad de "no había voz": se
   descartan los segmentos que el propio Whisper marca como silencio, con los
   mismos umbrales que usa Whisper por defecto (0.6 / -1.0). */
static char *text_from_verbose(cJSON *j)
{
    cJSON *segs = cJSON_GetObjectItem(j, "segments");
    if (!cJSON_IsArray(segs)) {
        cJSON *t = cJSON_GetObjectItem(j, "text");
        return str_trim(cJSON_IsString(t) ? t->valuestring : "");
    }
    StrBuf sb;
    sb_init(&sb);
    cJSON *seg;
    cJSON_ArrayForEach(seg, segs)
    {
        cJSON *t = cJSON_GetObjectItem(seg, "text");
        cJSON *nsp = cJSON_GetObjectItem(seg, "no_speech_prob");
        cJSON *lp = cJSON_GetObjectItem(seg, "avg_logprob");
        if (!cJSON_IsString(t)) continue;
        double no_speech = cJSON_IsNumber(nsp) ? nsp->valuedouble : 0;
        double logprob = cJSON_IsNumber(lp) ? lp->valuedouble : 0;
        if (no_speech > 0.6 && logprob < -1.0) continue;
        if (is_hallucination(t->valuestring)) continue;
        sb_append(&sb, t->valuestring);
        sb_append_char(&sb, ' ');
    }
    char *raw = sb_steal(&sb);
    char *r = str_trim(raw ? raw : "");
    free(raw);
    str_collapse_spaces(r);
    return r;
}

char *groq_transcribe(const int16_t *pcm, size_t samples, int sample_rate, GroqError *err)
{
    size_t wav_len;
    unsigned char *wav = wav_encode(pcm, samples, sample_rate, &wav_len);
    char boundary[48];
    unsigned char rnd[12];
    random_bytes(rnd, sizeof rnd);
    char *hex = hex_encode(rnd, sizeof rnd);
    snprintf(boundary, sizeof boundary, "----sokari%s", hex);
    free(hex);

    char *result = NULL;
    for (size_t m = 0; m < sizeof STT_MODELS / sizeof *STT_MODELS; m++) {
        StrBuf body;
        sb_init(&body);
        sb_appendf(&body,
                   "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                   "Content-Type: audio/wav\r\n\r\n",
                   boundary);
        sb_append_n(&body, (const char *)wav, wav_len);
        sb_appendf(&body, "\r\n--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", boundary,
                   STT_MODELS[m]);
        sb_appendf(&body, "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nes\r\n", boundary);
        sb_appendf(&body,
                   "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json\r\n", boundary);
        sb_appendf(&body, "--%s\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0\r\n", boundary);
        sb_appendf(&body, "--%s--\r\n", boundary);

        char *ctype = str_printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
        char *headers = auth_header(ctype);
        free(ctype);
        HttpRequest req = {.method = "POST", .url = GROQ_BASE "/audio/transcriptions", .headers = headers,
                           .body = body.data, .body_len = body.len, .timeout_ms = 60000};
        HttpResponse r = http_request(&req);
        free(headers);
        sb_free(&body);

        GroqStatus st = classify(&r);
        if (st == GROQ_OK) {
            cJSON *j = cJSON_Parse(r.body);
            if (j) {
                result = text_from_verbose(j);
                cJSON_Delete(j);
            } else {
                set_error(err, GROQ_BAD_RESPONSE, &r, "respuesta de transcripción ilegible");
            }
            http_response_free(&r);
            break;
        }
        char *detail = error_detail(&r);
        log_msg("Groq STT (%s) falló: %s", STT_MODELS[m], detail);
        set_error(err, st, &r, detail);
        free(detail);
        http_response_free(&r);
        if (st != GROQ_RATE_LIMITED && st != GROQ_SERVER_ERROR) break;
    }
    free(wav);
    return result;
}

static cJSON *clean_message(cJSON *msg)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "role", "assistant");
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content)) cJSON_AddStringToObject(out, "content", content->valuestring);
    else cJSON_AddStringToObject(out, "content", "");
    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
        cJSON *clean_calls = cJSON_AddArrayToObject(out, "tool_calls");
        cJSON *c;
        cJSON_ArrayForEach(c, calls)
        {
            cJSON *id = cJSON_GetObjectItem(c, "id");
            cJSON *fn = cJSON_GetObjectItem(c, "function");
            cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
            if (!cJSON_IsString(id) || !cJSON_IsString(name)) continue;
            cJSON *cc = cJSON_CreateObject();
            cJSON_AddStringToObject(cc, "id", id->valuestring);
            cJSON_AddStringToObject(cc, "type", "function");
            cJSON *f = cJSON_AddObjectToObject(cc, "function");
            cJSON_AddStringToObject(f, "name", name->valuestring);
            if (cJSON_IsString(args)) cJSON_AddStringToObject(f, "arguments", args->valuestring);
            else if (args) {
                char *s = cJSON_PrintUnformatted(args);
                cJSON_AddStringToObject(f, "arguments", s ? s : "{}");
                free(s);
            } else cJSON_AddStringToObject(f, "arguments", "{}");
            cJSON_AddItemToArray(clean_calls, cc);
        }
    }
    return out;
}

/* ------------------------------------------------ quién contesta el chat --- */

/* Groq primero (es el más rápido y ya tiene tu key) y, si tienes keys de
   respaldo, las demás: todas hablan el mismo formato (el de OpenAI). Cada
   modelo tiene su propio cupo, así que mientras más modelos, más cupo. */
typedef struct {
    const char *key;  /* como va en SOKARI_AI_ORDER */
    const char *name; /* para el log */
    const char *base;
    const char *const *prefer; /* pedazos del id, del mejor al más flojo */
    const char *const *fallback; /* si no se pudo preguntar qué modelos hay */
    bool free_only; /* OpenRouter: solo los ":free" */
} Provider;

static const char *const GROQ_PREFER[] = {"gpt-oss-120b", "qwen3", "kimi-k2", "llama-4-maverick", "llama-3.3-70b",
                                          "llama-4-scout", "gpt-oss-20b", NULL};
static const char *const GROQ_FALLBACK[] = {"openai/gpt-oss-120b", "qwen/qwen3.8-27b", "openai/gpt-oss-20b", NULL};
static const char *const NVIDIA_PREFER[] = {"gpt-oss-120b", "qwen3", "kimi-k2", "llama-3.3-70b", "llama-4-maverick",
                                            "deepseek-v3", "gpt-oss-20b", NULL};
static const char *const NVIDIA_FALLBACK[] = {"openai/gpt-oss-120b", "meta/llama-3.3-70b-instruct", NULL};
static const char *const DEEPSEEK_PREFER[] = {"deepseek-chat", NULL};
static const char *const DEEPSEEK_FALLBACK[] = {"deepseek-chat", NULL};
static const char *const OPENROUTER_PREFER[] = {"gpt-oss-120b", "qwen3", "kimi-k2", "llama-3.3-70b", "deepseek-chat",
                                                "deepseek-v3", "llama-4-maverick", "gpt-oss-20b", NULL};
static const char *const OPENROUTER_FALLBACK[] = {"openai/gpt-oss-120b:free", "meta-llama/llama-3.3-70b-instruct:free",
                                                  NULL};
static const char *const GLM_PREFER[] = {"glm-4.5-flash", "glm-4.7-flash", "glm-4-flash", "flash", NULL};
static const char *const GLM_FALLBACK[] = {"glm-4.5-flash", NULL};

enum { P_GROQ, P_NVIDIA, P_DEEPSEEK, P_OPENROUTER, P_GLM, P_COUNT };
static Provider PROVIDERS[P_COUNT] = {
    {"groq", "Groq", GROQ_BASE, GROQ_PREFER, GROQ_FALLBACK, false},
    {"nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1", NVIDIA_PREFER, NVIDIA_FALLBACK, false},
    {"deepseek", "DeepSeek", "https://api.deepseek.com", DEEPSEEK_PREFER, DEEPSEEK_FALLBACK, false},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", OPENROUTER_PREFER, OPENROUTER_FALLBACK, true},
    {"glm", "GLM", "https://api.z.ai/api/paas/v4", GLM_PREFER, GLM_FALLBACK, false},
};
/* Para las pruebas: otra dirección por proveedor (un servidor falso local). */
static char *g_base_override[P_COUNT];

#define MAX_MODELS_PER_PROVIDER 6

typedef struct {
    int provider;
    char id[128];
    int max_out;         /* tokens de respuesta que se piden */
    double cool_until;   /* no usar antes de este momento (epoch) */
    int remaining;       /* último cupo de tokens por minuto informado, -1 = desconocido */
    double measured_at;
    double reset_secs;
    bool dead;           /* el proveedor dijo que no existe: no se vuelve a probar */
} ChatModel;

static SRWLOCK g_models_lock = SRWLOCK_INIT;
static ChatModel *g_models;
static int g_nmodels;
static char *g_models_sig; /* con qué orden y keys se armó la lista */
static double g_models_at;
static double g_provider_off[P_COUNT]; /* key mala o sin créditos: no usar hasta aquí */

static const char *provider_base(int p)
{
    return g_base_override[p] ? g_base_override[p] : PROVIDERS[p].base;
}

static int provider_by_key(const char *key)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!_stricmp(key, PROVIDERS[i].key)) return i;
    return -1;
}

static char *provider_key(int p)
{
    return p == P_GROQ ? config_api_key() : config_provider_key(PROVIDERS[p].key);
}

static bool excluded_model(const char *low)
{
    static const char *const NO[] = {"whisper", "guard", "tts", "embed", "rerank", "audio", "image", "compound",
                                     "allam", "vision", "ocr", "safety", "reward"};
    for (size_t i = 0; i < sizeof NO / sizeof *NO; i++)
        if (strstr(low, NO[i])) return true;
    return false;
}

static bool supports_tools(const cJSON *m)
{
    const cJSON *params = cJSON_GetObjectItem(m, "supported_parameters");
    if (!cJSON_IsArray(params)) return true; /* quien no lo dice, se prueba */
    const cJSON *p;
    cJSON_ArrayForEach(p, params)
    {
        if (cJSON_IsString(p) && !strcmp(p->valuestring, "tools")) return true;
    }
    return false;
}

int groq_pick_models(const char *provider_key_name, const char *models_json, char ***out)
{
    *out = NULL;
    int p = provider_by_key(provider_key_name);
    cJSON *j = cJSON_Parse(models_json);
    const cJSON *data = cJSON_GetObjectItem(j, "data");
    if (p < 0 || !cJSON_IsArray(data)) {
        cJSON_Delete(j);
        return 0;
    }
    char **ids = xcalloc(MAX_MODELS_PER_PROVIDER, sizeof *ids);
    int n = 0;
    for (const char *const *pref = PROVIDERS[p].prefer; *pref && n < MAX_MODELS_PER_PROVIDER; pref++) {
        const cJSON *m;
        cJSON_ArrayForEach(m, data)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
            if (!id || n >= MAX_MODELS_PER_PROVIDER || strlen(id) >= 127) continue;
            const cJSON *active = cJSON_GetObjectItem(m, "active");
            if (cJSON_IsFalse(active)) continue;
            char *low = str_lower(id);
            bool ok = strstr(low, *pref) && !excluded_model(low) && supports_tools(m) &&
                      (!PROVIDERS[p].free_only || str_ends_with(low, ":free"));
            free(low);
            for (int k = 0; k < n && ok; k++) ok = strcmp(ids[k], id) != 0;
            if (ok) ids[n++] = xstrdup(id);
        }
    }
    cJSON_Delete(j);
    if (!n) {
        free(ids);
        return 0;
    }
    *out = ids;
    return n;
}

static int max_out_for(int p, const char *id)
{
    /* qwen en el plan gratis de Groq deja 1000 tokens de respuesta por minuto:
       si se le piden 1024, Groq rechaza TODOS los pedidos ("Request too large"). */
    return p == P_GROQ && strstr(id, "qwen") ? 700 : 1024;
}

/* Qué modelos tiene este proveedor ahora (preguntándole), o su lista fija. */
static int provider_models(int p, char ***out)
{
    char *key = provider_key(p);
    char *headers = str_printf("Authorization: Bearer %s\r\n", key);
    SecureZeroMemory(key, strlen(key));
    free(key);
    char *url = str_printf("%s/models", provider_base(p));
    HttpRequest hr = {.method = "GET", .url = url, .headers = headers, .timeout_ms = 8000};
    HttpResponse r = http_request(&hr);
    SecureZeroMemory(headers, strlen(headers));
    free(headers);
    free(url);
    int n = r.status == 200 && r.body ? groq_pick_models(PROVIDERS[p].key, r.body, out) : 0;
    http_response_free(&r);
    if (n) return n;
    int k = 0;
    while (PROVIDERS[p].fallback[k]) k++;
    char **ids = xcalloc((size_t)k, sizeof *ids);
    for (int i = 0; i < k; i++) ids[i] = xstrdup(PROVIDERS[p].fallback[i]);
    *out = ids;
    return k;
}

/* El orden de SOKARI_AI_ORDER con las keys que hay: "groq,nvidia". */
static char *active_signature(int order[P_COUNT], int *norder)
{
    char *cfg = config_ai_order();
    StrBuf sig;
    sb_init(&sig);
    *norder = 0;
    bool seen[P_COUNT] = {false};
    char *copy = xstrdup(cfg), *ctx = NULL;
    for (char *tok = strtok_s(copy, ", ;", &ctx); tok; tok = strtok_s(NULL, ", ;", &ctx)) {
        int p = provider_by_key(tok);
        if (p < 0 || seen[p]) continue;
        seen[p] = true;
        char *key = provider_key(p);
        bool has = *key != 0;
        SecureZeroMemory(key, strlen(key));
        free(key);
        if (!has) continue;
        order[(*norder)++] = p;
        sb_appendf(&sig, "%s%s", sig.len ? "," : "", PROVIDERS[p].key);
    }
    /* Groq siempre va, aunque lo hayan quitado del orden: es el de la voz. */
    if (!seen[P_GROQ]) {
        memmove(order + 1, order, sizeof *order * (size_t)*norder);
        order[0] = P_GROQ;
        (*norder)++;
        sb_appendf(&sig, "%sgroq", sig.len ? "," : "");
    }
    free(copy);
    free(cfg);
    return sig.data ? sig.data : xstrdup("");
}

/* Arma (o rearma, si cambió el orden o las keys, o cada 12 h) la lista de
   modelos a probar, en orden. Con g_models_lock tomado en exclusiva. */
static void ensure_models_locked(void)
{
    int order[P_COUNT], norder = 0;
    char *sig = active_signature(order, &norder);
    double now = now_epoch();
    if (g_models && g_models_sig && !strcmp(sig, g_models_sig) && now - g_models_at < 12 * 3600) {
        free(sig);
        return;
    }
    free(g_models);
    g_models = NULL;
    g_nmodels = 0;
    StrBuf names;
    sb_init(&names);
    for (int i = 0; i < norder; i++) {
        int p = order[i];
        char **ids;
        int n = provider_models(p, &ids);
        g_models = xrealloc(g_models, sizeof *g_models * (size_t)(g_nmodels + n));
        for (int k = 0; k < n; k++) {
            ChatModel *m = &g_models[g_nmodels++];
            memset(m, 0, sizeof *m);
            m->provider = p;
            snprintf(m->id, sizeof m->id, "%s", ids[k]);
            m->max_out = max_out_for(p, m->id);
            m->remaining = -1;
            sb_appendf(&names, "%s%s: %s", names.len ? "; " : "", PROVIDERS[p].name, ids[k]);
            free(ids[k]);
        }
        free(ids);
    }
    log_msg("Modelos para platicar, en orden: %s.", names.data ? names.data : "(ninguno)");
    sb_free(&names);
    free(g_models_sig);
    g_models_sig = sig;
    g_models_at = now;
}

void groq_set_base_url(const char *provider_key_name, const char *url)
{
    int p = provider_by_key(provider_key_name);
    if (p < 0) return;
    free(g_base_override[p]);
    g_base_override[p] = url ? xstrdup(url) : NULL;
    groq_reset_models();
}

void groq_reset_models(void)
{
    AcquireSRWLockExclusive(&g_models_lock);
    free(g_models);
    g_models = NULL;
    g_nmodels = 0;
    free(g_models_sig);
    g_models_sig = NULL;
    for (int i = 0; i < P_COUNT; i++) g_provider_off[i] = 0;
    ReleaseSRWLockExclusive(&g_models_lock);
}

#define TOKEN_LIMIT 8000.0
#define EST_REQUEST_TOKENS 2600.0
#define MAX_SILENT_WAIT 12.0

static bool model_ready(const ChatModel *m, double now, double *wait)
{
    if (m->dead) return false;
    if (now < g_provider_off[m->provider]) {
        *wait = g_provider_off[m->provider] - now;
        return false;
    }
    if (now < m->cool_until) {
        *wait = m->cool_until - now;
        return false;
    }
    if (m->remaining >= 0 && m->remaining < EST_REQUEST_TOKENS && m->reset_secs > 0) {
        double needed = m->reset_secs * (EST_REQUEST_TOKENS - m->remaining) / (TOKEN_LIMIT - m->remaining + 1.0);
        double ready = m->measured_at + needed;
        if (now < ready) {
            *wait = ready - now;
            return false;
        }
    }
    return true;
}

static void remember_limits(ChatModel *m, const HttpResponse *r)
{
    double now = now_epoch();
    if (r->rl_remaining_tokens >= 0) {
        m->remaining = r->rl_remaining_tokens;
        m->reset_secs = r->rl_reset_tokens > 0 ? r->rl_reset_tokens : 60;
        m->measured_at = now;
    }
    if (r->status == 429) m->cool_until = now + (r->retry_after > 0 ? r->retry_after : 20);
}

static cJSON *chat_once(ChatModel *model, const cJSON *messages, const cJSON *tools, GroqError *err, bool *retryable)
{
    *retryable = false;
    int p = model->provider;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model->id);
    cJSON_AddItemToObject(req, "messages", cJSON_Duplicate(messages, 1));
    if (tools) cJSON_AddItemToObject(req, "tools", cJSON_Duplicate(tools, 1));
    if (p == P_GROQ) {
        cJSON_AddNumberToObject(req, "max_completion_tokens", model->max_out);
        if (strstr(model->id, "qwen")) {
            cJSON_AddStringToObject(req, "reasoning_effort", "none");
            cJSON_AddStringToObject(req, "reasoning_format", "hidden");
        } else if (strstr(model->id, "gpt-oss")) {
            cJSON_AddStringToObject(req, "reasoning_effort", "low");
            cJSON_AddFalseToObject(req, "include_reasoning");
        }
    } else {
        cJSON_AddNumberToObject(req, "max_tokens", model->max_out);
    }
    char *payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char *key = provider_key(p);
    char *headers = str_printf("Authorization: Bearer %s\r\nContent-Type: application/json\r\n%s", key,
                               p == P_OPENROUTER ? "X-Title: Sokari\r\n" : "");
    SecureZeroMemory(key, strlen(key));
    free(key);
    char *url = str_printf("%s/chat/completions", provider_base(p));
    HttpRequest hr = {.method = "POST", .url = url, .headers = headers, .body = payload,
                      .body_len = strlen(payload), .timeout_ms = 60000};
    uint64_t t0 = GetTickCount64();
    HttpResponse r = http_request(&hr);
    SecureZeroMemory(headers, strlen(headers));
    free(headers);
    free(payload);
    free(url);
    remember_limits(model, &r);

    GroqStatus st = classify(&r);
    cJSON *result = NULL;
    if (st == GROQ_OK) {
        cJSON *j = cJSON_Parse(r.body);
        cJSON *choices = j ? cJSON_GetObjectItem(j, "choices") : NULL;
        cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
        /* Cuánto gastó de verdad: así se ve en el log cuánto dura el cupo diario. */
        cJSON *usage = j ? cJSON_GetObjectItem(j, "usage") : NULL;
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens"), *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        double secs = (double)(GetTickCount64() - t0) / 1000.0;
        if (cJSON_IsNumber(pt) && cJSON_IsNumber(ct)) {
            log_msg("%s (%s): %d tokens de entrada, %d de respuesta, %.1f s.", PROVIDERS[p].name, model->id,
                    pt->valueint, ct->valueint, secs);
            turn_stats_llm(pt->valueint, ct->valueint);
        } else {
            log_msg("%s (%s): contestó en %.1f s.", PROVIDERS[p].name, model->id, secs);
            turn_stats_llm(0, 0);
        }
        if (cJSON_IsObject(msg)) result = clean_message(msg);
        else {
            set_error(err, GROQ_BAD_RESPONSE, &r, "no mandó respuesta");
            *retryable = true;
        }
        cJSON_Delete(j);
    } else {
        char *detail = error_detail(&r);
        log_msg("%s (%s) HTTP %d: %s", PROVIDERS[p].name, model->id, r.status, detail);
        double now = now_epoch();
        if (r.status == 401 || r.status == 403 || r.status == 402) {
            /* Key mala o sin créditos: ese proveedor descansa un rato y se
               sigue con el siguiente. */
            g_provider_off[p] = now + (r.status == 402 ? 3600 : 1800);
            log_msg(r.status == 402 ? "%s: sin créditos; sigo con los demás."
                                    : "%s: la API key no sirve (revísala en Configuración → IA); sigo con los demás.",
                    PROVIDERS[p].name);
            *retryable = true;
        } else if (r.status == 404) {
            model->dead = true;
            *retryable = true;
        } else {
            /* gpt-oss a veces arma mal una tool call y el servidor devuelve 400
               ("tool_use_failed" / "Failed to parse tool call"): con otro
               modelo o reintentando suele salir bien. */
            *retryable = st == GROQ_RATE_LIMITED || st == GROQ_SERVER_ERROR || st == GROQ_NETWORK_ERROR ||
                         (st == GROQ_BAD_RESPONSE && r.body &&
                          (strstr(r.body, "tool_use_failed") || strstr(r.body, "tool call")));
        }
        if (st == GROQ_RATE_LIMITED && r.retry_after > 120)
            log_msg("%s (%s): sin cupo por hoy (vuelve en %d min).", PROVIDERS[p].name, model->id, r.retry_after / 60);
        set_error(err, p == P_GROQ || st != GROQ_AUTH_ERROR ? st : GROQ_SERVER_ERROR, &r, detail);
        free(detail);
    }
    http_response_free(&r);
    return result;
}

cJSON *groq_chat(const cJSON *messages, const cJSON *tools, GroqError *err)
{
    bool auth_failed = false;
    for (int attempt = 0; attempt < 4; attempt++) {
        AcquireSRWLockExclusive(&g_models_lock);
        ensure_models_locked();
        double now = now_epoch(), min_wait = 1e9;
        for (int i = 0; i < g_nmodels; i++) {
            ChatModel *m = &g_models[i];
            double wait = 0;
            if (!model_ready(m, now, &wait)) {
                if (!m->dead && wait < min_wait) min_wait = wait;
                continue;
            }
            bool retryable = false;
            /* El pedido puede tardar: la lista no se toca mientras tanto (solo
               la usa el hilo que platica, así que no estorba a nadie). */
            cJSON *msg = chat_once(m, messages, tools, err, &retryable);
            if (msg) {
                if (i && m->provider != g_models[0].provider)
                    log_msg("Contestó %s porque los de antes no tenían cupo.", PROVIDERS[m->provider].name);
                ReleaseSRWLockExclusive(&g_models_lock);
                if (err) err->status = GROQ_OK;
                return msg;
            }
            if (err && err->status == GROQ_AUTH_ERROR && m->provider == P_GROQ) auth_failed = true;
            if (!retryable) {
                ReleaseSRWLockExclusive(&g_models_lock);
                return NULL;
            }
            now = now_epoch();
            double w = m->cool_until - now;
            if (!m->dead && w > 0 && w < min_wait) min_wait = w;
        }
        ReleaseSRWLockExclusive(&g_models_lock);
        if (auth_failed && err) err->status = GROQ_AUTH_ERROR;
        if (min_wait > MAX_SILENT_WAIT || min_wait >= 1e8) {
            if (err && !auth_failed && min_wait < 1e8) {
                err->status = GROQ_RATE_LIMITED;
                err->retry_after = (int)(min_wait + 0.999);
            }
            return NULL;
        }
        if (min_wait > 0) {
            log_msg("Todos los modelos sin cupo; espero %.1fs.", min_wait);
            app_status("Esperando cupo de la IA…");
            Sleep((DWORD)(min_wait * 1000) + 250);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- con visión --- */

/* Los que ven imágenes, del mejor al más flojo (nombres de hoy; si uno ya no
   existe, el proveedor contesta 404 y se prueba el siguiente). */
static const struct {
    int p;
    const char *id;
} VISION[] = {
    {P_GROQ, "meta-llama/llama-4-scout-17b-16e-instruct"},
    {P_GROQ, "meta-llama/llama-4-maverick-17b-128e-instruct"},
    {P_OPENROUTER, "meta-llama/llama-4-scout:free"},
    {P_OPENROUTER, "qwen/qwen2.5-vl-72b-instruct:free"},
    {P_NVIDIA, "meta/llama-4-scout-17b-16e-instruct"},
    {P_NVIDIA, "meta/llama-3.2-90b-vision-instruct"},
    {P_GLM, "glm-4v-flash"},
};

char *groq_vision(const char *prompt, const char *jpeg_b64, int *tokens, GroqError *err)
{
    if (tokens) *tokens = 0;
    bool any_key = false;
    char *answer = NULL;
    for (size_t i = 0; i < sizeof VISION / sizeof *VISION && !answer; i++) {
        int p = VISION[i].p;
        char *key = provider_key(p);
        if (!key || !*key) {
            free(key);
            continue;
        }
        any_key = true;
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", VISION[i].id);
        cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON *content = cJSON_AddArrayToObject(m, "content");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "text");
        cJSON_AddStringToObject(t, "text", prompt);
        cJSON_AddItemToArray(content, t);
        cJSON *img = cJSON_CreateObject();
        cJSON_AddStringToObject(img, "type", "image_url");
        cJSON *url = cJSON_AddObjectToObject(img, "image_url");
        char *data = str_printf("data:image/jpeg;base64,%s", jpeg_b64);
        cJSON_AddStringToObject(url, "url", data);
        free(data);
        cJSON_AddItemToArray(content, img);
        cJSON_AddItemToArray(msgs, m);
        cJSON_AddNumberToObject(req, p == P_GROQ ? "max_completion_tokens" : "max_tokens", 400);
        char *payload = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        char *headers = str_printf("Authorization: Bearer %s\r\nContent-Type: application/json\r\n%s", key,
                                   p == P_OPENROUTER ? "X-Title: Sokari\r\n" : "");
        SecureZeroMemory(key, strlen(key));
        free(key);
        char *u = str_printf("%s/chat/completions", provider_base(p));
        HttpRequest hr = {.method = "POST", .url = u, .headers = headers, .body = payload,
                          .body_len = strlen(payload), .timeout_ms = 60000};
        HttpResponse r = http_request(&hr);
        SecureZeroMemory(headers, strlen(headers));
        free(headers);
        free(payload);
        free(u);
        if (classify(&r) == GROQ_OK) {
            cJSON *j = cJSON_Parse(r.body);
            cJSON *choices = j ? cJSON_GetObjectItem(j, "choices") : NULL;
            cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
            cJSON *msg = first ? cJSON_GetObjectItem(first, "message") : NULL;
            const char *c = cJSON_GetStringValue(msg ? cJSON_GetObjectItem(msg, "content") : NULL);
            if (c && *c) answer = xstrdup(c);
            cJSON *pt = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "usage"), "prompt_tokens");
            if (tokens && cJSON_IsNumber(pt)) *tokens = pt->valueint;
            if (answer) log_msg("%s (%s) vio la pantalla: %d tokens de entrada.", PROVIDERS[p].name, VISION[i].id,
                                cJSON_IsNumber(pt) ? pt->valueint : -1);
            cJSON_Delete(j);
        } else {
            char *detail = error_detail(&r);
            log_msg("%s (%s) con imagen, HTTP %d: %s", PROVIDERS[p].name, VISION[i].id, r.status, detail);
            set_error(err, classify(&r), &r, detail);
            free(detail);
        }
        http_response_free(&r);
    }
    if (!answer && !any_key) set_error(err, GROQ_BAD_RESPONSE, NULL, "sin-vision");
    return answer;
}

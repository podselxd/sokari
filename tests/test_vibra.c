/* La vibra: que lo que dice Sokari (y lo que le dices) se sienta en su cara y
   en su voz. La voz con emoción se mide sobre una «voz» de prueba (sílabas de
   una vocal con su tono): que dure lo que debe, que suba o baje el tono lo que
   debe, que no truene y que tiemble con miedo. Luego lo que entiende de cada
   frase, las respuestas sin IA (¿tienes emociones?, ¿cómo estás?, la muestra),
   los gestos nuevos y los ojos sin la rayita en diagonal en neutral. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "config.h"
#include "face.h"
#include "prosody.h"
#include "skills.h"
#include "sphere.h"
#include "util.h"
#include "vibe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE 24000

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

/* ------------------------------------------------------------ la voz --- */

/* Sílabas de 200 ms de una vocal (armónicos con caída) con 50 ms entre una y
   otra, y 300 ms de silencio al final, como lo que da un sintetizador. */
static int16_t *voice_like(float f0, float seconds, float amp, size_t *n)
{
    size_t body = (size_t)(seconds * RATE), tail = (size_t)(0.3f * RATE);
    *n = body + tail;
    int16_t *x = xcalloc(*n, sizeof *x);
    double ph = 0;
    for (size_t i = 0; i < body; i++) {
        double t = (double)i / RATE, syl = fmod(t, 0.25);
        double env = syl < 0.2 ? sin(M_PI * syl / 0.2) : 0;
        ph += 2 * M_PI * f0 / RATE;
        double v = 0;
        for (int h = 1; h <= 12; h++) v += sin(h * ph) / h;
        x[i] = (int16_t)(v * env * amp);
    }
    return x;
}

/* El tono (F0) por autocorrelación: el primer pico casi tan alto como el
   mayor (así no confunde el tono con su octava de abajo), afinado con una
   parábola. */
static float pitch_of(const int16_t *x, size_t n)
{
    size_t len = n < (size_t)RATE ? n : (size_t)RATE;
    int lo = RATE / 500, hi = RATE / 60;
    double *r = xcalloc((size_t)hi + 2, sizeof *r), top = -1;
    for (int L = lo - 1; L <= hi + 1; L++) {
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i + (size_t)L < len; i++) {
            ab += (double)x[i] * x[i + L];
            aa += (double)x[i] * x[i];
            bb += (double)x[i + L] * x[i + L];
        }
        r[L] = ab / sqrt(aa * bb + 1e-9);
        if (L >= lo && L <= hi && r[L] > top) top = r[L];
    }
    int lag = 0;
    for (int L = lo; L <= hi && !lag; L++)
        if (r[L] >= 0.9 * top && r[L] >= r[L - 1] && r[L] >= r[L + 1]) lag = L;
    double d = r[lag - 1] - 2 * r[lag] + r[lag + 1];
    double off = fabs(d) > 1e-12 ? 0.5 * (r[lag - 1] - r[lag + 1]) / d : 0;
    free(r);
    return (float)(RATE / (lag + off));
}

static double rms(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return n ? sqrt(s / (double)n) : 0;
}

static size_t tail_silence(const int16_t *x, size_t n)
{
    size_t end = n;
    while (end > 0 && abs(x[end - 1]) <= 200) end--;
    return n - end;
}

static bool close_to(double got, double want, double tol)
{
    return fabs(got - want) <= tol * fabs(want);
}

static void test_prosody(void)
{
    printf("-- la voz con emoción (sobre el audio) --\n");
    char what[200];
    size_t n0, n;
    int16_t *x = voice_like(180, 2.0f, 9000, &n0);
    float f0 = pitch_of(x, n0);
    check(close_to(f0, 180, 0.01), "la voz de prueba tiene su tono (180 Hz) bien medido");

    Prosody p = prosody_neutral();
    n = n0;
    int16_t *y = prosody_apply(x, &n, RATE, &p);
    check(y == x && n == n0, "neutral: tu voz como es (ni se toca)");

    p = prosody_neutral();
    p.tempo = 1.1f;
    y = voice_like(180, 2.0f, 9000, &n);
    y = prosody_apply(y, &n, RATE, &p);
    snprintf(what, sizeof what, "10 %% más rápida: dura 1/1.1 (%.3f s de %.3f s) sin cambiar el tono (%.1f Hz)",
             (double)n / RATE, (double)n0 / RATE, pitch_of(y, n));
    check(close_to((double)n, n0 / 1.1, 0.02) && close_to(pitch_of(y, n), 180, 0.02), what);
    free(y);

    p = prosody_neutral();
    p.pitch = 2;
    y = voice_like(180, 2.0f, 9000, &n);
    y = prosody_apply(y, &n, RATE, &p);
    float want = 180 * powf(2, 2 / 12.0f);
    snprintf(what, sizeof what, "2 semitonos más aguda: %.1f Hz (esperado %.1f) y dura lo mismo (%.3f s)",
             pitch_of(y, n), want, (double)n / RATE);
    check(close_to(pitch_of(y, n), want, 0.02) && close_to((double)n, n0, 0.02), what);
    free(y);

    p = prosody_neutral();
    p.pitch = -1.8f;
    p.tempo = 0.86f;
    y = voice_like(180, 2.0f, 9000, &n);
    y = prosody_apply(y, &n, RATE, &p);
    want = 180 * powf(2, -1.8f / 12.0f);
    snprintf(what, sizeof what, "más grave y lenta a la vez: %.1f Hz (esperado %.1f), %.3f s (esperado %.3f)",
             pitch_of(y, n), want, (double)n / RATE, n0 / 0.86 / RATE);
    check(close_to(pitch_of(y, n), want, 0.02) && close_to((double)n, n0 / 0.86, 0.02), what);
    free(y);

    /* más fuerte sin tronar */
    int16_t *loud = voice_like(180, 1.0f, 26000, &n);
    double before = rms(loud, n);
    p = prosody_neutral();
    p.gain_db = 2.5f;
    loud = prosody_apply(loud, &n, RATE, &p);
    int top = 0, pegged = 0;
    for (size_t i = 0; i < n; i++) {
        top = abs(loud[i]) > top ? abs(loud[i]) : top;
        pegged += abs(loud[i]) >= 32767;
    }
    snprintf(what, sizeof what, "más fuerte (%.0f → %.0f de volumen) sin tronar (pico %d, %d muestras topadas)", before,
             rms(loud, n), top, pegged);
    check(rms(loud, n) > before * 1.1 && top < 32767 && pegged == 0, what);
    free(loud);

    /* tiembla: el volumen late (un tono parejo deja de serlo) */
    size_t nt = RATE;
    int16_t *tone = xcalloc(nt, sizeof *tone);
    for (size_t i = 0; i < nt; i++) tone[i] = (int16_t)(8000 * sin(2 * M_PI * 200 * (double)i / RATE));
    p = prosody_neutral();
    p.tremble = 1;
    tone = prosody_apply(tone, &nt, RATE, &p);
    double lo = 1e9, hi = 0;
    for (size_t w = RATE / 10; w + 240 <= nt - RATE / 10; w += 240) {
        double e = rms(tone + w, 240);
        lo = e < lo ? e : lo;
        hi = e > hi ? e : hi;
    }
    snprintf(what, sizeof what, "con miedo la voz tiembla (el volumen sube y baja %.0f %%)", 100 * (hi - lo) / hi);
    check((hi - lo) / hi > 0.1, what);
    free(tone);

    /* el silencio del final */
    y = voice_like(180, 1.0f, 9000, &n);
    size_t n1 = n, s1 = tail_silence(y, n);
    p = prosody_neutral();
    p.pause_ms = 240;
    y = prosody_apply(y, &n, RATE, &p);
    snprintf(what, sizeof what, "pausa más larga: +%.0f ms de silencio al final", (double)(n - n1) * 1000 / RATE);
    check(close_to((double)(n - n1), 0.24 * RATE, 0.02) && tail_silence(y, n) > s1, what);
    free(y);
    y = voice_like(180, 1.0f, 9000, &n);
    p = prosody_neutral();
    p.pause_ms = -600;
    y = prosody_apply(y, &n, RATE, &p);
    snprintf(what, sizeof what, "pausa más corta, pero nunca pegada a lo siguiente (quedan %.0f ms)",
             (double)tail_silence(y, n) * 1000 / RATE);
    check(tail_silence(y, n) >= (size_t)(0.025 * RATE) && tail_silence(y, n) < s1, what);
    free(y);

    /* cosas raras: ni se cae ni se queda sin nada */
    bool fine = true;
    for (int k = 0; k < 4; k++) {
        size_t m = k == 0 ? 1 : k == 1 ? 10 : k == 2 ? 700 : 3000;
        int16_t *z = xcalloc(m, sizeof *z);
        if (k == 3)
            for (size_t i = 0; i < m; i++) z[i] = (int16_t)(i * 7919 % 20000 - 10000);
        Prosody q = {0.8f, 3, 3, 1, -300};
        z = prosody_apply(z, &m, RATE, &q);
        fine &= z != NULL && m > 0;
        free(z);
    }
    check(fine, "audio de 1, 10, 700 muestras o puro ruido: no se cae");

    /* rápido: una frase larga en lo que tarda en sonar la anterior */
    y = voice_like(180, 4.0f, 9000, &n);
    p = prosody_neutral();
    p.pitch = 1.8f;
    p.tempo = 1.1f;
    p.tremble = 1;
    p.gain_db = 1;
    uint64_t t0 = GetTickCount64();
    y = prosody_apply(y, &n, RATE, &p);
    uint64_t ms = GetTickCount64() - t0;
    snprintf(what, sizeof what, "4 s de voz se procesan en %llu ms", (unsigned long long)ms);
    check(ms < 1500, what);
    free(y);
    free(x);

    printf("-- cómo suena cada emoción --\n");
    float w[AFF_COUNT];
    Prosody e[AFF_COUNT];
    for (int k = 0; k < AFF_COUNT; k++) {
        memset(w, 0, sizeof w);
        w[k] = 1;
        e[k] = prosody_for(w);
    }
    check(prosody_is_neutral(&e[AFF_NEUTRAL]), "neutral: igual que siempre");
    affect_weights_at(0, 0, w);
    Prosody calm = prosody_for(w);
    check(prosody_is_neutral(&calm), "tranquila (las demás pesan poquito): tu voz tal cual, ni un temblorcito");
    float am[AFF_COUNT] = {0}, pv, pa;
    am[AFF_ANGER] = 0.8f;
    affect_point(am, &pv, &pa);
    affect_weights_at(pv, pa, w);
    Prosody angry = prosody_for(w);
    char what2[160];
    snprintf(what2, sizeof what2, "furia (que en la mezcla trae algo de temor): fuerte (%+.1f dB) y casi sin temblar (%.2f)",
             angry.gain_db, angry.tremble);
    check(angry.gain_db > 1.5f && angry.tremble < 0.2f, what2);
    check(e[AFF_JOY].tempo > 1.05f && e[AFF_JOY].pitch > 0.3f && e[AFF_JOY].pitch < 1.2f && e[AFF_JOY].pause_ms < 0,
          "alegría: más rápida, apenas más aguda (sin chillar) y con pausas cortas");
    check(e[AFF_SADNESS].tempo < 0.9f && e[AFF_SADNESS].pitch < -1 && e[AFF_SADNESS].gain_db < -2 &&
              e[AFF_SADNESS].pause_ms > 150,
          "tristeza: lenta, grave, bajita y con pausas largas");
    check(e[AFF_ANGER].gain_db > 2 && e[AFF_ANGER].pitch <= 0, "furia: firme y más fuerte");
    check(e[AFF_FEAR].tremble > 0.5f && e[AFF_FEAR].tempo > 1.05f, "temor: rápida y temblorosa");
    check(e[AFF_DISGUST].tempo < 1 && e[AFF_DISGUST].pitch < 0, "desagrado: un poco lenta y grave");
    bool small = true;
    for (int k = 0; k < AFF_COUNT; k++)
        small &= e[k].tempo >= 0.85f && e[k].tempo <= 1.15f && fabsf(e[k].pitch) <= 2 && fabsf(e[k].gain_db) <= 3;
    check(small, "cambios chicos: 15 % de velocidad, 2 semitonos y 3 dB como mucho (no suena a caricatura)");
    affect_weights_at(0.3f, 0.2f, w);
    Prosody mixed = prosody_for(w);
    check(mixed.tempo > 1 && mixed.tempo < e[AFF_JOY].tempo, "un poco contenta: un poco más rápida, no del todo");
}

/* ----------------------------------------------------------- la vibra --- */

static AffectKind strongest(const float w[AFF_COUNT])
{
    int best = 0;
    for (int k = 1; k < AFF_COUNT; k++)
        if (w[k] > w[best]) best = k;
    return (AffectKind)best;
}

static void user_case(const char *text, AffectKind want, AffectCue cue)
{
    Vibe v = vibe_of_user(text);
    char what[200];
    snprintf(what, sizeof what, "«%s» → %s", text,
             !v.found && want == AFF_NEUTRAL ? "nada (no va para ella)" : want == AFF_SADNESS ? "se agüita" : "se sonroja");
    bool ok = want == AFF_NEUTRAL ? !v.found : v.found && v.amount[want] >= 0.6f && v.cue == cue;
    check(ok, what);
}

static void text_case(const char *text, AffectKind want, AffectCue cue)
{
    Vibe v = vibe_of_text(text);
    char what[200];
    snprintf(what, sizeof what, "«%s» → %s%s", text, want == AFF_NEUTRAL ? "sin emoción" : affect_name(want),
             cue == AFF_CUE_NONE ? "" : " y su gesto");
    int best = 0;
    for (int k = 1; k < AFF_COUNT; k++)
        if (v.amount[k] > v.amount[best]) best = k;
    bool ok = (want == AFF_NEUTRAL ? v.found == 0 : v.found && best == (int)want) && v.cue == cue;
    check(ok, what);
}

static void test_vibe(void)
{
    printf("-- lo que le dices a ella --\n");
    user_case("tienes culera voz", AFF_SADNESS, AFF_CUE_SIGH);
    user_case("eres una inútil", AFF_SADNESS, AFF_CUE_SIGH);
    user_case("tonta", AFF_SADNESS, AFF_CUE_SIGH);
    user_case("Sokari, no sirves para nada", AFF_SADNESS, AFF_CUE_SIGH);
    user_case("te odio", AFF_SADNESS, AFF_CUE_SIGH);
    user_case("qué culera canción está sonando", AFF_NEUTRAL, AFF_CUE_NONE);
    user_case("mi jefe es un idiota, ayúdame con el correo", AFF_NEUTRAL, AFF_CUE_NONE);
    user_case("eres la mejor", AFF_JOY, AFF_CUE_BLUSH);
    user_case("qué linda", AFF_JOY, AFF_CUE_BLUSH);
    user_case("me gusta tu voz", AFF_JOY, AFF_CUE_BLUSH);
    user_case("qué linda canción, ponla otra vez", AFF_NEUTRAL, AFF_CUE_NONE);
    check(vibe_only_insult("tienes culera voz") && vibe_only_insult("oye, eres una inútil") &&
              vibe_only_insult("pinche Sokari tonta"),
          "solo un insulto: lo contesta ella, agüitada, sin IA");
    check(!vibe_only_insult("tienes culera voz, cámbiala por otra") && !vibe_only_insult("¿qué hora es, tonta?") &&
              !vibe_only_insult("pon música"),
          "un insulto con un pedido: el pedido se hace (va a la IA)");

    printf("-- lo que dice ella, frase por frase --\n");
    text_case("¡Listo! Ya te puse la música.", AFF_JOY, AFF_CUE_SUCCESS);
    text_case("Lo siento, no pude abrirlo.", AFF_SADNESS, AFF_CUE_SIGH);
    text_case("¡Me encanta esa canción!", AFF_JOY, AFF_CUE_HAPPY);
    text_case("¡Cuidado! La batería está muy baja.", AFF_FEAR, AFF_CUE_NONE);
    text_case("¡Guácala, qué asco!", AFF_DISGUST, AFF_CUE_NONE);
    text_case("¡No manches! ¿En serio?", AFF_NEUTRAL, AFF_CUE_SURPRISE);
    text_case("Hasta luego, que descanses.", AFF_NEUTRAL, AFF_CUE_GOODBYE);
    text_case("Eso no es genial, la verdad.", AFF_NEUTRAL, AFF_CUE_NONE);
    text_case("Son las 3 y 20 de la tarde.", AFF_NEUTRAL, AFF_CUE_NONE);

    float base[AFF_COUNT];
    affect_weights_at(0, 0, base);
    VibeLine *l;
    int n = vibe_lines("Lo siento, no pude abrir el archivo que me pediste. Pero ya te puse la música, ¡listo!", base, &l);
    check(n == 2 && strongest(l[0].weights) == AFF_SADNESS && l[0].cue == AFF_CUE_SIGH &&
              l[0].prosody.tempo < 1 && l[0].prosody.pitch < 0 && l[1].weights[AFF_JOY] > l[0].weights[AFF_JOY] &&
              l[1].prosody.tempo > 1 && l[1].cue == AFF_CUE_SUCCESS,
          "«lo siento, no pude…» suena bajita y lenta; «¡listo!» de después, más arriba");
    vibe_lines_free(l, n);
    float sad[AFF_COUNT];
    affect_weights_at(-0.5f, -0.6f, sad);
    n = vibe_lines("Son las tres y veinte de la tarde.", sad, &l);
    check(n == 1 && !l[0].own && strongest(l[0].weights) == AFF_SADNESS && l[0].prosody.tempo < 0.95f,
          "una frase sin nada propio va con la emoción de toda la respuesta (agüitada, también la hora)");
    vibe_lines_free(l, n);

    printf("-- las marcas (solo las pone Sokari) --\n");
    const char *demo = "[afecto: alegría 1] ¡Así me pongo cuando algo sale bien! [afecto: temor 1] Así, cuando algo me "
                       "asusta. [afecto: neutral 1] Y así, tranquila.";
    n = vibe_lines(demo, base, &l);
    check(n == 3 && l[0].own && strongest(l[0].weights) == AFF_JOY && strongest(l[1].weights) == AFF_FEAR &&
              strongest(l[2].weights) == AFF_NEUTRAL && !strchr(l[0].text, '[') && l[1].prosody.pause_ms > 300,
          "cada frase con la emoción de su marca, sin la marca, y un respiro entre una y otra");
    vibe_lines_free(l, n);
    n = vibe_lines("[gesto: sonrojo] ¡Ay, gracias! [gesto: guiño] Nos vemos.", base, &l);
    check(n == 2 && l[0].cue == AFF_CUE_BLUSH && l[1].cue == AFF_CUE_GOODBYE, "las marcas de gesto");
    vibe_lines_free(l, n);
    char *clean = vibe_strip(demo);
    check(!strchr(clean, '[') && !strstr(clean, "afecto") && str_starts_with(clean, "¡Así me pongo"),
          "sin marcas para los subtítulos, el historial y la memoria");
    free(clean);
    clean = vibe_strip("Mira [esto] y [gesto: suspiro] aquello");
    check(!strcmp(clean, "Mira [esto] y aquello"), "un corchete que no es marca se queda");
    free(clean);

    printf("-- «¿cómo estás?» en palabras --\n");
    AffectState s;
    memset(&s, 0, sizeof s);
    bool no_numbers = true;
    const char *seen[AFF_COUNT];
    for (int k = 0; k < AFF_COUNT; k++) {
        s.dominant = (AffectKind)k;
        s.intensity = 0.4f;
        char *r = vibe_how_i_feel(&s), *plain = vibe_strip(r);
        no_numbers &= !strpbrk(plain, "0123456789%");
        seen[k] = k == AFF_SADNESS ? (strstr(plain, "agüitada") ? "ok" : NULL)
                  : k == AFF_JOY   ? (strstr(plain, "contenta") ? "ok" : NULL)
                                   : "ok";
        free(plain);
        free(r);
    }
    check(no_numbers, "nunca dice números");
    check(seen[AFF_SADNESS] && seen[AFF_JOY], "agüitada si está triste, contenta si está alegre");
    s.dominant = AFF_SADNESS;
    char *r = vibe_how_i_feel(&s);
    check(strstr(r, "[gesto: suspiro]") != NULL, "y hace el gesto que va (suspira si está agüitada)");
    free(r);
    s.dominant = AFF_NEUTRAL;
    r = vibe_am_i(&s, AFF_SADNESS);
    clean = vibe_strip(r);
    check(str_starts_with(clean, "No."), "«¿estás triste?» estando tranquila: «No. …»");
    free(clean);
    free(r);
    s.dominant = AFF_ANGER;
    r = vibe_am_i(&s, AFF_ANGER);
    check(str_starts_with(r, "Sí"), "«¿estás enojada?» estando molesta: «Sí, …»");
    free(r);
}

/* --------------------------------------------- con el motor de verdad --- */

static void test_engine(void)
{
    printf("-- que la emoción dure mientras habla --\n");
    float joy[AFF_COUNT] = {0};
    joy[AFF_JOY] = 0.8f;
    AffectEngine *a = affect_engine_create(NULL), *b = affect_engine_create(NULL);
    affect_engine_emotions(a, 1, joy, "prueba");
    affect_engine_emotions(b, 1, joy, "prueba");
    affect_engine_sustain(b, 1.5, 10);
    affect_engine_advance(a, 12);
    affect_engine_advance(b, 12);
    float va, vb;
    affect_engine_target(a, &va, NULL);
    affect_engine_target(b, &vb, NULL);
    char what[200];
    snprintf(what, sizeof what, "a los 11 s: sin sostener ya se fue (%.2f); sostenida mientras habla, sigue (%.2f)", va,
             vb);
    check(vb > 0.4f && vb > va + 0.2f, what);
    affect_engine_destroy(a);
    affect_engine_destroy(b);

    printf("-- lo que le dices, con el afecto de Sokari --\n");
    vibe_after_turn("tienes culera voz", "Oye… eso me agüitó.", false);
    float v, ar;
    affect_target(&v, &ar);
    float w[AFF_COUNT];
    affect_target_weights(w);
    check(v < -0.3f && strongest(w) == AFF_SADNESS, "un insulto la agüita (aunque la IA dijera neutral)");
    unsigned seq0;
    affect_last_cue(&seq0);
    vibe_after_turn("eres la mejor", "¡Gracias!", true);
    unsigned seq1;
    AffectCue c = affect_last_cue(&seq1);
    affect_target(&v, &ar);
    check(c == AFF_CUE_BLUSH && seq1 != seq0 && v > 0.2f, "un halago la sonroja y la alegra");
    vibe_after_turn("¿qué hora es?", "Lo siento, no pude ver la hora.", true);
    float after;
    affect_target(&after, NULL);
    check(fabsf(after - v) < 0.15f, "con la etiqueta de la IA, lo que contesta ya no se vuelve a contar");
}

/* ------------------------------------------------ respuestas sin IA --- */

static char *ask(const char *text)
{
    const SkillInfo *which;
    bool end;
    return skills_try(text, &which, &end);
}

static void test_answers(void)
{
    printf("-- lo que contesta sin IA --\n");
    char *r = ask("¿Tienes emociones?"), *clean = r ? vibe_strip(r) : NULL;
    check(clean && str_starts_with(clean, "Sí, a mi manera") && !strpbrk(clean, "0123456789"),
          "«¿tienes emociones?» → «Sí, a mi manera…» (sin explicar cómo funciona)");
    free(clean);
    free(r);
    r = ask("¿Cómo van tus emociones?");
    check(r && !strpbrk(r, "0123456789%"), "«¿cómo van tus emociones?» → en palabras, sin números");
    free(r);
    r = ask("¿Cómo estás, Sokari?");
    check(r && !strpbrk(r, "0123456789%") && strstr(r, "[gesto:"), "«¿cómo estás?» → como de veras está, con su gesto");
    free(r);
    r = ask("Muéstrame tus emociones");
    int marks = 0;
    for (const char *p = r ? r : ""; (p = strstr(p, "[afecto:")); p++) marks++;
    check(marks == 6, "«muéstrame tus emociones» → las 6, una tras otra (sin números ni comandos)");
    if (r) {
        float base[AFF_COUNT];
        affect_weights_at(0, 0, base);
        VibeLine *l;
        int n = vibe_lines(r, base, &l);
        static const AffectKind ORDER[] = {AFF_JOY, AFF_FEAR, AFF_ANGER, AFF_DISGUST, AFF_SADNESS, AFF_NEUTRAL};
        bool ok = n == 6;
        for (int i = 0; ok && i < 6; i++) ok = strongest(l[i].weights) == ORDER[i];
        check(ok, "y cada frase se dice con la suya (alegría, temor, furia, desagrado, tristeza, tranquila)");
        vibe_lines_free(l, n);
    }
    free(r);
    r = ask("Muéstrame tus gestos");
    check(r && strstr(r, "[gesto: sonrojo]") && strstr(r, "[gesto: sorpresa]"), "«muéstrame tus gestos» → los nuevos");
    free(r);
    r = ask("Enójate");
    check(r && strstr(r, "[afecto: furia 1]"), "«enójate» → se enoja (de a mentis)");
    free(r);
    r = ask("tienes culera voz");
    clean = r ? vibe_strip(r) : NULL;
    check(clean && (strstr(clean, "agüit") || strstr(clean, "dolió") || strstr(clean, "mejor") || strstr(clean, "así")),
          "«tienes culera voz» → se agüita (0 tokens)");
    free(clean);
    free(r);
    r = ask("tienes culera voz, cámbiala por otra");
    check(!r, "«tienes culera voz, cámbiala por otra» → eso sí va a la IA (para cambiarla)");
    free(r);
    r = ask("¿Estás triste?");
    check(r != NULL, "«¿estás triste?» → contesta según cómo está");
    free(r);
}

/* ------------------------------------------------------------ la cara --- */

static bool finite_face(const SphereFace *o)
{
    const float *v = &o->eye_w;
    for (size_t i = 0; i < (sizeof *o - offsetof(SphereFace, eye_w)) / sizeof(float); i++)
        if (!isfinite(v[i])) return false;
    return true;
}

static void test_face(void)
{
    printf("-- los gestos nuevos --\n");
    static const FaceGesture NEW[] = {FACE_G_HAPPY, FACE_G_SURPRISE, FACE_G_SIGH, FACE_G_BLUSH};
    static const char *const NAME[] = {"ojos felices", "sorpresa", "suspiro", "sonrojo"};
    for (int g = 0; g < 4; g++) {
        Face *f = face_create(7);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        in.symbols = true;
        affect_weights_at(0, 0, in.affect.weights);
        SphereFace o;
        face_step(f, 0.02, &in, &o);
        face_play(f, NEW[g]);
        float happy = 0, eye = 0, lid = 0, blush = 0, scale = 2;
        bool bang = false, finite = true;
        for (int i = 0; i < 180; i++) {
            face_step(f, 1.0 / 60, &in, &o);
            finite &= finite_face(&o);
            happy = fmaxf(happy, o.happy);
            eye = fmaxf(eye, o.eye_w);
            lid = fmaxf(lid, o.lid_top);
            blush = fmaxf(blush, o.blush);
            scale = fminf(scale, o.scale);
            for (int k = 0; k < o.nsym; k++) bang |= o.sym[k].kind == SPHERE_SYM_EXCLAIM;
        }
        bool ok = finite && !strcmp(face_gesture_name(NEW[g]), NAME[g]);
        char what[160];
        switch (NEW[g]) {
        case FACE_G_HAPPY: ok &= happy > 0.9f; snprintf(what, sizeof what, "ojos felices: «^ ^» (%.2f)", happy); break;
        case FACE_G_SURPRISE:
            ok &= eye > 1.25f && bang;
            snprintf(what, sizeof what, "sorpresa: ojos grandes (%.2f) y «!»", eye);
            break;
        case FACE_G_SIGH:
            ok &= lid > 0.3f && scale < 0.97f;
            snprintf(what, sizeof what, "suspiro: baja los párpados (%.2f) y se desinfla (%.2f)", lid, scale);
            break;
        default: ok &= blush > 0.9f; snprintf(what, sizeof what, "sonrojo: chapitas (%.2f)", blush); break;
        }
        check(ok, what);
        face_destroy(f);
    }

    printf("-- cada cosa con su gesto --\n");
    static const struct {
        AffectCue cue;
        FaceGesture g;
    } MAP[] = {{AFF_CUE_HAPPY, FACE_G_HAPPY},     {AFF_CUE_SURPRISE, FACE_G_SURPRISE}, {AFF_CUE_SIGH, FACE_G_SIGH},
               {AFF_CUE_BLUSH, FACE_G_BLUSH},     {AFF_CUE_GOODBYE, FACE_G_WINK},      {AFF_CUE_SEARCH, FACE_G_LOOK},
               {AFF_CUE_SUCCESS, FACE_G_BOUNCE}, {AFF_CUE_DONE, FACE_G_NOD},         {AFF_CUE_FAIL, FACE_G_SHAKE}};
    bool all = true;
    for (size_t i = 0; i < sizeof MAP / sizeof *MAP; i++) {
        Face *f = face_create(3);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        affect_weights_at(0, 0, in.affect.weights);
        SphereFace o;
        face_step(f, 0.02, &in, &o);
        in.cue = MAP[i].cue;
        in.cue_seq = 1;
        face_step(f, 0.02, &in, &o);
        all &= face_gesture(f) == MAP[i].g;
        face_destroy(f);
    }
    check(all, "ojos felices, sorpresa, suspiro, sonrojo, guiño al despedirse, mira de lado al buscar, rebota si "
               "sale bien, asiente al hacer algo, niega si falla");
    {
        Face *f = face_create(3);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        affect_weights_at(0, 0, in.affect.weights);
        SphereFace o;
        face_step(f, 0.02, &in, &o);
        in.cue = AFF_CUE_BLUSH;
        in.cue_seq = 1;
        face_step(f, 0.02, &in, &o);
        /* le entra la alegría justo después del sonrojo: no lo tapa el rebote */
        affect_weights_at(0.8f, 0.45f, in.affect.weights);
        in.affect.dominant = AFF_JOY;
        for (int i = 0; i < 30; i++) face_step(f, 1.0 / 60, &in, &o);
        check(face_gesture(f) == FACE_G_BLUSH, "un halago: se sonroja y el rebote de la alegría no se lo tapa");
        in.cue = AFF_CUE_SUCCESS;
        in.cue_seq = 2;
        face_step(f, 0.02, &in, &o);
        check(face_gesture(f) == FACE_G_BLUSH, "«listo» (rebotar un poco) no corta otro gesto");
        face_destroy(f);
    }

    printf("-- los ojos: la diagonal solo como ceja --\n");
    for (int act = FACE_IDLE; act <= FACE_SPEAKING; act += FACE_SPEAKING) {
        Face *f = face_create(11);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        in.activity = (FaceActivity)act;
        affect_weights_at(0, 0, in.affect.weights);
        SphereFace o;
        float tilt = 0, shade = 0, top = 0;
        for (int i = 0; i < 240; i++) {
            in.pulse = in.voice = act == FACE_SPEAKING ? 0.5f + 0.5f * sinf((float)i * 0.7f) : 0;
            face_step(f, 1.0 / 60, &in, &o);
            if (i > 60) {
                tilt = fmaxf(tilt, fabsf(o.lid_tilt));
                shade = fmaxf(shade, o.shade);
                top = fmaxf(top, o.lid_top);
            }
        }
        char what[200];
        snprintf(what, sizeof what, "neutral %s: sin diagonal (inclinación %.3f, sombra %.3f, párpado %.3f)",
                 act == FACE_IDLE ? "y callada" : "y hablando", tilt, shade, top);
        check(tilt == 0 && shade == 0 && top < 0.001f, what);
        face_destroy(f);
    }
    static const struct {
        AffectKind k;
        const char *what;
    } BROW[] = {{AFF_ANGER, "furia"}, {AFF_SADNESS, "tristeza"}, {AFF_FEAR, "temor"}};
    for (size_t i = 0; i < 3; i++) {
        Face *f = face_create(11);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        float v, a;
        affect_reference(BROW[i].k, &v, &a);
        affect_weights_at(v, a, in.affect.weights);
        in.affect.dominant = BROW[i].k;
        SphereFace o;
        for (int j = 0; j < 60; j++) face_step(f, 1.0 / 60, &in, &o);
        char what[160];
        snprintf(what, sizeof what, "%s: la diagonal sí (inclinación %.2f, sombra %.2f)", BROW[i].what, o.lid_tilt, o.shade);
        bool ok = fabsf(o.lid_tilt) > 0.2f;
        if (BROW[i].k == AFF_FEAR) ok = o.shade > 0.7f; /* el gris del temor, más marcado */
        if (BROW[i].k == AFF_ANGER) ok &= o.lid_top > 0.3f; /* y los ojos chiquitos de la furia se quedan */
        check(ok, what);
        face_destroy(f);
    }
    {
        /* un poquito de temor ya no deja la rayita */
        Face *f = face_create(11);
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.level = FACE_LEVEL_HIGH;
        affect_weights_at(-0.08f, 0.2f, in.affect.weights);
        SphereFace o;
        for (int j = 0; j < 60; j++) face_step(f, 1.0 / 60, &in, &o);
        char what[160];
        snprintf(what, sizeof what, "apenas un poco de temor (%.2f): sin sombra ni diagonal", in.affect.weights[AFF_FEAR]);
        check(o.shade == 0 && o.lid_tilt == 0, what);
        face_destroy(f);
    }

    printf("-- el sonrojo y el «!» se dibujan --\n");
    const int size = 200;
    SphereRenderer *rr = sphere_create(size);
    uint32_t *plain = xcalloc((size_t)size * size, 4), *pink = xcalloc((size_t)size * size, 4);
    SphereFace face;
    sphere_face_neutral(&face);
    sphere_set_face(rr, &face);
    sphere_render(rr, 1.0, 0.3, 1.0, &SPHERE_IDLE, 0, 0, SPHERE_STYLE_FACE_EYES, plain, size, false);
    face.blush = 1;
    face.nsym = 1;
    face.sym[0] = (SphereSymbol){SPHERE_SYM_EXCLAIM, 0.7f, -0.78f, 1, 1, 0};
    sphere_set_face(rr, &face);
    sphere_render(rr, 1.0, 0.3, 1.0, &SPHERE_IDLE, 0, 0, SPHERE_STYLE_FACE_EYES, pink, size, false);
    long redder = 0, changed_top = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t a = plain[y * size + x], b = pink[y * size + x];
            int ra = (a >> 16) & 255, ga = (a >> 8) & 255, rb = (b >> 16) & 255, gb = (b >> 8) & 255;
            if (y > size / 2 && rb - gb > ra - ga + 20) redder++;
            if (y < size / 2 && x > size / 2 && a != b) changed_top++;
        }
    char what[160];
    snprintf(what, sizeof what, "chapitas rosas en los cachetes (%ld píxeles) y el «!» arriba a la derecha (%ld)", redder,
             changed_top);
    check(redder > 60 && changed_top > 20, what);
    free(plain);
    free(pink);
    sphere_destroy(rr);
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    paths_init();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_vibra");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.memory_dir);
    g_paths.memory_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");
    static const wchar_t *const FILES[] = {L"config.env", L"afecto.json", L"afecto.jsonl"};
    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        wchar_t *f = path_join(dir, FILES[i]);
        DeleteFileW(f);
        free(f);
    }
    config_load();

    test_prosody();
    test_vibe();
    test_engine();
    test_answers();
    test_face();

    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        wchar_t *f = path_join(dir, FILES[i]);
        DeleteFileW(f);
        free(f);
    }
    RemoveDirectoryW(dir);
    free(dir);
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

/* El estado afectivo de Sokari (affect.c): límites, transiciones acotadas,
   histéresis, regreso a la base, ánimo, sorpresa, habituación, trayectoria y
   la etiqueta de la IA. Todo con el reloj propio del motor: no espera ni
   controla la PC.

   También es el simulador del documento maestro (§60): con
   SOKARI_SIMULAR=escenario.txt corre ese escenario, imprime cada paso y
   sale. Cada renglón: «<segundos> <orden> …», por ejemplo
       0    estimulo v=0.7 a=0.2 imp=0.5 nov=0 sor=0 ctl=0 cer=0 causa=gracias
       3    emocion alegria 0.6 temor 0.2
       5    resultado esperado=0.9 ok=0 imp=0.35 causa=abrir_app
       6    herramienta abrir_app falla
       8    evento gracias        (saludo, chiste, resuelto, delicado, sin_conexion, sin_cupo)
       30   ver */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "config.h"
#include "sphere.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    if (!ok) g_fail++;
}

static float joy_v(float f)
{
    float v, a;
    affect_reference(AFF_JOY, &v, &a);
    return v * f;
}

static float joy_a(float f)
{
    float v, a;
    affect_reference(AFF_JOY, &v, &a);
    return a * f;
}

/* Un estímulo que pone el objetivo casi exacto en (v, a): temperamento sensible. */
static void push(AffectEngine *e, double t, float v, float a, const char *cause)
{
    AffectStimulus s = {0};
    s.valence = v;
    s.arousal = a;
    s.importance = 1;
    s.cause = cause;
    affect_engine_stimulus(e, t, &s);
}

static AffectEngine *sensitive(void)
{
    AffectTemperament t = {0, 0, 2.0f, 0};
    return affect_engine_create(&t);
}

/* ------------------------------------------------------------------ §63 */

static unsigned g_seed = 12345;
static float rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 16777216.0f;
}

static float wild(void)
{
    float r = rnd();
    if (r < 0.03f) return NAN;
    if (r < 0.06f) return INFINITY;
    if (r < 0.09f) return -INFINITY;
    return (rnd() * 2 - 1) * (r < 0.3f ? 5.0f : 1.0f);
}

static int out_of_bounds(const AffectEngine *e)
{
    AffectState s = affect_engine_state(e);
    int bad = 0;
    float sum = 0;
    bad += !(s.valence >= -1 && s.valence <= 1) + !(s.arousal >= -1 && s.arousal <= 1);
    bad += !(s.intensity >= 0 && s.intensity <= 1);
    bad += !(s.mood_valence >= -0.6f && s.mood_valence <= 0.6f) + !(s.mood_arousal >= -0.6f && s.mood_arousal <= 0.6f);
    bad += !(s.velocity <= 0.9f + 1e-4f);
    for (int k = 0; k < AFF_COUNT; k++) {
        bad += !(s.weights[k] >= 0 && s.weights[k] <= 1);
        sum += s.weights[k];
    }
    bad += !(fabsf(sum - 1) < 1e-3f);
    bad += !(s.dominant >= 0 && s.dominant < AFF_COUNT && s.secondary >= 0 && s.secondary < AFF_COUNT &&
             s.dominant != s.secondary);
    AffectMetrics m = affect_engine_metrics(e);
    bad += !isfinite(m.trend) + !isfinite(m.volatility) + !isfinite(m.deviation);
    return bad;
}

static void test_limites(void)
{
    printf("-- siempre dentro de sus límites (§63) --\n");
    AffectTemperament temps[3] = {{0, 0, 1, 0.5f}, {0.5f, -0.5f, 2, 0}, {-3, 3, 0.1f, 1}};
    int bad = 0, steps = 0, jumps = 0;
    for (int ti = 0; ti < 3; ti++) {
        AffectEngine *e = affect_engine_create(&temps[ti]);
        double t = 0;
        AffectState prev = affect_engine_state(e);
        for (int i = 0; i < 3000; i++) {
            float r = rnd();
            if (r < 0.35f) {
                AffectStimulus s = {wild(), wild(), wild(), wild(), wild(), wild(), wild(), "azar"};
                affect_engine_stimulus(e, t, &s);
            } else if (r < 0.55f) {
                float am[AFF_COUNT];
                for (int k = 0; k < AFF_COUNT; k++) am[k] = wild();
                affect_engine_emotions(e, t, am, "azar");
            } else if (r < 0.7f) {
                affect_engine_outcome(e, t, wild(), rnd() < 0.5f, wild(), "azar");
            } else if (r < 0.8f) {
                affect_engine_event(e, t, (AffectEvent)(rnd() * AFF_EV_COUNT));
            } else if (r < 0.9f) {
                affect_engine_tool(e, t, rnd() < 0.5f ? "abrir_app" : "buscar_web", rnd() < 0.7f);
            }
            /* a veces pasos cortos (y se revisa que no salte), a veces horas */
            double dt = rnd() < 0.97f ? rnd() * 0.3 : rnd() * 20000;
            t += dt;
            affect_engine_advance(e, t);
            AffectState s = affect_engine_state(e);
            if (dt < 0.3 && hypotf(s.valence - prev.valence, s.arousal - prev.arousal) > 0.9f * (float)dt + 1e-3f)
                jumps++;
            prev = s;
            bad += out_of_bounds(e);
            steps++;
        }
        affect_engine_destroy(e);
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%d pasos al azar (con valores locos, NaN e infinitos): 0 fuera de rango (hubo %d)", steps,
             bad);
    check(bad == 0, msg);
    snprintf(msg, sizeof msg, "nunca salta: se mueve a lo más 0.9 por segundo (saltos: %d)", jumps);
    check(jumps == 0, msg);
}

/* ------------------------------------------------------------------ §12 */

static void test_trivial(void)
{
    printf("-- nada de «neutral → furia máxima» (§12) --\n");
    AffectEngine *e = affect_engine_create(NULL);
    AffectStimulus s = {-0.75f, 0.6f, 0.02f, 0, 0, 0, 0, "algo sin importancia"};
    affect_engine_stimulus(e, 0, &s);
    unsigned why = affect_engine_take_reasons(e);
    float maxdist = 0;
    for (double t = 0.1; t <= 10; t += 0.1) {
        affect_engine_advance(e, t);
        AffectState st = affect_engine_state(e);
        float d = hypotf(st.valence, st.arousal);
        if (d > maxdist) maxdist = d;
    }
    AffectState st = affect_engine_state(e);
    check(st.dominant == AFF_NEUTRAL && maxdist < 0.35f, "un estímulo trivial hacia la furia: sigue neutral, apenas se mueve");
    (void)why;
    affect_engine_destroy(e);

    e = affect_engine_create(NULL);
    AffectStimulus big = {-0.75f, 0.6f, 1, 1, 1, -1, -1, "algo grave"};
    affect_engine_stimulus(e, 0, &big);
    affect_engine_advance(e, 0.1);
    st = affect_engine_state(e);
    check(hypotf(st.valence, st.arousal) <= 0.09f + 1e-3f, "aun con algo grave, en 0.1 s se mueve a lo más 0.09");
    unsigned r = affect_engine_take_reasons(e);
    check((r & AFF_R_ACCEL_CAP) && (r & AFF_R_SURPRISE) && (r & AFF_R_LOW_CONTROL) && (r & AFF_R_UNCERTAIN),
          "y dice por qué: tope de aceleración, sorpresa, sin control, incertidumbre");
    affect_engine_destroy(e);

    /* estabilidad (§83): lo chiquito casi no mueve */
    AffectTemperament stable = {0, 0, 1, 1};
    e = affect_engine_create(&stable);
    AffectStimulus tiny = {0.05f, 0, 0, 0, 0, 0, 0, "nada"};
    affect_engine_stimulus(e, 0, &tiny);
    check(affect_engine_take_reasons(e) & AFF_R_TRIVIAL, "un estímulo chiquito se amortigua (estabilidad)");
    affect_engine_destroy(e);
}

static void test_transicion(void)
{
    printf("-- stimulus → appraisal → objetivo → transición acotada --\n");
    AffectEngine *e = affect_engine_create(NULL);
    affect_engine_emotion(e, 0, AFF_JOY, 0.8f, "IA");
    float tv, ta;
    affect_engine_target(e, &tv, &ta);
    AffectState s0 = affect_engine_state(e);
    check(tv > 0.4f && ta > 0.2f && s0.valence == 0 && s0.arousal == 0,
          "la etiqueta mueve el objetivo, no el estado (todavía)");
    double when = -1;
    float vmax = 0;
    for (double t = 0.02; t <= 5; t += 0.02) {
        affect_engine_advance(e, t);
        AffectState s = affect_engine_state(e);
        if (s.velocity > vmax) vmax = s.velocity;
        if (when < 0 && s.dominant == AFF_JOY) when = t;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "llega a alegría en %.2f s (entre 0.5 y 3), sin pasar de 0.9/s (máx %.2f)", when, vmax);
    check(when >= 0.5 && when <= 3 && vmax <= 0.9f + 1e-4f, msg);
    AffectState s = affect_engine_state(e);
    check(s.weights[AFF_JOY] > 0.6f && s.intensity > 0.5f, "alegría pesa más de 0.6 y la intensidad pasa de 0.5");
    affect_engine_destroy(e);
}

/* ------------------------------------------------------------------ §13 */

static void test_histeresis(void)
{
    printf("-- histéresis: no parpadea (§13) --\n");
    AffectEngine *e = sensitive();
    int argmax_flips = 0, last = -1;
    for (int i = 0; i < 150; i++) {
        double t = i * 0.4;
        float f = i % 2 ? 0.54f : 0.46f;
        push(e, t, joy_v(f), joy_a(f), i % 2 ? "a" : "b");
        for (double u = t + 0.1; u <= t + 0.4 + 1e-9; u += 0.1) {
            affect_engine_advance(e, u);
            AffectState s = affect_engine_state(e);
            int am = s.weights[AFF_JOY] > s.weights[AFF_NEUTRAL] ? AFF_JOY : AFF_NEUTRAL;
            if (last >= 0 && am != last) argmax_flips++;
            last = am;
        }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "60 s oscilando justo en la frontera: la mayor cambió %d veces, la dominante %d",
             argmax_flips, affect_engine_switches(e));
    check(argmax_flips >= 20 && affect_engine_switches(e) == 0, msg);

    push(e, 61, joy_v(0.85f), joy_a(0.85f), "c");
    affect_engine_advance(e, 64);
    check(affect_engine_state(e).dominant == AFF_JOY, "con alegría de verdad (0.85) sí entra");
    push(e, 64, joy_v(0.55f), joy_a(0.55f), "d");
    affect_engine_advance(e, 70);
    check(affect_engine_state(e).dominant == AFF_JOY, "bajando a la zona de en medio se sostiene");
    push(e, 70, joy_v(0.3f), joy_a(0.3f), "e");
    affect_engine_advance(e, 76);
    check(affect_engine_state(e).dominant == AFF_NEUTRAL, "y ya abajo de 0.45 sale a neutral");
    affect_engine_destroy(e);

    /* dura al menos 1.2 s */
    e = sensitive();
    push(e, 0, joy_v(0.95f), joy_a(0.95f), "alegre");
    double switched = -1;
    for (double t = 0.02; t < 5 && switched < 0; t += 0.02) {
        affect_engine_advance(e, t);
        if (affect_engine_state(e).dominant == AFF_JOY) switched = t;
    }
    float sv, sa;
    affect_reference(AFF_SADNESS, &sv, &sa);
    push(e, switched, sv, sa, "triste");
    double next = -1;
    for (double t = switched + 0.02; t < switched + 6 && next < 0; t += 0.02) {
        affect_engine_advance(e, t);
        if (affect_engine_state(e).dominant != AFF_JOY) next = t;
    }
    snprintf(msg, sizeof msg, "una dominante dura al menos 1.2 s (cambió a los %.2f s)", next - switched);
    check(switched > 0 && next - switched >= 1.2 - 1e-6, msg);
    affect_engine_destroy(e);
}

/* ------------------------------------------------------------------ §15 */

static void test_regreso(void)
{
    printf("-- la emoción regresa sola a la base --\n");
    AffectEngine *e = affect_engine_create(NULL);
    affect_engine_emotion(e, 0, AFF_JOY, 0.9f, "IA");
    affect_engine_advance(e, 8);
    AffectMetrics m = affect_engine_metrics(e);
    check(m.recovery_time == -1 && m.deviation > 0.3f, "a los 8 s sigue lejos de la base (todavía no se recupera)");
    affect_engine_advance(e, 25);
    m = affect_engine_metrics(e);
    check(m.trend < 0, "la tendencia de la valencia va hacia abajo");
    affect_engine_advance(e, 90);
    AffectState s = affect_engine_state(e);
    m = affect_engine_metrics(e);
    char msg[160];
    snprintf(msg, sizeof msg, "al minuto y medio: neutral, desviación %.3f, se recuperó en %.1f s", m.deviation,
             m.recovery_time);
    check(s.dominant == AFF_NEUTRAL && m.deviation < 0.05f && m.recovery_time > 5 && m.recovery_time < 80, msg);
    affect_engine_destroy(e);
}

static void test_animo(void)
{
    printf("-- el ánimo: minutos a horas, y se guarda --\n");
    AffectEngine *e = affect_engine_create(NULL);
    for (int i = 0; i < 30; i++) affect_engine_event(e, i * 60.0, AFF_EV_THANKS);
    affect_engine_advance(e, 1800);
    AffectState s = affect_engine_state(e);
    char msg[160];
    snprintf(msg, sizeof msg, "media hora de «gracias»: el ánimo sube (%.3f), sin pasar de 0.6", s.mood_valence);
    check(s.mood_valence > 0.05f && s.mood_valence <= 0.6f, msg);
    float v8, a8;
    affect_engine_advance(e, 1800 + 8 * 3600);
    s = affect_engine_state(e);
    v8 = s.mood_valence;
    a8 = s.mood_arousal;
    snprintf(msg, sizeof msg, "8 horas después regresa al temperamento (%.3f, %.3f)", v8, a8);
    check(fabsf(v8) < 0.03f && fabsf(a8) < 0.03f, msg);
    affect_engine_destroy(e);

    e = affect_engine_create(NULL);
    affect_engine_set_mood(e, 0.5f, 0.2f, 4 * 3600);
    s = affect_engine_state(e);
    check(fabsf(s.mood_valence - 0.5f * expf(-1)) < 0.01f && fabsf(s.mood_arousal - 0.2f * expf(-1)) < 0.01f,
          "el ánimo guardado hace 4 horas llega a 1/e de lo que era");
    affect_engine_set_mood(e, 3, -3, -50);
    s = affect_engine_state(e);
    check(s.mood_valence == 0.6f && s.mood_arousal == -0.6f, "un ánimo guardado fuera de rango se acota a 0.6");
    affect_engine_destroy(e);

    AffectTemperament happy = {0.3f, 0.1f, 1, 0.5f};
    e = affect_engine_create(&happy);
    affect_engine_emotion(e, 0, AFF_SADNESS, 1, "IA");
    affect_engine_advance(e, 12 * 3600);
    s = affect_engine_state(e);
    check(fabsf(s.mood_valence - 0.3f) < 0.02f && fabsf(s.valence - 0.3f) < 0.02f,
          "con otro temperamento, regresa a ese (no a cero)");
    affect_engine_destroy(e);
}

/* ------------------------------------------------------------------ sorpresa */

static float pushed(AffectEngine *e)
{
    float v, a;
    affect_engine_target(e, &v, &a);
    AffectState s = affect_engine_state(e);
    return hypotf(v - s.mood_valence, a - s.mood_arousal);
}

static void test_sorpresa(void)
{
    printf("-- sorpresa = error de predicción --\n");
    AffectEngine *a = affect_engine_create(NULL), *b = affect_engine_create(NULL);
    affect_engine_outcome(a, 0, 0.95f, false, 0.4f, "casi seguro");
    affect_engine_outcome(b, 0, 0.3f, false, 0.4f, "ya se esperaba");
    unsigned ra = affect_engine_take_reasons(a), rb = affect_engine_take_reasons(b);
    check(pushed(a) > pushed(b) && (ra & AFF_R_SURPRISE) && !(rb & AFF_R_SURPRISE),
          "fallar lo que casi seguro salía mueve más que fallar lo que se esperaba");
    affect_engine_advance(a, 3);
    AffectState s = affect_engine_state(a);
    check(s.valence < 0 && s.weights[AFF_SADNESS] > s.weights[AFF_ANGER],
          "fallar se va hacia la tristeza, no al enojo");
    affect_engine_destroy(a);
    affect_engine_destroy(b);

    AffectEngine *e = affect_engine_create(NULL);
    affect_engine_tool(e, 0, "abrir_app", false);
    check(affect_engine_take_reasons(e) & AFF_R_SURPRISE, "la primera vez que una herramienta falla, sorprende");
    for (int i = 1; i <= 10; i++) affect_engine_tool(e, i * 40.0, "abrir_app", false);
    affect_engine_take_reasons(e);
    affect_engine_tool(e, 440, "abrir_app", false);
    check(!(affect_engine_take_reasons(e) & AFF_R_SURPRISE), "si siempre falla, ya no sorprende (aprendió lo esperado)");
    affect_engine_tool(e, 480, "abrir_app", true);
    check(affect_engine_take_reasons(e) & AFF_R_SURPRISE, "y que por fin salga bien, sí");
    affect_engine_destroy(e);
}

static void test_habituacion(void)
{
    printf("-- lo mismo, otra vez y pronto, pesa menos --\n");
    AffectEngine *a = affect_engine_create(NULL), *b = affect_engine_create(NULL);
    char cause[16];
    for (int i = 0; i < 5; i++) {
        AffectStimulus s = {0.8f, 0.4f, 0.3f, 0, 0, 0, 0, "igual"};
        affect_engine_stimulus(a, i, &s);
        snprintf(cause, sizeof cause, "otro %d", i);
        s.cause = cause;
        affect_engine_stimulus(b, i, &s);
    }
    float av, aa, bv, ba;
    affect_engine_target(a, &av, &aa);
    affect_engine_target(b, &bv, &ba);
    check(av < bv && (affect_engine_take_reasons(a) & AFF_R_REPEATED) && !(affect_engine_take_reasons(b) & AFF_R_REPEATED),
          "cinco veces lo mismo en 5 s mueve menos que cinco cosas distintas");
    affect_engine_destroy(a);
    affect_engine_destroy(b);
}

/* ------------------------------------------------------------------ §14 */

typedef struct {
    int stimuli, changes, samples, total;
    long long last_id;
    bool ids_ok;
} Traces;

static void count_trace(void *ctx, const AffectTrace *tr)
{
    Traces *c = ctx;
    c->total++;
    if (tr->id != c->last_id + 1) c->ids_ok = false;
    c->last_id = tr->id;
    if (!strcmp(tr->event, "estimulo")) c->stimuli++;
    else if (!strcmp(tr->event, "cambio")) c->changes++;
    else if (!strcmp(tr->event, "muestra")) c->samples++;
}

static void test_trayectoria(void)
{
    printf("-- la trayectoria: eventos, no cada cuadro (§14) --\n");
    Traces c = {0, 0, 0, 0, 0, true};
    AffectEngine *e = affect_engine_create(NULL);
    affect_engine_on_trace(e, count_trace, &c);
    for (double t = 0; t < 600; t += 1.0 / 60) affect_engine_advance(e, t);
    check(c.total == 0, "10 minutos en calma (36 000 cuadros): 0 renglones");
    affect_engine_emotion(e, 600, AFF_JOY, 0.9f, "IA");
    affect_engine_event(e, 640, AFF_EV_NETWORK_ERROR);
    affect_engine_event(e, 700, AFF_EV_THANKS);
    affect_engine_advance(e, 800);
    char msg[160];
    snprintf(msg, sizeof msg, "3 estímulos: %d renglones de estímulo, %d cambios, %d muestras (%d en total)", c.stimuli,
             c.changes, c.samples, c.total);
    check(c.stimuli == 3 && c.changes >= 2 && c.total <= 20 && c.ids_ok, msg);
    affect_engine_destroy(e);
}

/* ------------------------------------------------------------------ la etiqueta */

static void tag_case(const char *in, const char *want, int want_found, AffectKind k, float want_value)
{
    float am[AFF_COUNT];
    int found = -1;
    char *out = affect_take_tags(in, am, &found);
    bool ok = !strcmp(out, want) && found == want_found &&
              (want_found == 0 || fabsf(am[k] - want_value) < 1e-4f);
    char msg[300];
    snprintf(msg, sizeof msg, "«%s» → «%s» (%d)", in, out, found);
    check(ok, msg);
    free(out);
}

static void test_etiquetas(void)
{
    printf("-- la etiqueta de la IA nunca llega a la voz ni a los subtítulos --\n");
    tag_case("Listo, ya quedó. [afecto: alegría 0.6]", "Listo, ya quedó.", 1, AFF_JOY, 0.6f);
    tag_case("[Afecto: ALEGRÍA 60%] Hola", "Hola", 1, AFF_JOY, 0.6f);
    tag_case("Ok (afecto: tristeza 0,3)", "Ok", 1, AFF_SADNESS, 0.3f);
    tag_case("Va [emoción: feliz]", "Va", 1, AFF_JOY, 0.5f);
    tag_case("Hmm [afecto alegria 7/10]", "Hmm", 1, AFF_JOY, 0.7f);
    tag_case("Qué susto [afecto: temor .8]", "Qué susto", 1, AFF_FEAR, 0.8f);
    tag_case("Uy [afecto: enojo 150]", "Uy", 1, AFF_ANGER, 1.0f);
    tag_case("Nada [afecto: sorpresa 0.5]", "Nada", 0, AFF_NEUTRAL, 0);
    tag_case("Cortado [afecto: alegr", "Cortado", 0, AFF_NEUTRAL, 0);
    tag_case("Cortado [af", "Cortado", 0, AFF_NEUTRAL, 0);
    tag_case("Cortado [", "Cortado", 0, AFF_NEUTRAL, 0);
    tag_case("[afecto: neutral 0.5]", "", 1, AFF_NEUTRAL, 0.5f);
    tag_case("Hola [afecto: alegría 0.4] ¿qué tal? [afecto: temor 0.2]", "Hola ¿qué tal?", 2, AFF_FEAR, 0.2f);
    tag_case("afecto: alegría 0.4\nClaro que sí.", "Claro que sí.", 1, AFF_JOY, 0.4f);
    tag_case("Emoción: es un estado del ánimo que dura poco.", "Emoción: es un estado del ánimo que dura poco.", 0,
             AFF_NEUTRAL, 0);
    tag_case("Normal [1] con (paréntesis) y a < b", "Normal [1] con (paréntesis) y a < b", 0, AFF_NEUTRAL, 0);
    tag_case("  espacios  dobles  sin etiqueta  ", "  espacios  dobles  sin etiqueta  ", 0, AFF_NEUTRAL, 0);
    tag_case("", "", 0, AFF_NEUTRAL, 0);
    float am[AFF_COUNT];
    int n = 0;
    char *out = affect_take_tags("Va [afecto: alegría 0.5, temor 0.2]", am, &n);
    check(!strcmp(out, "Va") && n == 2 && fabsf(am[AFF_JOY] - 0.5f) < 1e-4f && fabsf(am[AFF_FEAR] - 0.2f) < 1e-4f,
          "dos emociones en una etiqueta");
    free(out);
    out = affect_take_tags("Sí [afecto: alegría 0.6]", NULL, NULL);
    check(!strcmp(out, "Sí"), "sin dónde guardar las emociones, igual la quita");
    free(out);
    const char *p = affect_prompt();
    check(strstr(p, "[afecto: alegría 0.6]") && strlen(p) < 520, "la instrucción para el modelo es corta y trae el ejemplo");
}

/* ------------------------------------------------------------------ el global y la opción */

static void test_global(const wchar_t *dir)
{
    printf("-- el de Sokari: reloj real, afecto.json y afecto.jsonl --\n");
    affect_event(AFF_EV_THANKS);
    float v, a;
    affect_target(&v, &a);
    check(v > 0.2f, "un «gracias» mueve el objetivo hacia lo positivo");
    float am[AFF_COUNT] = {0};
    am[AFF_JOY] = 0.7f;
    affect_emotions(am, "la IA");
    AffectState s = affect_get();
    check(s.valence >= -1 && s.valence <= 1 && s.weights[AFF_NEUTRAL] > 0, "affect_get da un estado válido");
    affect_save();
    wchar_t *json = path_join(dir, L"afecto.json"), *jsonl = path_join(dir, L"afecto.jsonl");
    char *saved = read_file_all(json, NULL);
    check(saved && strstr(saved, "\"animo_valencia\"") && strstr(saved, "\"guardado\""), "guarda el ánimo en afecto.json");
    free(saved);
    char *trail = read_file_all(jsonl, NULL);
    check(trail && strstr(trail, "\"evento\":\"estimulo\"") && strstr(trail, "\"causa\":\"gracias\"") &&
              strstr(trail, "\"causa\":\"la IA\""),
          "la trayectoria en afecto.jsonl trae los estímulos y su causa");
    free(trail);
    DeleteFileW(json);
    DeleteFileW(jsonl);
    free(json);
    free(jsonl);

    printf("-- el estilo de la esfera en config.env --\n");
    bool round_trip = true;
    for (int i = 0; i < SPHERE_STYLE_COUNT; i++) round_trip &= sphere_style_from_key(sphere_style_key(i)) == i;
    check(round_trip && !strcmp(sphere_style_key(2), "cara_ojos") && sphere_style_from_key("lineas") == 1 &&
              sphere_style_from_key("girasol") == 0 && sphere_style_from_key(NULL) == 0,
          "puntos, lineas, cara_ojos, cara_boca, cara_puntos (lo que no conoce es puntos)");
    const char *text = "SOKARI_SPHERE_STYLE=cara_boca\n";
    write_file_atomic(g_paths.config_file, text, strlen(text));
    config_load();
    check(config_face(), "con una cara puesta, config_face() dice que sí");
    AppConfig c = config_snapshot();
    c.sphere_style = 0;
    config_apply(&c);
    config_free(&c);
    check(!config_face(), "con el halo de puntos, no");
    char *cfg = read_file_all(g_paths.config_file, NULL);
    check(cfg && strstr(cfg, "SOKARI_SPHERE_STYLE=puntos\n"), "y lo guarda como puntos");
    free(cfg);
}

/* ------------------------------------------------------------------ §60 el simulador */

static void print_state(const char *label, const AffectEngine *e)
{
    AffectState s = affect_engine_state(e);
    printf("   %-9s v=%+.2f a=%+.2f  I=%.2f  %s (%.2f) / %s (%.2f)  ánimo %+.2f,%+.2f\n", label, s.valence, s.arousal,
           s.intensity, affect_name(s.dominant), s.weights[s.dominant], affect_name(s.secondary),
           s.weights[s.secondary], s.mood_valence, s.mood_arousal);
}

static void sim_trace(void *ctx, const AffectTrace *tr)
{
    (void)ctx;
    if (strcmp(tr->event, "estimulo"))
        printf("   · %s: %s (I=%.2f, v=%+.2f a=%+.2f)\n", tr->event, affect_name(tr->state.dominant), tr->state.intensity,
               tr->state.valence, tr->state.arousal);
}

static float field(const char *line, const char *key, float def)
{
    char pat[16];
    snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat);
    return p ? (float)atof(p + strlen(pat)) : def;
}

static int simulate(const char *script)
{
    AffectEngine *e = affect_engine_create(NULL);
    affect_engine_on_trace(e, sim_trace, NULL);
    char *copy = xstrdup(script), *ctx = NULL;
    double last = 0;
    for (char *line = strtok_s(copy, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx)) {
        double t;
        char cmd[32];
        int used = 0;
        if (*line == '#' || sscanf(line, "%lf %31s %n", &t, cmd, &used) < 2) continue;
        if (t < last) t = last;
        affect_engine_advance(e, t);
        last = t;
        const char *rest = line + used;
        char spaced[400];
        snprintf(spaced, sizeof spaced, " %s", rest);
        printf("t=%6.1f  %s %s\n", t, cmd, rest);
        print_state("antes:", e);
        affect_engine_take_reasons(e);
        if (!strcmp(cmd, "estimulo")) {
            char cause[64] = "simulador";
            const char *c = strstr(spaced, " causa=");
            if (c) sscanf(c + 7, "%63s", cause);
            AffectStimulus s = {field(spaced, "v", 0),   field(spaced, "a", 0),   field(spaced, "imp", 0),
                                field(spaced, "nov", 0), field(spaced, "sor", 0), field(spaced, "ctl", 0),
                                field(spaced, "cer", 0), cause};
            affect_engine_stimulus(e, t, &s);
        } else if (!strcmp(cmd, "emocion")) {
            char *tagged = str_printf("[afecto: %s]", rest);
            float am[AFF_COUNT];
            int n = 0;
            free(affect_take_tags(tagged, am, &n));
            free(tagged);
            if (n) affect_engine_emotions(e, t, am, "simulador");
        } else if (!strcmp(cmd, "resultado")) {
            char cause[64] = "simulador";
            const char *c = strstr(spaced, " causa=");
            if (c) sscanf(c + 7, "%63s", cause);
            affect_engine_outcome(e, t, field(spaced, "esperado", 0.85f), field(spaced, "ok", 1) != 0,
                                  field(spaced, "imp", 0.35f), cause);
        } else if (!strcmp(cmd, "herramienta")) {
            char name[40] = "", how[16] = "";
            sscanf(rest, "%39s %15s", name, how);
            affect_engine_tool(e, t, name, strcmp(how, "falla") != 0);
        } else if (!strcmp(cmd, "evento")) {
            static const char *const EV[AFF_EV_COUNT] = {"gracias",  "saludo",       "chiste",   "resuelto",
                                                          "delicado", "sin_conexion", "sin_cupo"};
            for (int i = 0; i < AFF_EV_COUNT; i++)
                if (str_starts_with(rest, EV[i])) affect_engine_event(e, t, (AffectEvent)i);
        }
        float tv, ta;
        affect_engine_target(e, &tv, &ta);
        unsigned why = affect_engine_take_reasons(e);
        printf("   objetivo: v=%+.2f a=%+.2f   motivos:", tv, ta);
        for (int i = 0; i < AFF_R_COUNT; i++)
            if (why & (1u << i)) printf(" %s;", affect_reason_name(1u << i));
        printf("\n");
        affect_engine_advance(e, t + 2);
        last = t + 2;
        print_state("a los 2 s:", e);
    }
    AffectMetrics m = affect_engine_metrics(e);
    printf("métricas: tendencia %+.3f/min, volatilidad %.3f, recuperación %.1f s, desviación %.3f\n", m.trend,
           m.volatility, m.recovery_time, m.deviation);
    free(copy);
    affect_engine_destroy(e);
    return 0;
}

static const char *const DEMO = "0 evento saludo\n"
                                "5 emocion alegria 0.7\n"
                                "20 herramienta abrir_app falla\n"
                                "25 evento delicado\n"
                                "40 emocion tristeza 0.5, temor 0.2\n"
                                "120 ver\n";

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    const char *sim = getenv("SOKARI_SIMULAR");
    if (sim && *sim) {
        FILE *f = fopen(sim, "rb");
        if (!f) {
            printf("No pude abrir %s\n", sim);
            return 1;
        }
        char buf[65536];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        return simulate(buf);
    }
    paths_init();
    /* En una carpeta temporal: nunca se toca tu config.env ni tu ánimo. */
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t *dir = path_join(tmp, L"sokari_test_afecto");
    ensure_dir(dir);
    free(g_paths.local_dir);
    g_paths.local_dir = xwcsdup(dir);
    free(g_paths.config_file);
    g_paths.config_file = path_join(dir, L"config.env");

    test_limites();
    test_trivial();
    test_transicion();
    test_histeresis();
    test_regreso();
    test_animo();
    test_sorpresa();
    test_habituacion();
    test_trayectoria();
    test_etiquetas();
    test_global(dir);
    printf("-- el simulador (§60), con un escenario de ejemplo --\n");
    simulate(DEMO);

    DeleteFileW(g_paths.config_file);
    RemoveDirectoryW(dir);
    free(dir);
    printf("%d/%d pruebas %s\n", g_total - g_fail, g_total, g_fail ? "— HAY FALLAS" : "ok");
    return g_fail ? 1 : 0;
}

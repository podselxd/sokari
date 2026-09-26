/* Skills locales del tiempo que corre: temporizadores y cronómetro (en
   memoria, al segundo) y alarmas y recordatorios (en recordatorios.json, como
   los que crea la IA: siguen ahí si cierras Sokari). */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intents.h"
#include "memory.h"
#include "skills_internal.h"
#include "util.h"

/* ------------------------------------------------------ temporizadores --- */

#define MAX_TIMERS 8

typedef struct {
    time_t due;
    long secs;
} Timer;

static SRWLOCK g_lock = SRWLOCK_INIT;
static Timer g_timers[MAX_TIMERS];
static int g_ntimers;
static bool g_sw_running;
static time_t g_sw_start;
static long g_sw_acc;

char **skills_due_timers(int *n)
{
    *n = 0;
    time_t now = sk_now();
    char **out = NULL;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_ntimers; i++) {
        if (g_timers[i].due > now) continue;
        char *what = sk_say_duration(g_timers[i].secs);
        out = xrealloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = str_printf("¡Se acabó el temporizador de %s!", what);
        free(what);
        g_timers[i--] = g_timers[--g_ntimers];
    }
    ReleaseSRWLockExclusive(&g_lock);
    return out;
}

/* Lo que les falta a los temporizadores ("5 minutos y 3 segundos"; heap). */
static char *timers_left(void)
{
    time_t now = sk_now();
    StrBuf sb;
    sb_init(&sb);
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_ntimers; i++) {
        char *left = sk_say_duration((long)(g_timers[i].due - now)), *of = sk_say_duration(g_timers[i].secs);
        if (g_ntimers == 1) sb_appendf(&sb, "Le quedan %s.", left);
        else sb_appendf(&sb, "%sAl de %s le quedan %s.", sb.len ? " " : "", of, left);
        free(left);
        free(of);
    }
    ReleaseSRWLockShared(&g_lock);
    return sb_steal(&sb);
}

static char *stopwatch(Heard *h)
{
    time_t now = sk_now();
    AcquireSRWLockExclusive(&g_lock);
    long elapsed = g_sw_acc + (g_sw_running ? (long)(now - g_sw_start) : 0);
    char *r = NULL;
    if (sk_take(h, "cuanto lleva") >= 0 || sk_take(h, "cuanto tiempo lleva") >= 0 ||
        sk_take(h, "cuanto va") >= 0 || sk_take(h, "cuanto tiempo va") >= 0) {
        char *t = sk_say_duration(elapsed);
        r = g_sw_running || elapsed ? str_printf("Lleva %s.", t) : xstrdup("El cronómetro no está corriendo.");
        free(t);
    } else if (sk_take(h, "reinicia") >= 0 || sk_take(h, "reinicialo") >= 0 || sk_take(h, "reset") >= 0) {
        g_sw_running = true, g_sw_start = now, g_sw_acc = 0;
        r = xstrdup("Cronómetro en cero y corriendo.");
    } else if (sk_find(h, "deten") >= 0 || sk_find(h, "para") >= 0 || sk_find(h, "pausa") >= 0 ||
               sk_find(h, "detener") >= 0 || sk_find(h, "paralo") >= 0 || sk_find(h, "detenlo") >= 0 ||
               sk_find(h, "apaga") >= 0) {
        sk_take_any(h, " deten para pausa detener paralo detenlo apaga ");
        char *t = sk_say_duration(elapsed);
        r = g_sw_running ? str_printf("Cronómetro detenido en %s.", t) : xstrdup("El cronómetro no estaba corriendo.");
        free(t);
        g_sw_acc = elapsed, g_sw_running = false;
    } else {
        sk_take_any(h, " inicia iniciar inicialo empieza empezar arranca arrancar pon ponme activa corre echa un "
                       "una nuevo ");
        if (g_sw_running) {
            char *t = sk_say_duration(elapsed);
            r = str_printf("El cronómetro ya va corriendo: lleva %s.", t);
            free(t);
        } else {
            g_sw_running = true, g_sw_start = now - g_sw_acc;
            r = xstrdup(g_sw_acc ? "Cronómetro corriendo otra vez." : "Cronómetro corriendo.");
            g_sw_acc = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!sk_rest_is_filler(h)) {
        free(r);
        return NULL;
    }
    return r;
}

char *sk_timers(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    if (sk_take(h, "cronometro") >= 0) {
        sk_take_any(h, " el del ");
        /* Deshacer lo que no fue: stopwatch revisa todo lo demás. */
        return stopwatch(h);
    }
    int word = -1;
    static const char *const NAMES[] = {"temporizador", "temporizadores", "timer", "cuenta regresiva"};
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES && word < 0; i++) word = sk_take(h, NAMES[i]);
    int first;
    long secs = sk_take_duration(h, &first);
    if (word < 0) {
        /* "Avísame en 10 minutos", sin decir de qué: un temporizador. */
        if (!secs || !(first > 0 && !strcmp(h->w[first - 1], "en"))) return NULL;
        h->used[first - 1] = true;
        if (sk_take(h, "avisame") < 0 && sk_take(h, "recuerdame") < 0 && sk_take(h, "me avisas") < 0) return NULL;
    }
    if (secs) {
        sk_take_any(h, " pon ponme poner crea creame inicia iniciar empieza arranca activa programa haz hazme un una de "
                       "por en otro nuevo ");
        if (!sk_rest_is_filler(h)) return NULL;
        if (secs > 24 * 3600) return xstrdup("Un temporizador puede ser de hasta 24 horas.");
        AcquireSRWLockExclusive(&g_lock);
        bool full = g_ntimers == MAX_TIMERS;
        if (!full) g_timers[g_ntimers++] = (Timer){sk_now() + secs, secs};
        ReleaseSRWLockExclusive(&g_lock);
        if (full) return xstrdup("Ya tienes muchos temporizadores. Cancela alguno primero.");
        char *what = sk_say_duration(secs), *r = str_printf("Listo, temporizador de %s.", what);
        free(what);
        return r;
    }
    AcquireSRWLockShared(&g_lock);
    int count = g_ntimers;
    ReleaseSRWLockShared(&g_lock);
    static const char *const CANCEL[] = {"cancela", "cancelar", "quita", "quitar", "borra", "deten", "apaga", "para",
                                         "cancelalo", "quitalo", "detenlo", "elimina"};
    for (size_t i = 0; i < sizeof CANCEL / sizeof *CANCEL; i++) {
        if (sk_take(h, CANCEL[i]) < 0) continue;
        sk_take_any(h, " el los todos mis ");
        if (!sk_rest_is_filler(h)) return NULL;
        AcquireSRWLockExclusive(&g_lock);
        g_ntimers = 0;
        ReleaseSRWLockExclusive(&g_lock);
        return xstrdup(!count ? "No tienes temporizadores." : count == 1 ? "Listo, cancelé el temporizador."
                                                                         : "Listo, cancelé los temporizadores.");
    }
    sk_take_any(h, " cuanto cuanta le les queda quedan falta faltan tiempo al a los el tengo hay ");
    if (!sk_rest_is_filler(h)) return NULL;
    if (!count) return xstrdup("No tienes temporizadores.");
    return timers_left();
}

/* ------------------------------------------------ alarmas y recordatorios --- */

/* El texto de lo que hay que recordar: lo que queda sin usar, sin el relleno
   de las orillas ("que", "de") (heap; "" si no hay). */
static char *reminder_text(Heard *h)
{
    static const char *const EDGE = " que de a y me te por favor porfa oye sokari ";
    int a = -1, b = -1;
    for (int i = 0; i < h->n; i++) {
        if (h->used[i]) continue;
        if (a < 0) {
            if (sk_is(h, i, EDGE)) continue;
            a = i;
        }
        b = i;
    }
    while (b >= a && a >= 0 && (h->used[b] || sk_is(h, b, EDGE))) b--;
    if (a < 0 || b < a) return xstrdup("");
    StrBuf sb;
    sb_init(&sb);
    for (int i = a; i <= b; i++)
        if (!h->used[i]) sb_appendf(&sb, "%s%s", sb.len ? " " : "", h->orig[i]);
    return sb_steal(&sb);
}

char *sk_alarms(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    const char *who = current_speaker();
    bool alarm_word = sk_find(h, "alarma") >= 0 || sk_find(h, "alarmas") >= 0;
    bool wake = sk_find(h, "despiertame") >= 0 || sk_find(h, "despertarme") >= 0 || sk_find(h, "levantame") >= 0;
    if (alarm_word || wake) {
        static const char *const CANCEL[] = {"cancela", "quita", "borra", "apaga", "desactiva", "elimina", "quitame",
                                             "cancelame"};
        for (size_t i = 0; i < sizeof CANCEL / sizeof *CANCEL && alarm_word; i++) {
            if (sk_take(h, CANCEL[i]) < 0) continue;
            sk_take_any(h, " alarma alarmas la las mi mis todas ");
            if (!sk_rest_is_filler(h)) return NULL;
            int n = reminders_cancel_alarms(who);
            return xstrdup(!n ? "No tienes alarmas puestas." : n == 1 ? "Listo, quité tu alarma."
                                                                      : "Listo, quité tus alarmas.");
        }
        time_t when;
        bool has_clock;
        if (!sk_take_moment(h, true, &when, &has_clock)) {
            /* "¿Qué alarmas tengo?", "¿A qué hora es mi alarma?" */
            sk_take_any(h, " que cuales alarma alarmas tengo hay a hora es son mis puestas pusiste ");
            if (!alarm_word || !sk_rest_is_filler(h)) return NULL;
            double *times;
            int n = reminders_alarm_times(who, &times);
            StrBuf sb;
            sb_init(&sb);
            for (int i = 0; i < n; i++) {
                char *m = sk_say_moment((time_t)times[i]);
                sb_appendf(&sb, "%s%s", i == 0 ? "" : i == n - 1 ? " y " : ", ", m);
                free(m);
            }
            free(times);
            char *list = sb_steal(&sb), *r;
            r = !n ? xstrdup("No tienes alarmas puestas.")
                   : str_printf("Tienes %s: %s.", n == 1 ? "una alarma" : "alarmas", list);
            free(list);
            return r;
        }
        sk_take_any(h, " alarma pon ponme poner programa programame crea creame activa despiertame despertarme "
                       "levantame una la para a de me ");
        if (!sk_rest_is_filler(h)) return NULL;
        struct tm t = sk_local(when);
        char *clock = sk_say_clock(&t, false), *moment = sk_say_moment(when);
        char *text = str_printf("¡Es hora! Tu alarma de %s.", clock);
        bool ok = reminders_add(who, text, (double)when, true);
        char *r = ok ? str_printf("Listo, alarma para %s.", moment) : xstrdup("No pude guardar la alarma.");
        free(text);
        free(clock);
        free(moment);
        return r;
    }

    /* Recordatorios: "recuérdame sacar la ropa en 20 minutos". */
    static const char *const ASK[] = {"recuerdame", "recordarme", "me recuerdas", "me recuerdes", "recuerdamelo",
                                      "no me dejes olvidar", "avisame", "me avisas"};
    int at = -1;
    for (size_t i = 0; i < sizeof ASK / sizeof *ASK && at < 0; i++) at = sk_take(h, ASK[i]);
    if (at < 0) return NULL;
    time_t when;
    bool has_clock;
    if (!sk_take_moment(h, false, &when, &has_clock)) return NULL;
    sk_take_any(h, " puedes podrias ");
    char *text = reminder_text(h);
    char *low = intents_normalize(text);
    bool conditional = !strncmp(low, " si ", 4) || !strncmp(low, " cuando ", 8);
    free(low);
    if (!*text || conditional) {
        /* Sin qué recordar es un temporizador; "avísame si llueve" es para la IA. */
        free(text);
        return NULL;
    }
    char *moment = sk_say_moment(when), *r;
    long in = (long)(when - sk_now());
    if (!reminders_add(who, text, (double)when, false)) {
        r = xstrdup("No pude guardar el recordatorio.");
    } else if (in < 3600 && in > 0) {
        char *d = sk_say_duration(in);
        r = str_printf("Listo, en %s te recuerdo: %s.", d, text);
        free(d);
    } else {
        r = str_printf("Listo, %s te recuerdo: %s.", moment, text);
    }
    free(moment);
    free(text);
    return r;
}

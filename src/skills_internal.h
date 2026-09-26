#ifndef SOKARI_SKILLS_INTERNAL_H
#define SOKARI_SKILLS_INTERNAL_H

/* Lo que comparten las skills locales (skills*.c). */

#include <stdbool.h>
#include <time.h>

#include "skills.h"

#define SK_MAX 48

/* Lo que dijiste, palabra por palabra. w: en minúsculas y sin acentos; los
   números como "25", "2.5" o "7:30" y los signos + - * / ^ % aparte. orig:
   como lo dijiste. used: lo que la skill ya entendió. */
typedef struct {
    int n;
    char *w[SK_MAX];
    char *orig[SK_MAX];
    bool used[SK_MAX];
} Heard;

time_t sk_now(void);
struct tm sk_local(time_t t);

/* Estas palabras seguidas y sin usar ("que hora es"): su posición, marcándolas
   como usadas (sk_take) o sin marcarlas (sk_find); -1 si no están. */
int sk_take(Heard *h, const char *words);
int sk_find(const Heard *h, const char *words);
/* ¿La palabra i es una de la lista (" a b c ")? */
bool sk_is(const Heard *h, int i, const char *list);
/* Marca como usadas todas las que estén en la lista. */
void sk_take_any(Heard *h, const char *list);
/* ¿Lo que no se usó es solo relleno ("oye", "por favor", "Sokari")? */
bool sk_rest_is_filler(const Heard *h);
/* ¿Dijo "no" o "nunca" (no pongas, no me digas)? */
bool sk_negated(const Heard *h);
/* ¿Hay algún número en lo que dijo? */
bool sk_has_number(const Heard *h);

/* El número que empieza en la palabra i ("25", "2.5", "veinticinco", "treinta
   y cinco", "dos punto cinco", "un"): cuántas palabras ocupa (0 si no hay). */
int sk_number(const Heard *h, int i, double *v);
/* Una duración en cualquier parte ("5 minutos", "hora y media", "media hora",
   "1 hora 20 minutos"): segundos, o 0. Marca sus palabras; *first: dónde empieza. */
int sk_take_duration(Heard *h, int *first);
/* Un momento: "en 20 minutos", "a las 7 y media", "mañana a las 9 de la
   noche", "el viernes", "el 15 de octubre". alarm: "a las 7" sin decir de la
   mañana o de la noche es la próxima de la mañana. *has_clock: dijo la hora. */
bool sk_take_moment(Heard *h, bool alarm, time_t *when, bool *has_clock);

/* "las 3 y 25 de la tarde", "la 1 de la madrugada"; exact: "las 7 en punto" (heap). */
char *sk_say_clock(const struct tm *t, bool exact);
/* "hoy", "mañana", "el viernes", "el sábado 3 de octubre" (heap). */
char *sk_say_day(time_t when);
/* "hoy a las 7 de la noche", "mañana a las 9 de la mañana" (heap). */
char *sk_say_moment(time_t when);
/* "5 minutos", "1 hora y 30 minutos", "45 segundos" (heap). */
char *sk_say_duration(long secs);
/* 100 -> "100", 2.5 -> "2.5", 3.14159 con 2 decimales -> "3.14" (heap). */
char *sk_say_number(double x, int decimals);
/* Lo que dijiste de la palabra a a la b (sin incluirla), tal cual (heap). */
char *sk_orig_span(const Heard *h, int a, int b);
/* Una distinta cada vez. */
const char *sk_pick(const char *const *options, int n);

extern const char *const SK_MONTHS[12];
extern const char *const SK_DAYS[7];

/* Cada skill: qué decir (heap) o NULL si la frase no es para ella. */
char *sk_user(Heard *h, bool *end);
char *sk_time_date(Heard *h, bool *end);
char *sk_timers(Heard *h, bool *end);
char *sk_alarms(Heard *h, bool *end);
char *sk_calc(Heard *h, bool *end);
char *sk_weather(Heard *h, bool *end);
char *sk_notes(Heard *h, bool *end);
char *sk_pc(Heard *h, bool *end);
char *sk_chat(Heard *h, bool *end);

/* Clima: las direcciones de Open-Meteo (o las de la prueba). */
const char *sk_geo_url(void);
const char *sk_forecast_url(void);

#endif

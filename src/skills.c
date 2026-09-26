/* Skills locales: el registro, cómo se lee lo que dijiste (palabras, números,
   duraciones, horas y fechas en español) y las skills de la hora y la fecha,
   de cómo va la PC y de la plática. Las de temporizadores y alarmas, cuentas,
   clima y notas están en skills_*.c. Nada de esto le pregunta a la IA. */
#include <windows.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "intents.h"
#include "log.h"
#include "skills_internal.h"
#include "third_party/cJSON.h"
#include "tools.h"
#include "util.h"

/* ------------------------------------------------------------ registro --- */

typedef char *(*SkillFn)(Heard *h, bool *end);

static const struct {
    SkillInfo info;
    SkillFn fn;
} SKILLS[] = {
    /* Primero las tuyas: una rutina "buenos días" le gana al saludo. */
    {{"mis_skills", "Tus rutinas y skills", "Las que tú creas: «modo estudio», «resumen de noticias»"}, sk_user},
    {{"hora", "Hora y fecha", "«¿Qué hora es?», «¿Qué día es hoy?», «¿Cuánto falta para Navidad?»"}, sk_time_date},
    {{"temporizador", "Temporizadores y cronómetro", "«Pon un temporizador de 10 minutos», «Inicia el cronómetro»"},
     sk_timers},
    {{"alarma", "Alarmas y recordatorios", "«Despiértame a las 7», «Recuérdame en 20 minutos sacar la ropa»"},
     sk_alarms},
    {{"calculadora", "Cuentas y conversiones", "«¿Cuánto es 25 por 4?», «¿Cuántas libras son 70 kilos?»"}, sk_calc},
    {{"clima", "El clima", "«¿Cómo está el clima?», «¿Va a llover mañana?»"}, sk_weather},
    {{"notas", "Notas y pendientes", "«Anota comprar leche», «¿Qué tengo pendiente?»"}, sk_notes},
    {{"pc", "Cómo va la PC", "«¿Cuánta batería tengo?», «¿Cómo va la compu?»"}, sk_pc},
    {{"platica", "Saludos, plática y chistes", "«Buenos días», «¿Cómo estás?», «Cuéntame un chiste»"}, sk_chat},
};
#define NSKILLS ((int)(sizeof SKILLS / sizeof *SKILLS))

int skills_count(void)
{
    return NSKILLS;
}

const SkillInfo *skills_get(int i)
{
    return i >= 0 && i < NSKILLS ? &SKILLS[i].info : NULL;
}

static void heard_parse(Heard *h, const char *text);
static void heard_free(Heard *h);

char *skills_try(const char *text, const SkillInfo **which, bool *end)
{
    *which = NULL;
    *end = false;
    Heard h;
    heard_parse(&h, text);
    char *r = NULL;
    if (h.n > 0 && h.n < SK_MAX) {
        for (int i = 0; i < NSKILLS && !r; i++) {
            if (!config_skill_enabled(SKILLS[i].info.id)) continue;
            memset(h.used, 0, sizeof h.used);
            r = SKILLS[i].fn(&h, end);
            if (r) *which = &SKILLS[i].info;
        }
    }
    heard_free(&h);
    return r;
}

/* ---------------------------------------------------------------- hora --- */

static volatile LONG64 g_clock;

void skills_set_clock(time_t now)
{
    InterlockedExchange64(&g_clock, (LONG64)now);
}

time_t sk_now(void)
{
    time_t fixed = (time_t)g_clock;
    return fixed ? fixed : time(NULL);
}

struct tm sk_local(time_t t)
{
    struct tm r;
    if (localtime_s(&r, &t) != 0) memset(&r, 0, sizeof r);
    return r;
}

/* --------------------------------------------------------- las palabras --- */

static char accent_base(unsigned char c)
{
    switch (c) {
    case 0xA1: case 0x81: case 0xA0: case 0x80: case 0xA4: case 0x84: return 'a';
    case 0xA9: case 0x89: case 0xA8: case 0x88: case 0xAB: case 0x8B: return 'e';
    case 0xAD: case 0x8D: case 0xAC: case 0x8C: case 0xAF: case 0x8F: return 'i';
    case 0xB3: case 0x93: case 0xB2: case 0x92: case 0xB6: case 0x96: return 'o';
    case 0xBA: case 0x9A: case 0xB9: case 0x99: case 0xBC: case 0x9C: return 'u';
    case 0xB1: case 0x91: return 'n';
    default: return 0;
    }
}

static void heard_push(Heard *h, StrBuf *w, StrBuf *o)
{
    if (w->len && h->n < SK_MAX) {
        h->w[h->n] = sb_steal(w);
        h->orig[h->n] = sb_steal(o);
        h->n++;
    }
    sb_free(w);
    sb_free(o);
    sb_init(w);
    sb_init(o);
}

static void heard_parse(Heard *h, const char *text)
{
    memset(h, 0, sizeof *h);
    StrBuf w, o;
    sb_init(&w);
    sb_init(&o);
    int kind = 0; /* 0 nada, 1 palabra, 2 número */
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        unsigned char c = *p;
        char base = 0;
        if (isalpha(c) && c < 0x80) base = (char)tolower(c);
        else if (c == 0xC3 && p[1]) base = accent_base(p[1]);
        if (base) {
            if (kind == 2) heard_push(h, &w, &o);
            kind = 1;
            sb_append_char(&w, base);
            sb_append_n(&o, (const char *)p, c == 0xC3 ? 2 : 1);
            p += c == 0xC3 ? 2 : 1;
            continue;
        }
        if (isdigit(c)) {
            if (kind == 1) heard_push(h, &w, &o);
            kind = 2;
            sb_append_char(&w, (char)c);
            sb_append_char(&o, (char)c);
            p++;
            continue;
        }
        if (kind == 2 && (c == '.' || c == ',' || c == ':') && isdigit(p[1])) {
            int digits = 0;
            while (isdigit(p[1 + digits])) digits++;
            bool thousands = c == ',' && digits == 3 && !strchr(w.data, '.');
            if (!thousands) sb_append_char(&w, c == ',' ? '.' : (char)c);
            sb_append_char(&o, (char)c);
            p++;
            continue;
        }
        heard_push(h, &w, &o);
        kind = 0;
        if (strchr("+-*/^%", c)) {
            char s[2] = {(char)c, 0};
            sb_append(&w, s);
            sb_append(&o, s);
            heard_push(h, &w, &o);
        }
        /* Otro carácter de varios bytes (¿, ¡, °, emoji): se salta entero. */
        if (c >= 0xC0) {
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        } else {
            p++;
        }
    }
    heard_push(h, &w, &o);
    sb_free(&w);
    sb_free(&o);
}

static void heard_free(Heard *h)
{
    for (int i = 0; i < h->n && i < SK_MAX; i++) {
        free(h->w[i]);
        free(h->orig[i]);
    }
}

static bool in_list(const char *list, const char *w)
{
    char pat[48];
    if (strlen(w) + 3 > sizeof pat) return false;
    snprintf(pat, sizeof pat, " %s ", w);
    return strstr(list, pat) != NULL;
}

static int match_at(const Heard *h, int i, const char *words)
{
    char *copy = xstrdup(words), *ctx = NULL;
    int k = 0;
    bool ok = true;
    for (char *t = strtok_s(copy, " ", &ctx); t && ok; t = strtok_s(NULL, " ", &ctx), k++)
        ok = i + k < h->n && !h->used[i + k] && !strcmp(h->w[i + k], t);
    free(copy);
    return ok ? k : 0;
}

int sk_find(const Heard *h, const char *words)
{
    for (int i = 0; i < h->n; i++)
        if (match_at(h, i, words)) return i;
    return -1;
}

int sk_take(Heard *h, const char *words)
{
    for (int i = 0; i < h->n; i++) {
        int k = match_at(h, i, words);
        if (k) {
            for (int j = 0; j < k; j++) h->used[i + j] = true;
            return i;
        }
    }
    return -1;
}

bool sk_is(const Heard *h, int i, const char *list)
{
    return i >= 0 && i < h->n && in_list(list, h->w[i]);
}

void sk_take_any(Heard *h, const char *list)
{
    for (int i = 0; i < h->n; i++)
        if (in_list(list, h->w[i])) h->used[i] = true;
}

/* Lo que puede acompañar a cualquier skill sin cambiarla. */
static const char *const FILLER =
    " y e o a al el la lo los las le les me mi mis tu te de del en con por favor porfa porfis plis please oye oiga "
    "hey ey eh ah oh ok okey sale si pues ya ahora ahorita puedes podrias puedas pudieras quiero quisiera necesito "
    "haz hazme dale andale orale bueno mira entonces rapido rapidito porfavor sabes dime decirme dices dijeras "
    "amigo amiga carnal compa bro wey guey we mano jefe jefa hola que tal vez sokari fijate oiga nomas nada mas "
    "quiubo quihubo buenas buenos buen dia dias tardes noches manito neta sobres gracias decir disculpa perdon ";

bool sk_rest_is_filler(const Heard *h)
{
    for (int i = 0; i < h->n; i++)
        if (!h->used[i] && !in_list(FILLER, h->w[i]) && !intents_is_name_word(h->w[i])) return false;
    return true;
}

bool sk_negated(const Heard *h)
{
    for (int i = 0; i < h->n; i++)
        if (in_list(" no nunca tampoco ", h->w[i]) && !sk_is(h, i + 1, " mames manches inventes friegues "))
            return true;
    return false;
}

bool sk_has_number(const Heard *h)
{
    for (int i = 0; i < h->n; i++) {
        double v;
        if (sk_number(h, i, &v)) return true;
    }
    return false;
}

/* -------------------------------------------------------------- números --- */

static const struct {
    const char *w;
    int v;
} NUM_WORDS[] = {
    {"cero", 0},          {"un", 1},             {"uno", 1},           {"una", 1},           {"dos", 2},
    {"tres", 3},          {"cuatro", 4},         {"cinco", 5},         {"seis", 6},          {"siete", 7},
    {"ocho", 8},          {"nueve", 9},          {"diez", 10},         {"once", 11},         {"doce", 12},
    {"trece", 13},        {"catorce", 14},       {"quince", 15},       {"dieciseis", 16},    {"diecisiete", 17},
    {"dieciocho", 18},    {"diecinueve", 19},    {"veinte", 20},       {"veintiun", 21},     {"veintiuno", 21},
    {"veintiuna", 21},    {"veintidos", 22},     {"veintitres", 23},   {"veinticuatro", 24}, {"veinticinco", 25},
    {"veintiseis", 26},   {"veintisiete", 27},   {"veintiocho", 28},   {"veintinueve", 29},  {"treinta", 30},
    {"cuarenta", 40},     {"cincuenta", 50},     {"sesenta", 60},      {"setenta", 70},      {"ochenta", 80},
    {"noventa", 90},      {"cien", 100},         {"ciento", 100},      {"doscientos", 200},  {"doscientas", 200},
    {"trescientos", 300}, {"trescientas", 300},  {"cuatrocientos", 400}, {"cuatrocientas", 400},
    {"quinientos", 500},  {"quinientas", 500},   {"seiscientos", 600}, {"seiscientas", 600}, {"setecientos", 700},
    {"setecientas", 700}, {"ochocientos", 800},  {"ochocientas", 800}, {"novecientos", 900}, {"novecientas", 900},
    {"mil", 1000},
};

static int word_value(const char *w)
{
    for (size_t i = 0; i < sizeof NUM_WORDS / sizeof *NUM_WORDS; i++)
        if (!strcmp(NUM_WORDS[i].w, w)) return NUM_WORDS[i].v;
    return -1;
}

static bool is_digits(const char *w, bool allow_point)
{
    if (!isdigit((unsigned char)*w)) return false;
    int points = 0;
    for (const char *p = w; *p; p++) {
        if (*p == '.' && allow_point) points++;
        else if (!isdigit((unsigned char)*p)) return false;
    }
    return points <= 1;
}

/* Un número en palabras: "ciento veinticinco", "treinta y cinco", "dos mil". */
static int words_number(const Heard *h, int i, double *v)
{
    int total = 0, cur = 0, last = 100000, j = i;
    while (j < h->n) {
        int x = word_value(h->w[j]);
        if (x < 0) {
            /* "treinta y cinco": el y solo va después de las decenas. */
            if (!strcmp(h->w[j], "y") && j > i && last >= 20 && last < 100 && last % 10 == 0 && j + 1 < h->n) {
                int u = word_value(h->w[j + 1]);
                if (u >= 1 && u <= 9) {
                    cur += u;
                    last = u;
                    j += 2;
                    continue;
                }
            }
            break;
        }
        if (x == 1000) {
            total += (cur ? cur : 1) * 1000;
            cur = 0;
            last = 1000;
        } else if (j > i && x >= last) {
            break; /* "dos tres" son dos números */
        } else {
            cur += x;
            last = x;
        }
        j++;
    }
    if (j == i) return 0;
    *v = total + cur;
    return j - i;
}

int sk_number(const Heard *h, int i, double *v)
{
    if (i < 0 || i >= h->n) return 0;
    int used;
    if (is_digits(h->w[i], true)) {
        *v = strtod(h->w[i], NULL);
        used = 1;
    } else {
        used = words_number(h, i, v);
        if (!used) return 0;
    }
    /* "dos punto cinco", "3 punto 14" */
    int j = i + used;
    if (j + 1 < h->n && (!strcmp(h->w[j], "punto") || !strcmp(h->w[j], "coma"))) {
        double d;
        int k = is_digits(h->w[j + 1], false) ? 1 : words_number(h, j + 1, &d);
        if (k) {
            const char *digits = h->w[j + 1];
            char buf[32];
            if (is_digits(digits, false)) {
                snprintf(buf, sizeof buf, "0.%s", digits);
            } else {
                snprintf(buf, sizeof buf, "0.%d", (int)d);
            }
            *v += strtod(buf, NULL);
            used += 1 + k;
        }
    }
    return used;
}

/* ------------------------------------------------------------ duraciones --- */

static int unit_seconds(const char *w)
{
    if (in_list(" segundo segundos seg segs segunditos ", w)) return 1;
    if (in_list(" minuto minutos min mins minutito minutitos ", w)) return 60;
    if (in_list(" hora horas hr hrs horita horitas ", w)) return 3600;
    if (in_list(" dia dias ", w)) return 86400;
    return 0;
}

int sk_take_duration(Heard *h, int *first)
{
    for (int i = 0; i < h->n; i++) {
        if (h->used[i]) continue;
        int j = i;
        long total = 0;
        bool any = false;
        for (;;) {
            double n = 0;
            int k = 0, unit = 0;
            if (j + 1 < h->n && !strcmp(h->w[j], "media") && unit_seconds(h->w[j + 1]) == 3600) {
                total += 1800, j += 2, any = true; /* media hora */
            } else if (j + 3 < h->n && sk_is(h, j, " un ") && !strcmp(h->w[j + 1], "cuarto") &&
                       !strcmp(h->w[j + 2], "de") && unit_seconds(h->w[j + 3]) == 3600) {
                total += 900, j += 4, any = true; /* un cuarto de hora */
            } else if ((k = sk_number(h, j, &n)) > 0 && j + k < h->n && (unit = unit_seconds(h->w[j + k])) > 0 &&
                       n > 0 && n <= 1000) {
                total += (long)(n * unit + 0.5), j += k + 1, any = true;
                /* "hora y media", "5 minutos y medio" */
                if (j + 1 < h->n && !strcmp(h->w[j], "y") && sk_is(h, j + 1, " media medio ")) {
                    total += unit / 2, j += 2;
                } else if (j + 1 < h->n && !strcmp(h->w[j], "y") && !strcmp(h->w[j + 1], "cuarto") && unit == 3600) {
                    total += 900, j += 2;
                }
            } else if (!any && (unit = unit_seconds(h->w[j])) > 0 && unit >= 60 &&
                       (j == 0 || !sk_is(h, j - 1, " unos unas varios varias algunos algunas pocos "))) {
                /* "hora y media" sin número: una hora. */
                if (j + 2 < h->n && !strcmp(h->w[j + 1], "y") && sk_is(h, j + 2, " media medio cuarto ")) {
                    total += unit + (strcmp(h->w[j + 2], "cuarto") ? unit / 2 : 900), j += 3, any = true;
                } else {
                    break;
                }
            } else {
                break;
            }
            /* "1 hora 20 minutos", "1 hora con 20", "2 minutos y 30 segundos" */
            int save = j;
            if (j < h->n && sk_is(h, j, " y con ")) j++;
            double m;
            int kk = sk_number(h, j, &m);
            if (!(kk && j + kk < h->n && unit_seconds(h->w[j + kk]))) {
                j = save;
                break;
            }
        }
        if (any && total > 0) {
            for (int k = i; k < j; k++) h->used[k] = true;
            *first = i;
            return (int)total;
        }
    }
    return 0;
}

/* ----------------------------------------------------------- momentos --- */

const char *const SK_MONTHS[12] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                                   "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
const char *const SK_DAYS[7] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
static const char *const DAY_WORDS[7] = {"domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"};

static int month_index(const char *w)
{
    for (int i = 0; i < 12; i++)
        if (!strcmp(w, SK_MONTHS[i])) return i;
    return !strcmp(w, "setiembre") ? 8 : -1;
}

/* De la mañana (1), de la tarde (2) o de la noche (3), en la palabra i;
   cuántas palabras usa. */
static int period_at(const Heard *h, int i, int *period)
{
    if (i + 2 < h->n && sk_is(h, i, " de en por ") && !strcmp(h->w[i + 1], "la")) {
        if (sk_is(h, i + 2, " manana madrugada ")) return *period = 1, 3;
        if (sk_is(h, i + 2, " tarde ")) return *period = 2, 3;
        if (sk_is(h, i + 2, " noche ")) return *period = 3, 3;
    }
    if (i + 1 < h->n && !strcmp(h->w[i], "del") && !strcmp(h->w[i + 1], "dia")) return *period = 2, 2;
    if (i < h->n && sk_is(h, i, " am ")) return *period = 1, 1;
    if (i < h->n && sk_is(h, i, " pm ")) return *period = 2, 1;
    if (i + 1 < h->n && !strcmp(h->w[i + 1], "m") && sk_is(h, i, " a p ")) return *period = h->w[i][0] == 'a' ? 1 : 2, 2;
    return 0;
}

static void mark(Heard *h, int a, int b)
{
    for (int k = a; k < b && k < h->n; k++) h->used[k] = true;
}

/* La hora: "a las 7", "a las 7:30", "a la una y media", "a las 8 menos cuarto",
   "a las 10 de la noche", "al mediodía". */
static bool take_clock(Heard *h, int *hour, int *minute, int *period)
{
    *period = 0;
    for (int i = 0; i < h->n; i++) {
        if (h->used[i]) continue;
        if (!strcmp(h->w[i], "mediodia") || !strcmp(h->w[i], "medianoche")) {
            *hour = 12;
            *minute = 0;
            *period = h->w[i][5] == 'd' ? 2 : 3;
            int a = i;
            if (a > 0 && sk_is(h, a - 1, " al a el ")) a--;
            if (a > 0 && !strcmp(h->w[a], "el") && !strcmp(h->w[a - 1], "a")) a--;
            mark(h, a, i + 1);
            return true;
        }
        /* "las 7", "la 1": con artículo, así no se confunde con otro número. */
        if (!sk_is(h, i, " las la ")) continue;
        int j = i + 1, hh = -1, mm = 0;
        if (j < h->n && strchr(h->w[j], ':')) {
            if (sscanf(h->w[j], "%d:%d", &hh, &mm) != 2) continue;
            j++;
        } else {
            double v;
            int k = sk_number(h, j, &v);
            if (!k || v != floor(v)) continue;
            hh = (int)v;
            j += k;
        }
        if (hh < 0 || hh > 24 || mm < 0 || mm > 59) continue;
        if (!strcmp(h->w[i], "la") && hh != 1) continue;
        if (j + 1 < h->n && !strcmp(h->w[j], "y") && sk_is(h, j + 1, " media cuarto ")) {
            mm = h->w[j + 1][0] == 'm' ? 30 : 15;
            j += 2;
        } else if (j + 1 < h->n && !strcmp(h->w[j], "menos")) {
            double m;
            int k = sk_is(h, j + 1, " cuarto ") ? 1 : sk_number(h, j + 1, &m);
            if (k) {
                mm = sk_is(h, j + 1, " cuarto ") ? 45 : 60 - (int)m;
                hh = hh == 0 ? 23 : hh - 1;
                if (hh == 0 && !strcmp(h->w[i], "la")) hh = 12;
                j += 1 + k;
            }
        } else if (j < h->n && !strcmp(h->w[j], "y")) {
            double m;
            int k = sk_number(h, j + 1, &m);
            if (k && m >= 1 && m < 60) {
                mm = (int)m;
                j += 1 + k;
                if (j < h->n && sk_is(h, j, " minutos minuto ")) j++;
            }
        } else if (mm == 0 && j < h->n && is_digits(h->w[j], false) && strlen(h->w[j]) == 2) {
            mm = atoi(h->w[j]); /* "a las 7 30" */
            if (mm > 59) continue;
            j++;
        }
        if (j + 1 < h->n && !strcmp(h->w[j], "en") && !strcmp(h->w[j + 1], "punto")) j += 2;
        if (j < h->n && sk_is(h, j, " horas hrs hr ")) j++;
        j += period_at(h, j, period);
        int a = i;
        if (a > 0 && sk_is(h, a - 1, " a para como desde ")) a--;
        if (a > 0 && !strcmp(h->w[a], "a") && !strcmp(h->w[a - 1], "como")) a--;
        mark(h, a, j);
        *hour = hh == 24 ? 12 : hh;
        *minute = mm;
        if (hh == 24) *period = 3;
        if (*hour >= 13) *period = 2;
        return true;
    }
    return false;
}

/* La hora del día (0-23) con lo que se dijo: 12 de la noche es medianoche. */
static int hour24(int hh, int period, int *extra_day)
{
    *extra_day = 0;
    if (period == 1) return hh == 12 ? 0 : hh;
    if (period == 3 && hh == 12) return *extra_day = 1, 0;
    if (period >= 2 && hh < 12) return hh + 12;
    return hh;
}

bool sk_take_moment(Heard *h, bool alarm, time_t *when, bool *has_clock)
{
    time_t now = sk_now();
    *has_clock = false;
    /* "en 20 minutos", "dentro de una hora" */
    bool before[SK_MAX];
    memcpy(before, h->used, sizeof before);
    int f = 0, secs = sk_take_duration(h, &f);
    if (secs) {
        if (f > 0 && !strcmp(h->w[f - 1], "en")) {
            h->used[f - 1] = true;
        } else if (f > 1 && !strcmp(h->w[f - 2], "dentro") && !strcmp(h->w[f - 1], "de")) {
            h->used[f - 1] = h->used[f - 2] = true;
        } else {
            memcpy(h->used, before, sizeof before);
            secs = 0;
        }
        if (secs) {
            *when = now + secs;
            *has_clock = true;
            return true;
        }
    }

    struct tm t = sk_local(now);
    int day_off = -1, wday = -1, mon = -1, mday = 0, i, day_period = 0;
    bool next_week = false;
    if ((i = sk_take(h, "pasado manana")) >= 0) {
        day_off = 2;
    } else if ((i = sk_find(h, "manana")) >= 0 && !(i > 0 && !strcmp(h->w[i - 1], "la"))) {
        h->used[i] = true;
        day_off = 1;
    } else if (sk_take(h, "hoy") >= 0) {
        day_off = 0;
    }
    if (sk_take(h, "esta noche") >= 0) day_off = day_off < 0 ? 0 : day_off, day_period = 3;
    else if (sk_take(h, "esta tarde") >= 0) day_off = day_off < 0 ? 0 : day_off, day_period = 2;
    for (int d = 0; d < 7 && wday < 0; d++) {
        if ((i = sk_find(h, DAY_WORDS[d])) < 0) continue;
        h->used[i] = true;
        wday = d;
        if (i > 0 && sk_is(h, i - 1, " el este proximo siguiente ")) {
            next_week = sk_is(h, i - 1, " proximo siguiente ");
            h->used[i - 1] = true;
            if (i > 1 && !strcmp(h->w[i - 2], "el") && next_week) h->used[i - 2] = true;
        }
        if (i + 2 < h->n && !strcmp(h->w[i + 1], "que") && !strcmp(h->w[i + 2], "viene")) {
            next_week = true;
            mark(h, i + 1, i + 3);
        }
    }
    for (int k = 2; k < h->n && mon < 0; k++) {
        double d;
        int m = month_index(h->w[k]), used;
        if (m < 0 || h->used[k] || strcmp(h->w[k - 1], "de")) continue;
        for (int s0 = k - 2; s0 >= 0 && s0 >= k - 3; s0--) {
            if ((used = sk_number(h, s0, &d)) && s0 + used == k - 1 && d >= 1 && d <= 31 && d == floor(d)) {
                mon = m, mday = (int)d;
                mark(h, s0 > 0 && !strcmp(h->w[s0 - 1], "el") ? s0 - 1 : s0, k + 1);
                break;
            }
        }
    }

    int hour = -1, minute = 0, period = 0;
    bool clock = take_clock(h, &hour, &minute, &period);
    if (!period) {
        /* "en la mañana", "de la noche" dicho aparte de la hora */
        for (int k = 0; k < h->n && !period; k++) {
            int used = h->used[k] ? 0 : period_at(h, k, &period);
            if (used) mark(h, k, k + used);
        }
    }
    if (!period) period = day_period;
    if (!clock && day_off < 0 && wday < 0 && mon < 0) return false;
    if (!clock && alarm) return false;

    bool other_day = day_off > 0 || wday >= 0 || mon >= 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    if (mon >= 0) {
        struct tm today = t;
        t.tm_mon = mon, t.tm_mday = mday;
        if (mon < today.tm_mon || (mon == today.tm_mon && mday < today.tm_mday)) t.tm_year++;
    } else if (wday >= 0) {
        int d = (wday - t.tm_wday + 7) % 7;
        if (d == 0 && (next_week || !clock)) d = 7;
        t.tm_mday += d;
    } else if (day_off > 0) {
        t.tm_mday += day_off;
    }
    if (!clock) {
        t.tm_hour = period == 3 ? 21 : period == 2 ? 16 : 9, t.tm_min = 0;
    } else if (!period && hour >= 1 && hour <= 12 && !other_day) {
        /* Sin decir de la mañana o de la noche: la próxima vez que den esas
           horas (una alarma: la de la mañana). */
        *has_clock = true;
        for (int d = 0; d < (day_off == 0 ? 1 : 2); d++) {
            for (int o = 0; o < (alarm ? 1 : 2); o++) {
                struct tm c = t;
                c.tm_mday += d;
                c.tm_hour = hour % 12 + 12 * o;
                if (hour == 12) c.tm_hour = o ? 0 : 12, c.tm_mday += o;
                c.tm_min = minute;
                time_t x = mktime(&c);
                if (x > now) {
                    *when = x;
                    return true;
                }
            }
        }
        return false;
    } else {
        int extra = 0, hh = period ? hour24(hour, period, &extra) : hour;
        /* "mañana a las 3": de la tarde; "mañana a las 8": de la mañana. */
        if (!period && other_day && !alarm && hh >= 1 && hh <= 6) hh += 12;
        t.tm_hour = hh, t.tm_min = minute, t.tm_mday += extra;
        *has_clock = true;
    }
    time_t r = mktime(&t);
    if (r <= now) {
        if (other_day || day_off == 0) return false;
        t.tm_mday++;
        r = mktime(&t);
    }
    *when = r;
    return true;
}

/* ------------------------------------------------------------ al hablar --- */

char *sk_say_clock(const struct tm *t, bool exact)
{
    int h = t->tm_hour, m = t->tm_min, h12 = h % 12 == 0 ? 12 : h % 12;
    const char *period = h < 6 ? "de la madrugada" : h < 12 ? "de la mañana" : h < 13 ? "del día"
                       : h < 20 ? "de la tarde" : "de la noche";
    if (h == 0 && m == 0) return xstrdup("las 12 de la noche");
    char mins[24] = "";
    if (m == 15) snprintf(mins, sizeof mins, " y cuarto");
    else if (m == 30) snprintf(mins, sizeof mins, " y media");
    else if (m) snprintf(mins, sizeof mins, " y %d", m);
    else if (exact) snprintf(mins, sizeof mins, " en punto");
    return str_printf("%s %d%s %s", h12 == 1 ? "la" : "las", h12, mins, period);
}

static long day_number(time_t t)
{
    struct tm x = sk_local(t);
    x.tm_hour = 12, x.tm_min = 0, x.tm_sec = 0, x.tm_isdst = -1;
    return (long)(mktime(&x) / 86400);
}

char *sk_say_day(time_t when)
{
    long d = day_number(when) - day_number(sk_now());
    struct tm t = sk_local(when);
    if (d == 0) return xstrdup("hoy");
    if (d == 1) return xstrdup("mañana");
    if (d == 2) return xstrdup("pasado mañana");
    if (d > 2 && d < 7) return str_printf("el %s", SK_DAYS[t.tm_wday]);
    return str_printf("el %s %d de %s", SK_DAYS[t.tm_wday], t.tm_mday, SK_MONTHS[t.tm_mon]);
}

char *sk_say_moment(time_t when)
{
    struct tm t = sk_local(when);
    char *day = sk_say_day(when), *clock = sk_say_clock(&t, false);
    char *r = str_printf("%s a %s", day, clock);
    free(day);
    free(clock);
    return r;
}

char *sk_say_duration(long secs)
{
    long h = secs / 3600, m = secs % 3600 / 60, s = secs % 60;
    StrBuf sb;
    sb_init(&sb);
    if (h) sb_appendf(&sb, "%ld hora%s", h, h == 1 ? "" : "s");
    if (m) sb_appendf(&sb, "%s%ld minuto%s", sb.len ? (s ? ", " : " y ") : "", m, m == 1 ? "" : "s");
    if (s || !sb.len) sb_appendf(&sb, "%s%ld segundo%s", sb.len ? " y " : "", s, s == 1 ? "" : "s");
    return sb_steal(&sb);
}

char *sk_say_number(double x, int decimals)
{
    double f = pow(10, decimals);
    double r = round(x * f) / f;
    if (fabs(r) < 1e15 && r == floor(r)) return str_printf("%.0f", r);
    char *s = str_printf("%.*f", decimals, r);
    for (size_t n = strlen(s); n && s[n - 1] == '0'; n--) s[n - 1] = 0;
    return s;
}

char *sk_orig_span(const Heard *h, int a, int b)
{
    StrBuf sb;
    sb_init(&sb);
    for (int i = a; i < b && i < h->n; i++) sb_appendf(&sb, "%s%s", sb.len ? " " : "", h->orig[i]);
    char *r = sb_steal(&sb);
    return r ? r : xstrdup("");
}

const char *sk_pick(const char *const *options, int n)
{
    static volatile LONG turn;
    return options[(unsigned)InterlockedIncrement(&turn) % (unsigned)n];
}

/* ------------------------------------------------------ hora y fecha --- */

typedef struct {
    const char *words; /* como se dice, normalizado */
    const char *name;  /* como se dice de vuelta */
    int mon, mday;     /* mday 0: se calcula */
} Holiday;

static const Holiday HOLIDAYS[] = {
    {"navidad", "Navidad", 11, 25},
    {"noche buena", "Nochebuena", 11, 24},
    {"nochebuena", "Nochebuena", 11, 24},
    {"ano nuevo", "Año Nuevo", 0, 1},
    {"fin de ano", "fin de año", 11, 31},
    {"ano viejo", "fin de año", 11, 31},
    {"dia de muertos", "el Día de Muertos", 10, 2},
    {"halloween", "Halloween", 9, 31},
    {"san valentin", "San Valentín", 1, 14},
    {"dia del amor", "el Día del Amor", 1, 14},
    {"dia de las madres", "el Día de las Madres", 4, 10},
    {"dia de la madre", "el Día de las Madres", 4, 10},
    {"dia del padre", "el Día del Padre", 5, 0},
    {"dia de reyes", "el Día de Reyes", 0, 6},
    {"dia de la independencia", "el Día de la Independencia", 8, 16},
    {"el grito", "el Grito", 8, 15},
};

static long days_until(struct tm target)
{
    target.tm_hour = 12, target.tm_min = target.tm_sec = 0, target.tm_isdst = -1;
    return (long)(mktime(&target) / 86400) - day_number(sk_now());
}

/* ¿Cuánto falta para…? Navidad, un día del año o un día de la semana. */
static char *until_reply(Heard *h)
{
    int at = sk_take(h, "cuanto falta para");
    if (at < 0) at = sk_take(h, "cuantos dias faltan para");
    if (at < 0) at = sk_take(h, "cuanto queda para");
    if (at < 0) at = sk_take(h, "cuanto le falta para");
    if (at < 0) return NULL;
    struct tm today = sk_local(sk_now()), t = today;
    const char *name = NULL;
    char name_buf[48];
    for (size_t k = 0; k < sizeof HOLIDAYS / sizeof *HOLIDAYS && !name; k++) {
        if (sk_take(h, HOLIDAYS[k].words) < 0) continue;
        name = HOLIDAYS[k].name;
        t.tm_mon = HOLIDAYS[k].mon;
        t.tm_mday = HOLIDAYS[k].mday;
        if (!t.tm_mday) {
            /* El Día del Padre: el tercer domingo de junio. */
            struct tm first = t;
            first.tm_mday = 1, first.tm_hour = 12, first.tm_isdst = -1;
            mktime(&first);
            t.tm_mday = 1 + (7 - first.tm_wday) % 7 + 14;
        }
    }
    if (!name) {
        /* "el 20 de octubre", "el viernes", "las 5" */
        time_t when;
        bool has_clock;
        if (!sk_take_moment(h, false, &when, &has_clock)) return NULL;
        if (has_clock) {
            if (!sk_rest_is_filler(h)) return NULL;
            struct tm c = sk_local(when);
            char *left = sk_say_duration((long)(when - sk_now()) / 60 * 60), *clock = sk_say_clock(&c, false), *r;
            if (when - sk_now() < 60) r = str_printf("Ya casi son %s.", clock);
            else r = str_printf("%s %s para %s.", !strncmp(left, "1 ", 2) ? "Falta" : "Faltan", left, clock);
            free(left);
            free(clock);
            return r;
        }
        t = sk_local(when);
        /* Se contesta como lo dijiste: "el 1 de octubre" o "el viernes". */
        bool said_date = false;
        for (int k = 0; k < h->n; k++) said_date |= month_index(h->w[k]) >= 0;
        if (!said_date) snprintf(name_buf, sizeof name_buf, "el %s", SK_DAYS[t.tm_wday]);
        else snprintf(name_buf, sizeof name_buf, "el %d de %s", t.tm_mday, SK_MONTHS[t.tm_mon]);
        name = name_buf;
    } else if (days_until(t) < 0) {
        t.tm_year++;
    }
    sk_take_any(h, " dias ");
    if (!sk_rest_is_filler(h)) return NULL;
    long d = days_until(t);
    if (d == 0) return str_printf("¡%s es hoy!", name);
    if (d == 1) return str_printf("¡%s es mañana!", name);
    return str_printf("Faltan %ld días para %s.", d, name);
}

char *sk_time_date(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    struct tm t = sk_local(sk_now());
    static const char *const HOUR_Q[] = {"que hora es",      "que horas son", "que hora tienes", "que horas tienes",
                                         "me dices la hora", "dime la hora",  "me das la hora",  "la hora"};
    for (size_t i = 0; i < sizeof HOUR_Q / sizeof *HOUR_Q; i++) {
        if (sk_take(h, HOUR_Q[i]) < 0) continue;
        sk_take_any(h, " exacta ahorita ");
        if (!sk_rest_is_filler(h)) return NULL;
        char *clock = sk_say_clock(&t, true), *r;
        r = str_printf("%s %s.", t.tm_hour % 12 == 1 ? "Es" : "Son", clock);
        free(clock);
        r[0] = (char)toupper((unsigned char)r[0]);
        return r;
    }
    static const char *const DATE_Q[] = {"que dia es hoy", "que fecha es hoy", "a cuantos estamos",
                                         "en que fecha estamos", "que dia es", "que fecha es", "que dia estamos",
                                         "en que dia estamos"};
    for (size_t i = 0; i < sizeof DATE_Q / sizeof *DATE_Q; i++) {
        if (sk_take(h, DATE_Q[i]) < 0) continue;
        bool tomorrow = sk_take(h, "manana") >= 0, yesterday = !tomorrow && sk_take(h, "ayer") >= 0;
        sk_take_any(h, " hoy ");
        if (!sk_rest_is_filler(h)) return NULL;
        struct tm d = t;
        d.tm_mday += tomorrow ? 1 : yesterday ? -1 : 0;
        d.tm_hour = 12, d.tm_isdst = -1;
        mktime(&d);
        return str_printf("%s %s %d de %s de %d.", tomorrow ? "Mañana es" : yesterday ? "Ayer fue" : "Hoy es",
                          SK_DAYS[d.tm_wday], d.tm_mday, SK_MONTHS[d.tm_mon], d.tm_year + 1900);
    }
    if (sk_take(h, "en que ano estamos") >= 0 || sk_take(h, "que ano es") >= 0 || sk_take(h, "en que ano vamos") >= 0) {
        if (!sk_rest_is_filler(h)) return NULL;
        return str_printf("Estamos en %d.", t.tm_year + 1900);
    }
    if (sk_take(h, "en que mes estamos") >= 0 || sk_take(h, "que mes es") >= 0) {
        if (!sk_rest_is_filler(h)) return NULL;
        return str_printf("Estamos en %s.", SK_MONTHS[t.tm_mon]);
    }
    return until_reply(h);
}

/* ------------------------------------------------------------------- PC --- */

/* De "CPU al 12%; RAM al 51% (8GB de 16GB); …" el pedazo que empieza así. */
static char *segment(const char *info, const char *prefix)
{
    const char *p = strstr(info, prefix);
    if (!p) return NULL;
    const char *e = strchr(p, ';');
    return e ? xstrndup(p, (size_t)(e - p)) : xstrdup(p);
}

char *sk_pc(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    bool battery = false, ram = false, disk = false, cpu = false, all = false;
    if (sk_find(h, "bateria") >= 0 || sk_find(h, "pila") >= 0) {
        battery = true;
        sk_take_any(h, " bateria pila cuanta cuanto queda tengo tiene como esta va trae nivel carga cargando "
                       "le me la ");
    }
    if (sk_find(h, "ram") >= 0) {
        ram = true;
        sk_take_any(h, " ram memoria cuanta cuanto uso usando estoy usa esta libre ");
    }
    if (sk_find(h, "espacio") >= 0 || sk_find(h, "disco") >= 0) {
        disk = true;
        sk_take_any(h, " espacio disco cuanto libre queda tengo hay duro usado ");
    }
    if (sk_find(h, "cpu") >= 0 || sk_find(h, "procesador") >= 0) {
        cpu = true;
        sk_take_any(h, " cpu procesador uso como va esta cuanto usando ");
    }
    static const char *const ALL[] = {"como va la pc",           "como va la compu",  "como va la computadora",
                                      "como esta la pc",         "como esta la compu", "como esta la computadora",
                                      "estado de la pc",         "estado de la compu", "estado de la computadora",
                                      "como anda la pc",         "como anda la compu", "como va la maquina"};
    for (size_t i = 0; i < sizeof ALL / sizeof *ALL && !all; i++) all = sk_take(h, ALL[i]) >= 0;
    if (!battery && !ram && !disk && !cpu && !all) return NULL;
    sk_take_any(h, " pc compu computadora laptop maquina equipo de la mi ");
    if (!sk_rest_is_filler(h)) return NULL;

    cJSON *args = cJSON_CreateObject();
    char *info = tool_info_sistema(args);
    cJSON_Delete(args);
    char *c = segment(info, "CPU al "), *m = segment(info, "RAM al "), *d = segment(info, "disco al ");
    char *bat = segment(info, "batería al ");
    char *parts[4];
    int n = 0, pct = 0;
    unsigned a = 0, b = 0;
    char state[32] = "";
    if ((all || cpu) && c && sscanf(c, "CPU al %d%%", &pct) == 1) parts[n++] = str_printf("el procesador va al %d%%", pct);
    if ((all || ram) && m && sscanf(m, "RAM al %d%% (%uGB de %uGB)", &pct, &a, &b) == 3)
        parts[n++] = str_printf("la RAM va al %d%%, %u de %u GB", pct, a, b);
    if ((all || disk) && d && sscanf(d, "disco al %d%% usado (%uGB libres)", &pct, &a) == 2)
        parts[n++] = str_printf("te quedan %u GB libres en el disco", a);
    if ((all || battery) && bat && sscanf(bat, "batería al %d%% (%31[^)])", &pct, state) == 2)
        parts[n++] = str_printf("tienes %d%% de batería%s", pct, strcmp(state, "cargando") ? ", sin cargador"
                                                                                            : " y está cargando");
    StrBuf sb;
    sb_init(&sb);
    for (int k = 0; k < n; k++) {
        sb_appendf(&sb, "%s%s", k == 0 ? "" : k == n - 1 ? " y " : ", ", parts[k]);
        free(parts[k]);
    }
    char *r;
    if (sb.len) {
        sb_append(&sb, ".");
        r = sb_steal(&sb);
        r[0] = (char)toupper((unsigned char)r[0]);
    } else {
        r = xstrdup(battery && !all ? "Esta PC no tiene batería, o no la pude leer." : "No pude leer cómo va la PC.");
    }
    sb_free(&sb);
    free(bat);
    free(c);
    free(m);
    free(d);
    free(info);
    return r;
}

/* -------------------------------------------------------------- plática --- */

static const char *const JOKES[] = {
    "¿Qué le dijo un techo a otro? Techo de menos.",
    "¿Por qué el libro de matemáticas está triste? Porque tiene muchos problemas.",
    "¿Qué hace una abeja en el gimnasio? ¡Zum-ba!",
    "¿Cuál es el café más peligroso del mundo? El ex-preso.",
    "¿Qué le dijo un pez a otro? Nada.",
    "¿Qué le dijo una iguana a su hermana gemela? Somos iguanitas.",
    "¿Qué le dijo el número 1 al 10? Para ser como yo, tienes que ser sincero.",
    "¿Cómo se despiden los químicos? Ácido un placer.",
    "¿Qué le dijo una impresora a otra? ¿Esa hoja es tuya o es impresión mía?",
    "¿Cuál es el colmo de un electricista? Que su esposa se llame Luz y sus hijos le sigan la corriente.",
    "¿Por qué la computadora fue al doctor? Porque tenía un virus.",
    "¿Qué le dijo la luna al sol? Tan grande y no te dejan salir de noche.",
    "¿Qué hace un perro con un taladro? Taladrando.",
    "¿Qué le dijo un semáforo a otro? No me veas, me estoy cambiando.",
    "¿Cuál es el animal más antiguo? La cebra, porque está en blanco y negro.",
    "¿Qué le dijo una uva verde a una morada? ¡Respira, respira!",
    "¿Qué le dijo el cero al ocho? Qué bonito cinturón.",
    "¿Por qué el tomate no toma café? Porque toma-té.",
    "¿Cuál es el colmo de un jardinero? Que lo dejen plantado.",
    "¿Qué le dijo una pared a otra? Nos vemos en la esquina.",
};

static const char *greeting_part(void)
{
    struct tm t = sk_local(sk_now());
    return t.tm_hour < 5 ? "Hola" : t.tm_hour < 12 ? "Buenos días" : t.tm_hour < 19 ? "Buenas tardes" : "Buenas noches";
}

static char *what_i_do(void)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "Sin gastar nada de IA puedo");
    static const char *const PARTS[][2] = {
        {"hora", " decirte la hora y la fecha"}, {"temporizador", " poner temporizadores"},
        {"alarma", " poner alarmas y recordatorios"}, {"calculadora", " hacer cuentas y conversiones"},
        {"clima", " decirte el clima"}, {"notas", " anotar tus pendientes"}, {"pc", " decirte cómo va la PC"},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof PARTS / sizeof *PARTS; i++)
        if (config_skill_enabled(PARTS[i][0])) sb_appendf(&sb, "%s%s", n++ ? "," : "", PARTS[i][1]);
    sb_append(&sb, n ? ", poner tu música y manejar tus ventanas y apps. " : " poner tu música y manejar tus "
                                                                          "ventanas y apps. ");
    sb_append(&sb, "Para lo demás, como buscar en internet, escribir o platicar de lo que sea, le pregunto a la IA.");
    return sb_steal(&sb);
}

char *sk_chat(Heard *h, bool *end)
{
    char *name = config_user_name(), *r = NULL;
    if (sk_take(h, "cuentame un chiste") >= 0 || sk_take(h, "dime un chiste") >= 0 ||
        sk_take(h, "otro chiste") >= 0 || sk_take(h, "echate un chiste") >= 0 || sk_take(h, "un chiste") >= 0) {
        sk_take_any(h, " otro chiste bueno cuentame ");
        if (sk_rest_is_filler(h)) r = xstrdup(sk_pick(JOKES, (int)(sizeof JOKES / sizeof *JOKES)));
    } else if (sk_take(h, "quien eres") >= 0 || sk_take(h, "como te llamas") >= 0 || sk_take(h, "que eres") >= 0) {
        sk_take_any(h, " tu ");
        if (sk_rest_is_filler(h))
            r = xstrdup("Soy Sokari, tu asistente de voz. Te ayudo con tu PC, tus pendientes y lo que me preguntes.");
    } else if (sk_take(h, "que puedes hacer") >= 0 || sk_take(h, "que sabes hacer") >= 0 ||
               sk_take(h, "en que me puedes ayudar") >= 0) {
        if (sk_rest_is_filler(h)) r = what_i_do();
    } else if (sk_take(h, "como estas") >= 0 || sk_take(h, "como andas") >= 0 || sk_take(h, "como te va") >= 0 ||
               sk_take(h, "como amaneciste") >= 0 || sk_take(h, "que tal estas") >= 0 ||
               sk_take(h, "como te sientes") >= 0) {
        sk_take_any(h, " tu hoy ");
        static const char *const FINE[] = {"¡Muy bien, gracias! ¿Y tú?", "¡Aquí, lista para ayudarte! ¿Y tú cómo estás?",
                                           "¡Bien! ¿Qué tal tú?"};
        if (sk_rest_is_filler(h)) r = xstrdup(sk_pick(FINE, 3));
    } else if (sk_take(h, "estas ahi") >= 0 || sk_take(h, "me escuchas") >= 0 || sk_take(h, "sigues ahi") >= 0 ||
               sk_take(h, "me oyes") >= 0) {
        if (sk_rest_is_filler(h)) r = xstrdup("Aquí estoy. ¿Qué necesitas?");
    } else if (sk_take(h, "te quiero") >= 0 || sk_take(h, "te amo") >= 0 || sk_take(h, "eres la mejor") >= 0 ||
               sk_take(h, "eres genial") >= 0 || sk_take(h, "que lista eres") >= 0 ||
               sk_take(h, "eres muy lista") >= 0) {
        sk_take_any(h, " mucho muy ");
        if (sk_rest_is_filler(h)) r = xstrdup("¡Gracias! Tú también me caes muy bien.");
    } else if (h->n <= 5 && (sk_take(h, "no muy bien") >= 0 || sk_take(h, "no tan bien") >= 0 ||
                             sk_take(h, "mal") >= 0 || sk_take(h, "masomenos") >= 0 || sk_take(h, "mas o menos") >= 0)) {
        sk_take_any(h, " muy pues la verdad ");
        if (sk_rest_is_filler(h)) r = xstrdup("Lo siento. Si puedo ayudarte con algo, aquí estoy.");
    } else if (h->n <= 5 && (sk_take(h, "todo bien") >= 0 || sk_take(h, "bien") >= 0)) {
        sk_take_any(h, " muy todo y tu gracias aqui bien ");
        if (sk_rest_is_filler(h) && !sk_negated(h)) r = xstrdup("¡Qué bueno! ¿En qué te ayudo?");
    } else {
        /* Solo un saludo: "hola", "buenos días", "qué onda, Sokari". */
        bool greet = false;
        static const char *const HI[] = {"buenos dias", "buenas tardes", "buenas noches", "que onda", "hola",
                                         "quiubo", "que tal", "buen dia", "saludos", "hey", "buenas", "buenos"};
        for (size_t i = 0; i < sizeof HI / sizeof *HI; i++) greet |= sk_take(h, HI[i]) >= 0;
        if (greet && sk_rest_is_filler(h)) {
            if (*name) r = str_printf("¡%s, %s! ¿En qué te ayudo?", greeting_part(), name);
            else r = str_printf("¡%s! ¿En qué te ayudo?", greeting_part());
        }
    }
    free(name);
    return r;
}

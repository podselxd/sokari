/* Skills locales de números: cuentas dichas en voz ("¿cuánto es 25 por 4?",
   "raíz cuadrada de 144", "15 por ciento de 300") con la calculadora de
   siempre, y conversiones de unidades ("¿cuántas libras son 70 kilos?"). */
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skills_internal.h"
#include "tools.h"
#include "util.h"

/* -------------------------------------------------------- conversiones --- */

typedef struct {
    const char *words; /* " kilometro kilometros km " */
    char dim;          /* 'l' largo, 'm' masa, 'v' volumen, 't' temperatura */
    double factor;     /* a la unidad base (metro, kilo, litro) */
    const char *one, *many;
} Unit;

static const Unit UNITS[] = {
    {" kilometro kilometros km kms ", 'l', 1000, "kilómetro", "kilómetros"},
    {" metro metros mts ", 'l', 1, "metro", "metros"},
    {" centimetro centimetros cm cms ", 'l', 0.01, "centímetro", "centímetros"},
    {" milimetro milimetros mm ", 'l', 0.001, "milímetro", "milímetros"},
    {" milla millas ", 'l', 1609.344, "milla", "millas"},
    {" pie pies ", 'l', 0.3048, "pie", "pies"},
    {" pulgada pulgadas ", 'l', 0.0254, "pulgada", "pulgadas"},
    {" yarda yardas ", 'l', 0.9144, "yarda", "yardas"},
    {" kilo kilos kilogramo kilogramos kg kgs ", 'm', 1, "kilo", "kilos"},
    {" gramo gramos gr grs ", 'm', 0.001, "gramo", "gramos"},
    {" libra libras lb lbs ", 'm', 0.45359237, "libra", "libras"},
    {" onza onzas oz ", 'm', 0.028349523125, "onza", "onzas"},
    {" tonelada toneladas ", 'm', 1000, "tonelada", "toneladas"},
    {" litro litros lt lts ", 'v', 1, "litro", "litros"},
    {" mililitro mililitros ml ", 'v', 0.001, "mililitro", "mililitros"},
    {" galon galones ", 'v', 3.785411784, "galón", "galones"},
    {" celsius centigrados centigrado ", 't', 0, "grado centígrado", "grados centígrados"},
    {" fahrenheit farenheit faren ", 't', 1, "grado Fahrenheit", "grados Fahrenheit"},
    {" kelvin kelvins ", 't', 2, "kelvin", "kelvins"},
};
#define NUNITS ((int)(sizeof UNITS / sizeof *UNITS))

static int unit_at(const Heard *h, int i)
{
    for (int u = 0; u < NUNITS; u++)
        if (sk_is(h, i, UNITS[u].words)) return u;
    /* "grados" solo: centígrados (salvo que diga "grados Fahrenheit"). */
    if (sk_is(h, i, " grados grado ")) {
        int next = i + 1 < h->n ? unit_at(h, i + 1) : -1;
        return next >= 0 && UNITS[next].dim == 't' ? -2 : 16;
    }
    return -1;
}

static double to_base(int u, double v)
{
    if (UNITS[u].dim != 't') return v * UNITS[u].factor;
    return UNITS[u].factor == 0 ? v : UNITS[u].factor == 1 ? (v - 32) * 5 / 9 : v - 273.15;
}

static double from_base(int u, double v)
{
    if (UNITS[u].dim != 't') return v / UNITS[u].factor;
    return UNITS[u].factor == 0 ? v : UNITS[u].factor == 1 ? v * 9 / 5 + 32 : v + 273.15;
}

static char *convert(Heard *h)
{
    int src = -1, dst = -1, srcpos = -1;
    double value = 0;
    /* La de origen va después del número; la otra es a la que se convierte. */
    for (int i = 0; i < h->n && src < 0; i++) {
        double v;
        int k = sk_number(h, i, &v);
        if (!k) continue;
        int j = i + k, u = j < h->n ? unit_at(h, j) : -1;
        if (u == -2) u = unit_at(h, ++j);
        if (u < 0) continue;
        src = u, srcpos = j, value = v;
        for (int x = i; x <= j; x++) h->used[x] = true;
        if (i > 0 && !strcmp(h->w[i - 1], "-")) value = -value, h->used[i - 1] = true;
    }
    if (src < 0) return NULL;
    for (int i = 0; i < h->n && dst < 0; i++) {
        if (i == srcpos || h->used[i]) continue;
        int u = unit_at(h, i);
        if (u == -2) {
            h->used[i] = true;
            u = unit_at(h, i + 1), i++;
        }
        if (u >= 0 && u != src) {
            dst = u;
            h->used[i] = true;
        }
    }
    if (dst < 0 || UNITS[dst].dim != UNITS[src].dim) return NULL;
    sk_take_any(h, " cuantos cuantas cuanto son es a en convierte convierteme convertir pasa pasame pasar de hay "
                   "equivale equivalen tiene serian seria da grados grado ");
    if (!sk_rest_is_filler(h)) return NULL;
    double out = from_base(dst, to_base(src, value));
    char *a = sk_say_number(value, 2), *b = sk_say_number(out, 2);
    char *r = str_printf("%s %s %s %s %s.", a, value == 1 ? UNITS[src].one : UNITS[src].many,
                         value == 1 ? "es" : "son", b, fabs(out - 1) < 1e-9 ? UNITS[dst].one : UNITS[dst].many);
    free(a);
    free(b);
    return r;
}

/* -------------------------------------------------------------- cuentas --- */

typedef struct {
    StrBuf expr, said;
    int numbers, ops;
} Math;

static void emit(Math *m, const char *expr, const char *said)
{
    sb_append(&m->expr, expr);
    if (said && *said) sb_appendf(&m->said, "%s%s", m->said.len ? " " : "", said);
}

/* Las palabras desde i como una cuenta: "25 por 4", "raíz de 144". */
static bool read_math(Heard *h, int i, Math *m)
{
    int open = 0;
    while (i < h->n) {
        if (h->used[i]) {
            i++;
            continue;
        }
        double v;
        int k = sk_number(h, i, &v);
        const char *w = h->w[i];
        if (!k && !m->numbers && !m->ops && sk_is(h, i, " el la los las que de ")) {
            h->used[i++] = true; /* "cuánto es el 15 por ciento…" */
            continue;
        }
        if (k) {
            char *num = sk_say_number(v, 10);
            emit(m, num, num);
            free(num);
            if (open) {
                sb_append(&m->expr, ")"); /* la raíz es de este número */
                open--;
            }
            m->numbers++;
            for (int x = i; x < i + k; x++) h->used[x] = true;
            i += k;
            /* "15 por ciento de 300", "15 % de 300" */
            if (i + 1 < h->n && ((!strcmp(h->w[i], "por") && !strcmp(h->w[i + 1], "ciento")) ||
                                 !strcmp(h->w[i], "%") || !strcmp(h->w[i], "porciento"))) {
                int skip = !strcmp(h->w[i], "por") ? 2 : 1;
                bool of = i + skip < h->n && !strcmp(h->w[i + skip], "de");
                emit(m, of ? "/100*" : "/100", of ? "por ciento de" : "por ciento");
                for (int x = i; x < i + skip + of; x++) h->used[x] = true;
                i += skip + of;
                m->ops++;
            } else if (i < h->n && (!strcmp(h->w[i], "%") || !strcmp(h->w[i], "porciento"))) {
                emit(m, "/100", "por ciento");
                h->used[i++] = true;
                m->ops++;
            }
            continue;
        }
        const char *op = NULL, *said = NULL;
        int used = 1;
        if (sk_is(h, i, " mas + ")) op = "+", said = "más";
        else if (sk_is(h, i, " menos - ")) op = "-", said = "menos";
        else if (sk_is(h, i, " por x * veces ")) op = "*", said = "por";
        else if (!strcmp(w, "multiplicado")) op = "*", said = "por", used = sk_is(h, i + 1, " por ") ? 2 : 1;
        else if (sk_is(h, i, " entre / sobre ")) op = "/", said = "entre";
        else if (!strcmp(w, "dividido")) op = "/", said = "entre", used = sk_is(h, i + 1, " entre por ") ? 2 : 1;
        else if (!strcmp(w, "elevado")) {
            op = "**", said = "elevado a";
            used = sk_is(h, i + 1, " a ") ? (sk_is(h, i + 2, " la ") ? 3 : 2) : 1;
        } else if (sk_is(h, i, " ^ ")) op = "**", said = "elevado a";
        else if (!strcmp(w, "a") && sk_is(h, i + 1, " la ") && m->numbers) op = "**", said = "a la", used = 2;
        else if (!strcmp(w, "al") && sk_is(h, i + 1, " cuadrado cubo ")) {
            op = sk_is(h, i + 1, " cuadrado ") ? "**2" : "**3";
            said = sk_is(h, i + 1, " cuadrado ") ? "al cuadrado" : "al cubo";
            used = 2;
        } else if (!strcmp(w, "raiz")) {
            used = 1;
            if (sk_is(h, i + 1, " cuadrada ")) used++;
            if (sk_is(h, i + used, " de ")) used++;
            op = "sqrt(", said = "la raíz cuadrada de";
            open++;
        }
        if (!op) break;
        emit(m, op, said);
        m->ops++;
        for (int x = i; x < i + used; x++) h->used[x] = true;
        i += used;
    }
    while (open-- > 0) sb_append(&m->expr, ")");
    return m->numbers >= 1 && m->ops >= 1;
}

char *sk_calc(Heard *h, bool *end)
{
    if (sk_negated(h) || !sk_has_number(h)) return NULL;
    bool before[SK_MAX];
    memcpy(before, h->used, sizeof before);
    char *r = convert(h);
    if (r) return r;
    memcpy(h->used, before, sizeof before);

    static const char *const ASK[] = {"cuanto es",  "cuanto son",  "cuanto da", "cuanto seria", "cuanto sale",
                                      "calcula",    "calculame",   "calcular",  "resuelve",     "cuanto dan",
                                      "cual es",    "dime cuanto es"};
    int at = -1;
    for (size_t i = 0; i < sizeof ASK / sizeof *ASK && at < 0; i++) at = sk_take(h, ASK[i]);
    /* Sin "cuánto es", la frase tiene que empezar con la cuenta ("25 por 4"). */
    int start = 0;
    if (at < 0) {
        double v;
        int first = 0;
        while (first < h->n && !sk_number(h, first, &v) && strcmp(h->w[first], "raiz")) first++;
        for (int i = 0; i < first; i++)
            if (!sk_is(h, i, " oye sokari y ")) return NULL;
        start = first;
    }
    Math m = {0};
    sb_init(&m.expr);
    sb_init(&m.said);
    bool ok = read_math(h, start, &m);
    sk_take_any(h, " la el que resultado ");
    if (!ok || !sk_rest_is_filler(h)) {
        sb_free(&m.expr);
        sb_free(&m.said);
        return NULL;
    }
    double v;
    if (!calc_evaluate(m.expr.data, &v)) {
        r = xstrdup("No pude hacer esa cuenta. ¿Me la repites?");
    } else {
        char *num = sk_say_number(v, 4);
        r = str_printf("%s da %s.", m.said.data, num);
        free(num);
        if (r[0] == 'l') r[0] = 'L';
    }
    sb_free(&m.expr);
    sb_free(&m.said);
    return r;
}

/* Calculadora en sandbox: un parser recursivo propio (sin evaluar código),
   con la misma lista blanca de operaciones y funciones que la versión en
   Python y el mismo tope de exponente — 9**9**9 no puede colgar a Sokari. */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools.h"
#include "util.h"

#define MAX_EXPONENT 1000
#define MAX_LIST 64
#define MAX_DEPTH 48

typedef struct {
    bool is_list;
    double num;
    int n;
    double items[MAX_LIST];
} Value;

typedef struct {
    const char *p;
    jmp_buf fail;
    char error[160];
    int depth;
} Parser;

static void fail(Parser *ps, const char *msg)
{
    snprintf(ps->error, sizeof ps->error, "%s", msg);
    longjmp(ps->fail, 1);
}

static void skip_ws(Parser *ps)
{
    while (isspace((unsigned char)*ps->p)) ps->p++;
}

static bool accept(Parser *ps, const char *tok)
{
    skip_ws(ps);
    size_t n = strlen(tok);
    if (strncmp(ps->p, tok, n)) return false;
    if (n == 1 && (tok[0] == '*' || tok[0] == '/') && ps->p[1] == tok[0]) return false;
    ps->p += n;
    return true;
}

static double num_of(Parser *ps, Value *v)
{
    if (v->is_list) fail(ps, "no se puede operar con una lista así");
    return v->num;
}

static Value number(double x)
{
    Value v;
    v.is_list = false;
    v.num = x;
    v.n = 0;
    return v;
}

static double checked(Parser *ps, double r)
{
    if (isnan(r)) fail(ps, "el resultado no está definido (por ejemplo raíz de un negativo)");
    if (isinf(r)) fail(ps, "el resultado es demasiado grande");
    return r;
}

static double safe_pow(Parser *ps, double base, double exp)
{
    if (fabs(exp) > MAX_EXPONENT) fail(ps, "el exponente es demasiado grande (máximo 1000)");
    if (base == 0 && exp < 0) fail(ps, "división entre cero");
    return checked(ps, pow(base, exp));
}

static Value expr(Parser *ps);
static Value unary(Parser *ps);

static double py_round(double x, int nd)
{
    double m = pow(10.0, nd);
    double y = x * m;
    double r = nearbyint(y); /* redondeo a par, como round() de Python */
    return r / m;
}

static Value call(Parser *ps, const char *name, Value *args, int nargs)
{
    double a0 = 0;
    if (nargs >= 1 && !args[0].is_list) a0 = args[0].num;
#define NEED(k)                                                                                  \
    if (nargs != (k)) fail(ps, "cantidad de argumentos incorrecta para esa función");
    if (!strcmp(name, "sqrt")) { NEED(1); if (a0 < 0) fail(ps, "raíz de un número negativo"); return number(sqrt(num_of(ps, &args[0]))); }
    if (!strcmp(name, "sin")) { NEED(1); return number(sin(num_of(ps, &args[0]))); }
    if (!strcmp(name, "cos")) { NEED(1); return number(cos(num_of(ps, &args[0]))); }
    if (!strcmp(name, "tan")) { NEED(1); return number(checked(ps, tan(num_of(ps, &args[0])))); }
    if (!strcmp(name, "exp")) { NEED(1); return number(checked(ps, exp(num_of(ps, &args[0])))); }
    if (!strcmp(name, "abs")) { NEED(1); return number(fabs(num_of(ps, &args[0]))); }
    if (!strcmp(name, "log10")) { NEED(1); if (a0 <= 0) fail(ps, "logaritmo de un número no positivo"); return number(log10(a0)); }
    if (!strcmp(name, "log")) {
        if (nargs < 1 || nargs > 2) fail(ps, "cantidad de argumentos incorrecta para log");
        double x = num_of(ps, &args[0]);
        if (x <= 0) fail(ps, "logaritmo de un número no positivo");
        if (nargs == 1) return number(log(x));
        double b = num_of(ps, &args[1]);
        if (b <= 0 || b == 1) fail(ps, "base de logaritmo inválida");
        return number(log(x) / log(b));
    }
    if (!strcmp(name, "round")) {
        if (nargs < 1 || nargs > 2) fail(ps, "cantidad de argumentos incorrecta para round");
        int nd = nargs == 2 ? (int)num_of(ps, &args[1]) : 0;
        if (nd > 15) nd = 15;
        return number(py_round(num_of(ps, &args[0]), nd));
    }
    if (!strcmp(name, "pow")) { NEED(2); return number(safe_pow(ps, num_of(ps, &args[0]), num_of(ps, &args[1]))); }
    if (!strcmp(name, "min") || !strcmp(name, "max") || !strcmp(name, "sum") || !strcmp(name, "len")) {
        double vals[MAX_LIST];
        int n = 0;
        if (nargs == 1 && args[0].is_list) {
            memcpy(vals, args[0].items, sizeof(double) * (size_t)args[0].n);
            n = args[0].n;
        } else {
            if (!strcmp(name, "sum") || !strcmp(name, "len")) fail(ps, "sum y len necesitan una lista, ej: sum([1, 2, 3])");
            for (int i = 0; i < nargs; i++) vals[n++] = num_of(ps, &args[i]);
        }
        if (!strcmp(name, "len")) return number(n);
        if (!strcmp(name, "sum")) {
            double s = 0;
            for (int i = 0; i < n; i++) s += vals[i];
            return number(checked(ps, s));
        }
        if (!n) fail(ps, "min/max de una lista vacía");
        double r = vals[0];
        for (int i = 1; i < n; i++) r = !strcmp(name, "min") ? fmin(r, vals[i]) : fmax(r, vals[i]);
        return number(r);
    }
#undef NEED
    fail(ps, "esa expresión tiene algo que no sé calcular");
    return number(0);
}

static Value atom(Parser *ps)
{
    skip_ws(ps);
    if (++ps->depth > MAX_DEPTH) fail(ps, "la expresión está demasiado anidada");
    Value v;
    if (accept(ps, "(")) {
        v = expr(ps);
        if (!accept(ps, ")")) fail(ps, "falta cerrar un paréntesis");
    } else if (accept(ps, "[")) {
        v.is_list = true;
        v.n = 0;
        v.num = 0;
        if (!accept(ps, "]")) {
            do {
                Value e = expr(ps);
                if (v.n >= MAX_LIST) fail(ps, "la lista es demasiado larga");
                v.items[v.n++] = num_of(ps, &e);
            } while (accept(ps, ","));
            if (!accept(ps, "]")) fail(ps, "falta cerrar un corchete");
        }
    } else if (isdigit((unsigned char)*ps->p) || (*ps->p == '.' && isdigit((unsigned char)ps->p[1]))) {
        char buf[64];
        size_t n = 0;
        /* Un número que no cabe en buf es un error, no se corta en silencio:
           antes, 63 dígitos seguidos de "e-5" escribían fuera de buf. */
        while (isdigit((unsigned char)*ps->p) || *ps->p == '.' || *ps->p == '_') {
            if (n >= sizeof buf - 1) fail(ps, "ese número es demasiado largo");
            if (*ps->p != '_') buf[n++] = *ps->p;
            ps->p++;
        }
        if ((*ps->p == 'e' || *ps->p == 'E') &&
            (isdigit((unsigned char)ps->p[1]) || ((ps->p[1] == '-' || ps->p[1] == '+') && isdigit((unsigned char)ps->p[2])))) {
            if (n >= sizeof buf - 3) fail(ps, "ese número es demasiado largo");
            buf[n++] = *ps->p++;
            if (*ps->p == '-' || *ps->p == '+') buf[n++] = *ps->p++;
            while (isdigit((unsigned char)*ps->p)) {
                if (n >= sizeof buf - 1) fail(ps, "ese número es demasiado largo");
                buf[n++] = *ps->p++;
            }
        }
        buf[n] = 0;
        char *end;
        double x = strtod(buf, &end);
        if (*end) fail(ps, "número mal escrito");
        v = number(x);
    } else if (isalpha((unsigned char)*ps->p)) {
        char name[16];
        size_t n = 0;
        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_') && n < sizeof name - 1) name[n++] = *ps->p++;
        name[n] = 0;
        if (accept(ps, "(")) {
            Value args[8];
            int nargs = 0;
            if (!accept(ps, ")")) {
                do {
                    if (nargs >= 8) fail(ps, "demasiados argumentos");
                    args[nargs++] = expr(ps);
                } while (accept(ps, ","));
                if (!accept(ps, ")")) fail(ps, "falta cerrar un paréntesis");
            }
            v = call(ps, name, args, nargs);
        } else if (!strcmp(name, "pi")) {
            v = number(M_PI);
        } else if (!strcmp(name, "e")) {
            v = number(M_E);
        } else {
            fail(ps, "esa expresión tiene algo que no sé calcular");
        }
    } else {
        fail(ps, "esa expresión tiene algo que no sé calcular");
    }
    ps->depth--;
    return v;
}

static Value power(Parser *ps)
{
    Value base = atom(ps);
    if (accept(ps, "**") || accept(ps, "^")) {
        /* 2**2**2... se encadena por recursión: cuenta para el mismo tope que
           los paréntesis, si no una cadena de unos cientos agota la pila. */
        if (++ps->depth > MAX_DEPTH) fail(ps, "la expresión está demasiado anidada");
        Value e = unary(ps);
        ps->depth--;
        return number(safe_pow(ps, num_of(ps, &base), num_of(ps, &e)));
    }
    return base;
}

/* Los signos seguidos ("--5") se cuentan en un bucle y no por recursión:
   unos cientos de "-" agotaban la pila del hilo y cerraban Sokari. */
static Value unary(Parser *ps)
{
    bool neg = false;
    for (;;) {
        if (accept(ps, "-")) neg = !neg;
        else if (!accept(ps, "+")) break;
    }
    Value v = power(ps);
    return neg ? number(-num_of(ps, &v)) : v;
}

static Value term(Parser *ps)
{
    Value v = unary(ps);
    for (;;) {
        if (accept(ps, "*")) {
            Value r = unary(ps);
            v = number(checked(ps, num_of(ps, &v) * num_of(ps, &r)));
        } else if (accept(ps, "//")) {
            Value r = unary(ps);
            double d = num_of(ps, &r);
            if (d == 0) fail(ps, "división entre cero");
            v = number(floor(num_of(ps, &v) / d));
        } else if (accept(ps, "/")) {
            Value r = unary(ps);
            double d = num_of(ps, &r);
            if (d == 0) fail(ps, "división entre cero");
            v = number(checked(ps, num_of(ps, &v) / d));
        } else if (accept(ps, "%")) {
            Value r = unary(ps);
            double d = num_of(ps, &r);
            if (d == 0) fail(ps, "división entre cero");
            double m = fmod(num_of(ps, &v), d);
            if (m != 0 && ((m < 0) != (d < 0))) m += d; /* signo del divisor, como en Python */
            v = number(m);
        } else {
            return v;
        }
    }
}

static Value expr(Parser *ps)
{
    Value v = term(ps);
    for (;;) {
        if (accept(ps, "+")) {
            Value r = term(ps);
            v = number(checked(ps, num_of(ps, &v) + num_of(ps, &r)));
        } else if (accept(ps, "-")) {
            Value r = term(ps);
            v = number(checked(ps, num_of(ps, &v) - num_of(ps, &r)));
        } else {
            return v;
        }
    }
}

static char *format_number(double x)
{
    if (x == 0) return xstrdup("0");
    if (fabs(x) < 1e15 && x == floor(x)) return str_printf("%.0f", x);
    for (int prec = 1; prec <= 17; prec++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*g", prec, x);
        if (strtod(buf, NULL) == x) return xstrdup(buf);
    }
    return str_printf("%.17g", x);
}

bool calc_evaluate(const char *expression, double *out)
{
    Parser *ps = xcalloc(1, sizeof *ps);
    ps->p = expression ? expression : "";
    bool ok = false;
    if (setjmp(ps->fail) == 0) {
        Value v = expr(ps);
        skip_ws(ps);
        ok = !*ps->p && !v.is_list && isfinite(v.num);
        if (ok) *out = v.num;
    }
    free(ps);
    return ok;
}

char *calc_format(double x)
{
    return format_number(x);
}

char *tool_calcular(const cJSON *a)
{
    char *expresion = str_trim(arg_str(a, "expresion"));
    if (!*expresion) {
        free(expresion);
        return xstrdup("No me diste ninguna expresión para calcular.");
    }
    Parser *ps = xcalloc(1, sizeof *ps);
    ps->p = expresion;
    char *r;
    if (setjmp(ps->fail) == 0) {
        Value v = expr(ps);
        skip_ws(ps);
        if (*ps->p) fail(ps, "esa expresión tiene algo que no sé calcular");
        if (v.is_list) fail(ps, "el resultado es una lista, no un número");
        char *num = format_number(v.num);
        r = str_printf("El resultado es %s.", num);
        free(num);
    } else {
        r = str_printf("No pude calcular '%s': %s", expresion, ps->error);
    }
    free(ps);
    free(expresion);
    return r;
}

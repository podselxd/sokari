/* Skill local del clima, con Open-Meteo (gratis y sin key; no gasta nada de
   IA): "¿cómo está el clima?", "¿va a llover mañana?", "clima en Monterrey".
   Tu ciudad se dice una vez ("mi ciudad es Chihuahua") y se guarda en la
   configuración. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "http.h"
#include "log.h"
#include "skills_internal.h"
#include "third_party/cJSON.h"
#include "util.h"

static char *g_geo_url, *g_forecast_url;

void skills_set_weather_urls(const char *geocoding, const char *forecast)
{
    free(g_geo_url);
    free(g_forecast_url);
    g_geo_url = geocoding ? xstrdup(geocoding) : NULL;
    g_forecast_url = forecast ? xstrdup(forecast) : NULL;
}

const char *sk_geo_url(void)
{
    return g_geo_url ? g_geo_url : "https://geocoding-api.open-meteo.com/v1/search";
}

const char *sk_forecast_url(void)
{
    return g_forecast_url ? g_forecast_url : "https://api.open-meteo.com/v1/forecast";
}

/* Códigos del tiempo de la OMM (los que usa Open-Meteo). */
static const char *sky(int code)
{
    switch (code) {
    case 0: return "despejado";
    case 1: return "casi despejado";
    case 2: return "parcialmente nublado";
    case 3: return "nublado";
    case 45: case 48: return "con niebla";
    case 51: case 53: case 55: return "con llovizna";
    case 56: case 57: return "con llovizna helada";
    case 61: return "con lluvia ligera";
    case 63: return "lloviendo";
    case 65: return "con lluvia fuerte";
    case 66: case 67: return "con lluvia helada";
    case 71: return "con nevada ligera";
    case 73: case 77: return "nevando";
    case 75: return "con nevada fuerte";
    case 80: return "con chubascos ligeros";
    case 81: return "con chubascos";
    case 82: return "con chubascos fuertes";
    case 85: case 86: return "con chubascos de nieve";
    case 95: return "con tormenta";
    case 96: case 99: return "con tormenta y granizo";
    default: return "";
    }
}

static cJSON *get_json(const char *url)
{
    HttpRequest req = {.method = "GET", .url = url, .timeout_ms = 8000};
    HttpResponse r = http_request(&req);
    cJSON *j = r.status == 200 && r.body ? cJSON_Parse(r.body) : NULL;
    if (!j) log_msg("Clima: %s respondió %d%s%s", url, r.status, r.error ? ": " : "", r.error ? r.error : "");
    http_response_free(&r);
    return j;
}

/* Dónde queda la ciudad (y cómo se llama bien escrita). */
static bool geocode(const char *city, double *lat, double *lon, char **name)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static char *cached_q, *cached_name;
    static double cached_lat, cached_lon;
    AcquireSRWLockExclusive(&lock);
    bool hit = cached_q && !strcmp(cached_q, city);
    if (hit) *lat = cached_lat, *lon = cached_lon, *name = xstrdup(cached_name);
    ReleaseSRWLockExclusive(&lock);
    if (hit) return true;
    char *q = url_encode(city);
    char *url = str_printf("%s?name=%s&count=1&language=es&format=json", sk_geo_url(), q);
    free(q);
    cJSON *j = get_json(url);
    free(url);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "results"), 0);
    const cJSON *la = cJSON_GetObjectItem(first, "latitude"), *lo = cJSON_GetObjectItem(first, "longitude");
    const cJSON *nm = cJSON_GetObjectItem(first, "name");
    bool ok = cJSON_IsNumber(la) && cJSON_IsNumber(lo);
    if (ok) {
        *lat = la->valuedouble, *lon = lo->valuedouble;
        *name = xstrdup(cJSON_IsString(nm) ? nm->valuestring : city);
        AcquireSRWLockExclusive(&lock);
        free(cached_q);
        free(cached_name);
        cached_q = xstrdup(city), cached_name = xstrdup(*name), cached_lat = *lat, cached_lon = *lon;
        ReleaseSRWLockExclusive(&lock);
    }
    cJSON_Delete(j);
    return ok;
}

static double num_at(const cJSON *daily, const char *key, int day)
{
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, key), day);
    return cJSON_IsNumber(v) ? v->valuedouble : -999;
}

typedef enum { ASK_GENERAL, ASK_RAIN, ASK_TEMP } Ask;

static char *forecast(const char *city, int day, Ask ask)
{
    double lat, lon;
    char *name = NULL;
    if (!geocode(city, &lat, &lon, &name)) return str_printf("No pude encontrar %s, o no hay internet ahorita.", city);
    char *url = str_printf("%s?latitude=%.4f&longitude=%.4f&current=temperature_2m,apparent_temperature,weather_code"
                           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                           "&timezone=auto&forecast_days=3",
                           sk_forecast_url(), lat, lon);
    cJSON *j = get_json(url);
    free(url);
    const cJSON *cur = cJSON_GetObjectItem(j, "current"), *daily = cJSON_GetObjectItem(j, "daily");
    const cJSON *temp = cJSON_GetObjectItem(cur, "temperature_2m"), *code = cJSON_GetObjectItem(cur, "weather_code");
    double hi = num_at(daily, "temperature_2m_max", day), lo = num_at(daily, "temperature_2m_min", day);
    double rain = num_at(daily, "precipitation_probability_max", day), dcode = num_at(daily, "weather_code", day);
    char *r;
    if (!j || hi < -900) {
        r = xstrdup("No pude consultar el clima ahorita. Intenta en un rato.");
    } else {
        const char *when = day == 0 ? "Hoy" : day == 1 ? "Mañana" : "Pasado mañana";
        char *rain_s = rain >= 0 ? str_printf(", con %.0f%% de probabilidad de lluvia", rain) : xstrdup("");
        if (ask == ASK_RAIN) {
            const char *verdict = rain >= 60 ? "sí va a llover, mejor lleva paraguas"
                                : rain >= 30 ? "puede que llueva" : "no parece que vaya a llover";
            r = str_printf("%s en %s hay %.0f%% de probabilidad de lluvia: %s.", when, name, rain < 0 ? 0 : rain,
                           verdict);
        } else if (day == 0 && cJSON_IsNumber(temp)) {
            const char *s = cJSON_IsNumber(code) ? sky(code->valueint) : "";
            r = str_printf("En %s hay %.0f grados%s%s. Hoy: máxima de %.0f y mínima de %.0f%s.", name, temp->valuedouble,
                           *s ? " y está " : "", s, hi, lo, rain_s);
        } else {
            const char *s = dcode > -900 ? sky((int)dcode) : "";
            r = str_printf("%s en %s: %s%smáxima de %.0f y mínima de %.0f%s.", when, name, s, *s ? ", " : "", hi, lo,
                           rain_s);
        }
        free(rain_s);
    }
    cJSON_Delete(j);
    free(name);
    return r;
}

char *sk_weather(Heard *h, bool *end)
{
    if (sk_negated(h)) return NULL;
    /* "Mi ciudad es Chihuahua", "vivo en Ciudad Juárez" */
    int at = sk_take(h, "mi ciudad es");
    if (at < 0) at = sk_take(h, "vivo en");
    if (at >= 0) {
        int a = -1, b = -1;
        for (int i = 0; i < h->n; i++)
            if (!h->used[i]) {
                if (a < 0) a = i;
                b = i;
            }
        if (a < 0 || b - a > 4) return NULL;
        char *city = sk_orig_span(h, a, b + 1);
        config_set_city(city);
        char *r = str_printf("Listo, tu ciudad es %s. Ya te puedo decir el clima.", city);
        free(city);
        return r;
    }

    Ask ask = ASK_GENERAL;
    int k;
    if (sk_take(h, "clima") >= 0 || sk_take(h, "pronostico") >= 0 || sk_take(h, "pronostico del tiempo") >= 0) {
        ask = ASK_GENERAL;
    } else if (sk_find(h, "cuanto tiempo") < 0 && sk_find(h, "a tiempo") < 0 &&
               (sk_take(h, "que tiempo hace") >= 0 || sk_take(h, "el tiempo") >= 0)) {
        ask = ASK_GENERAL;
    } else if ((k = sk_take(h, "temperatura")) >= 0 || sk_take(h, "cuantos grados hace") >= 0 ||
               sk_take(h, "hace frio") >= 0 || sk_take(h, "hace calor") >= 0 || sk_take(h, "va a hacer frio") >= 0 ||
               sk_take(h, "va a hacer calor") >= 0) {
        ask = ASK_TEMP;
    } else if (sk_take(h, "va a llover") >= 0 || sk_take(h, "llovera") >= 0 ||
               sk_take(h, "llueve") >= 0 || sk_take(h, "hay lluvia") >= 0 || sk_take(h, "va a haber lluvia") >= 0 ||
               sk_take(h, "probabilidad de lluvia") >= 0) {
        ask = ASK_RAIN;
    } else {
        return NULL;
    }
    int day = 0;
    if (sk_take(h, "pasado manana") >= 0) day = 2;
    else if (sk_take(h, "manana") >= 0) day = 1;
    sk_take_any(h, " hoy ahorita como esta estara va a hacer hace que hay afuera ahi el la del de para dia como "
                   "sera cual es cuanta ");
    /* "clima en Monterrey": lo que queda es la ciudad. */
    int a = -1, b = -1;
    for (int i = 0; i < h->n; i++)
        if (!h->used[i] && !sk_is(h, i, " en por favor porfa oye sokari ")) {
            if (a < 0) a = i;
            b = i;
        }
    char *city;
    if (a >= 0) {
        /* Una ciudad va con mayúscula: "Monterrey", no "ebullición del agua". */
        const unsigned char *o = (const unsigned char *)h->orig[a];
        bool capital = (o[0] >= 'A' && o[0] <= 'Z') || (o[0] == 0xC3 && o[1] >= 0x80 && o[1] <= 0x9D);
        if (b - a > 3 || a == 0 || !sk_is(h, a - 1, " en de ") || !capital) return NULL;
        for (int i = a - 1; i <= b; i++) h->used[i] = true;
        city = sk_orig_span(h, a, b + 1);
    } else {
        city = config_city();
    }
    if (!sk_rest_is_filler(h)) {
        free(city);
        return NULL;
    }
    if (!*city) {
        free(city);
        return xstrdup("¿De qué ciudad? Dime «mi ciudad es…» una vez y la recuerdo.");
    }
    char *r = forecast(city, day, ask);
    free(city);
    return r;
}

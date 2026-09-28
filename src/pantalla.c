#include "pantalla.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "groq.h"
#include "log.h"
#include "tools.h"
#include "util.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "third_party/stb_image_write.h"
#pragma GCC diagnostic pop

#define MAX_WIDTH 1280

static void append(void *ctx, void *data, int size)
{
    sb_append_n((StrBuf *)ctx, (const char *)data, (size_t)size);
}

static char *base64(const unsigned char *p, size_t n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = xmalloc((n + 2) / 3 * 4 + 1), *q = o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *q++ = T[v >> 18 & 63];
        *q++ = T[v >> 12 & 63];
        *q++ = i + 1 < n ? T[v >> 6 & 63] : '=';
        *q++ = i + 2 < n ? T[v & 63] : '=';
    }
    *q = 0;
    return o;
}

char *screen_jpeg_base64(const uint8_t *rgb, int w, int h, int *out_w, int *out_h)
{
    /* Más chica (promediando cuadritos): la IA la lee igual y gasta menos. */
    int f = (w + MAX_WIDTH - 1) / MAX_WIDTH;
    if (f < 1) f = 1;
    int W = w / f, H = h / f;
    uint8_t *small = xmalloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) {
                unsigned s = 0;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) s += rgb[((size_t)(y * f + j) * w + (x * f + i)) * 3 + c];
                small[((size_t)y * W + x) * 3 + c] = (uint8_t)(s / (unsigned)(f * f));
            }
    StrBuf sb;
    sb_init(&sb);
    int ok = stbi_write_jpg_to_func(append, &sb, W, H, 3, small, 70);
    free(small);
    char *b = ok ? base64((const unsigned char *)sb.data, sb.len) : NULL;
    sb_free(&sb);
    if (out_w) *out_w = W;
    if (out_h) *out_h = H;
    return b;
}

char *tool_ver_pantalla(const cJSON *a)
{
    if (!config_screen_view())
        return xstrdup("El usuario apagó «Puede ver mi pantalla» en Configuración: no puedes ver su pantalla. "
                       "Díselo y que la prenda si quiere.");
    const char *q = arg_str(a, "pregunta");
    app_subtitle(false, "(Viendo tu pantalla…)");
    int w = 0, h = 0;
    char *why = NULL;
    uint8_t *rgb = screen_grab(&w, &h, &why);
    if (!rgb) {
        char *r = str_printf("No pude tomar la captura: %s", why ? why : "no sé por qué");
        free(why);
        return r;
    }
    int W = 0, H = 0;
    char *b64 = screen_jpeg_base64(rgb, w, h, &W, &H);
    free(rgb);
    if (!b64) return xstrdup("No pude preparar la captura.");
    char *prompt = str_printf("Esta es una captura de la pantalla del usuario. %s Contesta en español, corto y "
                              "concreto (lo que se vería leyendo en voz alta). Si ves contraseñas o datos "
                              "privados, no los repitas.",
                              *q ? q : "Describe qué hay en la pantalla y qué está haciendo.");
    int tokens = 0;
    GroqError err = {0};
    char *seen = groq_vision(prompt, b64, &tokens, &err);
    free(prompt);
    free(b64);
    char *r;
    if (seen) {
        log_msg("Vi tu pantalla (%dx%d): %d tokens de entrada.", W, H, tokens);
        r = str_printf("Lo que se ve en su pantalla (según la IA con visión): %s", seen);
        free(seen);
    } else if (err.status == GROQ_BAD_RESPONSE && err.detail && !strcmp(err.detail, "sin-vision")) {
        r = xstrdup("Ninguna de las keys de IA configuradas tiene un modelo que vea imágenes: dile al usuario que "
                    "no puedes ver su pantalla con las keys que tiene.");
    } else {
        r = str_printf("No pude ver la pantalla: %s", err.detail ? err.detail : "la IA con visión no contestó");
    }
    groq_error_free(&err);
    return r;
}

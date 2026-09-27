/* La esfera de Sokari. Dos estilos con la misma matemática del diseño
   original (Main.dc.html): ondulación, rotación + bamboleo, perspectiva,
   color por intensidad (fresnel + relieve + profundidad) en 12 escalones,
   mezcla aditiva y resplandor desenfocado.
   - Puntos: un halo de puntos repartidos parejo sobre la esfera (densidad
     uniforme, sin acumularse en los polos); el borde brilla solo porque ahí
     los puntos se apilan en perspectiva.
   - Líneas: 140 meridianos de 60 segmentos, como el diseño original.
   Siempre en movimiento; con la voz de Sokari agrega una ondulación rápida
   con destellos, y el halo de puntos además palpita con cada sílaba (se
   agranda y se aclara hacia blanco). Rasterizador propio
   con antialiasing, repartido en varios hilos por franjas horizontales. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sphere.h"
#include "util.h"

#define MERIDIANS 140
#define LINE_POINTS 60
#define DOT_RINGS 92
#define DOT_MAX_PER_RING 170
#define BUCKETS 12
#define MAX_WORKERS 8
#define MARGIN 0.80f /* la esfera ocupa menos que el lienzo: las ondas y el pulso de los puntos no se cortan */
/* Las líneas al hablar llegan a 1.6 veces su radio (medido): con un lienzo
   1.32 veces más grande tampoco se cortan. */
#define LINES_ROOM 1.32f

const SphereParams SPHERE_IDLE = {{0x45, 0x50, 0xe6}, {0xff, 0x2b, 0xd1}, 0.15f, 0.24f, 8.0f};
const SphereParams SPHERE_SPEAK = {{0x5b, 0x3d, 0xf0}, {0xff, 0x47, 0xe0}, 0.32f, 0.40f, 11.0f};

typedef struct {
    float x0, y0, x1, y1;
    int bucket;
    float a; /* 1, salvo al entrar o salir de la pantalla */
} Seg;

typedef struct {
    float x, y, rad;
    float c[3];
} Dot;

typedef struct {
    float phi, theta, bx, by, bz;
} DotBase;

typedef struct {
    struct SphereRenderer *r;
    int index;
    HANDLE thread, start, done;
} Worker;

struct SphereRenderer {
    int size, half, factor; /* half = lado del buffer del resplandor = size / factor */
    float phi[MERIDIANS], theta[LINE_POINTS + 1];
    float bx[MERIDIANS][LINE_POINTS + 1], by[MERIDIANS][LINE_POINTS + 1], bz[MERIDIANS][LINE_POINTS + 1];
    Seg segs[MERIDIANS * LINE_POINTS];
    DotBase *dot_base;
    Dot *dots;
    int ndots;
    SphereStyle style;
    float fit; /* fracción del tamaño normal de la esfera en este lienzo */
    SphereAnim anim; /* al entrar o salir de la pantalla (sphere_set_presence) */
    float presence;
    float colors[BUCKETS][3];
    float widths[BUCKETS];
    float *acc, *glow_a, *glow_b, *colacc;
    int nworkers;
    Worker workers[MAX_WORKERS];
    volatile LONG quit;
    int job;
    uint32_t *out;
    int stride;
    bool premul;
};

void sphere_lerp(SphereParams *out, const SphereParams *a, const SphereParams *b, float f)
{
    for (int i = 0; i < 3; i++) {
        out->low[i] = a->low[i] + (b->low[i] - a->low[i]) * f;
        out->high[i] = a->high[i] + (b->high[i] - a->high[i]) * f;
    }
    out->rotation_speed = a->rotation_speed + (b->rotation_speed - a->rotation_speed) * f;
    out->ripple = a->ripple + (b->ripple - a->ripple) * f;
    out->glow = a->glow + (b->glow - a->glow) * f;
}

static void draw_segment(SphereRenderer *r, const Seg *s, int band0, int band1)
{
    float w = r->widths[s->bucket];
    float scale = 1.0f;
    if (w < 1.0f) {
        scale = w;
        w = 1.0f;
    }
    float hw = w * 0.5f, ext = hw + 0.5f, ext2 = ext * ext;
    float miny = fminf(s->y0, s->y1) - ext, maxy = fmaxf(s->y0, s->y1) + ext;
    int iy0 = (int)floorf(miny), iy1 = (int)ceilf(maxy);
    if (iy0 < band0) iy0 = band0;
    if (iy1 > band1 - 1) iy1 = band1 - 1;
    if (iy0 > iy1) return;
    float minx = fminf(s->x0, s->x1) - ext, maxx = fmaxf(s->x0, s->x1) + ext;
    int ix0 = (int)floorf(minx), ix1 = (int)ceilf(maxx);
    if (ix0 < 0) ix0 = 0;
    if (ix1 > r->size - 1) ix1 = r->size - 1;
    if (ix0 > ix1) return;
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float len2 = dx * dx + dy * dy;
    float inv = len2 > 1e-8f ? 1.0f / len2 : 0.0f;
    float len = sqrtf(len2);
    scale *= s->a;
    float cr = r->colors[s->bucket][0] * scale, cg = r->colors[s->bucket][1] * scale,
          cb = r->colors[s->bucket][2] * scale;
    for (int y = iy0; y <= iy1; y++) {
        float py = (float)y + 0.5f;
        int xa = ix0, xb = ix1;
        if (fabsf(dy) > 0.5f) {
            float xc = s->x0 + (py - s->y0) * dx / dy;
            float halfw = ext * len / fabsf(dy) + ext;
            int a = (int)floorf(xc - halfw), b = (int)ceilf(xc + halfw);
            if (a > xa) xa = a;
            if (b < xb) xb = b;
        }
        float *row = r->acc + (size_t)y * (size_t)r->size * 3;
        for (int x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f;
            float t = ((px - s->x0) * dx + (py - s->y0) * dy) * inv;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = s->x0 + t * dx - px, ey = s->y0 + t * dy - py;
            float d2 = ex * ex + ey * ey;
            if (d2 >= ext2) continue;
            float cov = hw + 0.5f - sqrtf(d2);
            if (cov > 1) cov = 1;
            float *p = row + (size_t)x * 3;
            p[0] += cr * cov;
            p[1] += cg * cov;
            p[2] += cb * cov;
        }
    }
}

static void draw_dot(SphereRenderer *r, const Dot *d, int band0, int band1)
{
    float rad = d->rad, scale = 1.0f;
    if (rad < 0.6f) {
        scale = rad / 0.6f;
        rad = 0.6f;
    }
    float ext = rad + 0.5f, ext2 = ext * ext;
    int iy0 = (int)floorf(d->y - ext), iy1 = (int)ceilf(d->y + ext);
    if (iy0 < band0) iy0 = band0;
    if (iy1 > band1 - 1) iy1 = band1 - 1;
    if (iy0 > iy1) return;
    int ix0 = (int)floorf(d->x - ext), ix1 = (int)ceilf(d->x + ext);
    if (ix0 < 0) ix0 = 0;
    if (ix1 > r->size - 1) ix1 = r->size - 1;
    float cr = d->c[0] * scale, cg = d->c[1] * scale, cb = d->c[2] * scale;
    for (int y = iy0; y <= iy1; y++) {
        float ey = (float)y + 0.5f - d->y;
        float *row = r->acc + (size_t)y * (size_t)r->size * 3;
        for (int x = ix0; x <= ix1; x++) {
            float ex = (float)x + 0.5f - d->x;
            float d2 = ex * ex + ey * ey;
            if (d2 >= ext2) continue;
            float cov = rad + 0.5f - sqrtf(d2);
            if (cov > 1) cov = 1;
            float *px = row + (size_t)x * 3;
            px[0] += cr * cov;
            px[1] += cg * cov;
            px[2] += cb * cov;
        }
    }
}

static void band_bounds(SphereRenderer *r, int index, int *y0, int *y1)
{
    int n = r->nworkers + 1;
    *y0 = r->size * index / n;
    *y1 = r->size * (index + 1) / n;
}

static void raster_band(SphereRenderer *r, int index)
{
    int y0, y1;
    band_bounds(r, index, &y0, &y1);
    memset(r->acc + (size_t)y0 * r->size * 3, 0, sizeof(float) * (size_t)(y1 - y0) * r->size * 3);
    if (r->style == SPHERE_STYLE_LINES) {
        for (int i = 0; i < MERIDIANS * LINE_POINTS; i++) draw_segment(r, &r->segs[i], y0, y1);
    } else {
        for (int i = 0; i < r->ndots; i++) draw_dot(r, &r->dots[i], y0, y1);
    }
    for (float *p = r->acc + (size_t)y0 * r->size * 3, *e = r->acc + (size_t)y1 * r->size * 3; p < e; p++)
        if (*p > 255.0f) *p = 255.0f;
}

static void compose_band(SphereRenderer *r, int index)
{
    int y0, y1;
    band_bounds(r, index, &y0, &y1);
    int size = r->size, h = r->half;
    float inv_f = 1.0f / (float)r->factor;
    for (int y = y0; y < y1; y++) {
        float gy = ((float)y + 0.5f) * inv_f - 0.5f;
        int gy0 = (int)floorf(gy);
        float fy = gy - (float)gy0;
        int ya = gy0 < 0 ? 0 : gy0 >= h ? h - 1 : gy0;
        int yb = gy0 + 1 < 0 ? 0 : gy0 + 1 >= h ? h - 1 : gy0 + 1;
        const float *ga = r->glow_a + (size_t)ya * h * 3, *gb = r->glow_a + (size_t)yb * h * 3;
        const float *src = r->acc + (size_t)y * size * 3;
        uint32_t *dst = r->out + (size_t)y * r->stride;
        for (int x = 0; x < size; x++) {
            float gx = ((float)x + 0.5f) * inv_f - 0.5f;
            int gx0 = (int)floorf(gx);
            float fx = gx - (float)gx0;
            int xa = gx0 < 0 ? 0 : gx0 >= h ? h - 1 : gx0;
            int xb = gx0 + 1 < 0 ? 0 : gx0 + 1 >= h ? h - 1 : gx0 + 1;
            float c[3];
            for (int k = 0; k < 3; k++) {
                float top = ga[xa * 3 + k] + (ga[xb * 3 + k] - ga[xa * 3 + k]) * fx;
                float bot = gb[xa * 3 + k] + (gb[xb * 3 + k] - gb[xa * 3 + k]) * fx;
                float v = src[x * 3 + k] + 0.85f * (top + (bot - top) * fy);
                c[k] = v > 255.0f ? 255.0f : v;
            }
            uint32_t R = (uint32_t)c[0], G = (uint32_t)c[1], B = (uint32_t)c[2];
            uint32_t A = 255;
            if (r->premul) {
                A = R > G ? R : G;
                if (B > A) A = B;
            }
            dst[x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

static void run_job(SphereRenderer *r, int index)
{
    if (r->job == 1) raster_band(r, index);
    else compose_band(r, index);
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    Worker *w = arg;
    for (;;) {
        WaitForSingleObject(w->start, INFINITE);
        if (InterlockedCompareExchange(&w->r->quit, 1, 1)) return 0;
        run_job(w->r, w->index + 1);
        SetEvent(w->done);
    }
}

static void parallel(SphereRenderer *r, int job)
{
    r->job = job;
    for (int i = 0; i < r->nworkers; i++) SetEvent(r->workers[i].start);
    run_job(r, 0);
    for (int i = 0; i < r->nworkers; i++) WaitForSingleObject(r->workers[i].done, INFINITE);
}

/* Desenfoque gaussiano aproximado con 3 pasadas de caja separables, sobre
   un buffer a 1/2 o 1/4 de resolución: el resplandor es muy suave, no hace
   falta más. Las dos pasadas recorren la memoria en orden (la vertical lleva
   un acumulador por columna en vez de saltar fila por fila). */
static void box_horizontal(const float *src, float *dst, int w, int h, int radius)
{
    float norm = 1.0f / (float)(2 * radius + 1);
    for (int y = 0; y < h; y++) {
        const float *s = src + (size_t)y * w * 3;
        float *d = dst + (size_t)y * w * 3;
        for (int k = 0; k < 3; k++) {
            float acc = 0;
            for (int i = -radius; i <= radius; i++) acc += s[(i < 0 ? 0 : i >= w ? w - 1 : i) * 3 + k];
            for (int x = 0; x < w; x++) {
                d[x * 3 + k] = acc * norm;
                int add = x + radius + 1, sub = x - radius;
                acc += s[(add >= w ? w - 1 : add) * 3 + k] - s[(sub < 0 ? 0 : sub) * 3 + k];
            }
        }
    }
}

static void box_vertical(const float *src, float *dst, int w, int h, int radius, float *acc)
{
    size_t row = (size_t)w * 3;
    float norm = 1.0f / (float)(2 * radius + 1);
    memset(acc, 0, sizeof(float) * row);
    for (int i = -radius; i <= radius; i++) {
        const float *s = src + (size_t)(i < 0 ? 0 : i >= h ? h - 1 : i) * row;
        for (size_t x = 0; x < row; x++) acc[x] += s[x];
    }
    for (int y = 0; y < h; y++) {
        float *d = dst + (size_t)y * row;
        int add = y + radius + 1, sub = y - radius;
        const float *sa = src + (size_t)(add >= h ? h - 1 : add) * row;
        const float *ss = src + (size_t)(sub < 0 ? 0 : sub) * row;
        for (size_t x = 0; x < row; x++) {
            d[x] = acc[x] * norm;
            acc[x] += sa[x] - ss[x];
        }
    }
}

static void blur_glow(SphereRenderer *r, float sigma)
{
    int size = r->size, h = r->half, f = r->factor;
    float inv = 1.0f / (float)(f * f);
    for (int y = 0; y < h; y++) {
        float *d = r->glow_a + (size_t)y * h * 3;
        memset(d, 0, sizeof(float) * (size_t)h * 3);
        for (int dy = 0; dy < f; dy++) {
            const float *a = r->acc + (size_t)(y * f + dy) * size * 3;
            for (int x = 0; x < h; x++)
                for (int dx = 0; dx < f; dx++)
                    for (int k = 0; k < 3; k++) d[x * 3 + k] += a[(x * f + dx) * 3 + k];
        }
        for (int i = 0; i < h * 3; i++) d[i] *= inv;
    }
    float s = sigma / (float)f;
    if (s < 0.5f) return;
    int radius = (int)floorf(sqrtf(12.0f * s * s / 3.0f + 1.0f) * 0.5f);
    if (radius < 1) radius = 1;
    for (int pass = 0; pass < 3; pass++) {
        box_horizontal(r->glow_a, r->glow_b, h, h, radius);
        box_vertical(r->glow_b, r->glow_a, h, h, radius, r->colacc);
    }
}

/* Anillos de latitud con una cantidad de puntos proporcional a su radio:
   densidad pareja en toda la superficie, sin el montón de puntos que se
   formaría en los polos si se usaran los mismos meridianos. */
static void build_dots(SphereRenderer *r)
{
    int cap = DOT_RINGS * DOT_MAX_PER_RING;
    r->dot_base = xmalloc(sizeof(DotBase) * (size_t)cap);
    r->dots = xmalloc(sizeof(Dot) * (size_t)cap);
    int n = 0;
    for (int j = 1; j < DOT_RINGS; j++) {
        float theta = (float)j / DOT_RINGS * (float)M_PI;
        int count = (int)lroundf(DOT_MAX_PER_RING * sinf(theta));
        if (count < 6) count = 6;
        float offset = (j & 1) ? 0.5f : 0.0f;
        for (int k = 0; k < count && n < cap; k++) {
            float phi = ((float)k + offset) / (float)count * 2.0f * (float)M_PI;
            r->dot_base[n] = (DotBase){phi, theta, sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi)};
            n++;
        }
    }
    r->ndots = n;
}

float sphere_room(SphereStyle style)
{
    return style == SPHERE_STYLE_LINES ? LINES_ROOM : 1.0f;
}

SphereRenderer *sphere_create(int size)
{
    return sphere_create_fit(size, 1.0f);
}

SphereRenderer *sphere_create_fit(int size, float fit)
{
    if (size < 64) size = 64;
    SphereRenderer *r = xcalloc(1, sizeof *r);
    r->fit = fit > 0.0f && fit <= 1.0f ? fit : 1.0f;
    r->anim = SPHERE_ANIM_NONE;
    r->presence = 1.0f;
    r->factor = size >= 800 ? 4 : 2;
    size -= size % r->factor;
    r->size = size;
    r->half = size / r->factor;
    for (int m = 0; m < MERIDIANS; m++) r->phi[m] = (float)m / MERIDIANS * 2.0f * (float)M_PI;
    for (int j = 0; j <= LINE_POINTS; j++) r->theta[j] = (float)j / LINE_POINTS * (float)M_PI;
    for (int m = 0; m < MERIDIANS; m++)
        for (int j = 0; j <= LINE_POINTS; j++) {
            r->bx[m][j] = sinf(r->theta[j]) * cosf(r->phi[m]);
            r->by[m][j] = cosf(r->theta[j]);
            r->bz[m][j] = sinf(r->theta[j]) * sinf(r->phi[m]);
        }
    build_dots(r);
    float s = (float)size / 960.0f * r->fit;
    for (int i = 0; i < BUCKETS; i++) {
        float f = (float)i / (BUCKETS - 1);
        r->widths[i] = (0.5f + f * 1.3f) * s;
    }
    r->acc = xmalloc(sizeof(float) * (size_t)size * size * 3);
    r->glow_a = xmalloc(sizeof(float) * (size_t)r->half * r->half * 3);
    r->glow_b = xmalloc(sizeof(float) * (size_t)r->half * r->half * 3);
    r->colacc = xmalloc(sizeof(float) * (size_t)r->half * 3);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = (int)si.dwNumberOfProcessors;
    r->nworkers = cores > 2 ? cores - 1 : 0;
    if (r->nworkers > 3) r->nworkers = 3;
    for (int i = 0; i < r->nworkers; i++) {
        Worker *w = &r->workers[i];
        w->r = r;
        w->index = i;
        w->start = CreateEventW(NULL, FALSE, FALSE, NULL);
        w->done = CreateEventW(NULL, FALSE, FALSE, NULL);
        w->thread = CreateThread(NULL, 0, worker_main, w, 0, NULL);
    }
    return r;
}

void sphere_destroy(SphereRenderer *r)
{
    if (!r) return;
    InterlockedExchange(&r->quit, 1);
    for (int i = 0; i < r->nworkers; i++) SetEvent(r->workers[i].start);
    for (int i = 0; i < r->nworkers; i++) {
        WaitForSingleObject(r->workers[i].thread, 2000);
        CloseHandle(r->workers[i].thread);
        CloseHandle(r->workers[i].start);
        CloseHandle(r->workers[i].done);
    }
    free(r->dot_base);
    free(r->dots);
    free(r->acc);
    free(r->glow_a);
    free(r->glow_b);
    free(r->colacc);
    free(r);
}

int sphere_size(const SphereRenderer *r)
{
    return r->size;
}

typedef struct {
    float cx, cy, R, D, ripple, voice;
    float white; /* cuánto se aclaran los puntos hacia blanco (el pulso) */
    float cosA, sinA, cosT, sinT;
    float vt;
} Frame;

typedef struct {
    float sx, sy, z2, wave;
} Projected;

/* La ondulación base viaja lenta alrededor de la esfera (el "GIF" en reposo);
   con la voz se suma una ondulación rápida de alta frecuencia que la hace
   vibrar mientras Sokari habla. */
static Projected project(const Frame *f, float phi, float theta, float bx, float by, float bz, float base_wave,
                         float voice_amp)
{
    Projected o;
    float wave = base_wave;
    if (f->voice > 0.001f) {
        wave += f->voice * voice_amp *
                (sinf(phi * 7.0f + f->vt * 5.3f) * sinf(theta * 3.0f - f->vt * 4.1f) +
                 0.6f * sinf(phi * 13.0f - f->vt * 7.7f) * sinf(theta * 6.0f + f->vt * 6.2f));
    }
    float rad = 1.0f + wave;
    float x = bx * rad, y = by * rad, z = bz * rad;
    float x1 = x * f->cosA - z * f->sinA;
    float z1 = x * f->sinA + z * f->cosA;
    float y1 = y * f->cosT - z1 * f->sinT;
    o.z2 = y * f->sinT + z1 * f->cosT;
    float persp = f->D / (f->D - o.z2 * f->R * 0.55f);
    o.sx = f->cx + x1 * f->R * persp;
    o.sy = f->cy + y1 * f->R * persp;
    o.wave = wave;
    return o;
}

static int line_bucket(const Frame *f, const Projected *p)
{
    float fresnel = 1.0f - fabsf(p->z2);
    float bulge = fmaxf(0.0f, p->wave) / f->ripple;
    float depth = (p->z2 + 1.0f) * 0.5f;
    float intensity = fresnel * 0.5f + bulge * 0.75f + depth * 0.15f;
    if (intensity > 1) intensity = 1;
    if (intensity < 0) intensity = 0;
    int b = (int)floorf(intensity * BUCKETS);
    return b > BUCKETS - 1 ? BUCKETS - 1 : b;
}

static float smoothstep(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3.0f - 2.0f * t);
}

/* ------------------------------------------------ entrar y salir --- */

static float clamp01(float x)
{
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

/* Un número de 0 a 1 que siempre es el mismo para el mismo pedazo. */
static float hash01(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0xffffff) / 16777216.0f;
}

static float ease_out_cubic(float x)
{
    x = 1.0f - x;
    return 1.0f - x * x * x;
}

/* Se pasa un poco de 1 antes de llegar: el rebote. */
static float ease_out_back(float x)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float y = x - 1.0f;
    return 1.0f + c3 * y * y * y + c1 * y * y;
}

/* Materializarse: cada pedazo (un punto, o un tramo de línea) llega de afuera
   por su lado y a su tiempo, con un giro, y al irse se dispersa por el mismo
   camino. Mueve (x, y) y devuelve qué tan visible va. */
static float scatter_piece(float cx, float cy, int size, float pres, unsigned id, float *x, float *y)
{
    float h1 = hash01(id * 3u + 1u), h2 = hash01(id * 3u + 2u), h3 = hash01(id * 3u + 3u);
    float e = ease_out_cubic(clamp01(pres * 1.6f - h1 * 0.6f));
    float dx = *x - cx, dy = *y - cy;
    float len = sqrtf(dx * dx + dy * dy) + 1e-3f;
    float push = (1.0f - e) * (0.5f + 1.3f * h2) * (float)size * 0.5f;
    float turn = (1.0f - e) * (h3 - 0.5f) * 2.4f;
    float ux = dx / len, uy = dy / len, c = cosf(turn), sn = sinf(turn);
    *x += (ux * c - uy * sn) * push;
    *y += (uy * c + ux * sn) * push;
    return e;
}

void sphere_set_presence(SphereRenderer *r, SphereAnim anim, float presence)
{
    r->anim = (int)anim >= 0 && (int)anim < SPHERE_ANIM_COUNT ? anim : SPHERE_ANIM_NONE;
    r->presence = presence;
}

float sphere_slide_offset(float presence)
{
    return 1.0f - ease_out_back(clamp01(presence));
}

#define TEST_STAY 1.0 /* Probar con la esfera oculta: cuánto se queda antes de irse */
#define TEST_GAP 0.35 /* Probar con la esfera en pantalla: cuánto tarda en regresar */

void sphere_appear_init(SphereAppear *a)
{
    *a = (SphereAppear){1.0f, 0, 0, 0.0, SPHERE_ANIM_NONE};
}

void sphere_appear_enter(SphereAppear *a, SphereAnim anim, bool from_zero)
{
    a->anim = anim;
    a->then = 0;
    a->hold = 0;
    if (anim == SPHERE_ANIM_NONE) {
        a->presence = 1.0f;
        a->dir = 0;
        return;
    }
    if (from_zero) a->presence = 0.0f;
    a->dir = a->presence < 1.0f ? 1 : 0;
}

bool sphere_appear_leave(SphereAppear *a, SphereAnim anim)
{
    a->anim = anim;
    a->then = 0;
    a->hold = 0;
    if (anim == SPHERE_ANIM_NONE) {
        a->presence = 0.0f;
        a->dir = 0;
        return true;
    }
    a->dir = -1;
    return false;
}

void sphere_appear_test(SphereAppear *a, SphereAnim anim, bool shown)
{
    if (anim == SPHERE_ANIM_NONE) return; /* nada que ver: no se muestra para probarla */
    a->anim = anim;
    a->hold = 0;
    if (shown) {
        a->dir = -1;
        a->then = 1;
    } else {
        a->presence = 0.0f;
        a->dir = 1;
        a->then = -1;
    }
}

bool sphere_appear_step(SphereAppear *a, double dt)
{
    if (a->hold > 0) {
        a->hold -= dt;
        if (a->hold <= 0) {
            a->hold = 0;
            a->dir = a->then;
            a->then = 0;
        }
        return false;
    }
    if (a->dir > 0) {
        a->presence += (float)(dt / SPHERE_ENTER_SECONDS);
        if (a->presence >= 1.0f) {
            a->presence = 1.0f;
            a->dir = 0;
            if (a->then) a->hold = TEST_STAY;
        }
    } else if (a->dir < 0) {
        a->presence -= (float)(dt / SPHERE_LEAVE_SECONDS);
        if (a->presence <= 0.0f) {
            a->presence = 0.0f;
            a->dir = 0;
            if (!a->then) return true;
            a->hold = TEST_GAP;
        }
    }
    return false;
}

bool sphere_appear_moving(const SphereAppear *a)
{
    return a->dir != 0 || a->hold > 0;
}

/* Halo de puntos: el brillo sale del borde (fresnel) y el color de las
   crestas de las ondas: azul casi todo, magenta solo donde la superficie
   se levanta, como en la referencia. */
static void dot_style(const SphereParams *p, const Frame *f, const Projected *pr, float s, Dot *d)
{
    float fresnel = 1.0f - fabsf(pr->z2);
    float rim = fresnel * fresnel * fresnel;
    float depth = (pr->z2 + 1.0f) * 0.5f;
    float crest = fmaxf(0.0f, pr->wave) / f->ripple;
    float hue = smoothstep(0.45f, 1.05f, crest) * (0.35f + 0.65f * fresnel) + 0.12f * rim;
    hue += 0.25f * f->voice * smoothstep(0.2f, 0.9f, crest);
    if (hue > 1) hue = 1;
    float bright = 0.12f + 0.75f * rim + 0.12f * depth + 0.40f * smoothstep(0.5f, 1.1f, crest);
    bright *= 1.0f + 0.35f * f->voice;
    for (int k = 0; k < 3; k++) {
        float c = (p->low[k] + (p->high[k] - p->low[k]) * hue) * bright;
        d->c[k] = c + (255.0f * bright - c) * f->white;
    }
    d->rad = (0.75f + 0.85f * fminf(bright, 1.0f)) * s;
    d->x = pr->sx;
    d->y = pr->sy;
}

void sphere_render(SphereRenderer *r, double t, double angle, double voice_t, const SphereParams *p, float voice,
                   float pulse, SphereStyle style, uint32_t *out, int stride, bool premultiplied)
{
    int size = r->size;
    float s = (float)size / 960.0f * r->fit;
    if (voice < 0) voice = 0;
    if (voice > 1) voice = 1;
    bool dots = style != SPHERE_STYLE_LINES;
    /* Al entrar o salir de la pantalla. Con la esfera completa no se toca
       nada: sale idéntica a la de siempre. */
    float pres = r->anim == SPHERE_ANIM_NONE ? 1.0f : clamp01(r->presence);
    if (pres <= 0.0f) {
        uint32_t empty = premultiplied ? 0u : 0xFF000000u;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) out[(size_t)y * stride + x] = empty;
        return;
    }
    bool moving = pres < 1.0f, scatter = moving && r->anim == SPHERE_ANIM_MATERIALIZE;
    float fade = 1.0f;
    /* El pulso es solo del halo de puntos: las líneas ya se mueven de sobra. */
    if (pulse < 0 || !dots) pulse = 0;
    if (pulse > 1) pulse = 1;
    Frame f;
    f.cx = size * 0.5f;
    f.cy = size * 0.5f;
    f.R = 360.0f * s * MARGIN * (dots ? 1.0f + 0.04f * voice + 0.11f * pulse : 1.0f + 0.06f * voice);
    f.white = 0.5f * pulse;
    f.D = 760.0f * s * MARGIN;
    if (moving && r->anim == SPHERE_ANIM_ZOOM) {
        /* Radio y perspectiva juntos: la misma esfera, más chica. */
        float e = fmaxf(ease_out_back(pres), 0.001f);
        f.R *= e;
        f.D *= e;
        fade = clamp01(pres * 2.0f);
    } else if (moving && r->anim == SPHERE_ANIM_SLIDE) {
        fade = clamp01(pres * 3.0f);
    }
    f.ripple = p->ripple > 0.0001f ? p->ripple : 0.0001f;
    f.voice = voice;
    f.cosA = cosf((float)angle);
    f.sinA = sinf((float)angle);
    float wob = sinf((float)t * 0.12f) * 0.22f;
    f.cosT = cosf(wob);
    f.sinT = sinf(wob);
    f.vt = (float)t;
    float wt = (float)voice_t;
    r->style = style;

    if (dots) {
        /* El halo es más redondo que las líneas: misma onda, menos amplitud. */
        Frame fd = f;
        fd.ripple = f.ripple * 0.38f;
        for (int i = 0; i < r->ndots; i++) {
            const DotBase *b = &r->dot_base[i];
            float base = fd.ripple * (sinf(b->phi * 4.0f + wt * 0.6f) * sinf(b->theta * 2.0f + wt * 0.3f) +
                                      0.5f * sinf(b->phi * 11.0f - wt * 0.9f) * sinf(b->theta * 5.0f + wt * 0.5f));
            Projected pr = project(&fd, b->phi, b->theta, b->bx, b->by, b->bz, base, 0.055f);
            dot_style(p, &fd, &pr, s, &r->dots[i]);
            if (moving) {
                Dot *d = &r->dots[i];
                float a = scatter ? scatter_piece(fd.cx, fd.cy, size, pres, (unsigned)i, &d->x, &d->y) : fade;
                for (int k = 0; k < 3; k++) d->c[k] *= a;
            }
        }
    } else {
        for (int i = 0; i < BUCKETS; i++) {
            float fi = (float)i / (BUCKETS - 1);
            float a = 0.06f + fi * fi * 0.85f;
            for (int k = 0; k < 3; k++) r->colors[i][k] = roundf(p->low[k] + (p->high[k] - p->low[k]) * fi) * a;
        }
        float s2t[LINE_POINTS + 1], s5t[LINE_POINTS + 1];
        for (int j = 0; j <= LINE_POINTS; j++) {
            s2t[j] = sinf(r->theta[j] * 2.0f + wt * 0.3f);
            s5t[j] = sinf(r->theta[j] * 5.0f + wt * 0.5f);
        }
        int n = 0;
        for (int m = 0; m < MERIDIANS; m++) {
            float s4 = sinf(r->phi[m] * 4.0f + wt * 0.6f);
            float s11 = sinf(r->phi[m] * 11.0f - wt * 0.9f);
            float psx = 0, psy = 0;
            for (int j = 0; j <= LINE_POINTS; j++) {
                float base = f.ripple * (s4 * s2t[j] + 0.5f * s11 * s5t[j]);
                Projected pr = project(&f, r->phi[m], r->theta[j], r->bx[m][j], r->by[m][j], r->bz[m][j], base, 0.12f);
                if (j > 0) {
                    Seg *sg = &r->segs[n];
                    *sg = (Seg){psx, psy, pr.sx, pr.sy, line_bucket(&f, &pr), 1.0f};
                    if (scatter) {
                        /* Cada tramo es un pedazo: se mueve entero. */
                        float mx = (sg->x0 + sg->x1) * 0.5f, my = (sg->y0 + sg->y1) * 0.5f, nx = mx, ny = my;
                        sg->a = scatter_piece(f.cx, f.cy, size, pres, (unsigned)n + 0x10000u, &nx, &ny);
                        sg->x0 += nx - mx;
                        sg->x1 += nx - mx;
                        sg->y0 += ny - my;
                        sg->y1 += ny - my;
                    } else if (moving) {
                        sg->a = fade;
                    }
                    n++;
                }
                psx = pr.sx;
                psy = pr.sy;
            }
        }
    }

    parallel(r, 1);
    float glow = p->glow + 4.0f * voice + 3.0f * pulse;
    if (moving) glow *= scatter ? pres * pres : fade;
    blur_glow(r, glow * s);
    if (glow <= 0.01f) memset(r->glow_a, 0, sizeof(float) * (size_t)r->half * r->half * 3);
    r->out = out;
    r->stride = stride;
    r->premul = premultiplied;
    parallel(r, 2);
}

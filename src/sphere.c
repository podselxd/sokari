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
/* Con cara se mueve, se infla, se aplasta y se estira: más lienzo, y hasta
   dónde llega lo que se ve de la esfera (en radios, con su ondulación). */
#define FACE_ROOM 1.3f
#define FACE_REACH 1.4f

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

/* La cara de un cuadro ya puesta en el lienzo (ver sphere_set_face). */
typedef struct {
    bool on;
    SphereStyle style;
    float cx, cy, R;         /* centro de la cara y radio de la esfera, en píxeles */
    float cosT, sinT, sx, sy;
    float aa;                /* ancho del antialias, en radios */
    float ex, ey, rx, ry;    /* los ojos */
    float my, mw0, thick;    /* la boca (thick 0: sin boca) */
    float tint[3], color[3]; /* el núcleo (blanco teñido) y el resplandor */
    float alpha;
    bool shade_on;           /* la sombra gris de preocupación (temor): se aplica al final */
    int shx0, shy0, shx1, shy1;
} FaceGeom;

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
    bool has_face;    /* sphere_set_face */
    SphereFace face;
    FaceGeom fg;      /* la cara de este cuadro, en píxeles */
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

static void face_band(SphereRenderer *r, int y0, int y1);
static void face_glow(SphereRenderer *r);
static void face_uv(const FaceGeom *g, float px, float py, float *u, float *v);
static float face_shade(const FaceGeom *g, const SphereFace *p, float u, float v);
static float blush_cov(const FaceGeom *g, const SphereFace *p, float u, float v);
static const float PINK[3] = {255, 105, 150}; /* el sonrojo */

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
    if (r->fg.on) face_band(r, y0, y1);
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
            if (r->fg.shade_on && y >= r->fg.shy0 && y <= r->fg.shy1 && x >= r->fg.shx0 && x <= r->fg.shx1) {
                float u, vv;
                face_uv(&r->fg, (float)x + 0.5f, (float)y + 0.5f, &u, &vv);
                float k = face_shade(&r->fg, &r->face, u, vv);
                c[0] *= k, c[1] *= k, c[2] *= k;
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
    if (r->fg.on) face_glow(r);
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
    if (style == SPHERE_STYLE_LINES) return LINES_ROOM;
    return sphere_style_is_face(style) ? FACE_ROOM : 1.0f;
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
    float qx, qy; /* aplastar y estirar (la cara); 1 = redonda */
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
    o.sx = f->cx + x1 * f->R * persp * f->qx;
    o.sy = f->cy + y1 * f->R * persp * f->qy;
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

static void face_setup(SphereRenderer *r, SphereStyle style, const Frame *f, float face_cx, float face_cy, float alpha);
static void face_uv(const FaceGeom *g, float px, float py, float *u, float *v);
static float face_cov(const FaceGeom *g, const SphereFace *p, float u, float v, bool shaded);
static float face_shade(const FaceGeom *g, const SphereFace *p, float u, float v);

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
    f.qx = f.qy = 1.0f;
    /* La cara: la esfera se mueve, se infla, se aplasta y se estira con ella.
       Sin cara (o con otro estilo) no se toca nada. */
    bool face = sphere_style_is_face(style) && r->has_face;
    float face_cx = f.cx, face_cy = f.cy;
    if (face) {
        const SphereFace *fp = &r->face;
        float room = 0.55f; /* lo que puede moverse sin salirse del lienzo (en radios) */
        float sc = fp->scale < 0.6f ? 0.6f : fp->scale > 1.3f ? 1.3f : fp->scale;
        f.R *= sc;
        f.qx = fp->sx < 0.7f ? 0.7f : fp->sx > 1.3f ? 1.3f : fp->sx;
        f.qy = fp->sy < 0.7f ? 0.7f : fp->sy > 1.3f ? 1.3f : fp->sy;
        float anchor = (1.0f - f.qy) * f.R * 0.9f; /* al aplastarse, se queda sobre el piso */
        float sdx = fmaxf(-room, fminf(room, fp->sphere_dx)), sdy = fmaxf(-room, fminf(room, fp->sphere_dy));
        float fdx = fmaxf(-room, fminf(room, fp->fx)), fdy = fmaxf(-room, fminf(room, fp->fy));
        face_cx = f.cx + fdx * f.R;
        face_cy = f.cy + fdy * f.R + anchor;
        f.cx += sdx * f.R;
        f.cy += sdy * f.R + anchor;
        /* Que nunca se salga del lienzo: lo que ocupa (estirada, con su
           ondulación y su resplandor) más hasta dónde la lleva el gesto no
           pasa del borde. Cerca del borde el movimiento se va frenando (el
           salto se nota igual, sin cortarse); si ni quieta cabe, se achica. */
        const float glow_pad = 2.5f * (p->glow + 4.0f * voice + 3.0f * pulse) * s;
        const float lim = size * 0.5f - 2.0f;
        float ex = f.R * f.qx * FACE_REACH + glow_pad, ey = f.R * f.qy * FACE_REACH + glow_pad;
        const float big = fmaxf(ex, ey);
        if (big > lim) {
            const float k = (lim - glow_pad) / (big - glow_pad);
            f.R *= k;
            ex = (ex - glow_pad) * k + glow_pad;
            ey = (ey - glow_pad) * k + glow_pad;
        }
        const float c0 = size * 0.5f, mx = fmaxf(lim - ex, 0.0f), my = fmaxf(lim - ey, 0.0f);
        const float nx = mx > 0 ? c0 + mx * tanhf((f.cx - c0) / mx) : c0;
        const float ny = my > 0 ? c0 + my * tanhf((f.cy - c0) / my) : c0;
        face_cx += nx - f.cx;
        face_cy += ny - f.cy;
        f.cx = nx;
        f.cy = ny;
    }
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

    if (face) face_setup(r, style, &f, face_cx, face_cy, moving ? (scatter ? pres * pres : fade) : 1.0f);
    else r->fg.on = false;
    bool dot_face = face && style == SPHERE_STYLE_FACE_DOTS;

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
            if (dot_face) {
                /* Cara de puntos: los que caen en los ojos o la boca se prenden en blanco. */
                Dot *d = &r->dots[i];
                float u, v;
                face_uv(&r->fg, d->x, d->y, &u, &v);
                float m = face_cov(&r->fg, &r->face, u, v, false);
                if (m > 0.0f) {
                    for (int k = 0; k < 3; k++) d->c[k] += (r->fg.tint[k] * 1.3f - d->c[k]) * m;
                    d->rad *= 1.0f + 0.6f * m;
                }
                float bl = blush_cov(&r->fg, &r->face, u, v);
                if (bl > 0.0f) {
                    for (int k = 0; k < 3; k++) d->c[k] += (PINK[k] - d->c[k]) * bl;
                    d->rad *= 1.0f + 0.3f * bl;
                }
            }
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

/* ------------------------------------------------------------- la cara --- */

void sphere_face_neutral(SphereFace *f)
{
    memset(f, 0, sizeof *f);
    f->eye_w = f->eye_h = 1.0f;
    f->round = 4.0f;
    f->smile = 0.15f;
    f->mouth_w = 1.0f;
    f->scale = f->sx = f->sy = 1.0f;
    f->glow = 1.0f;
    f->alpha = 1.0f;
    f->color[0] = SPHERE_IDLE.high[0];
    f->color[1] = SPHERE_IDLE.high[1];
    f->color[2] = SPHERE_IDLE.high[2];
}

void sphere_set_face(SphereRenderer *r, const SphereFace *face)
{
    r->has_face = face != NULL;
    if (face) r->face = *face;
}

static float cov_of(float d, float aa)
{
    return clamp01(0.5f - d / aa);
}

static float ellipse_sd(float u, float v, float rx, float ry)
{
    return (sqrtf((u / rx) * (u / rx) + (v / ry) * (v / ry)) - 1.0f) * fminf(rx, ry);
}

/* Un píxel del lienzo en coordenadas de la cara (radios; v hacia abajo). */
static void face_uv(const FaceGeom *g, float px, float py, float *u, float *v)
{
    float dx = px - g->cx, dy = py - g->cy;
    *u = (g->cosT * dx + g->sinT * dy) / (g->R * g->sx);
    *v = (-g->sinT * dx + g->cosT * dy) / (g->R * g->sy);
}

/* Y al revés: de la cara al lienzo. */
static void face_px(const FaceGeom *g, float u, float v, float *px, float *py)
{
    float a = u * g->R * g->sx, b = v * g->R * g->sy;
    *px = g->cx + g->cosT * a - g->sinT * b;
    *py = g->cy + g->sinT * a + g->cosT * b;
}

/* El rectángulo del lienzo que cubre [u0,u1]×[v0,v1] de la cara, recortado. */
static bool face_box(const FaceGeom *g, int size, float u0, float v0, float u1, float v1, int *x0, int *y0, int *x1,
                     int *y1)
{
    float xs[4], ys[4];
    face_px(g, u0, v0, &xs[0], &ys[0]);
    face_px(g, u1, v0, &xs[1], &ys[1]);
    face_px(g, u0, v1, &xs[2], &ys[2]);
    face_px(g, u1, v1, &xs[3], &ys[3]);
    float a = xs[0], b = xs[0], c = ys[0], d = ys[0];
    for (int i = 1; i < 4; i++) {
        a = fminf(a, xs[i]), b = fmaxf(b, xs[i]);
        c = fminf(c, ys[i]), d = fmaxf(d, ys[i]);
    }
    *x0 = (int)floorf(a) - 1, *x1 = (int)ceilf(b) + 1, *y0 = (int)floorf(c) - 1, *y1 = (int)ceilf(d) + 1;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > size - 1) *x1 = size - 1;
    if (*y1 > size - 1) *y1 = size - 1;
    return *x0 <= *x1 && *y0 <= *y1;
}

/* Un ojo: 0 izquierdo, 1 derecho. Cuánto lo cubre (0..1) en (u, v). */
static float eye_cov(const FaceGeom *g, const SphereFace *p, int i, float u, float v, float *shade_zone)
{
    if (shade_zone) *shade_zone = 0.0f;
    float side = i ? 1.0f : -1.0f;
    float a = p->asym * (i ? -0.6f : 1.0f); /* el izquierdo se entrecierra, el derecho se abre */
    float blink = clamp01(p->blink[i]), aa = g->aa;
    float rx = g->rx * p->eye_w * (1.0f + 0.22f * blink); /* al parpadear se aplasta y se estira */
    float ry = g->ry * p->eye_h * (1.0f + 0.18f * fmaxf(-a, 0.0f)) * fmaxf(1.0f - blink, 0.05f);
    float lu = u - (side * g->ex + p->gaze_x), lv = v - (g->ey + p->eye_dy + p->gaze_y);
    if (fabsf(lu) > rx * 1.1f + aa || fabsf(lv) > ry * 1.1f + aa) return 0.0f;
    float qx = lu / rx, qy = lv / ry, k2 = sqrtf(qx * qx + qy * qy), k = k2;
    if (g->style == SPHERE_STYLE_FACE_EYES) {
        /* cuadrado redondeado (4) u óvalo (2), o algo en medio */
        float q2x = qx * qx, q2y = qy * qy, k4 = sqrtf(sqrtf(q2x * q2x + q2y * q2y));
        float f = clamp01((p->round - 2.0f) * 0.5f);
        k = k2 + (k4 - k2) * f;
    }
    float m = cov_of((k - 1.0f) * fminf(rx, ry), aa);
    if (m <= 0.0f) return 0.0f;
    float top = p->lid_top + 0.38f * fmaxf(a, 0.0f), tilt = p->lid_tilt;
    if (top > 0.001f || fabsf(tilt) > 0.001f) m *= cov_of((-ry + 2.0f * ry * top + tilt * (-side) * lu * 0.9f) - lv, aa);
    float bot = p->lid_bot + 0.32f * fmaxf(a, 0.0f);
    if (bot > 0.001f) m *= cov_of(lv - (ry - 2.0f * ry * bot), aa);
    /* «^ ^»: un óvalo sube desde abajo y se come el ojo; con poca alegría
       apenas lo toca (sin dejar un fantasma gris). */
    float hp = clamp01(p->happy);
    if (hp > 0.001f) m *= 1.0f - cov_of(ellipse_sd(lu, lv - ry * (0.5f + 1.8f * (1.0f - hp)), rx * 1.2f, ry * 1.12f), aa);
    /* la sombra de preocupación: arriba afuera, en diagonal */
    if (shade_zone && p->shade > 0.001f) *shade_zone = m * cov_of(lv - (-0.2f * ry + side * 0.5f * lu), aa);
    return m;
}

static float mouth_cov(const FaceGeom *g, const SphereFace *p, float u, float v)
{
    float aa = g->aa;
    if (fabsf(u) > g->mw0 * p->mouth_w + 0.2f || v < g->my - 0.3f || v > g->my + 0.4f) return 0.0f;
    /* De la línea a la «O» del temor sin encimarse (se veía como un chupón):
       primero la línea se encoge y se apaga, y luego crece la «O». */
    float o = smoothstep(0.35f, 0.65f, p->mouth_o);
    float line_k = 1.0f - clamp01(o * 2.0f), ring_k = clamp01(o * 2.0f - 1.0f), m = 0.0f;
    if (line_k > 0.0f) {
        float mw = g->mw0 * p->mouth_w * (0.25f + 0.75f * line_k);
        float x = u / mw;
        x = x < -1.3f ? -1.3f : x > 1.3f ? 1.3f : x;
        float q = fmaxf(1.0f - x * x, 0.0f);
        float curve = g->my + p->smile * 0.15f * q - p->mouth_asym * 0.07f * x + p->wave * 0.03f * sinf(x * 3.0f * (float)M_PI);
        float lower = curve + (p->mouth_open + p->talk * 0.9f) * 0.2f * powf(q, 0.8f);
        float d = fminf(fabsf(v - curve) - g->thick * 0.5f, fmaxf(curve - v, v - lower));
        m = cov_of(d, aa) * clamp01((mw - fabsf(u)) / aa + 0.5f) * line_k;
    }
    if (ring_k > 0.0f) {
        /* una «O» más alta que ancha, como 😨; con la voz se abre más */
        float k = 0.4f + 0.6f * ring_k;
        float rx = (0.055f + 0.03f * p->talk) * k, ry = (0.085f + 0.04f * p->talk) * k;
        float ring = fabsf(ellipse_sd(u, v - (g->my + 0.03f), rx, ry)) - g->thick * 0.5f;
        m = fmaxf(m, cov_of(ring, aa) * ring_k);
    }
    return m;
}

/* Las chapitas del sonrojo: dos óvalos suaves abajo y afuera de los ojos. */
static float blush_cov(const FaceGeom *g, const SphereFace *p, float u, float v)
{
    float b = clamp01(p->blush);
    if (b <= 0.001f) return 0.0f;
    float cx = g->ex + 0.04f, cy = g->ey + g->ry * 1.2f + p->eye_dy;
    float rx = fmaxf(g->rx * 1.05f, 0.13f), ry = fmaxf(g->ry * 0.42f, 0.07f);
    float du = (fabsf(u) - cx) / rx, dv = (v - cy) / ry;
    return b * (1.0f - smoothstep(0.35f, 1.0f, sqrtf(du * du + dv * dv))) * g->alpha;
}

/* Toda la cara en (u, v): los ojos y, si hay, la boca. shaded: con la zona de
   la sombra ya apagada (para el resplandor; al núcleo se la pone compose). */
static float face_cov(const FaceGeom *g, const SphereFace *p, float u, float v, bool shaded)
{
    float z0, z1;
    float m = fmaxf(eye_cov(g, p, 0, u, v, &z0), eye_cov(g, p, 1, u, v, &z1));
    if (shaded) m *= 1.0f - clamp01(p->shade) * fmaxf(z0, z1);
    if (g->thick > 0.0f) m = fmaxf(m, mouth_cov(g, p, u, v));
    return m * g->alpha;
}

/* Cuánto se apaga el píxel (u, v) por la sombra: 1 = nada. */
static float face_shade(const FaceGeom *g, const SphereFace *p, float u, float v)
{
    float z0, z1;
    eye_cov(g, p, 0, u, v, &z0);
    eye_cov(g, p, 1, u, v, &z1);
    return 1.0f - clamp01(p->shade) * fmaxf(z0, z1) * g->alpha;
}

/* Un símbolo en (u, v): cuánto lo cubre. */
static float symbol_cov(const FaceGeom *g, const SphereSymbol *s, float u, float v)
{
    float aa = g->aa, du = u - s->x, dv = v - s->y, sz = s->size;
    if (sz <= 0.01f) return 0.0f;
    switch (s->kind) {
    case SPHERE_SYM_TEAR:
    case SPHERE_SYM_SWEAT: {
        /* una gota: redonda abajo y en punta arriba */
        float r = 0.05f * sz;
        if (dv >= -0.25f * r) return cov_of(sqrtf(du * du + dv * dv) - r, aa);
        float h = (dv + 2.3f * r) / (2.05f * r);
        if (h <= 0.0f) return 0.0f;
        return clamp01((r * 0.95f * powf(clamp01(h), 0.9f) - fabsf(du)) / aa + 0.5f);
    }
    case SPHERE_SYM_SPARK: {
        /* estrella de 4 picos */
        float rr = sqrtf(du * du + dv * dv), outer = 0.17f * sz, inner = outer * 0.27f;
        if (rr > outer + aa) return 0.0f;
        float ang = atan2f(dv, du) - s->rot;
        float c = fabsf(cosf(2.0f * ang));
        float lim = inner + (outer - inner) * powf(c, 6.0f);
        return clamp01((lim - rr) / aa + 0.5f);
    }
    case SPHERE_SYM_QUESTION: {
        /* «?»: un gancho, el palito y el punto */
        float k = 0.6f * sz, w = 0.09f * k;
        float cu = du, cv = dv + 0.3f * k; /* el centro del gancho */
        float rr = sqrtf(cu * cu + cv * cv), ang = atan2f(cv, cu);
        float hook = (ang < 1.2f || ang > 2.6f) ? fabsf(rr - 0.2f * k) - w : 1e3f;
        float su = du - 0.0f, sv = dv - 0.02f * k; /* el palito */
        float stem = fmaxf(fabsf(su) - w, fabsf(sv) - 0.1f * k);
        float dot = sqrtf(du * du + (dv - 0.28f * k) * (dv - 0.28f * k)) - 1.2f * w;
        return cov_of(fminf(hook, fminf(stem, dot)), aa);
    }
    case SPHERE_SYM_ANGER: {
        /* cuatro arcos que miran al centro */
        float r = 0.15f * sz, rr = 0.8f * r, w = 0.18f * r, best = 1e3f;
        for (int q = 0; q < 4; q++) {
            float qx = (q & 1) ? r : -r, qy = (q & 2) ? r : -r;
            float px = du - qx, py = dv - qy, d = sqrtf(px * px + py * py);
            /* solo el cuarto de arco del lado del centro */
            if (px * -qx < 0.0f || py * -qy < 0.0f) continue;
            best = fminf(best, fabsf(d - rr) - w);
        }
        return cov_of(best, aa);
    }
    case SPHERE_SYM_EXCLAIM: {
        /* «!»: un palito redondo arriba que se afila hacia abajo, y el punto */
        float k = 0.6f * sz, w0 = 0.075f * k, w1 = 0.045f * k, top = -0.36f * k, bot = 0.1f * k;
        float w = w0 + (w1 - w0) * clamp01((dv - top) / (bot - top));
        float bar = dv < top ? sqrtf(du * du + (dv - top) * (dv - top)) - w0 : fmaxf(fabsf(du) - w, dv - bot);
        float dot = sqrtf(du * du + (dv - 0.24f * k) * (dv - 0.24f * k)) - 1.15f * w0;
        return cov_of(fminf(bar, dot), aa);
    }
    case SPHERE_SYM_DOTS: {
        float m = 0.0f;
        for (int j = 0; j < 3; j++) {
            float a = clamp01(sz - (float)j);
            if (a <= 0.0f) break;
            float cx = 0.14f * j, cy = -0.08f * j, r = 0.045f + 0.016f * j;
            m = fmaxf(m, a * cov_of(sqrtf((du - cx) * (du - cx) + (dv - cy) * (dv - cy)) - r, aa));
        }
        return m;
    }
    }
    return 0.0f;
}

static const float *symbol_color(SphereSymbolKind k)
{
    static const float C[7][3] = {{140, 205, 255}, {185, 228, 255}, {255, 246, 170}, {245, 240, 255},
                                  {255, 80, 60},   {240, 240, 255}, {255, 236, 140}};
    return C[(int)k >= 0 && (int)k < 7 ? (int)k : 5];
}

/* Qué tan grande es la zona de un símbolo (en radios, desde su centro). */
static float symbol_reach(const SphereSymbol *s)
{
    switch (s->kind) {
    case SPHERE_SYM_QUESTION:
    case SPHERE_SYM_EXCLAIM: return 0.45f * s->size + 0.05f;
    case SPHERE_SYM_DOTS: return 0.45f;
    case SPHERE_SYM_ANGER: return 0.32f * s->size + 0.05f;
    case SPHERE_SYM_SPARK: return 0.2f * s->size + 0.05f;
    default: return 0.16f * s->size + 0.05f;
    }
}

/* Los ojos y la boca de luz (y los símbolos) sobre los puntos de esta franja. */
static void face_band(SphereRenderer *r, int y0, int y1)
{
    const FaceGeom *g = &r->fg;
    const SphereFace *p = &r->face;
    int size = r->size, bx0, by0, bx1, by1;
    if (g->style != SPHERE_STYLE_FACE_DOTS && face_box(g, size, -0.8f, -0.7f, 0.8f, 0.75f, &bx0, &by0, &bx1, &by1)) {
        if (by0 < y0) by0 = y0;
        if (by1 > y1 - 1) by1 = y1 - 1;
        for (int y = by0; y <= by1; y++) {
            float *row = r->acc + (size_t)y * size * 3;
            for (int x = bx0; x <= bx1; x++) {
                float u, v;
                face_uv(g, (float)x + 0.5f, (float)y + 0.5f, &u, &v);
                float *px = row + (size_t)x * 3;
                float b = blush_cov(g, p, u, v);
                if (b > 0.0f)
                    for (int k = 0; k < 3; k++) px[k] = px[k] * (1.0f - 0.5f * b) + b * PINK[k] * 0.85f;
                float m = face_cov(g, p, u, v, false);
                if (m <= 0.0f) continue;
                for (int k = 0; k < 3; k++) px[k] = px[k] * (1.0f - 0.88f * m) + m * g->tint[k];
            }
        }
    }
    for (int i = 0; i < p->nsym && i < SPHERE_MAX_SYMBOLS; i++) {
        const SphereSymbol *s = &p->sym[i];
        float reach = symbol_reach(s), a = clamp01(s->alpha) * g->alpha;
        if (a <= 0.0f || !face_box(g, size, s->x - reach, s->y - reach, s->x + reach, s->y + reach, &bx0, &by0, &bx1, &by1))
            continue;
        if (by0 < y0) by0 = y0;
        if (by1 > y1 - 1) by1 = y1 - 1;
        const float *col = symbol_color(s->kind);
        for (int y = by0; y <= by1; y++) {
            float *row = r->acc + (size_t)y * size * 3;
            for (int x = bx0; x <= bx1; x++) {
                float u, v;
                face_uv(g, (float)x + 0.5f, (float)y + 0.5f, &u, &v);
                float m = symbol_cov(g, s, u, v) * a;
                if (m <= 0.0f) continue;
                float *px = row + (size_t)x * 3;
                for (int k = 0; k < 3; k++) px[k] = px[k] * (1.0f - 0.9f * m) + m * col[k];
            }
        }
    }
}

/* El resplandor de color de la cara y los símbolos, sobre el buffer chico
   del resplandor (antes de desenfocarlo). */
static void face_glow(SphereRenderer *r)
{
    const FaceGeom *g = &r->fg;
    const SphereFace *p = &r->face;
    int h = r->half, f = r->factor, bx0, by0, bx1, by1;
    float gain = g->style == SPHERE_STYLE_FACE_DOTS ? 0.5f : 0.55f + 0.35f * p->glow;
    if (face_box(g, r->size, -0.8f, -0.7f, 0.8f, 0.75f, &bx0, &by0, &bx1, &by1)) {
        for (int y = by0 / f; y <= by1 / f && y < h; y++)
            for (int x = bx0 / f; x <= bx1 / f && x < h; x++) {
                float u, v;
                face_uv(g, ((float)x + 0.5f) * f, ((float)y + 0.5f) * f, &u, &v);
                float *d = r->glow_a + ((size_t)y * h + x) * 3;
                float b = blush_cov(g, p, u, v);
                if (b > 0.0f)
                    for (int k = 0; k < 3; k++) d[k] += b * PINK[k] * 0.9f;
                float m = face_cov(g, p, u, v, true);
                if (m <= 0.0f) continue;
                for (int k = 0; k < 3; k++) d[k] += m * g->color[k] * gain * 1.6f;
            }
    }
    for (int i = 0; i < p->nsym && i < SPHERE_MAX_SYMBOLS; i++) {
        const SphereSymbol *s = &p->sym[i];
        float reach = symbol_reach(s) + 0.1f, a = clamp01(s->alpha) * g->alpha;
        if (a <= 0.0f || !face_box(g, r->size, s->x - reach, s->y - reach, s->x + reach, s->y + reach, &bx0, &by0, &bx1, &by1))
            continue;
        const float *col = symbol_color(s->kind);
        for (int y = by0 / f; y <= by1 / f && y < h; y++)
            for (int x = bx0 / f; x <= bx1 / f && x < h; x++) {
                float u, v;
                face_uv(g, ((float)x + 0.5f) * f, ((float)y + 0.5f) * f, &u, &v);
                float m = symbol_cov(g, s, u, v) * a;
                if (m <= 0.0f) continue;
                float *d = r->glow_a + ((size_t)y * h + x) * 3;
                for (int k = 0; k < 3; k++) d[k] += m * col[k] * 1.2f;
            }
    }
}

/* Pone la cara de este cuadro en el lienzo: dónde va, qué tan grande y de qué color. */
static void face_setup(SphereRenderer *r, SphereStyle style, const Frame *f, float face_cx, float face_cy, float alpha)
{
    FaceGeom *g = &r->fg;
    const SphereFace *p = &r->face;
    memset(g, 0, sizeof *g);
    g->on = true;
    g->style = style;
    g->cx = face_cx;
    g->cy = face_cy;
    g->R = fmaxf(f->R, 1.0f);
    g->cosT = cosf(p->tilt);
    g->sinT = sinf(p->tilt);
    g->sx = p->sx > 0.2f ? p->sx : 0.2f;
    g->sy = p->sy > 0.2f ? p->sy : 0.2f;
    g->aa = 1.3f / g->R;
    g->alpha = clamp01(alpha * p->alpha);
    if (style == SPHERE_STYLE_FACE_EYES) {
        g->ex = 0.32f, g->ey = -0.04f - 0.02f * p->talk, g->rx = 0.17f * (1.0f + 0.05f * p->talk);
        g->ry = 0.24f * (1.0f + 0.05f * p->talk);
    } else if (style == SPHERE_STYLE_FACE_MOUTH) {
        g->ex = 0.34f, g->ey = -0.18f, g->rx = 0.1f, g->ry = 0.13f;
        g->my = 0.3f, g->mw0 = 0.3f, g->thick = 0.05f;
    } else { /* de puntos: más grande y grueso, para que le toquen puntos */
        g->ex = 0.36f, g->ey = -0.2f, g->rx = 0.14f, g->ry = 0.17f;
        g->my = 0.3f, g->mw0 = 0.36f, g->thick = 0.09f;
        g->aa *= 1.5f;
    }
    for (int k = 0; k < 3; k++) {
        g->color[k] = p->color[k] < 0 ? 0 : p->color[k] > 255 ? 255 : p->color[k];
        g->tint[k] = 255.0f * 0.55f + g->color[k] * 0.45f;
    }
    g->shade_on = p->shade > 0.001f && face_box(g, r->size, -0.8f, -0.7f, 0.8f, 0.5f, &g->shx0, &g->shy0, &g->shx1, &g->shy1);
}

#include <math.h>
#include <stdlib.h>

#include "fft.h"
#include "util.h"

struct Fft {
    int n;
    float *cs, *sn; /* cos y sin de 2πk/n, k < n/2 */
    int *rev;
};

Fft *fft_create(int n)
{
    Fft *f = xcalloc(1, sizeof *f);
    f->n = n;
    f->cs = xmalloc(sizeof(float) * (size_t)(n / 2));
    f->sn = xmalloc(sizeof(float) * (size_t)(n / 2));
    f->rev = xmalloc(sizeof(int) * (size_t)n);
    for (int k = 0; k < n / 2; k++) {
        double a = 2.0 * M_PI * k / n;
        f->cs[k] = (float)cos(a);
        f->sn[k] = (float)sin(a);
    }
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
    }
    return f;
}

void fft_destroy(Fft *f)
{
    if (!f) return;
    free(f->cs);
    free(f->sn);
    free(f->rev);
    free(f);
}

int fft_size(const Fft *f)
{
    return f->n;
}

void fft_run(const Fft *f, float *re, float *im, bool inverse)
{
    const int n = f->n;
    for (int i = 0; i < n; i++) {
        int r = f->rev[i];
        if (i < r) {
            float t = re[i];
            re[i] = re[r];
            re[r] = t;
            t = im[i];
            im[i] = im[r];
            im[r] = t;
        }
    }
    const float sign = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                const float wr = f->cs[j * step], wi = sign * f->sn[j * step];
                float *ar = &re[i + j], *ai = &im[i + j], *br = &re[i + j + half], *bi = &im[i + j + half];
                const float vr = *br * wr - *bi * wi, vi = *br * wi + *bi * wr;
                *br = *ar - vr;
                *bi = *ai - vi;
                *ar += vr;
                *ai += vi;
            }
        }
    }
}

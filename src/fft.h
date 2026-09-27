#ifndef SOKARI_FFT_H
#define SOKARI_FFT_H

/* FFT compleja de 2^k puntos (radix 2, en su lugar), para el cancelador de
   eco y para medir con cuánto retraso llega su voz al micrófono. */

#include <stdbool.h>

typedef struct Fft Fft;

Fft *fft_create(int n); /* n: potencia de 2 */
void fft_destroy(Fft *f);
int fft_size(const Fft *f);
/* inverse: la transformada inversa, sin dividir entre n. */
void fft_run(const Fft *f, float *re, float *im, bool inverse);

#endif

#ifndef SOKARI_AEC_H
#define SOKARI_AEC_H

/* Cancelador de eco: resta del micrófono la voz de Sokari. Su voz sale por la
   bocina, rebota en el cuarto y entra al micrófono junto con la tuya; como
   Sokari sabe exactamente qué dijo, aprende cómo le llega (el «camino del
   eco») y lo quita. Lo que queda es tu voz (y el ruido del cuarto).

   Es un filtro adaptativo en frecuencia por bloques (MDF: bloques de 8 ms,
   24 de ellos: 192 ms de eco) con dos filtros: uno de fondo que aprende
   siempre y el que se usa, que solo se cambia por el de fondo cuando este
   quita más eco de forma clara. Y el paso de aprendizaje baja cuando lo que
   queda no se parece a su voz (cuando le hablas encima), para no desaprender
   el eco con tu voz. Es la idea de J.-M. Valin, «On Adjusting the Learning
   Rate in Frequency Domain Echo Cancellation With Double-Talk» (2007).

   Muestras en la escala de 16 bits (±32768) pero en float. */

#include <stdbool.h>

#define AEC_BLOCK 128  /* muestras por bloque (8 ms a 16 kHz) */
#define AEC_PARTS 24   /* bloques de eco que modela: 192 ms */

typedef struct Aec Aec;

/* Energías (suma de cuadrados) de un bloque. */
typedef struct {
    float mic;      /* lo que oyó el micrófono */
    float ref;      /* lo que sonaba en la bocina (ya alineado) */
    float echo;     /* el eco que se le quitó */
    float out;      /* lo que queda */
    float residual; /* cuánto de lo que queda se calcula que todavía es eco */
    float cross;    /* lo que queda por el eco que se quitó (suma de productos): muy negativo si
                       se está quitando un eco que ya no está ahí (cambió el retraso o la bocina) */
} AecStats;

Aec *aec_create(void);
void aec_destroy(Aec *a);
/* Un bloque: mic (lo que oyó) y ref (lo que sonaba, alineado: el eco llega
   de 0 a 192 ms después). out: mic sin el eco (puede ser el mismo arreglo que
   mic). */
void aec_process(Aec *a, const float *mic, const float *ref, float *out, AecStats *st);
/* Solo avanza lo que sonaba (para llenar la historia sin micrófono). */
void aec_push_far(Aec *a, const float *ref);
/* Lo que sonaba antes ya no cuenta (hubo un hueco). Lo aprendido se queda. */
void aec_forget_far(Aec *a);
/* El eco ahora llega `blocks` bloques antes (+) o después (-) respecto a ref:
   lo aprendido se recorre (sirve cuando se vuelve a medir el retraso). */
void aec_shift(Aec *a, int blocks);
/* Qué tanto del eco queda sin quitar (0..1; 1 = todavía no aprende). */
float aec_leak(const Aec *a);
/* Mientras le hablas encima, lo que queda es sobre todo tu voz: la cuenta
   de cuánto eco se escapa no se mueve (tu voz la inflaría). El eco lo sigue
   aprendiendo, con el paso chico que le toca cuando hay otra voz: así, si
   cambió de verdad (otra bocina), igual se ajusta. */
void aec_freeze(Aec *a, bool frozen);
/* dst queda igual a src (los dos de aec_create): para guardar cómo estaba y
   deshacer lo que aprendió de un cuadro que resultó ser tu voz. */
void aec_copy(Aec *dst, const Aec *src);

#endif

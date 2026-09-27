#ifndef SOKARI_PROSODY_H
#define SOKARI_PROSODY_H

/* La voz con emoción: el ritmo, el tono y el volumen de cada frase según lo
   que siente al decirla. Se hace sobre el audio ya sintetizado, así suena
   igual de cambiado con la voz de Windows, Piper o espeak-ng (ninguna deja
   calibrar el tono igual, y Piper ni lo tiene). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "affect.h"

typedef struct {
    float tempo;    /* 1 = igual; 1.1 = 10 % más rápido (sin cambiar el tono) */
    float pitch;    /* semitonos: + más aguda, - más grave (sin cambiar la duración) */
    float gain_db;  /* volumen */
    float tremble;  /* 0..1: la voz tiembla (temor) */
    float pause_ms; /* + más silencio al final de la frase, - menos */
} Prosody;

/* Tu voz como es, sin cambios. */
Prosody prosody_neutral(void);
bool prosody_is_neutral(const Prosody *p);
/* La de una mezcla de emociones (los pesos de affect, que suman 1). */
Prosody prosody_for(const float weights[AFF_COUNT]);
/* Le aplica p al audio (mono de 16 bits). Se queda con pcm y regresa el
   nuevo (o el mismo, si no hubo nada que cambiar); *n es la nueva longitud. */
int16_t *prosody_apply(int16_t *pcm, size_t *n, int rate, const Prosody *p);

#endif

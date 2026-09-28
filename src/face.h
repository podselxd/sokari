#ifndef SOKARI_FACE_H
#define SOKARI_FACE_H

/* La cara de Sokari (beta): de su estado afectivo (affect.c) y de lo que está
   haciendo sale la pose de cada cuadro, que sphere.c dibuja.

   - La forma de los ojos y la boca es la mezcla de las 6 poses con los pesos
     de las emociones (no una tabla por emoción), y el color también.
   - Los gestos salen de lo que acaba de pasar: asiente («gracias», algo que
     salió), niega (algo falló), guiña (un saludo), ladea con «?» (pide un
     «sí»), rebota, tiembla, se encoge, se infla o se aparta (cuando cambia la
     emoción). Pensando mira arriba, escuchando se inclina, y siempre respira
     y parpadea (a veces dos veces seguidas) y, callada, mira alrededor.
   - «Qué tanto se le nota» (poco, normal, mucho) escala todo eso. */

#include <stdbool.h>

#include "affect.h"
#include "sphere.h"

typedef enum { FACE_IDLE, FACE_LISTENING, FACE_THINKING, FACE_SPEAKING } FaceActivity; /* como JvState */

typedef enum { FACE_LEVEL_LOW, FACE_LEVEL_NORMAL, FACE_LEVEL_HIGH, FACE_LEVEL_COUNT } FaceLevel;

typedef enum {
    FACE_G_NONE,
    FACE_G_NOD,       /* asiente */
    FACE_G_SHAKE,     /* niega */
    FACE_G_BOUNCE,    /* alegría: rebota */
    FACE_G_TREMBLE,   /* temor: tiembla */
    FACE_G_SHRINK,    /* tristeza: se encoge */
    FACE_G_INFLATE,   /* furia: se infla */
    FACE_G_RECOIL,    /* desagrado: se aparta */
    FACE_G_DOUBT,     /* duda: ladea la cabeza */
    FACE_G_BLINK2,    /* parpadeo doble */
    FACE_G_LOOK,      /* mira alrededor */
    FACE_G_WINK,      /* guiño al saludar */
    FACE_G_HAPPY,     /* ojos felices «^ ^» */
    FACE_G_SURPRISE,  /* sorpresa: ojos grandes, un saltito y «!» */
    FACE_G_SIGH,      /* suspiro: toma aire, lo suelta y baja la mirada */
    FACE_G_BLUSH,     /* sonrojo: chapitas y mirada de lado */
    FACE_G_COUNT
} FaceGesture;

typedef struct {
    FaceActivity activity;
    float voice;         /* 0..1: su voz al hablar, la tuya al escucharte */
    float pulse;         /* 0..1: su voz sílaba por sílaba */
    AffectState affect;  /* affect_get() */
    AffectCue cue;       /* affect_last_cue() */
    unsigned cue_seq;
    FaceLevel level;     /* «Qué tanto se le nota» */
    bool symbols;        /* lágrima, destellos, gota de sudor, «?», enojo, «…» */
    float look_x, look_y; /* hacia dónde va al moverse por el escritorio (-1..1); 0 = al frente */
    bool walking;        /* caminando o saltando: rebota un poco */
} FaceInput;

typedef struct Face Face;

Face *face_create(unsigned seed);
void face_destroy(Face *f);
/* Un cuadro: avanza dt segundos y llena la pose. */
void face_step(Face *f, double dt, const FaceInput *in, SphereFace *out);
/* Los colores de la esfera con la cara: la mezcla de las emociones (lo
   neutral es el color de siempre, el de neutral). */
void face_sphere_colors(const AffectState *a, const SphereParams *neutral, SphereParams *out);
/* Para las pruebas y el botón Probar: un gesto ya. */
void face_play(Face *f, FaceGesture g);
FaceGesture face_gesture(const Face *f);
const char *face_gesture_name(FaceGesture g);

#endif

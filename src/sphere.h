#ifndef SOKARI_SPHERE_H
#define SOKARI_SPHERE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SPHERE_STYLE_DOTS = 0,  /* halo de puntos (default) */
    SPHERE_STYLE_LINES = 1, /* meridianos, el diseño de Main.dc.html */
} SphereStyle;

typedef struct {
    float low[3];  /* colorLow  (0-255) */
    float high[3]; /* colorHigh (0-255) */
    float rotation_speed;
    float ripple;
    float glow; /* radio del resplandor, en píxeles del lienzo original de 960 */
} SphereParams;

extern const SphereParams SPHERE_IDLE;
extern const SphereParams SPHERE_SPEAK;

typedef struct SphereRenderer SphereRenderer;

SphereRenderer *sphere_create(int size);
/* fit (0..1]: la esfera se dibuja a esa fracción de su tamaño normal y lo
   demás del lienzo queda como espacio (transparente en la esfera flotante)
   para que las ondas no se corten. sphere_create(size) usa fit = 1. */
SphereRenderer *sphere_create_fit(int size, float fit);
/* Cuánto más grande tiene que ser el lienzo, con la esfera del mismo tamaño,
   para que ese estilo nunca se salga al hablar: las líneas se deforman mucho
   más que los puntos. Se usa con sphere_create_fit(lienzo, 1 / room). */
float sphere_room(SphereStyle style);
void sphere_destroy(SphereRenderer *r);
int sphere_size(const SphereRenderer *r);

/* Dibuja un cuadro size x size en out (BGRA, stride en píxeles). voice (0..1)
   es la energía de la voz de Sokari en este instante: agranda la esfera,
   agrega una ondulación rápida y más destellos. pulse (0..1) es esa misma voz
   sílaba por sílaba (sube y baja rápido): en el halo de puntos hace que la
   esfera palpite, se agrande y se aclare hacia blanco con cada sílaba.
   Con premultiplied, el alfa sale de la intensidad (ventana flotante
   transparente); si no, fondo negro opaco. */
void sphere_render(SphereRenderer *r, double t, double angle, double voice_t, const SphereParams *p, float voice,
                   float pulse, SphereStyle style, uint32_t *out, int stride, bool premultiplied);

void sphere_lerp(SphereParams *out, const SphereParams *a, const SphereParams *b, float f);

/* Cómo entra y sale de la pantalla (Configuración → Pantalla). */
typedef enum {
    SPHERE_ANIM_MATERIALIZE = 0, /* llega de afuera en pedazos y se junta; al irse se dispersa */
    SPHERE_ANIM_SLIDE = 1,       /* sube desde abajo de la pantalla; al irse baja (ver sphere_slide_offset) */
    SPHERE_ANIM_ZOOM = 2,        /* crece desde un punto; al irse se encoge */
    SPHERE_ANIM_NONE = 3,        /* aparece y desaparece de golpe */
    SPHERE_ANIM_COUNT
} SphereAnim;

#define SPHERE_ENTER_SECONDS 1.0
#define SPHERE_LEAVE_SECONDS 0.8

/* Qué tanto está en pantalla para los cuadros que siguen: 0 no se dibuja
   nada, 1 es la esfera de siempre (idéntica, píxel por píxel). */
void sphere_set_presence(SphereRenderer *r, SphereAnim anim, float presence);

/* Deslizarse no se dibuja dentro del lienzo: la ventana (o el cuadro) se
   mueve. Cuánto más abajo va, en fracciones del recorrido hasta quedar fuera
   de la pantalla: 1 fuera, 0 en su lugar; un poco menos de 0 es el rebote. */
float sphere_slide_offset(float presence);

/* El reloj de la animación. La ventana se muestra antes de entrar y se
   esconde cuando sphere_appear_step dice que ya se fue. */
typedef struct {
    float presence; /* 0 fuera, 1 en pantalla */
    int dir;        /* +1 entrando, -1 saliendo, 0 quieta */
    int then;       /* Probar: el paso que sigue (+1 o -1) */
    double hold;    /* Probar: cuánto espera antes de ese paso */
    SphereAnim anim;
} SphereAppear;

/* En pantalla y quieta. */
void sphere_appear_init(SphereAppear *a);
/* Entra desde donde va (si se estaba yendo, regresa sin brincos) o, con
   from_zero, desde fuera. */
void sphere_appear_enter(SphereAppear *a, SphereAnim anim, bool from_zero);
/* Se va. true: no hay animación (Ninguna), escóndela ya. */
bool sphere_appear_leave(SphereAppear *a, SphereAnim anim);
/* El botón Probar. shown: sale, espera un momento y vuelve a entrar. Si no:
   entra, se queda un segundo y se va. */
void sphere_appear_test(SphereAppear *a, SphereAnim anim, bool shown);
/* Avanza dt segundos. true una sola vez, cuando terminó de irse: ahí se
   esconde la ventana. */
bool sphere_appear_step(SphereAppear *a, double dt);
/* Se está moviendo (o esperando en Probar): hay que seguir dibujando. */
bool sphere_appear_moving(const SphereAppear *a);

#endif

#ifndef SOKARI_SPHERE_H
#define SOKARI_SPHERE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SPHERE_STYLE_DOTS = 0,  /* halo de puntos (default) */
    SPHERE_STYLE_LINES = 1, /* meridianos, el diseño de Main.dc.html */
    /* Las caras (beta), sobre el halo de puntos. Sin sphere_set_face se ven
       como el halo solo. */
    SPHERE_STYLE_FACE_EYES = 2,  /* solo ojos de luz: la de fábrica de las caras */
    SPHERE_STYLE_FACE_MOUTH = 3, /* ojos y boca de luz */
    SPHERE_STYLE_FACE_DOTS = 4,  /* los puntos de la esfera forman la cara */
    SPHERE_STYLE_COUNT
} SphereStyle;

static inline bool sphere_style_is_face(SphereStyle s)
{
    return s >= SPHERE_STYLE_FACE_EYES && s < SPHERE_STYLE_COUNT;
}

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

/* Los símbolos alrededor de la cara (se pueden apagar). */
typedef enum {
    SPHERE_SYM_TEAR,     /* lágrima (tristeza) */
    SPHERE_SYM_SWEAT,    /* gota de sudor (temor) */
    SPHERE_SYM_SPARK,    /* destello (alegría) */
    SPHERE_SYM_QUESTION, /* «?» (duda) */
    SPHERE_SYM_ANGER,    /* la marca de enojo (furia) */
    SPHERE_SYM_DOTS,     /* «…» (pensando): size = cuántos puntos, de 0 a 3 */
} SphereSymbolKind;

typedef struct {
    SphereSymbolKind kind;
    float x, y;  /* en radios de la esfera, desde el centro de la cara */
    float size;  /* 1 = normal; 0 no se ve */
    float alpha; /* 0..1 */
    float rot;   /* radianes (destellos) */
} SphereSymbol;

#define SPHERE_MAX_SYMBOLS 6

/* La pose de la cara en un cuadro; la calcula face.c. Las medidas van en
   radios de la esfera y 0 es «como siempre». */
typedef struct {
    /* los ojos */
    float eye_w, eye_h;        /* escala (1 = normal) */
    float lid_top, lid_bot;    /* cuánto tapan los párpados (0..1) */
    float lid_tilt;            /* + furia (adentro abajo), - tristeza (afuera abajo) */
    float happy;               /* ojos en arco «^ ^» (0..1) */
    float asym;                /* uno entrecerrado y el otro abierto (desagrado, duda) */
    float round;               /* 2 = óvalo, 4 = cuadrado redondeado (solo ojos) */
    float eye_dy;              /* más arriba (-) o más abajo (+) */
    float shade;               /* sombra en diagonal arriba afuera (preocupación), 0..1 */
    float blink[2];            /* 0 abierto, 1 cerrado (izquierdo, derecho) */
    float gaze_x, gaze_y;      /* hacia dónde ve */
    /* la boca */
    float smile;               /* -1 triste, 1 sonrisa */
    float mouth_open, mouth_w, wave, mouth_o, mouth_asym;
    float talk;                /* 0..1: la voz, sílaba por sílaba */
    /* toda la cara (y la esfera con ella) */
    float fx, fy;              /* la cara sobre la esfera */
    float tilt;                /* radianes */
    float scale;               /* la esfera entera (1 = normal) */
    float sx, sy;              /* aplastar y estirar */
    float sphere_dx, sphere_dy; /* la esfera la sigue */
    float glow;                /* 1 = normal */
    float alpha;               /* 1 = normal; menos al entrar o salir */
    float color[3];            /* el color de la emoción (0-255) */
    int nsym;
    SphereSymbol sym[SPHERE_MAX_SYMBOLS];
} SphereFace;

/* La cara neutral, quieta y de frente. */
void sphere_face_neutral(SphereFace *f);
/* La cara de los cuadros que siguen (se copia); NULL la quita. Solo se ve
   con los estilos de cara. */
void sphere_set_face(SphereRenderer *r, const SphereFace *face);

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

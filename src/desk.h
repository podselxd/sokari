#ifndef SOKARI_DESK_H
#define SOKARI_DESK_H

/* Moverse sola por el escritorio (solo con la esfera flotante).

   Lo que hay en pantalla se sabe sin capturas: los monitores (y lo que deja
   libre la barra de tareas o el panel), las ventanas, la que tiene el foco,
   si algo está en pantalla completa, el cursor y dónde escribes. Con eso
   Sokari camina por el borde de abajo, se sienta sobre la ventana activa o
   se asoma por un lado, mirando hacia donde va.

   Nunca se mueve con algo en pantalla completa, ni a donde tape el cursor,
   donde escribes o la ventana activa, ni fuera de la pantalla. Que no se
   mueva mientras habla o escucha lo decide quien llama (allowed).

   Todo aquí es lógica pura, igual en Windows y Linux: cada sistema llena el
   DeskView y pone la ventana donde diga desk_step (sin quitarte el foco). */

#include <stdbool.h>

typedef struct {
    int x, y, w, h;
} DeskRect;

#define DESK_MAX_MONITORS 8
#define DESK_MAX_WINDOWS 64

typedef struct {
    DeskRect area; /* el monitor entero */
    DeskRect work; /* sin la barra de tareas o el panel */
} DeskMonitor;

typedef struct {
    DeskRect r;
    bool active; /* la que tiene el foco */
} DeskWindow;

typedef struct {
    int nmon;
    DeskMonitor mon[DESK_MAX_MONITORS];
    int nwin; /* de la de más arriba a la de más abajo, sin la de Sokari */
    DeskWindow win[DESK_MAX_WINDOWS];
    int cursor_x, cursor_y;
    bool has_caret;
    DeskRect caret;  /* donde escribes, si se sabe */
    bool fullscreen; /* algo en pantalla completa enfrente (un juego, un video) */
} DeskView;

typedef enum { DESK_BOTTOM, DESK_ON_WINDOW, DESK_SIDE } DeskPlace;

#define DESK_MAX_LEGS 3

typedef struct {
    float x, y; /* a dónde llega este tramo (esquina de su ventana) */
    int kind;   /* caminar, trepar o saltar */
} DeskLeg;

typedef struct {
    /* Su ventana: dónde está (la esquina de arriba a la izquierda) y su lado.
       El cuerpo es el cuadrado del centro, de lado body: la esfera sin el
       espacio de sus ondas, que es transparente. */
    float x, y;
    int size, body;
    DeskPlace place;
    DeskLeg legs[DESK_MAX_LEGS];
    int nlegs, leg;
    float leg_x0, leg_y0; /* de dónde salió el tramo de ahora */
    float leg_t, leg_len; /* cuánto lleva y cuánto dura (s) */
    double rest;          /* cuánto más descansa antes de ir a otro lado (s) */
    float look_x, look_y; /* hacia dónde mira (-1..1) */
    unsigned rng;
} DeskWalker;

typedef struct {
    int x, y;             /* su ventana va aquí */
    float look_x, look_y; /* hacia dónde mira (-1..1): a donde va */
    bool moving;          /* caminando, trepando o saltando: la cara rebota */
} DeskStep;

void desk_init(DeskWalker *w, int x, int y, int size, int body, unsigned seed);
/* La moviste tú (la arrastraste): se queda ahí y vuelve a descansar. */
void desk_place(DeskWalker *w, int x, int y);
/* Un paso de dt segundos. allowed: puede moverse ahora (la opción prendida,
   la esfera flotante, no habla ni escucha, no le dijiste «quieta» y ya pasó
   el tiempo sin hablarle). Si no, o si hay algo en pantalla completa, se
   queda donde está (si iba caminando, se detiene ahí). Devuelve true si se
   movió. */
bool desk_step(DeskWalker *w, const DeskView *v, double dt, bool allowed, DeskStep *out);

/* ¿Puede estar su cuerpo ahí? Dentro de un monitor (sin la barra de tareas),
   sin tapar la ventana activa, el cursor ni donde escribes. */
bool desk_body_ok(const DeskView *v, const DeskRect *body);
/* Su cuerpo, si su ventana está en (x, y). */
DeskRect desk_body_at(const DeskWalker *w, float x, float y);
/* Cuánto aire deja alrededor del cursor y de donde escribes. */
int desk_margin(const DeskWalker *w);

#endif

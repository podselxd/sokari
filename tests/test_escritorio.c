/* Moverse sola por el escritorio (src/desk.c), en escritorios simulados: uno
   o dos monitores, la barra de tareas abajo, arriba o a un lado, ventanas
   (a veces maximizadas), el cursor que se mueve y a veces donde escribes.
   Cada vez que se mueve: nunca queda sobre el cursor ni donde escribes,
   nunca empieza a tapar la ventana activa y nunca se sale de la pantalla.
   Con algo en pantalla completa, o sin permiso, no se mueve. Y sí se mueve
   (camina, se sienta sobre la ventana, se asoma por un lado), mirando hacia
   donde va. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "pantalla.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

static unsigned g_rng = 12345;
static float frand(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (float)((g_rng >> 8) & 0xFFFFFF) / 16777216.0f;
}
static int irand(int lo, int hi)
{
    return lo + (int)(frand() * (float)(hi - lo + 1));
}

static bool overlap(const DeskRect *a, const DeskRect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static bool inside(const DeskRect *o, const DeskRect *r)
{
    return r->x >= o->x && r->y >= o->y && r->x + r->w <= o->x + o->w && r->y + r->h <= o->y + o->h;
}

/* Un escritorio al azar. */
static void random_desk(DeskView *v)
{
    memset(v, 0, sizeof *v);
    v->nmon = frand() < 0.4f ? 2 : 1;
    int x = 0;
    for (int i = 0; i < v->nmon; i++) {
        int w = frand() < 0.5f ? 1920 : 1366, h = w == 1920 ? 1080 : 768;
        DeskMonitor *m = &v->mon[i];
        m->area = (DeskRect){x, i ? irand(-200, 200) : 0, w, h};
        m->work = m->area;
        int bar = 48, side = irand(0, 3);
        if (side == 0) m->work.h -= bar;                          /* abajo */
        else if (side == 1) m->work.y += bar, m->work.h -= bar;   /* arriba */
        else if (side == 2) m->work.x += bar, m->work.w -= bar;   /* izquierda */
        else m->work.w -= bar;                                     /* derecha */
        x += w;
    }
    v->nwin = irand(0, 6);
    for (int i = 0; i < v->nwin; i++) {
        const DeskMonitor *m = &v->mon[irand(0, v->nmon - 1)];
        DeskRect r;
        if (frand() < 0.15f) {
            r = m->work; /* maximizada */
        } else {
            r.w = irand(300, m->work.w - 50);
            r.h = irand(200, m->work.h - 50);
            r.x = m->work.x + irand(0, m->work.w - r.w);
            r.y = m->work.y + irand(0, m->work.h - r.h);
        }
        v->win[i].r = r;
    }
    if (v->nwin && frand() < 0.85f) v->win[0].active = true; /* la de enfrente */
    v->cursor_x = v->mon[0].area.x + irand(0, v->mon[0].area.w - 1);
    v->cursor_y = v->mon[0].area.y + irand(0, v->mon[0].area.h - 1);
    v->has_caret = v->nwin && frand() < 0.4f;
    if (v->has_caret) {
        const DeskRect *a = &v->win[0].r;
        v->caret = (DeskRect){a->x + irand(0, a->w - 2), a->y + irand(0, a->h - 20), 2, 18};
    }
}

static const DeskRect *active_rect(const DeskView *v)
{
    for (int i = 0; i < v->nwin; i++)
        if (v->win[i].active) return &v->win[i].r;
    return NULL;
}

typedef struct {
    int moves, bad_cursor, bad_caret, bad_active, bad_screen, moved_fullscreen, moved_blocked, wrong_look;
    int walked_right, walked_left, on_window, on_side, bottom;
} Stats;

/* dt de 1/30 s durante secs segundos; el cursor se mueve solo. */
static void simulate(DeskView *v, DeskWalker *w, double secs, bool allowed, Stats *s)
{
    int n = (int)(secs * 30);
    for (int i = 0; i < n; i++) {
        /* El cursor: a veces salta, a veces se arrastra. */
        if (frand() < 0.02f) {
            v->cursor_x = v->mon[0].area.x + irand(0, v->mon[0].area.w - 1);
            v->cursor_y = v->mon[0].area.y + irand(0, v->mon[0].area.h - 1);
        } else {
            v->cursor_x += irand(-6, 6);
            v->cursor_y += irand(-6, 6);
        }
        DeskRect before = desk_body_at(w, w->x, w->y);
        bool active_before = active_rect(v) && overlap(active_rect(v), &before);
        DeskStep st;
        float x0 = w->x;
        bool moved = desk_step(w, v, 1.0 / 30, allowed, &st);
        if (!moved) continue;
        s->moves++;
        if (!allowed) s->moved_blocked++;
        if (v->fullscreen) s->moved_fullscreen++;
        DeskRect b = desk_body_at(w, w->x, w->y);
        int m = desk_margin(w);
        DeskRect near = {b.x - m, b.y - m, b.w + 2 * m, b.h + 2 * m}, cur = {v->cursor_x, v->cursor_y, 1, 1};
        if (overlap(&near, &cur)) s->bad_cursor++;
        if (v->has_caret && overlap(&near, &v->caret)) s->bad_caret++;
        if (active_rect(v) && overlap(active_rect(v), &b) && !active_before) s->bad_active++;
        bool on = false;
        for (int k = 0; k < v->nmon && !on; k++) on = inside(&v->mon[k].work, &b);
        if (!on) s->bad_screen++;
        /* Caminando de lado, mira hacia donde va (ya girada, tras un momento). */
        if (st.moving && fabsf(w->x - x0) > 1.5f && fabsf(w->legs[w->leg].y - w->leg_y0) < 1) {
            if (w->x > x0) s->walked_right++, s->wrong_look += w->leg_t > 0.8f && st.look_x < 0.3f;
            else s->walked_left++, s->wrong_look += w->leg_t > 0.8f && st.look_x > -0.3f;
        }
        if (!st.moving) {
            if (w->place == DESK_ON_WINDOW) s->on_window++;
            else if (w->place == DESK_SIDE) s->on_side++;
            else s->bottom++;
        }
    }
}

int wmain(void)
{
    const int SIZE = 220, BODY = 160, SCENES = 400;
    Stats s;
    memset(&s, 0, sizeof s);
    int scenes_moved = 0;
    for (int k = 0; k < SCENES; k++) {
        DeskView v;
        random_desk(&v);
        DeskWalker w;
        const DeskRect *wk = &v.mon[0].work;
        desk_init(&w, wk->x + wk->w - SIZE, wk->y + wk->h - SIZE, SIZE, BODY, (unsigned)k + 1);
        int before = s.moves;
        simulate(&v, &w, 90, true, &s);
        if (s.moves > before) scenes_moved++;
    }
    printf("      %d escritorios, 90 s cada uno: se movió en %d · %d pasos · caminó a la derecha %d y a la izquierda %d "
           "· quieta abajo %d, sobre la ventana %d, de lado %d\n",
           SCENES, scenes_moved, s.moves, s.walked_right, s.walked_left, s.bottom, s.on_window, s.on_side);
    check(s.bad_cursor == 0, "nunca queda sobre el cursor");
    check(s.bad_caret == 0, "ni sobre donde escribes");
    check(s.bad_active == 0, "nunca empieza a tapar la ventana activa");
    check(s.bad_screen == 0, "nunca se sale de la pantalla (ni se mete en la barra de tareas), con uno o dos monitores");
    check(scenes_moved > SCENES * 3 / 4, "sí se mueve en casi todos los escritorios");
    check(s.on_window > 0 && s.on_side > 0 && s.bottom > 0, "camina abajo, se sienta sobre la ventana y se asoma de lado");
    check(s.walked_right > 0 && s.walked_left > 0 && s.wrong_look == 0, "caminando, mira hacia donde va");

    /* Pantalla completa y sin permiso: nada. */
    Stats q;
    memset(&q, 0, sizeof q);
    for (int k = 0; k < 60; k++) {
        DeskView v;
        random_desk(&v);
        v.fullscreen = true;
        DeskWalker w;
        const DeskRect *wk = &v.mon[0].work;
        desk_init(&w, wk->x, wk->y + wk->h - SIZE, SIZE, BODY, (unsigned)k + 7);
        simulate(&v, &w, 30, true, &q);
        v.fullscreen = false;
        simulate(&v, &w, 30, false, &q);
    }
    check(q.moves == 0, "con algo en pantalla completa (juego, video) o sin permiso, no se mueve");

    /* Si le quitan el permiso a media caminata, se queda ahí. */
    DeskView v;
    memset(&v, 0, sizeof v);
    v.nmon = 1;
    v.mon[0].area = (DeskRect){0, 0, 1920, 1080};
    v.mon[0].work = (DeskRect){0, 0, 1920, 1032};
    v.cursor_x = 960, v.cursor_y = 100;
    DeskWalker w;
    desk_init(&w, 1920 - SIZE, 1032 - SIZE + (SIZE - BODY) / 2, SIZE, BODY, 3);
    DeskStep st;
    int guard = 0;
    while (!w.nlegs && guard++ < 3000) desk_step(&w, &v, 1.0 / 30, true, &st);
    for (int i = 0; i < 10; i++) desk_step(&w, &v, 1.0 / 30, true, &st);
    float fx = w.x, fy = w.y;
    bool moved = false;
    for (int i = 0; i < 300; i++) moved |= desk_step(&w, &v, 1.0 / 30, false, &st);
    check(guard < 3000 && !moved && w.x == fx && w.y == fy, "si le hablas a media caminata, se queda donde iba");

    /* Con una ventana maximizada activa no hay dónde ponerse sin taparla. */
    v.nwin = 1;
    v.win[0] = (DeskWindow){{0, 0, 1920, 1032}, true};
    desk_init(&w, 0, 0, SIZE, BODY, 5);
    w.x = -500; /* fuera de todo: cualquier lugar la taparía */
    int moves = 0;
    for (int i = 0; i < 900; i++) moves += desk_step(&w, &v, 1.0 / 30, true, &st);
    check(moves == 0, "con la ventana activa maximizada no se mete encima: se queda quieta");

    /* La captura va a la IA en JPEG y a lo más de 1280 de ancho (menos cupo). */
    int cw = 2560, ch = 1440, ow = 0, oh = 0;
    unsigned char *rgb = malloc((size_t)cw * ch * 3);
    for (int i = 0; i < cw * ch * 3; i++) rgb[i] = (unsigned char)(i * 7 % 251);
    char *b64 = screen_jpeg_base64(rgb, cw, ch, &ow, &oh);
    free(rgb);
    check(b64 && !strncmp(b64, "/9j/", 4) && ow == 1280 && oh == 720,
          "la captura de 2560x1440 va como JPEG de 1280x720");
    free(b64);

    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

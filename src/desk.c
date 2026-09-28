#include "desk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { LEG_WALK, LEG_CLIMB, LEG_HOP };

/* Lo que tiene de malo un lugar para su cuerpo. */
enum { BAD_SCREEN = 1, BAD_ACTIVE = 2, BAD_CURSOR = 4, BAD_CARET = 8 };

static float rnd(DeskWalker *w)
{
    w->rng = w->rng * 1664525u + 1013904223u;
    return (float)(w->rng >> 8) / 16777216.0f;
}

static bool inside(const DeskRect *outer, const DeskRect *r)
{
    return r->x >= outer->x && r->y >= outer->y && r->x + r->w <= outer->x + outer->w &&
           r->y + r->h <= outer->y + outer->h;
}

static bool overlap(const DeskRect *a, const DeskRect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static const DeskWindow *active_window(const DeskView *v)
{
    for (int i = 0; i < v->nwin; i++)
        if (v->win[i].active) return &v->win[i];
    return NULL;
}

static int margin_for(int body)
{
    return body / 4 > 24 ? body / 4 : 24;
}

int desk_margin(const DeskWalker *w)
{
    return margin_for(w->body);
}

DeskRect desk_body_at(const DeskWalker *w, float x, float y)
{
    int pad = (w->size - w->body) / 2;
    DeskRect r = {(int)lroundf(x) + pad, (int)lroundf(y) + pad, w->body, w->body};
    return r;
}

static unsigned body_bad(const DeskView *v, const DeskRect *b)
{
    unsigned bad = BAD_SCREEN;
    for (int i = 0; i < v->nmon && bad; i++)
        if (inside(&v->mon[i].work, b)) bad = 0;
    const DeskWindow *a = active_window(v);
    if (a && overlap(&a->r, b)) bad |= BAD_ACTIVE;
    int m = margin_for(b->w);
    DeskRect near = {b->x - m, b->y - m, b->w + 2 * m, b->h + 2 * m};
    DeskRect cur = {v->cursor_x, v->cursor_y, 1, 1};
    if (overlap(&near, &cur)) bad |= BAD_CURSOR;
    if (v->has_caret && overlap(&near, &v->caret)) bad |= BAD_CARET;
    return bad;
}

bool desk_body_ok(const DeskView *v, const DeskRect *body)
{
    return body_bad(v, body) == 0;
}

/* Si ya estaba tapando la ventana activa (la abriste o la agrandaste debajo
   de ella), puede seguir tapándola mientras se quita; lo demás, nunca. */
static bool step_ok(unsigned before, unsigned after)
{
    if (after & (BAD_SCREEN | BAD_CURSOR | BAD_CARET)) return false;
    return !(after & BAD_ACTIVE) || (before & BAD_ACTIVE);
}

static int monitor_of(const DeskView *v, float cx, float cy)
{
    int best = -1;
    float bd = 0;
    for (int i = 0; i < v->nmon; i++) {
        const DeskRect *a = &v->mon[i].area;
        float dx = cx < a->x ? a->x - cx : cx > a->x + a->w ? cx - (a->x + a->w) : 0;
        float dy = cy < a->y ? a->y - cy : cy > a->y + a->h ? cy - (a->y + a->h) : 0;
        float d = dx * dx + dy * dy;
        if (best < 0 || d < bd) best = i, bd = d;
    }
    return best;
}

void desk_init(DeskWalker *w, int x, int y, int size, int body, unsigned seed)
{
    memset(w, 0, sizeof *w);
    w->size = size;
    w->body = body > 0 && body <= size ? body : size;
    w->rng = seed ? seed : 1;
    desk_place(w, x, y);
    w->rest = 0; /* la primera vez sale en cuanto puede */
}

void desk_place(DeskWalker *w, int x, int y)
{
    w->x = (float)x;
    w->y = (float)y;
    w->nlegs = w->leg = 0;
    w->rest = 6 + 6 * rnd(w);
}

static float walk_speed(const DeskWalker *w)
{
    float s = 0.9f * (float)w->body;
    return s < 60 ? 60 : s;
}

/* Dónde va en el tramo, a una fracción s (0..1) de su duración. */
static void leg_pos(const DeskWalker *w, const DeskLeg *l, float x0, float y0, float s, float *x, float *y)
{
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (l->kind == LEG_HOP) {
        /* Un salto: sube y baja en arco. */
        float dx = l->x - x0, dy = l->y - y0;
        float h = 0.45f * (float)w->body + 0.15f * fabsf(dx);
        if (dy < 0) h += -dy * 0.25f; /* hacia arriba: un poco más alto */
        *x = x0 + dx * s;
        *y = y0 + dy * s - h * 4 * s * (1 - s);
    } else {
        *x = x0 + (l->x - x0) * s;
        *y = y0 + (l->y - y0) * s;
    }
}

static float leg_duration(const DeskWalker *w, const DeskLeg *l, float x0, float y0)
{
    float d = hypotf(l->x - x0, l->y - y0);
    switch (l->kind) {
    case LEG_WALK: return d / walk_speed(w);
    case LEG_CLIMB: return d / (0.65f * walk_speed(w));
    default: {
        float t = 0.45f + d / (4.0f * (float)w->body);
        return t > 1.2f ? 1.2f : t;
    }
    }
}

/* ¿Se puede recorrer? Cada tantos píxeles, su cuerpo tiene que caber. */
static bool path_ok(const DeskWalker *w, const DeskView *v, const DeskLeg *legs, int n)
{
    float x0 = w->x, y0 = w->y;
    DeskRect b = desk_body_at(w, x0, y0);
    unsigned before = body_bad(v, &b);
    for (int i = 0; i < n; i++) {
        float d = hypotf(legs[i].x - x0, legs[i].y - y0);
        if (legs[i].kind == LEG_HOP) d *= 1.6f; /* el arco es más largo */
        int steps = (int)(d / ((float)w->body / 6.0f)) + 2;
        for (int k = 1; k <= steps; k++) {
            float x, y;
            leg_pos(w, &legs[i], x0, y0, (float)k / (float)steps, &x, &y);
            b = desk_body_at(w, x, y);
            unsigned after = body_bad(v, &b);
            if (!step_ok(before, after)) return false;
            before = after;
        }
        x0 = legs[i].x;
        y0 = legs[i].y;
    }
    return before == 0; /* llega a un lugar bueno */
}

/* Escoge a dónde ir y arma el camino. */
static bool plan(DeskWalker *w, const DeskView *v)
{
    int pad = (w->size - w->body) / 2;
    DeskRect b0 = desk_body_at(w, w->x, w->y);
    int m = monitor_of(v, (float)b0.x + (float)b0.w * 0.5f, (float)b0.y + (float)b0.h * 0.5f);
    if (m < 0) return false;
    const DeskRect *wa = &v->mon[m].work;
    if (wa->w < w->body || wa->h < w->body) return false;
    const DeskWindow *a = active_window(v);
    const float bottom = (float)(wa->y + wa->h - w->body), left = (float)wa->x,
                right = (float)(wa->x + wa->w - w->body);
    /* En qué «piso» está: el borde de abajo, un lado o arriba de la ventana activa. */
    const bool on_bottom = fabsf((float)b0.y - bottom) <= 2;
    const int on_side = fabsf((float)b0.x - left) <= 2 ? -1 : fabsf((float)b0.x - right) <= 2 ? 1 : 0;
    const bool on_window = a && abs(b0.y + w->body - a->r.y) <= 2 && b0.x + w->body > a->r.x && b0.x < a->r.x + a->r.w;
    for (int attempt = 0; attempt < 16; attempt++) {
        float r = rnd(w), bx = 0, by = 0;
        DeskPlace place = r < 0.5f ? DESK_BOTTOM : r < 0.8f ? DESK_ON_WINDOW : DESK_SIDE;
        if (place == DESK_BOTTOM) {
            by = bottom;
            bx = left + rnd(w) * (right - left);
        } else if (place == DESK_ON_WINDOW) {
            if (!a) continue;
            float lo = fmaxf((float)a->r.x, left), hi = fminf((float)(a->r.x + a->r.w - w->body), right);
            by = (float)(a->r.y - w->body);
            if (hi < lo || by < (float)wa->y) continue;
            bx = lo + rnd(w) * (hi - lo);
        } else {
            bx = rnd(w) < 0.5f ? left : right;
            by = (float)wa->y + (0.35f + 0.5f * rnd(w)) * (bottom - (float)wa->y);
        }
        bx = floorf(bx);
        by = floorf(by);
        if (fabsf(bx - (float)b0.x) + fabsf(by - (float)b0.y) < 1.2f * (float)w->body) continue;
        DeskRect bt = {(int)bx, (int)by, w->body, w->body};
        if (body_bad(v, &bt)) continue;
        /* El camino: por el mismo piso camina o trepa; si no, salta. */
        DeskLeg legs[DESK_MAX_LEGS];
        int n = 0;
        const float tx = bx - (float)pad, ty = by - (float)pad;
        if (place == DESK_BOTTOM && on_bottom) {
            legs[n++] = (DeskLeg){tx, ty, LEG_WALK};
        } else if (place == DESK_BOTTOM && on_side) {
            legs[n++] = (DeskLeg){w->x, ty, LEG_CLIMB}; /* baja por el lado */
            legs[n++] = (DeskLeg){tx, ty, LEG_WALK};
        } else if (place == DESK_SIDE && on_bottom) {
            legs[n++] = (DeskLeg){tx, w->y, LEG_WALK}; /* a la esquina */
            legs[n++] = (DeskLeg){tx, ty, LEG_CLIMB};  /* y sube */
        } else if (place == DESK_SIDE && on_side && (bx == left) == (on_side < 0)) {
            legs[n++] = (DeskLeg){tx, ty, LEG_CLIMB};
        } else if (place == DESK_ON_WINDOW && on_window) {
            legs[n++] = (DeskLeg){tx, ty, LEG_WALK};
        } else {
            legs[n++] = (DeskLeg){tx, ty, LEG_HOP};
        }
        if (!path_ok(w, v, legs, n)) continue;
        memcpy(w->legs, legs, sizeof legs[0] * (size_t)n);
        w->nlegs = n;
        w->leg = 0;
        w->leg_x0 = w->x;
        w->leg_y0 = w->y;
        w->leg_t = 0;
        w->leg_len = leg_duration(w, &w->legs[0], w->x, w->y);
        w->place = place;
        return true;
    }
    return false;
}

static float clamp1(float x)
{
    return x < -1 ? -1 : x > 1 ? 1 : x;
}

/* Hacia dónde mira: a donde va o, quieta, a lo que tiene cerca. */
static void target_look(const DeskWalker *w, const DeskView *v, float *lx, float *ly)
{
    *lx = *ly = 0;
    if (w->leg < w->nlegs) {
        const DeskLeg *l = &w->legs[w->leg];
        float dx = l->x - w->leg_x0, dy = l->y - w->leg_y0, d = hypotf(dx, dy);
        if (d > 1) {
            *lx = dx / d;
            *ly = l->kind == LEG_HOP ? 0.5f * dy / d : dy / d;
        }
        return;
    }
    DeskRect b = desk_body_at(w, w->x, w->y);
    float cx = (float)b.x + (float)b.w * 0.5f;
    int m = monitor_of(v, cx, (float)b.y + (float)b.h * 0.5f);
    if (m < 0) return;
    const DeskRect *wa = &v->mon[m].work;
    switch (w->place) {
    case DESK_ON_WINDOW: *ly = 0.6f; break; /* a la ventana, abajo */
    case DESK_SIDE: *lx = cx < (float)wa->x + (float)wa->w * 0.5f ? 0.8f : -0.8f; break;
    default: { /* a la ventana activa o al centro, un poco hacia arriba */
        const DeskWindow *a = active_window(v);
        float tx = a ? (float)a->r.x + (float)a->r.w * 0.5f : (float)wa->x + (float)wa->w * 0.5f;
        *lx = 0.6f * clamp1((tx - cx) / ((float)wa->w * 0.5f));
        *ly = -0.3f;
    }
    }
}

bool desk_step(DeskWalker *w, const DeskView *v, double dt, bool allowed, DeskStep *out)
{
    const float x_before = w->x, y_before = w->y;
    if (!allowed || v->fullscreen) {
        if (w->nlegs) {
            w->nlegs = w->leg = 0; /* se detiene donde va */
            w->rest = 1.5;
        }
    } else if (w->leg >= w->nlegs) {
        w->nlegs = w->leg = 0;
        w->rest -= dt;
        if (w->rest <= 0 && !plan(w, v)) w->rest = 3; /* no hay a dónde: luego vuelve a ver */
    } else {
        DeskLeg *l = &w->legs[w->leg];
        w->leg_t += (float)dt;
        float s = w->leg_len > 0 ? w->leg_t / w->leg_len : 1, x, y;
        leg_pos(w, l, w->leg_x0, w->leg_y0, s, &x, &y);
        DeskRect b0 = desk_body_at(w, w->x, w->y), b1 = desk_body_at(w, x, y);
        if (!step_ok(body_bad(v, &b0), body_bad(v, &b1))) {
            /* Algo se atravesó (el cursor, una ventana): se detiene ahí. */
            w->nlegs = w->leg = 0;
            w->rest = 2 + 2 * rnd(w);
        } else {
            w->x = x;
            w->y = y;
            if (s >= 1) {
                w->leg_x0 = l->x;
                w->leg_y0 = l->y;
                w->leg_t = 0;
                if (++w->leg < w->nlegs) {
                    w->leg_len = leg_duration(w, &w->legs[w->leg], w->x, w->y);
                } else {
                    w->nlegs = w->leg = 0;
                    w->rest = 6 + 9 * rnd(w);
                }
            }
        }
    }
    float lx, ly;
    target_look(w, v, &lx, &ly);
    float k = (float)(1.0 - exp(-dt / 0.25));
    w->look_x += (lx - w->look_x) * k;
    w->look_y += (ly - w->look_y) * k;
    if (out) {
        out->x = (int)lroundf(w->x);
        out->y = (int)lroundf(w->y);
        out->look_x = w->look_x;
        out->look_y = w->look_y;
        out->moving = w->leg < w->nlegs;
    }
    return lroundf(w->x) != lroundf(x_before) || lroundf(w->y) != lroundf(y_before);
}

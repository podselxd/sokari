/* Cómo se mueve Sokari por el escritorio, en un escritorio de mentira (lo usa
   el GIF de la 2.8.0): gif_escritorio <carpeta> <semilla>
   Escribe cuadro_NNNN.rgb (RGB, 960x540) con la misma lógica (src/desk.c) y
   la misma esfera con cara que la app. Mientras descansa, el tiempo corre
   más rápido. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "face.h"
#include "sphere.h"

#define W 960
#define H 540
#define SIZE 128
#define BODY 92

static uint8_t g_img[H][W][3];

static void fill(int x, int y, int w, int h, int r, int g, int b)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) g_img[j][i][0] = r, g_img[j][i][1] = g, g_img[j][i][2] = b;
}

static void window(const DeskRect *r, bool active, const char *tint)
{
    fill(r->x - 1, r->y - 1, r->w + 2, r->h + 2, 60, 60, 70);
    fill(r->x, r->y, r->w, 26, active ? 70 : 150, active ? 90 : 150, active ? 170 : 160);
    fill(r->x, r->y + 26, r->w, r->h - 26, tint[0], tint[1], tint[2]);
    for (int k = 0; k < 5; k++) fill(r->x + 18, r->y + 46 + k * 22, r->w / 2 + (k * 37) % 90, 8, 200, 200, 205);
    fill(r->x + r->w - 22, r->y + 8, 12, 10, 220, 90, 90);
}

static void cursor(int x, int y)
{
    for (int j = 0; j < 18; j++)
        for (int i = 0; i <= j / 2 + 1; i++) {
            bool edge = i == 0 || i == j / 2 + 1 || j == 17;
            int c = edge ? 0 : 255;
            if (x + i < W && y + j < H) g_img[y + j][x + i][0] = g_img[y + j][x + i][1] = g_img[y + j][x + i][2] = c;
        }
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    unsigned seed = (unsigned)atoi(argv[2]);
    DeskView v;
    memset(&v, 0, sizeof v);
    v.nmon = 1;
    v.mon[0].area = (DeskRect){0, 0, W, H};
    v.mon[0].work = (DeskRect){0, 0, W, H - 34};
    v.nwin = 2;
    v.win[0] = (DeskWindow){{300, 230, 400, 230}, true};
    v.win[1] = (DeskWindow){{80, 60, 330, 200}, false};
    v.cursor_x = 760, v.cursor_y = 120;
    DeskWalker w;
    desk_init(&w, W - SIZE - 10, H - 34 - SIZE + (SIZE - BODY) / 2, SIZE, BODY, seed);
    SphereRenderer *r = sphere_create_fit(SIZE, (float)BODY / SIZE);
    Face *face = face_create(seed + 3);
    uint32_t *px = calloc((size_t)SIZE * SIZE, 4);
    int frame = 0, places = 0, last_place = -1;
    double t = 0;
    const char tint0[3] = {(char)245, (char)246, (char)250}, tint1[3] = {(char)232, (char)236, (char)240};
    for (int step = 0; step < 30 * 60 && frame < 420; step++) {
        const double dt = 1.0 / 30;
        DeskStep st;
        desk_step(&w, &v, dt, true, &st);
        t += dt;
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.activity = FACE_IDLE;
        in.affect.weights[0] = 1;
        in.level = FACE_LEVEL_HIGH;
        in.look_x = st.look_x;
        in.look_y = st.look_y;
        in.walking = st.moving;
        SphereFace pose;
        face_step(face, dt, &in, &pose);
        if (!st.moving) {
            if (last_place != (int)w.place) places++, last_place = (int)w.place;
            if (step % 6) continue; /* descansando: más rápido */
        }
        /* El escritorio. */
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) g_img[j][i][0] = 40 + j / 12, g_img[j][i][1] = 70 + j / 10, g_img[j][i][2] = 110 + i / 20;
        window(&v.win[1].r, false, tint1);
        window(&v.win[0].r, true, tint0);
        fill(0, H - 34, W, 34, 24, 26, 34);
        for (int k = 0; k < 6; k++) fill(12 + k * 40, H - 27, 26, 20, 90 + k * 20, 120, 200 - k * 15);
        cursor(v.cursor_x, v.cursor_y);
        /* Sokari. */
        sphere_set_face(r, &pose);
        sphere_render(r, t, t * 0.15, t * 3, &SPHERE_IDLE, 0, 0, SPHERE_STYLE_FACE_EYES, px, SIZE, true);
        for (int j = 0; j < SIZE; j++)
            for (int i = 0; i < SIZE; i++) {
                int X = st.x + i, Y = st.y + j;
                if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                uint32_t p = px[j * SIZE + i];
                int a = (int)(p >> 24), pr = (int)((p >> 16) & 255), pg = (int)((p >> 8) & 255), pb = (int)(p & 255);
                g_img[Y][X][0] = (uint8_t)(pr + g_img[Y][X][0] * (255 - a) / 255);
                g_img[Y][X][1] = (uint8_t)(pg + g_img[Y][X][1] * (255 - a) / 255);
                g_img[Y][X][2] = (uint8_t)(pb + g_img[Y][X][2] * (255 - a) / 255);
            }
        char name[512];
        snprintf(name, sizeof name, "%s/cuadro_%04d.rgb", argv[1], frame++);
        FILE *f = fopen(name, "wb");
        if (!f) return 1;
        fwrite(g_img, 1, sizeof g_img, f);
        fclose(f);
        /* Mueve el cursor de vez en cuando, como alguien trabajando. */
        if (step % 150 == 149) v.cursor_x = 120 + (int)(fmod(t * 97, 700)), v.cursor_y = 90 + (int)(fmod(t * 53, 300));
    }
    printf("%d cuadros, %d lugares\n", frame, places);
    return 0;
}

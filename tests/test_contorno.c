/* El contorno de la esfera: con cada estilo, cada emoción y cada gesto (con
   «Qué tanto se le nota» al máximo, callada y hablando), cuadro por cuadro,
   nada de lo que se ve toca el borde del lienzo. Antes, con cara, el lienzo
   era el de la esfera sola y el salto de alegría (entre otros) se cortaba.
   Con CONTORNO_BMP=<carpeta>: escribe ahí el cuadro que más se acerca al
   borde de cada estilo, para verlo. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "face.h"
#include "sphere.h"
#include "util.h"

#define BASE 160   /* la esfera flotante de fábrica mide más; aquí basta para medir */
#define SEEN 10    /* alfa (de 255) desde el que algo ya se ve */
#define STEP 0.04  /* segundos por cuadro */

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

static AffectState pure(AffectKind k)
{
    AffectState a;
    memset(&a, 0, sizeof a);
    a.weights[k] = 1;
    a.dominant = k;
    a.secondary = k == AFF_NEUTRAL ? AFF_JOY : AFF_NEUTRAL;
    return a;
}

/* Lo más cerca que llegó algo visible al borde (píxeles; negativo: se salió). */
static int margin(const uint32_t *px, int size)
{
    int best = size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            if ((px[y * size + x] >> 24) < SEEN) continue;
            int m = x;
            if (y < m) m = y;
            if (size - 1 - x < m) m = size - 1 - x;
            if (size - 1 - y < m) m = size - 1 - y;
            if (m < best) best = m;
        }
    return best;
}

int wmain(void)
{
    const char *dump = getenv("CONTORNO_BMP");
    SetConsoleOutputCP(CP_UTF8);
    const SphereStyle styles[] = {SPHERE_STYLE_DOTS, SPHERE_STYLE_FACE_EYES, SPHERE_STYLE_FACE_MOUTH,
                                  SPHERE_STYLE_FACE_DOTS};
    const char *style_names[] = {"halo de puntos", "solo ojos", "ojos y boca", "cara de puntos"};
    for (int si = 0; si < 4; si++) {
        SphereStyle style = styles[si];
        int canvas = (int)(BASE * sphere_room(style));
        SphereRenderer *r = sphere_create_fit(canvas, (float)BASE / (float)canvas);
        uint32_t *px = xmalloc(sizeof(uint32_t) * (size_t)canvas * (size_t)canvas);
        int worst = canvas, frames = 0;
        const char *worst_what = "";
        char what[160];
        Face *f = face_create(7);
        /* Sin cara, la emoción y los gestos no la mueven: basta una vuelta. */
        const int nk = sphere_style_is_face(style) ? AFF_COUNT : 1, ng = sphere_style_is_face(style) ? FACE_G_COUNT : 1;
        for (int k = 0; k < nk; k++) {
            for (int g = FACE_G_NONE; g < ng; g++) {
                for (int speaking = 1; speaking < 2; speaking++) { /* hablando: la voz la agranda (lo peor) */
                    FaceInput in;
                    memset(&in, 0, sizeof in);
                    in.affect = pure((AffectKind)k);
                    in.level = FACE_LEVEL_HIGH;
                    in.symbols = true;
                    in.activity = speaking ? FACE_SPEAKING : FACE_IDLE;
                    SphereFace pose;
                    for (int i = 0; i < 25; i++) face_step(f, STEP, &in, &pose); /* que llegue a la emoción */
                    if (g != FACE_G_NONE) face_play(f, (FaceGesture)g);
                    for (int i = 0; i < 60; i++) { /* 2.4 s: el gesto completo */
                        double t = frames * STEP;
                        float voice = speaking ? 0.5f + 0.5f * (float)((i * 7) % 10) / 9.0f : 0.0f;
                        in.voice = voice;
                        in.pulse = speaking ? (float)((i * 3) % 5) / 4.0f : 0.0f;
                        face_step(f, STEP, &in, &pose);
                        if (i % 2) continue; /* se mide un cuadro sí y uno no */
                        sphere_set_face(r, &pose);
                        sphere_render(r, t, t * 0.15, t * 3.0, speaking ? &SPHERE_SPEAK : &SPHERE_IDLE, voice,
                                      in.pulse, style, px, canvas, true);
                        frames++;
                        int m = margin(px, canvas);
                        if (m < worst) {
                            worst = m;
                            snprintf(what, sizeof what, "%s, %s%s", affect_name((AffectKind)k),
                                     face_gesture_name((FaceGesture)g), speaking ? ", hablando" : "");
                            worst_what = what;
                            if (dump) {
                                /* el cuadro más apretado, para verlo */
                                wchar_t name[64];
                                swprintf(name, 64, L"contorno_%d.bmp", si);
                                wchar_t *wdir = utf8_to_wide(dump), *path = path_join(wdir, name);
                                free(wdir);
                                uint32_t fsz = 54 + (uint32_t)canvas * (uint32_t)canvas * 4;
                                unsigned char *bmp = xcalloc(fsz, 1);
                                uint32_t off = 54, hs = 40, wv = (uint32_t)canvas, img = fsz - 54;
                                int32_t hv = -canvas;
                                uint16_t planes = 1, bpp = 32;
                                bmp[0] = 'B';
                                bmp[1] = 'M';
                                memcpy(bmp + 2, &fsz, 4);
                                memcpy(bmp + 10, &off, 4);
                                memcpy(bmp + 14, &hs, 4);
                                memcpy(bmp + 18, &wv, 4);
                                memcpy(bmp + 22, &hv, 4);
                                memcpy(bmp + 26, &planes, 2);
                                memcpy(bmp + 28, &bpp, 2);
                                memcpy(bmp + 34, &img, 4);
                                memcpy(bmp + 54, px, img);
                                write_file_atomic(path, bmp, fsz);
                                free(bmp);
                                free(path);
                            }
                        }
                    }
                }
            }
        }
        face_destroy(f);
        printf("      %s: lienzo de %d para una esfera de %d · lo más cerca del borde: %d px (%s) · %d cuadros\n",
               style_names[si], canvas, BASE, worst, worst_what, frames);
        char msg[160];
        snprintf(msg, sizeof msg, "%s: ningún gesto ni emoción toca el borde (queda aire)", style_names[si]);
        check(worst >= 2, msg);
        free(px);
        sphere_destroy(r);
    }
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

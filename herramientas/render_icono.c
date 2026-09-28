/* La esfera con ojos para los íconos (la usa herramientas/iconos.py):
     render_icono <lienzo> <t> <salida>
   Dibuja la cara neutral del estilo «solo ojos» en el momento t, con el mismo
   código que la app, y escribe el cuadro en BGRA premultiplicado. En stdout va
   el lado real (el renderizador no baja de 64 ni deja lados impares). */
#include <stdio.h>
#include <stdlib.h>

#include "sphere.h"

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "uso: render_icono <lienzo> <t> <salida>\n");
        return 2;
    }
    SphereRenderer *r = sphere_create_fit(atoi(argv[1]), 0.92f);
    int n = sphere_size(r);
    double t = atof(argv[2]);
    SphereFace face;
    sphere_face_neutral(&face);
    sphere_set_face(r, &face);
    uint32_t *px = calloc((size_t)n * (size_t)n, sizeof *px);
    if (!px) return 1;
    sphere_render(r, t, t * 0.15, t * 3.0, &SPHERE_IDLE, 0.0f, 0.0f, SPHERE_STYLE_FACE_EYES, px, n, true);
    FILE *out = fopen(argv[3], "wb");
    if (!out || fwrite(px, sizeof *px, (size_t)n * (size_t)n, out) != (size_t)n * (size_t)n) return 1;
    fclose(out);
    printf("%d\n", n);
    free(px);
    sphere_destroy(r);
    return 0;
}

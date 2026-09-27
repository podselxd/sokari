/* Las miniaturas de los estilos (ver style_preview.h). */
#include <stdlib.h>
#include <string.h>

#include "affect.h"
#include "face.h"
#include "style_preview.h"
#include "util.h"

struct StylePreview {
    SphereStyle style;
    SphereRenderer *r;
    Face *face;
    double t, angle, voice_t;
};

StylePreview *style_preview_create(SphereStyle style, int size, unsigned seed)
{
    StylePreview *p = xcalloc(1, sizeof *p);
    p->style = style;
    /* Con el espacio de las ondas del estilo: así se ve entera, del mismo
       tamaño en todas. */
    p->r = sphere_create_fit(size < 16 ? 16 : size, 1.0f / sphere_room(style));
    if (sphere_style_is_face(style)) p->face = face_create(seed * 2654435761u + 7);
    p->t = 3.0 + (seed % 7) * 1.37; /* cada una en otro momento de su movimiento */
    p->angle = (seed % 5) * 0.7;
    return p;
}

void style_preview_destroy(StylePreview *p)
{
    if (!p) return;
    sphere_destroy(p->r);
    face_destroy(p->face);
    free(p);
}

int style_preview_size(const StylePreview *p)
{
    return sphere_size(p->r);
}

void style_preview_frame(StylePreview *p, double dt, bool symbols, uint32_t *out, int stride)
{
    if (!(dt >= 0)) dt = 0;
    if (dt > 0.2) dt = 0.2;
    p->t += dt;
    SphereParams params = SPHERE_IDLE;
    p->angle += params.rotation_speed * dt;
    p->voice_t += dt;
    if (p->face) {
        /* Tranquila: neutral, sin hacer nada; parpadea y mira alrededor. */
        AffectState a;
        memset(&a, 0, sizeof a);
        a.weights[AFF_NEUTRAL] = 1.0f;
        FaceInput in;
        memset(&in, 0, sizeof in);
        in.activity = FACE_IDLE;
        in.affect = a;
        in.level = FACE_LEVEL_HIGH;
        in.symbols = symbols;
        SphereFace pose;
        face_step(p->face, dt, &in, &pose);
        sphere_set_face(p->r, &pose);
        SphereParams base = params;
        face_sphere_colors(&a, &base, &params);
    }
    sphere_render(p->r, p->t, p->angle, p->voice_t, &params, 0.0f, 0.0f, p->style, out, stride, false);
}

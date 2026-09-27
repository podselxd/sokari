#ifndef SOKARI_STYLE_PREVIEW_H
#define SOKARI_STYLE_PREVIEW_H

/* Las miniaturas de Configuración → Pantalla → Estilo: cada estilo en
   movimiento (gira, respira y, las caras, parpadean y miran alrededor), con
   el mismo código que dibuja la esfera de verdad. Windows y Linux. */

#include <stdbool.h>
#include <stdint.h>

#include "sphere.h"

typedef struct StylePreview StylePreview;

/* size: el lado de la miniatura en píxeles; seed: para que no se muevan
   todas igual. */
StylePreview *style_preview_create(SphereStyle style, int size, unsigned seed);
void style_preview_destroy(StylePreview *p);
/* El lado real del cuadro (el que hay que reservar en out). */
int style_preview_size(const StylePreview *p);
/* Avanza dt segundos y dibuja un cuadro en out: BGRA opaco, fondo negro,
   stride en píxeles. symbols: los de la cara (lágrima, «?»…). */
void style_preview_frame(StylePreview *p, double dt, bool symbols, uint32_t *out, int stride);

#endif

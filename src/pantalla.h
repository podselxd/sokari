#ifndef SOKARI_PANTALLA_H
#define SOKARI_PANTALLA_H

#include <stdbool.h>
#include <stdint.h>

#include "third_party/cJSON.h"

/* «¿Qué ves en mi pantalla?»: solo cuando se lo pides. Se toma una captura
   (sin Sokari en ella, con un aviso mientras) y la revisa una IA con visión.

   Cada sistema la toma a su modo: RGB de 8 bits, fila por fila (heap). Si no
   se pudo, NULL y *why (heap) dice por qué. */
uint8_t *screen_grab(int *w, int *h, char **why);

/* JPEG de la captura, a lo más de 1280 de ancho (menos cupo), en base64. */
char *screen_jpeg_base64(const uint8_t *rgb, int w, int h, int *out_w, int *out_h);

/* La herramienta ver_pantalla {pregunta}. */
char *tool_ver_pantalla(const cJSON *a);

#endif

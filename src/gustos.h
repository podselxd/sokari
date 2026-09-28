#ifndef SOKARI_GUSTOS_H
#define SOKARI_GUSTOS_H

#include "third_party/cJSON.h"

/* Los gustos de Sokari: se van formando con lo que vive contigo (una canción
   que le encantó, un tema que le aburre) y se guardan en gustos.json, junto
   a su memoria, para que sea constante. */

/* Para el modelo, en cada pedido: «Tus gustos hasta ahora: …» (heap), o
   NULL si todavía no tiene. */
char *gustos_resumen(void);
/* anotar_gusto {cosa, nivel: encanta | gusta | desagrada}. */
char *tool_anotar_gusto(const cJSON *a);

#endif

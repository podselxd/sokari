#ifndef SOKARI_LECTOR_H
#define SOKARI_LECTOR_H

#include "third_party/cJSON.h"

/* Leer en voz alta con su voz, sin que el texto pase por la IA (0 tokens):
   lo que copiaste o un archivo (.txt, .md, .csv… y PDF en Linux). La
   herramienta devuelve el texto tal cual para decirlo. */
char *tool_leer_en_voz(const cJSON *a);
/* El texto de un PDF (heap), o NULL y *why si este sistema no puede. */
char *pdf_text(const wchar_t *path, char **why);

#endif

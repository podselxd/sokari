#ifndef SOKARI_GUSTOS_H
#define SOKARI_GUSTOS_H

#include "third_party/cJSON.h"

/* Los gustos de Sokari: se van formando con lo que vive contigo (una canción
   que le encantó, un tema que le aburre) y se guardan en gustos.json, junto
   a su memoria (y en su nota de Obsidian, si elegiste bóveda), para que sea
   constante.

   Algunos son para siempre: parte de quién es. Los decide ella (anotar_gusto
   con para_siempre) o se vuelven así solos cuando siente lo mismo en
   GUSTOS_DIAS días distintos. Son GUSTOS_MAX_SIEMPRE a lo más, no salen al
   llegar al tope de 40 ni con «borra la memoria» y no cambian aunque se lo
   pidan; solo se quitan editando su nota o su archivo, o con «Soltar sus
   gustos para siempre» en Configuración. */

#define GUSTOS_MAX_SIEMPRE 10
#define GUSTOS_DIAS 3

/* Para el modelo, en cada pedido: sus gustos para siempre primero y luego los
   demás (heap), o NULL si todavía no tiene. */
char *gustos_resumen(void);
/* Para Configuración: sus gustos en palabras (heap), o NULL si no tiene. */
char *gustos_describe(void);
/* Cuántos gustos para siempre tiene. */
int gustos_count_para_siempre(void);
/* Los de para siempre vuelven a ser normales; cuántos eran. */
int gustos_soltar_para_siempre(void);
/* Lee (trayendo lo que cambiaste en su nota) y reescribe su nota. */
void gustos_sync(void);
/* anotar_gusto {cosa, nivel: encanta | gusta | desagrada, para_siempre?, soltar?}. */
char *tool_anotar_gusto(const cJSON *a);

/* Para las pruebas: el día de hoy ("2026-09-28"); NULL, el de verdad. */
void gustos_set_today(const char *ymd);

#endif

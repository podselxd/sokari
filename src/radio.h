#ifndef SOKARI_RADIO_H
#define SOKARI_RADIO_H

#include <stdbool.h>

#include "third_party/cJSON.h"

/* Radio por internet sin YouTube: busca estaciones en radio-browser.info
   (abierto, gratis y sin key), las toca dentro de Sokari y guarda favoritos,
   historial y playlists en musica.json, junto a su memoria. */

/* La herramienta radio {accion, busqueda, nombre}. */
char *tool_radio(const cJSON *a);

/* Cada sistema: tocar una dirección (deja de tocar lo anterior) y parar. */
bool radio_play(const char *url, char **why);
void radio_stop(void);

/* Para las pruebas: la respuesta de radio-browser a una búsqueda (sin red). */
void radio_set_search_override(const char *json);
/* Para las pruebas: otro reproductor (NULL: el del sistema). */
void radio_set_player(bool (*play)(const char *url, char **why), void (*stop)(void));

#endif

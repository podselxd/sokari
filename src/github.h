#ifndef SOKARI_GITHUB_H
#define SOKARI_GITHUB_H

/* La última versión publicada en GitHub, para el actualizador de Windows y el
   de Linux.

   Primero la API: trae la huella (sha256) que calcula GitHub de cada archivo.
   Pero la API sin cuenta deja 60 consultas por hora por IP, y en redes que
   comparten IP (la de tu compañía de internet, una VPN) se acaban aunque
   Sokari pregunte cada 6 horas: responde 403. Entonces, la página del release:
   /releases/latest redirige al tag, el archivo se baja de
   /releases/download/<tag>/<archivo> y su huella de <archivo>.sha256 (la
   publica el release). Por la página, sin huella no hay versión: nunca se
   instala algo que no se pueda revisar. */

#include <stdbool.h>

#include "third_party/cJSON.h"

typedef struct {
    char *tag;       /* "v2.7.2" */
    char *url;       /* el archivo a bajar (NULL si no se pidió o no viene) */
    double size;     /* bytes; 0 si no se sabe */
    char *sha256;    /* en minúsculas; NULL si no se sabe */
    bool por_pagina; /* vino de la página del release (la API no dejó) */
} GhRelease;

/* asset: "Sokari.exe", "Sokari.deb"…, o NULL si solo importa la versión. */
bool gh_latest(const char *asset, GhRelease *out, char **error);
void gh_release_free(GhRelease *r);

/* Ese archivo en la lista "assets" de la API, o NULL. */
const cJSON *gh_pick_asset(const cJSON *assets, const char *name);
/* El tag de la dirección a la que redirige /releases/latest, o NULL si no es
   un tag de versión (vX.Y.Z) (heap). */
char *gh_tag_from_location(const char *location);
/* La huella de un .sha256 («abc…» o «abc…  Sokari.exe»), en minúsculas, o NULL (heap). */
char *gh_parse_sha256(const char *text);

/* Para las pruebas: otros servidores en vez de api.github.com y github.com
   (NULL: los de verdad). */
void gh_set_bases(const char *api, const char *web);

#endif

#ifndef SOKARI_BOVEDA_H
#define SOKARI_BOVEDA_H

/* La memoria de Sokari en tu bóveda de Obsidian (opcional: Configuración →
   Memoria). Lo que sabe de ti, sus gustos, tus pendientes y su música se
   escriben como notas en la carpeta «Sokari» de la bóveda, y si editas una
   nota a mano, lo que diga la nota manda.

   Sus archivos de siempre (gustos.json, hechos.json…) siguen siendo los que
   usa: cada tema, al leerse, primero se trae lo que cambiaste en su nota
   (boveda_pull) y, al guardarse, se reescribe su nota (boveda_push). Antes
   de que una nota tuya le quite algo, se respalda el archivo del día en
   <memoria>/respaldos. Sin bóveda, nada de esto hace algo. */

#include <stdbool.h>
#include <wchar.h>

#include "third_party/cJSON.h"

typedef enum {
    BOVEDA_GUSTOS,     /* gustos.json: [{cosa, nivel, siempre, dias}] */
    BOVEDA_DATOS,      /* hechos.json de un perfil: {clave: valor} */
    BOVEDA_PENDIENTES, /* notas.json de un perfil: [{texto, creado}] */
    BOVEDA_MUSICA,     /* musica.json: {favoritos, historial, playlists} */
    BOVEDA_KINDS
} BovedaKind;

/* La carpeta «Sokari» de la bóveda elegida (heap), o NULL si no elegiste
   bóveda o ya no existe. */
wchar_t *boveda_dir(void);
/* La bóveda de Obsidian abierta en esta PC (o la primera que exista), según
   la configuración de Obsidian (heap); NULL si no hay. */
wchar_t *boveda_detect(void);

/* La nota de ese tema (heap), o NULL sin bóveda. who: el nombre del perfil
   (DATOS y PENDIENTES), NULL o "default" para el de siempre. */
wchar_t *boveda_note_path(BovedaKind k, const char *who);

/* Al leer: si la nota cambió desde la última vez que Sokari la escribió, lo
   que diga manda (la primera vez que la ve, se juntan las dos). data es lo
   del archivo JSON y se cambia en su lugar; devuelve true si cambió (quien
   llama lo guarda, y con eso se reescribe la nota). */
bool boveda_pull(BovedaKind k, const char *who, cJSON *data);
/* Al guardar: reescribe su nota con lo de data. */
void boveda_push(BovedaKind k, const char *who, const cJSON *data);

/* Las notas, tal cual se escriben y se leen (heap). parse devuelve NULL si
   la nota no trae nada de ese tema. Para las pruebas. */
char *boveda_render(BovedaKind k, const char *who, const cJSON *data);
cJSON *boveda_parse(BovedaKind k, const char *text, const cJSON *current);

/* Escribe ya todas las notas (al elegir la bóveda): gustos, música y los
   datos y pendientes del perfil de ahora. */
void boveda_sync_all(void);

#endif

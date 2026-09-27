#ifndef SOKARI_VIBE_H
#define SOKARI_VIBE_H

/* La vibra: que lo que dice Sokari (y lo que le dices) se le note en la cara
   y en la voz, frase por frase, sin gastar IA. La etiqueta de la IA da la
   emoción de toda la respuesta; esto la afina y cubre lo que contesta sola. */

#include <stdbool.h>

#include "affect.h"
#include "prosody.h"

typedef struct {
    float amount[AFF_COUNT]; /* como la etiqueta de la IA: 0..1 */
    int found;               /* cuántas emociones trae */
    AffectCue cue;           /* el gesto que va (AFF_CUE_NONE: ninguno) */
    const char *cause;       /* para afecto.jsonl (nunca tu texto) */
} Vibe;

/* Lo que tú le dices a ella: un insulto (se agüita) o un halago (se sonroja). */
Vibe vibe_of_user(const char *text);
/* ¿Es solo un insulto para ella, sin pedir nada más? */
bool vibe_only_insult(const char *text);
/* Una frase (o toda una respuesta) de lo que ella dice. */
Vibe vibe_of_text(const char *text);
/* Al terminar cada turno; ai_tagged: la IA ya puso su etiqueta. */
void vibe_after_turn(const char *user, const char *reply, bool ai_tagged);

typedef struct {
    char *text;               /* lo que se dice, sin marcas */
    float weights[AFF_COUNT]; /* con qué emoción se dice */
    Prosody prosody;
    bool own;                 /* trae emoción propia (una marca o sus palabras) */
    float amount[AFF_COUNT];  /* esa emoción, para la cara */
    AffectCue cue;            /* y su gesto */
} VibeLine;

/* Parte lo que va a decir en frases, cada una con su emoción. El texto puede
   traer marcas antes de una frase: «[afecto: alegría 1]» o «[gesto: suspiro]»
   (solo las pone Sokari, nunca la IA). base: los pesos de toda la respuesta
   (affect_target_weights). */
int vibe_lines(const char *text, const float base[AFF_COUNT], VibeLine **out);
void vibe_lines_free(VibeLine *l, int n);
/* Empieza a sonar la frase (dura `seconds`): la cara la acompaña. */
void vibe_line_starts(const VibeLine *l, float seconds);
/* El texto sin marcas (heap). */
char *vibe_strip(const char *text);
bool vibe_has_marks(const char *text);

/* «¿Cómo estás?»: en palabras según cómo está (nunca números), con su gesto. */
char *vibe_how_i_feel(const AffectState *s);
/* «¿Estás triste?»: sí o no según cómo está, en palabras. */
char *vibe_am_i(const AffectState *s, AffectKind asked);

#endif

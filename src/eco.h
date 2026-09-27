#ifndef SOKARI_ECO_H
#define SOKARI_ECO_H

/* Quitarle al micrófono la voz de Sokari mientras habla, para oírte a ti
   encima de ella: «Hey Sokari» o, sin más, hablarle encima.

   Qué sonaba y cuándo: cada vez que algo empieza a sonar en la bocina se
   anota en la misma cuenta de muestras que lleva el micrófono (con un reloj
   común a los dos). Su voz llega al micrófono un poco después (lo que tarda
   la salida de audio en sonar, el cuarto y la entrada en grabar): ese retraso
   se mide solo, comparando lo que sonó con lo que se oyó, y con él se alinea
   lo que sonaba antes de restarlo (aec.h). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio.h"

/* ---- lo llaman el micrófono y la bocina (audio.c, audio_linux.c) ---- */

/* El micrófono ya dio `total` muestras desde que se abrió. */
void eco_mic_pushed(uint64_t total);
/* Se abrió (otra vez): su cuenta empieza en 0. */
void eco_mic_reset(void);
/* Va a sonar esto, desde ya (al empezar o al reanudar). Devuelve un número
   para cortarlo después. */
unsigned eco_play_begin(const int16_t *pcm, size_t n, int rate, float gain);
/* Lo que empezó con eco_play_begin se cortó (o se pausó) cuando habían
   sonado `played` de sus muestras. */
void eco_play_cut(unsigned id, size_t played);
/* Sigue lo que se cortó con eco_play_cut, justo después de lo último que
   sonó de aquello, más `gap` muestras (a rate) de silencio. Así la cuenta no
   se vuelve a adivinar con el reloj (y no hay que volver a medir el
   retraso). */
unsigned eco_play_continue(unsigned prev, size_t gap, const int16_t *pcm, size_t n, int rate, float gain);
/* Sigue después de una pausa que duró lo que diga el reloj (la salida de
   audio se detuvo por completo). */
unsigned eco_play_resume(unsigned prev, const int16_t *pcm, size_t n, int rate, float gain);

/* ---- lo usa el hilo de voz ---- */

typedef struct {
    bool playing;   /* estaba sonando algo (su voz o un sonido) */
    bool quiet;     /* no ha sonado nada en 240 ms: no hay eco que confunda */
    bool learned;   /* ya aprendió su eco (le quita 8 dB o más) */
    float mic;      /* energía (por muestra) de lo que oyó el micrófono */
    float out;      /* de lo que queda sin su voz */
    float residual; /* de lo que se calcula que todavía es su eco */
    float leak;     /* qué tanto de su eco se calcula que se escapa (0..1) */
    float mismatch; /* -1..1: muy negativo si se le está quitando un eco que ya no está ahí */
} EcoInfo;

/* El cuadro del micrófono que empieza en pos (mic_read_nowait_pos), sin su
   eco. out puede ser el mismo arreglo que in. */
void eco_frame(uint64_t pos, const int16_t in[MIC_FRAME], int16_t out[MIC_FRAME], EcoInfo *info);
/* Con cuánto retraso le llega su voz al micrófono (ms), según el reloj común
   (puede ser negativo si el reloj va atrasado), o ECO_NO_DELAY si todavía no
   se sabe. */
#define ECO_NO_DELAY (-100000)
int eco_delay_ms(void);

/* ¿Le estás hablando encima? Tu voz (según el detector de voz) y, si está
   sonando algo, mucho más fuerte que el eco que le queda, en 3 de los
   últimos 4 cuadros (240 ms). Mientras todavía no aprende su eco, solo
   cuando no está sonando nada: con su voz sin quitar no se sabe cuál es la
   tuya (para eso sigue «Hey Sokari»). Y no si lo que queda va en contra
   del eco que se quitó: eso es que cambió el retraso o la bocina (su voz
   mal quitada), no tu voz, que no tiene nada que ver con la suya. */
typedef struct {
    unsigned hist;
} EcoTalk;
bool eco_talk_feed(EcoTalk *t, bool voice, const EcoInfo *info);
bool eco_talk_frame(bool voice, const EcoInfo *info); /* ¿este cuadro es tu voz encima de la suya? */

/* Para las pruebas: otro reloj (en µs; NULL: el de verdad) y todo de cero. */
void eco_set_clock(uint64_t (*now_us)(void));
void eco_reset(void);

#endif

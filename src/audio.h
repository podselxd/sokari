#ifndef SOKARI_AUDIO_H
#define SOKARI_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_RATE 16000
#define MIC_FRAME 1280

/* Micrófono: un hilo propio drena waveIn a un buffer circular; el hilo de voz
   lee de a 80 ms. Así, mientras Sokari piensa o habla, no se pierde audio. */
bool mic_start(const char *device_name);
void mic_stop(void);
bool mic_restart(const char *device_name);
bool mic_read(int16_t out[MIC_FRAME], unsigned timeout_ms);
bool mic_read_nowait(int16_t out[MIC_FRAME]);
/* Igual, y en *pos la posición del cuadro (muestras desde que se abrió el
   micrófono): con ella se sabe qué sonaba en la bocina en ese momento y se
   le quita al cuadro su voz (eco.h). */
bool mic_read_nowait_pos(int16_t out[MIC_FRAME], uint64_t *pos);
void mic_flush(void);
float mic_level(void);
int mic_list_devices(char ***names_out);
void free_string_list(char **list, int n);

/* Reproducción PCM mono 16 bits. El callback se llama cada ~40 ms con el
   nivel (0..1) de lo que está sonando; si devuelve false se corta ahí. */
typedef bool (*PlayCallback)(float level, void *ctx);
bool speaker_play(const int16_t *pcm, size_t samples, int rate, float gain, PlayCallback cb, void *ctx);
/* Desde el callback: pausa lo que suena (sin cortarlo) o lo reanuda donde se
   quedó. En pausa, speaker_play sigue llamando al callback (con nivel 0) y
   no termina hasta que se reanuda o el callback devuelve false. */
void speaker_hold(bool hold);

/* Salida de audio por nombre (el que da Windows, como en mic_list_devices);
   NULL o "" = la predeterminada. Si esa salida no está conectada, se usa la
   predeterminada. */
void speaker_set_device(const char *name);
int speaker_list_devices(char ***names_out);

float frame_energy(const int16_t *pcm, size_t n);

/* Mientras te escucha, baja el volumen de Windows (un video o música de la
   PC ya no tapan tu voz) y luego lo regresa, salvo que tú le hayas movido.
   Necesita COM iniciado en ese hilo. Sin salida de audio, no hace nada. */
typedef struct {
    float before, ducked; /* before < 0: no se tocó */
} DuckState;
DuckState system_duck(float factor);
void system_unduck(DuckState d);
float volume_to_gain(int volume);

#endif

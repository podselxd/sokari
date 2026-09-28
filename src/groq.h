#ifndef SOKARI_GROQ_H
#define SOKARI_GROQ_H

#include <stddef.h>
#include <stdint.h>

#include "third_party/cJSON.h"

typedef enum {
    GROQ_OK = 0,
    GROQ_RATE_LIMITED,
    GROQ_AUTH_ERROR,
    GROQ_NETWORK_ERROR,
    GROQ_SERVER_ERROR,
    GROQ_BAD_RESPONSE,
} GroqStatus;

typedef struct {
    GroqStatus status;
    int http_status;
    int retry_after;
    char *detail;
} GroqError;

void groq_error_free(GroqError *e);

/* Texto transcripto (puede ser "" si no había voz real), o NULL si falló. */
char *groq_transcribe(const int16_t *pcm, size_t samples, int sample_rate, GroqError *err);

/* Devuelve el "message" de choices[0] ya limpio (role/content/tool_calls),
   o NULL si falló. Prueba los modelos en orden: los de Groq que sirven para
   usar herramientas (cada uno tiene su propio cupo) y luego los de las keys
   de respaldo que tengas (NVIDIA, DeepSeek, OpenRouter, GLM). Si uno no tiene
   cupo, pasa al siguiente sin esperar. */
cJSON *groq_chat(const cJSON *messages, const cJSON *tools, GroqError *err);

/* De la lista de modelos de un proveedor ("GET /models"), los que sirven,
   del mejor al más flojo (heap; *out con n cadenas). Sin efectos: para
   probarla sin red. */
int groq_pick_models(const char *provider, const char *models_json, char ***out);
/* Para las pruebas: otra dirección para un proveedor ("groq", "nvidia"…), y
   volver a armar la lista de modelos. */
void groq_set_base_url(const char *provider, const char *url);
void groq_reset_models(void);

/* Le pregunta a una IA que ve imágenes (de las keys que tengas) sobre una
   captura en JPEG (base64). *tokens: cuánto costó la entrada. NULL si
   ninguna pudo; err->detail "sin-vision" si ninguna de tus keys tiene. */
char *groq_vision(const char *prompt, const char *jpeg_b64, int *tokens, GroqError *err);

unsigned char *wav_encode(const int16_t *pcm, size_t samples, int sample_rate, size_t *out_len);

#endif

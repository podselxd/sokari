#ifndef SOKARI_AGENT_H
#define SOKARI_AGENT_H

#include <windows.h> /* LONG (en Linux, src/linux/include/windows.h) */

#include <stdbool.h>

typedef struct Conversation Conversation;

typedef struct {
    char *reply;     /* lo que hay que decir (heap), o NULL */
    char *speech;    /* lo mismo con marcas de emoción por frase para la voz (heap), o NULL */
    bool keep_going; /* ¿sigue abierta la conversación? */
    bool shutdown;   /* se dijo la palabra de apagado */
} TurnResult;

void agent_init(void);
Conversation *conv_create(bool load_recent_memory);
void conv_destroy(Conversation *c);
/* Otra vez "Hey Sokari": lo que se leyó de afuera en las conversaciones
   anteriores se borra del historial y ya no hace pedir confirmación; un
   "sí a todo" o una acción pendiente de antes tampoco siguen valiendo. */
void conv_new_session(Conversation *c);
/* Conversación que llega por la malla: las acciones que piden confirmación
   de voz se niegan en vez de quedar esperando un "sí". */
void conv_set_remote(Conversation *c, bool remote);

/* ALL: "sí a todo" (no vuelve a preguntar en esta conversación). REPEAT:
   "¿qué?", "no te entendí": se repite la pregunta en vez de cancelarla. */
typedef enum { ANSWER_OTHER, ANSWER_YES, ANSWER_NO, ANSWER_ALL, ANSWER_REPEAT } AgentAnswer;
AgentAnswer agent_classify_answer(const char *text);
/* ¿La respuesta del modelo termina pidiendo permiso para hacer algo? */
bool agent_asks_permission(const char *reply);
/* Quita el razonamiento en inglés que a veces se cuela y la respuesta repetida
   ("Listo.Listo."). Devuelve un texto nuevo (heap). */
char *agent_clean_reply(const char *reply);

/* Pipeline completo para un texto ya transcripto (o llegado por la malla):
   palabra de apagado (chequeo local, nunca llega a Groq) -> despedida ->
   modelo + herramientas. Llamar con state_lock() tomado. */
TurnResult agent_process(Conversation *c, const char *text);
/* Mientras agent_process corre (en otro hilo), si *flag deja de ser 0 ya no
   llama a la IA ni hace acciones y regresa sin respuesta. NULL: sin eso. */
void conv_set_cancel(Conversation *c, volatile LONG *flag);

#endif

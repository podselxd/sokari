#ifndef SOKARI_AFFECT_H
#define SOKARI_AFFECT_H

/* El estado afectivo de Sokari (documento maestro §10–15, 60, 63, 82, 83).

   Es un estado OPERACIONAL: números que resumen cómo le está yendo
   (valencia) y con cuánta energía (activación), para que la cara y el color
   lo expresen. No es un sentimiento real y nada aquí decide acciones.

   - Emoción (segundos a minutos): un punto (valencia, activación) en
     [-1, 1]² que va hacia un objetivo con velocidad y aceleración acotadas;
     el objetivo regresa solo a la línea base.
   - Ánimo (minutos a horas): la línea base. Sigue a la emoción muy despacio,
     regresa al temperamento en horas y se guarda entre sesiones.
   - Temperamento: fijo (aprenderlo con evidencia es PROPOSED, §15).
   - Las emociones son campos alrededor de 6 puntos de referencia: el estado
     es una mezcla con pesos, no una casilla. La dominante tiene histéresis
     (entra con 0.60, sale con 0.45, dura al menos 1.2 s) para no parpadear. */

#include <stdbool.h>

typedef enum {
    AFF_NEUTRAL,
    AFF_JOY,     /* alegría   (+0.80, +0.45) */
    AFF_FEAR,    /* temor     (-0.35, +0.85) */
    AFF_ANGER,   /* furia     (-0.75, +0.60) */
    AFF_DISGUST, /* desagrado (-0.85, -0.10) */
    AFF_SADNESS, /* tristeza  (-0.55, -0.75) */
    AFF_COUNT
} AffectKind;

typedef struct {
    float valence, arousal;   /* -1 .. 1 */
    float intensity;          /* 0 .. 1 */
    float weights[AFF_COUNT]; /* la mezcla (suman 1) */
    AffectKind dominant;      /* con histéresis */
    AffectKind secondary;     /* la que le sigue en la mezcla */
    float mood_valence, mood_arousal; /* el ánimo: la línea base */
    float velocity;                   /* qué tan rápido cambia (unidades por segundo) */
} AffectState;

/* Un estímulo ya evaluado (appraisal): hacia dónde empuja y cuánto pesa.
   Con todo en 0 es un estímulo neutro y sin importancia. */
typedef struct {
    float valence, arousal; /* hacia dónde (-1 .. 1) */
    float importance;       /* 0 .. 1 */
    float novelty;          /* 0 .. 1 */
    float surprise;         /* 0 .. 1: error de predicción */
    float control;          /* -1 .. 1, 0 = normal: sin control sube la activación, con control la baja */
    float certainty;        /* -1 .. 1, 0 = normal: la incertidumbre sube la activación */
    const char *cause;      /* por qué (queda en la trayectoria; nunca texto tuyo ni de la IA) */
} AffectStimulus;

/* Lo que pasa en Sokari y ya trae su evaluación (0 tokens). */
typedef enum {
    AFF_EV_THANKS,        /* le dijeron «gracias» */
    AFF_EV_GREETING,      /* un saludo o plática */
    AFF_EV_JOKE,          /* contó un chiste */
    AFF_EV_TASK_DONE,     /* resolvió algo solo (skill o comando local) */
    AFF_EV_DELICATE,      /* tiene que pedir un «sí» antes de algo delicado */
    AFF_EV_NETWORK_ERROR, /* sin internet o la IA no responde */
    AFF_EV_QUOTA,         /* se quedó sin cupo */
    AFF_EV_COUNT
} AffectEvent;

/* Temperamento (§83): adónde regresa el ánimo, cuánto le afecta un estímulo
   (sensibilidad) y cuánto resiste los cambios chicos (estabilidad). */
typedef struct {
    float valence, arousal; /* -0.6 .. 0.6 */
    float sensitivity;      /* 0.2 .. 2, 1 = normal */
    float stability;        /* 0 .. 1, 0.5 = normal */
} AffectTemperament;

typedef struct {
    float trend;         /* hacia dónde va la valencia (por minuto, del último minuto) */
    float volatility;    /* cuánto cambia (velocidad media cuadrática de los últimos 2 min) */
    float recovery_time; /* segundos que tardó en volver cerca de la base tras el último pico;
                            -1 si todavía no vuelve; 0 si no ha habido picos */
    float deviation;     /* qué tan lejos está de la línea base */
} AffectMetrics;

/* Por qué se movió (o no) el estado (§60: reason codes). */
enum {
    AFF_R_SURPRISE = 1 << 0,    /* sorpresa: error de predicción */
    AFF_R_NOVELTY = 1 << 1,     /* algo nuevo */
    AFF_R_LOW_CONTROL = 1 << 2, /* sin control */
    AFF_R_UNCERTAIN = 1 << 3,   /* incertidumbre */
    AFF_R_TRIVIAL = 1 << 4,     /* estímulo chico: amortiguado por la estabilidad */
    AFF_R_REPEATED = 1 << 5,    /* se repitió: pesa menos (habituación) */
    AFF_R_SPEED_CAP = 1 << 6,   /* topó con la velocidad máxima */
    AFF_R_ACCEL_CAP = 1 << 7,   /* topó con la aceleración máxima */
    AFF_R_HYSTERESIS = 1 << 8,  /* otra emoción pesaba más, pero la histéresis sostuvo la dominante */
    AFF_R_SWITCH = 1 << 9,      /* cambió la dominante */
    AFF_R_DECAY = 1 << 10,      /* el objetivo va regresando a la línea base */
    AFF_R_MOOD_LIMIT = 1 << 11, /* el ánimo llegó a su tope */
    AFF_R_AI = 1 << 12,         /* la etiqueta de la IA */
    AFF_R_COUNT = 13
};
const char *affect_reason_name(unsigned bit); /* "sorpresa", … */

/* Un renglón de la trayectoria (§14): cada estímulo, cada cambio de
   dominante y cada salto grande de intensidad; nunca cada cuadro. */
typedef struct {
    long long id;
    const char *event; /* "estimulo", "cambio" o "muestra" */
    const char *cause;
    AffectState state;
    float target_valence, target_arousal;
    float trend;
    unsigned reasons;
} AffectTrace;
typedef void (*AffectTraceFn)(void *ctx, const AffectTrace *tr);

/* ---- el motor, sin nada global (para probarlo con su propio reloj) ---- */

typedef struct AffectEngine AffectEngine;

/* NULL = el temperamento de fábrica (neutral, sensibilidad 1, estabilidad 0.5). */
AffectEngine *affect_engine_create(const AffectTemperament *temper);
void affect_engine_destroy(AffectEngine *e);
void affect_engine_on_trace(AffectEngine *e, AffectTraceFn fn, void *ctx);
/* Avanza hasta t (segundos, reloj propio; nunca retrocede). Horas enteras
   cuestan lo mismo que unos segundos: ya quieto, salta. */
void affect_engine_advance(AffectEngine *e, double t);
void affect_engine_stimulus(AffectEngine *e, double t, const AffectStimulus *s);
/* Emociones con intensidad (la etiqueta de la IA): su mezcla se vuelve el
   objetivo casi completo. amount[k] de 0 a 1; neutral calma. */
void affect_engine_emotions(AffectEngine *e, double t, const float amount[AFF_COUNT], const char *cause);
void affect_engine_emotion(AffectEngine *e, double t, AffectKind k, float intensity, const char *cause);
/* Un resultado contra lo esperado: expected = probabilidad de que saliera
   bien (0..1). La sorpresa es |resultado - esperado|. */
void affect_engine_outcome(AffectEngine *e, double t, float expected, bool ok, float importance, const char *cause);
void affect_engine_event(AffectEngine *e, double t, AffectEvent ev);
/* Una herramienta terminó: lo esperado sale de cómo le ha ido a esa
   herramienta (empieza en 85 %), así que la sorpresa es el error de predicción. */
void affect_engine_tool(AffectEngine *e, double t, const char *name, bool ok);
AffectState affect_engine_state(const AffectEngine *e);
/* El objetivo al que va (para el simulador). */
void affect_engine_target(const AffectEngine *e, float *valence, float *arousal);
AffectMetrics affect_engine_metrics(const AffectEngine *e);
/* Los motivos desde la última vez que se preguntó (y los borra). */
unsigned affect_engine_take_reasons(AffectEngine *e);
/* Ánimo guardado: se le aplica el tiempo que pasó (regresa al temperamento). */
void affect_engine_set_mood(AffectEngine *e, float valence, float arousal, double elapsed_seconds);
/* Cuántos cambios de dominante van (para las pruebas de parpadeo). */
int affect_engine_switches(const AffectEngine *e);

/* ---- el de Sokari (con candado, reloj real y archivos) ---- */

/* Lee el ánimo guardado (afecto.json en la carpeta de Sokari). No hace
   falta llamarla: todo lo demás la llama la primera vez. */
void affect_init(void);
void affect_stimulus(const AffectStimulus *s);
void affect_emotions(const float amount[AFF_COUNT], const char *cause);
void affect_outcome(float expected, bool ok, float importance, const char *cause);
void affect_event(AffectEvent ev);
void affect_tool(const char *name, bool ok);
AffectState affect_get(void);
void affect_target(float *valence, float *arousal);
AffectMetrics affect_metrics(void);
/* Guarda el ánimo (también lo hace sola cada 5 minutos si cambió). */
void affect_save(void);

const char *affect_name(AffectKind k); /* "alegría", "temor", … */
void affect_reference(AffectKind k, float *valence, float *arousal);

/* La etiqueta de la IA: «[afecto: alegría 0.6]» (o dos emociones, o mal
   escrita, o cortada al final). Devuelve el texto sin ninguna etiqueta (heap;
   si no traía, idéntico). amount (puede ser NULL) recibe las emociones que
   entendió y found cuántas; una cortada se quita pero no cuenta. Se aplican
   aparte (affect_emotions), cuando la respuesta sí se usa. */
char *affect_take_tags(const char *reply, float amount[AFF_COUNT], int *found);
/* La instrucción para el modelo cuando la cara está puesta. */
const char *affect_prompt(void);

#endif

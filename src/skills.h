#ifndef SOKARI_SKILLS_H
#define SOKARI_SKILLS_H

#include <stdbool.h>
#include <time.h>

/* Skills locales: lo de todos los días que Sokari contesta en tu PC, sin
   preguntarle a la IA (0 tokens): la hora y la fecha, cuánto falta para algo,
   temporizadores y cronómetro, alarmas y recordatorios, cuentas y
   conversiones, el clima, tus notas y pendientes, cómo va la PC y la plática
   de siempre (saludos, chistes). Solo contestan lo que es claramente para
   ellas: si la frase trae algo más, se va a la IA. Cada una se apaga en
   Configuración (SOKARI_SKILLS_OFF). */

typedef struct {
    const char *id;      /* "hora", "clima"… (así va en SOKARI_SKILLS_OFF) */
    const char *name;    /* "Hora y fecha" */
    const char *example; /* «¿Qué hora es?», «¿Cuánto falta para Navidad?» */
} SkillInfo;

int skills_count(void);
const SkillInfo *skills_get(int i);

/* Si una skill local prendida entiende la frase, la hace y devuelve qué decir
   (heap) y cuál fue; si no, NULL (se va a la IA). *end: la plática terminó. */
char *skills_try(const char *text, const SkillInfo **which, bool *end);

/* Tus skills (la carpeta skills de tu memoria, un .md por skill). */
wchar_t *skills_user_dir(void);
/* Si lo que dijiste activa una de tus skills de IA: sus instrucciones para el
   modelo (heap) y su nombre (*name, heap); si no, NULL. */
char *skills_ai_for(const char *text, char **name);
/* Crea (o reemplaza) una skill tuya y dice cómo quedó (heap). steps: líneas
   "- abre: Spotify" (rutina); body: las instrucciones (IA). */
char *skills_create(const char *name, bool routine, const char *phrases, const char *when, const char *body,
                    const char *steps);
/* Tus skills y comandos propios, separados por comas (heap), y cuántos son. */
char *skills_user_summary(int *count);
/* Una skill nueva de ejemplo para editar; su ruta (heap) o NULL. */
wchar_t *skills_new_template(void);
/* Tus pendientes (notas.json, el perfil de ahora): trae lo que cambiaste en
   su nota de Obsidian y la reescribe. */
void skills_notes_sync(void);

/* Los temporizadores que ya sonaron: qué decir de cada uno (heap). */
char **skills_due_timers(int *n);

/* Para las pruebas: la hora de "ahora" (0 = la de verdad) y otras direcciones
   para el clima (NULL = las de Open-Meteo). */
void skills_set_clock(time_t now);
void skills_set_weather_urls(const char *geocoding, const char *forecast);

#endif

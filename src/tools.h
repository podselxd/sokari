#ifndef SOKARI_TOOLS_H
#define SOKARI_TOOLS_H

#include <stdbool.h>

#include "third_party/cJSON.h"

/* Cada herramienta recibe sus argumentos ya parseados (objeto JSON, nunca
   NULL) y devuelve un string UTF-8 en el heap con el resultado para el modelo.
   Ninguna ejecuta comandos de shell libres: son acciones acotadas. */
typedef char *(*ToolFn)(const cJSON *args);

char *run_tool(const char *name, const char *arguments_json);

/* Confirmación de voz (tool_policy.c): qué herramientas traen texto de afuera
   y qué acciones, con ese texto en la conversación, esperan un "sí". */
bool tool_brings_outside_text(const char *name);
bool tool_needs_confirmation(const char *name, const cJSON *args);
char *tool_describe_action(const char *name, const cJSON *args);
const char *arg_str(const cJSON *args, const char *key);
bool arg_bool(const cJSON *args, const char *key);
int arg_int(const cJSON *args, const char *key, int def);

/* sistema */
char *tool_open_app(const cJSON *a);
/* Abre una dirección en ese navegador ("opera", "chrome"…) o, sin navegador, en
   el predeterminado. false si no se pudo (o no está ese navegador). */
bool open_url(const char *url, const char *browser);
/* El primer video de una página de resultados de YouTube: 11 caracteres en id. */
bool youtube_first_video_id(const char *html, char id[12]);
char *tool_poner_en_youtube(const cJSON *a);
char *tool_control_media(const cJSON *a);
char *tool_control_desktop(const cJSON *a);
char *tool_focus_window(const cJSON *a);
char *tool_list_windows(const cJSON *a);
char *tool_type_text(const cJSON *a);
char *tool_leer_portapapeles(const cJSON *a);
char *tool_copiar_portapapeles(const cJSON *a);
char *tool_info_sistema(const cJSON *a);
char *focus_window_by_title(const char *needle, bool *ok);
bool open_target_is_dangerous(const wchar_t *path);
bool open_app_targets_file(const cJSON *a);
/* "youtube.com", "www.x", "https://…": ¿parece una página y no una app? */
bool looks_like_url(const char *s);
/* "ms-settings:", "file:"…: un esquema que no es http(s) ni una unidad (C:). */
bool has_other_scheme(const char *s);
/* Cuántas letras hay que cambiar para pasar de un nombre a otro (99 si alguno
   pasa de 60): para sugerir apps parecidas a la que no encontró. */
int name_edit_distance(const char *a, const char *b);
bool is_terminal_window_info(const wchar_t *cls, const wchar_t *exe);
bool foreground_is_terminal(void);
/* El nombre de la app de una ventana (un HWND) por su programa: "Chrome",
   "Discord", "el Explorador" (heap; NULL si no se sabe). Nunca su título: los
   títulos los pone cada página y podrían traer instrucciones para el modelo. */
char *window_app_name(void *hwnd);
/* ¿Ahí Enter abre o ejecuta cosas? Terminales, el Explorador y el escritorio
   (Enter abre lo seleccionado y su barra de direcciones corre comandos),
   «Ejecutar», Inicio y la búsqueda, el Administrador de tareas. */
bool window_runs_commands(void *hwnd);
/* OpenClipboard con reintentos (otra app puede tenerlo abierto un momento). */
bool open_clipboard(void);

/* archivos */
char *tool_list_files(const cJSON *a);
char *tool_read_file(const cJSON *a);
char *tool_buscar_archivo(const cJSON *a);
char *tool_mover_archivo(const cJSON *a);
char *tool_borrar_archivo(const cJSON *a);
wchar_t *known_folder_alias(const char *alias);
/* "descargas", una ruta con %USERPROFILE% o entre comillas… (heap). */
wchar_t *resolve_path(const char *ruta);
bool path_is_off_limits(const wchar_t *path);

/* web */
char *tool_web_search(const cJSON *a);
char *tool_leer_pagina(const cJSON *a);
typedef enum { URL_OK, URL_PRIVATE, URL_UNRESOLVED, URL_BAD } UrlCheck;
UrlCheck web_url_check(const char *url);

/* memoria y perfiles */
char *tool_guardar_dato(const cJSON *a);
char *tool_recordar(const cJSON *a);
char *tool_borrar_memoria_reciente(const cJSON *a);
char *tool_identificarse(const cJSON *a);
char *tool_proteger_perfil(const cJSON *a);
char *tool_exportar_a_obsidian(const cJSON *a);
char *tool_crear_recordatorio(const cJSON *a);
char *tool_create_macro(const cJSON *a);
char *tool_run_macro(const cJSON *a);

/* teclado (tools_keys.c) */
char *tool_presionar_teclas(const cJSON *a);
char *tool_atajos_de_app(const cJSON *a);
char *tool_ir_a_pestana(const cJSON *a);
char *tool_subir_archivo(const cJSON *a);
/* El archivo en el portapapeles como archivo, igual que "Copiar" en el
   Explorador: pegado en un chat, se adjunta. */
bool clipboard_set_file(const wchar_t *path);

/* calculadora y malla */
char *tool_calcular(const cJSON *a);
/* La misma calculadora sin herramienta: "25*4" -> 100. false si no se puede. */
bool calc_evaluate(const char *expression, double *out);
/* 100 -> "100", 2.5 -> "2.5" (heap). */
char *calc_format(double x);
char *tool_registrar_dispositivo(const cJSON *a);
char *tool_gestionar_dispositivo(const cJSON *a);
char *tool_cambiar_permisos(const cJSON *a);

#endif

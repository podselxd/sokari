#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "tools.h"
#include "pantalla.h"
#include "gustos.h"
#include "radio.h"
#include "lector.h"
#include "util.h"

typedef struct {
    const char *name;
    ToolFn fn;
} ToolEntry;

static const ToolEntry TOOLS[] = {
    {"open_app", tool_open_app},
    {"web_search", tool_web_search},
    {"poner_en_youtube", tool_poner_en_youtube},
    {"control_media", tool_control_media},
    {"control_desktop", tool_control_desktop},
    {"create_macro", tool_create_macro},
    {"crear_skill", tool_crear_skill},
    {"run_macro", tool_run_macro},
    {"guardar_dato", tool_guardar_dato},
    {"recordar", tool_recordar},
    {"borrar_memoria_reciente", tool_borrar_memoria_reciente},
    {"identificarse", tool_identificarse},
    {"proteger_perfil", tool_proteger_perfil},
    {"exportar_a_obsidian", tool_exportar_a_obsidian},
    {"crear_recordatorio", tool_crear_recordatorio},
    {"focus_window", tool_focus_window},
    {"list_windows", tool_list_windows},
    {"type_text", tool_type_text},
    {"list_files", tool_list_files},
    {"read_file", tool_read_file},
    {"buscar_archivo", tool_buscar_archivo},
    {"leer_portapapeles", tool_leer_portapapeles},
    {"copiar_portapapeles", tool_copiar_portapapeles},
    {"info_sistema", tool_info_sistema},
    {"ver_pantalla", tool_ver_pantalla},
    {"anotar_gusto", tool_anotar_gusto},
    {"radio", tool_radio},
    {"leer_en_voz", tool_leer_en_voz},
    {"leer_pagina", tool_leer_pagina},
    {"mover_archivo", tool_mover_archivo},
    {"crear_archivo", tool_crear_archivo},
    {"borrar_archivo", tool_borrar_archivo},
    {"calcular", tool_calcular},
    {"registrar_dispositivo", tool_registrar_dispositivo},
    {"gestionar_dispositivo", tool_gestionar_dispositivo},
    {"cambiar_permisos", tool_cambiar_permisos},
    {"presionar_teclas", tool_presionar_teclas},
    {"atajos_de_app", tool_atajos_de_app},
    {"ir_a_pestana", tool_ir_a_pestana},
    {"subir_archivo", tool_subir_archivo},
};

const char *arg_str(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

bool arg_bool(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return str_eq_ci(v->valuestring, "true") || !strcmp(v->valuestring, "1");
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    return false;
}

int arg_int(const cJSON *args, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring) return atoi(v->valuestring);
    return def;
}

/* Un error adentro de una herramienta nunca tiene que tirar abajo a Sokari:
   argumentos raros o faltantes llegan como "" y cada herramienta responde con
   un mensaje en vez de fallar. */
char *run_tool(const char *name, const char *arguments_json)
{
    cJSON *args = cJSON_Parse(arguments_json && *arguments_json ? arguments_json : "{}");
    if (!args) args = cJSON_CreateObject();
    if (!cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return xstrdup("Argumentos de la herramienta con formato inesperado.");
    }
    char *result = NULL;
    for (size_t i = 0; i < sizeof TOOLS / sizeof *TOOLS; i++) {
        if (!strcmp(TOOLS[i].name, name)) {
            result = TOOLS[i].fn(args);
            break;
        }
    }
    if (!result) result = str_printf("Herramienta desconocida: %s", name);
    cJSON_Delete(args);
    return result;
}

/* Qué herramientas meten a la conversación texto que no dijo quien habla, y
   qué acciones, mientras ese texto siga en la conversación, solo se hacen con
   un "sí" de voz. Va aparte de tools.c para poder probarlo sin Groq. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "tools.h"
#include "util.h"

static bool in_list(const char *name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(name, list[i])) return true;
    return false;
}

/* Internet, archivos, portapapeles, títulos de ventanas (las pestañas los
   pone cada página) y lo que responde otra PC: cualquiera puede traer
   instrucciones escondidas para el modelo. */
bool tool_brings_outside_text(const char *name)
{
    static const char *const OUTSIDE[] = {"web_search",        "leer_pagina",  "read_file",
                                          "list_files",        "buscar_archivo", "leer_portapapeles",
                                          "list_windows",      "gestionar_dispositivo"};
    return name && in_list(name, OUTSIDE, sizeof OUTSIDE / sizeof *OUTSIDE);
}

bool tool_needs_confirmation(const char *name, const cJSON *args)
{
    if (!name) return false;
    if (!strcmp(name, "open_app")) return open_app_targets_file(args);
    if (!strcmp(name, "type_text")) return arg_bool(args, "enviar");
    /* Cualquier tecla: con ellas se puede abrir «Ejecutar» y correr un comando. */
    if (!strcmp(name, "presionar_teclas")) return true;
    /* Darse permiso para todo es delicado; quitárselo, nunca. */
    if (!strcmp(name, "cambiar_permisos")) return arg_bool(args, "acceso_completo");
    /* run_macro también: un comando guardado puede traer type_text con enviar. */
    static const char *const ALWAYS[] = {"mover_archivo",         "borrar_archivo",        "create_macro",
                                         "crear_skill",
                                         "run_macro",             "registrar_dispositivo", "gestionar_dispositivo",
                                         "subir_archivo",         "crear_archivo"};
    return in_list(name, ALWAYS, sizeof ALWAYS / sizeof *ALWAYS);
}

/* Recorta para que la pregunta de confirmación se pueda decir en voz alta. */
static char *clip(const char *s)
{
    char *t = str_trim(s);
    size_t cut = utf8_truncate_len(t, 90);
    if (!t[cut]) return t;
    char *r = str_printf("%.*s…", (int)cut, t);
    free(t);
    return r;
}

/* La descripción la arma el código, no el modelo: si el modelo está engañado,
   igual se escucha qué se va a hacer de verdad. */
char *tool_describe_action(const char *name, const cJSON *args)
{
    char *a = clip(arg_str(args, !strcmp(name, "open_app")                 ? "name"
                                 : !strcmp(name, "type_text")              ? "texto"
                                 : !strcmp(name, "mover_archivo")          ? "origen"
                                 : !strcmp(name, "borrar_archivo")         ? "ruta"
                                 : !strcmp(name, "crear_archivo")          ? "ruta"
                                 : !strcmp(name, "create_macro")           ? "name"
                                 : !strcmp(name, "crear_skill")            ? "nombre"
                                 : !strcmp(name, "run_macro")              ? "name"
                                 : !strcmp(name, "registrar_dispositivo")  ? "nombre"
                                 : !strcmp(name, "gestionar_dispositivo")  ? "nombre"
                                                                           : ""));
    char *r;
    if (!strcmp(name, "open_app")) {
        r = str_printf("abrir %s", a);
    } else if (!strcmp(name, "type_text")) {
        char *v = clip(arg_str(args, "ventana"));
        r = *v ? str_printf("escribir «%s» en %s y enviarlo", a, v) : str_printf("escribir «%s» y enviarlo", a);
        free(v);
    } else if (!strcmp(name, "mover_archivo")) {
        char *d = clip(arg_str(args, "destino_carpeta"));
        r = str_printf("mover %s a %s", a, d);
        free(d);
    } else if (!strcmp(name, "crear_archivo")) {
        r = str_printf(arg_bool(args, "agregar") ? "agregarle texto a %s" : "crear el archivo %s", a);
    } else if (!strcmp(name, "borrar_archivo")) {
        r = str_printf("mandar a la papelera %s", a);
    } else if (!strcmp(name, "create_macro")) {
        r = str_printf("crear el comando «%s»", a);
    } else if (!strcmp(name, "crear_skill")) {
        r = str_printf("crear la skill «%s»", a);
    } else if (!strcmp(name, "run_macro")) {
        r = str_printf("ejecutar el comando «%s»", a);
    } else if (!strcmp(name, "registrar_dispositivo")) {
        char *h = clip(arg_str(args, "host"));
        r = str_printf("registrar el dispositivo %s en %s", a, h);
        free(h);
    } else if (!strcmp(name, "gestionar_dispositivo")) {
        char *c = clip(arg_str(args, "comando"));
        r = str_printf("mandarle a %s la orden «%s»", a, c);
        free(c);
    } else if (!strcmp(name, "presionar_teclas")) {
        KeyCombo k;
        char *keys = keys_parse(arg_str(args, "teclas"), &k) ? keys_describe(&k) : clip(arg_str(args, "teclas"));
        char *v = clip(arg_str(args, "ventana"));
        int times = arg_int(args, "veces", 1);
        char t[24] = "";
        if (times > 1) snprintf(t, sizeof t, " %d veces", times > 20 ? 20 : times);
        bool win = k.n == 2 && k.vk[0] == VK_LWIN;
        const char *note = "";
#ifdef _WIN32
        if (k.has_delete) note = " (en el Explorador borra lo que tengas seleccionado)";
        else if (k.has_enter) note = " (manda o ejecuta lo que esté escrito)";
        else if (win && k.vk[1] == 'R') note = " (abre «Ejecutar», donde se corren comandos)";
        else if (win && k.vk[1] == 'X') note = " (abre el menú de administrador de Windows)";
#else
        bool alt = false, ctrl = false, f2 = false, t_key = false;
        for (int i = 0; i < k.n; i++) {
            alt |= k.vk[i] == VK_MENU;
            ctrl |= k.vk[i] == VK_CONTROL;
            f2 |= k.vk[i] == VK_F1 + 1;
            t_key |= k.vk[i] == 'T';
        }
        bool alt_f2 = k.n == 2 && alt && f2;
        bool ctrl_alt_t = k.n == 3 && ctrl && alt && t_key;
        (void)win;
        if (k.has_delete) note = " (en Archivos borra lo que tengas seleccionado)";
        else if (k.has_enter) note = " (manda o ejecuta lo que esté escrito)";
        else if (alt_f2) note = " (abre «Ejecutar un comando» de GNOME)";
        else if (ctrl_alt_t) note = " (abre una terminal)";
#endif
        r = str_printf("oprimir %s%s%s%s%s", keys, t, *v ? " en " : "", v, note);
        free(v);
        free(keys);
    } else if (!strcmp(name, "subir_archivo")) {
        char *f = clip(arg_str(args, "ruta"));
        wchar_t *w = utf8_to_wide(f);
        char *base = wide_to_utf8(path_basename(w));
        char *v = clip(arg_str(args, "ventana"));
        r = str_printf("subir %s a %s%s", base, v, arg_bool(args, "enviar") ? " y mandarlo" : "");
        free(v);
        free(base);
        free(w);
        free(f);
    } else if (!strcmp(name, "borrar_memoria_reciente")) {
        r = xstrdup(!strcmp(arg_str(args, "periodo"), "todo") ? "borrar todo lo que hemos hablado de mi memoria"
                                                                : "borrar de mi memoria lo que hablamos hoy");
    } else if (!strcmp(name, "cambiar_permisos")) {
        r = xstrdup(arg_bool(args, "acceso_completo") ? "darme acceso completo (ya no preguntarte nada salvo antes de borrar)"
                                                       : "volver a pedirte permiso");
    } else {
        r = str_printf("usar %s", name);
    }
    free(a);
    return r;
}

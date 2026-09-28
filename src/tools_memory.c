#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "boveda.h"
#include "config.h"
#include "log.h"
#include "memory.h"
#include "tools.h"
#include "util.h"

static const char *SAFE_STEP_TOOLS[] = {"open_app", "web_search", "control_media", "control_desktop", "focus_window",
                                        "type_text"};

static bool is_safe_step(const char *tool)
{
    for (size_t i = 0; i < sizeof SAFE_STEP_TOOLS / sizeof *SAFE_STEP_TOOLS; i++)
        if (!strcmp(tool, SAFE_STEP_TOOLS[i])) return true;
    return false;
}

static cJSON *profile_facts(cJSON *all, bool create)
{
    cJSON *f = cJSON_GetObjectItem(all, current_speaker());
    if (!cJSON_IsObject(f)) {
        if (!create) return NULL;
        cJSON_DeleteItemFromObject(all, current_speaker());
        f = cJSON_AddObjectToObject(all, current_speaker());
    }
    return f;
}

/* Guarda hechos.json y reescribe la nota de tus datos (si elegiste bóveda). */
static bool facts_save(const cJSON *all, const cJSON *facts)
{
    wchar_t *ff = memory_file(L"hechos.json");
    bool ok = json_save(ff, all);
    free(ff);
    char *who = profile_display_name(current_speaker());
    boveda_push(BOVEDA_DATOS, who, facts);
    free(who);
    return ok;
}

/* Tus datos (los del perfil de ahora) en *all; lo que cambiaste en su nota de
   Obsidian manda. */
static cJSON *facts_load(cJSON **all)
{
    wchar_t *ff = memory_file(L"hechos.json");
    *all = json_load_object(ff);
    free(ff);
    cJSON *facts = profile_facts(*all, true);
    char *who = profile_display_name(current_speaker());
    if (boveda_pull(BOVEDA_DATOS, who, facts)) facts_save(*all, facts);
    free(who);
    return facts;
}

void memory_facts_sync(void)
{
    if (!state_try_lock(2000)) return;
    cJSON *all, *facts = facts_load(&all);
    facts_save(all, facts);
    cJSON_Delete(all);
    state_unlock();
}

char *tool_guardar_dato(const cJSON *a)
{
    char *clave = str_trim(arg_str(a, "clave"));
    const char *valor = arg_str(a, "valor");
    if (!*clave) {
        free(clave);
        return xstrdup("No entendí qué dato guardar.");
    }
    char *norm = str_lower(clave);
    cJSON *all, *facts = facts_load(&all);
    bool existed = cJSON_GetObjectItem(facts, norm) != NULL;
    cJSON_DeleteItemFromObject(facts, norm);
    cJSON_AddStringToObject(facts, norm, valor);
    facts_save(all, facts);
    cJSON_Delete(all);
    char *r = str_printf("Listo, %s '%s': %s.", existed ? "actualicé" : "guardé", clave, valor);
    free(norm);
    free(clave);
    return r;
}

/* "Borra la memoria de hoy": lo que se habló desde la medianoche (o todo lo
   hablado, con periodo "todo"). Los datos que guardaste con guardar_dato no se
   tocan. Borrar siempre pide un sí de voz (ver must_confirm en agent.c). */
char *tool_borrar_memoria_reciente(const cJSON *a)
{
    bool all = !strcmp(arg_str(a, "periodo"), "todo");
    double since = 0;
    if (!all) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_s(&lt, &now);
        lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
        since = (double)mktime(&lt);
    }
    int n = memory_forget_since(since);
    if (n < 0) return xstrdup("No pude borrar la memoria de la conversación.");
    log_msg("Memoria de la conversación borrada (%s): %d mensajes.", all ? "toda" : "hoy", n);
    return str_printf("Listo: borré %d mensajes de lo que hablamos%s. Tus datos guardados siguen.", n,
                      all ? "" : " hoy");
}

char *tool_recordar(const cJSON *a)
{
    char *q = str_lower(arg_str(a, "query"));
    cJSON *all, *facts = facts_load(&all);
    StrBuf sb;
    sb_init(&sb);
    cJSON *f;
    cJSON_ArrayForEach(f, facts)
    {
        if (!f->string || !*q) continue;
        char *k = str_lower(f->string);
        if (strstr(q, k) || strstr(k, q)) {
            char *v = cJSON_IsString(f) ? xstrdup(f->valuestring) : cJSON_PrintUnformatted(f);
            sb_appendf(&sb, "%s%s: %s", sb.len ? "\n" : "", f->string, v);
            free(v);
        }
        free(k);
    }
    cJSON_Delete(all);
    if (sb.len) {
        free(q);
        return sb_steal(&sb);
    }

    wchar_t *mf = memory_file(L"memoria.jsonl");
    char *text = read_file_all(mf, NULL);
    free(mf);
    if (!text) {
        sb_free(&sb);
        free(q);
        return xstrdup("No encontré nada guardado sobre eso.");
    }
    char *terms[32];
    int nterms = 0;
    char *qcopy = xstrdup(q), *save = NULL;
    for (char *t = strtok_r(qcopy, " \t", &save); t && nterms < 32; t = strtok_r(NULL, " \t", &save))
        if (strlen(t) > 2) terms[nterms++] = t;
    char *hits[5] = {0};
    int nh = 0;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *e = cJSON_Parse(line);
        cJSON *content = e ? cJSON_GetObjectItem(e, "content") : NULL;
        cJSON *ts = e ? cJSON_GetObjectItem(e, "ts") : NULL;
        cJSON *role = e ? cJSON_GetObjectItem(e, "role") : NULL;
        if (cJSON_IsString(content) && cJSON_IsNumber(ts) && nterms) {
            char *low = str_lower(content->valuestring);
            bool match = false;
            for (int i = 0; i < nterms && !match; i++) match = strstr(low, terms[i]) != NULL;
            if (match) {
                char *when = format_epoch_local(ts->valuedouble, "%d/%m %H:%M");
                free(hits[nh % 5]);
                hits[nh % 5] = str_printf("[%s] %s: %s", when, cJSON_IsString(role) ? role->valuestring : "?",
                                          content->valuestring);
                nh++;
                free(when);
            }
            free(low);
        }
        cJSON_Delete(e);
    }
    free(text);
    free(qcopy);
    free(q);
    if (!nh) {
        sb_free(&sb);
        return xstrdup("No encontré nada sobre eso en la memoria.");
    }
    int count = nh < 5 ? nh : 5;
    for (int i = 0; i < count; i++) {
        int idx = nh < 5 ? i : (nh + i) % 5;
        sb_appendf(&sb, "%s%s", sb.len ? "\n" : "", hits[idx]);
    }
    for (int i = 0; i < 5; i++) free(hits[i]);
    return sb_steal(&sb);
}

char *tool_identificarse(const cJSON *a)
{
    char *nombre = str_trim(arg_str(a, "nombre"));
    const char *password = arg_str(a, "password");
    if (!*nombre) {
        free(nombre);
        return xstrdup("No entendí el nombre.");
    }
    char *alias = str_lower(nombre);
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    char *result = NULL;
    cJSON *p;
    cJSON_ArrayForEach(p, profiles)
    {
        if (!cJSON_IsObject(p)) continue;
        bool match = !strcmp(p->string, alias);
        cJSON *ap;
        cJSON_ArrayForEach(ap, cJSON_GetObjectItem(p, "apodos"))
        {
            if (cJSON_IsString(ap) && str_eq_ci(ap->valuestring, alias)) match = true;
        }
        if (!match) continue;
        cJSON *dn = cJSON_GetObjectItem(p, "nombre");
        const char *display = cJSON_IsString(dn) ? dn->valuestring : p->string;
        if (!strcmp(p->string, current_speaker())) {
            result = str_printf("Ya estás como %s.", display);
            break;
        }
        cJSON *stored = cJSON_GetObjectItem(p, "password");
        if (cJSON_IsString(stored) && *stored->valuestring) {
            char *h = hash_password(password);
            bool ok = secure_equal(h, stored->valuestring);
            free(h);
            if (!ok) {
                result = str_printf("%s tiene sus datos protegidos. Dime la contraseña para entrar.", display);
                break;
            }
        }
        set_current_speaker(p->string);
        char *pend = reminders_take_pending_for(p->string);
        result = *pend ? str_printf("Hola de nuevo, %s. Ah, y me pediste que te recuerde: %s.", display, pend)
                       : str_printf("Hola de nuevo, %s.", display);
        free(pend);
        break;
    }
    if (!result) {
        cJSON *np = cJSON_CreateObject();
        cJSON_AddStringToObject(np, "nombre", nombre);
        cJSON *ap = cJSON_AddArrayToObject(np, "apodos");
        cJSON_AddItemToArray(ap, cJSON_CreateString(alias));
        cJSON_AddNullToObject(np, "password");
        cJSON_AddItemToObject(profiles, alias, np);
        json_save(pf, profiles);
        set_current_speaker(alias);
        result = str_printf("Un gusto, %s. Te voy a reconocer la próxima vez que uses ese nombre o apodo. Ojo: como "
                            "no reconozco voces de verdad, si alguien más usa el mismo nombre va a entrar a tu perfil "
                            "— si te importa que sea solo tuyo, dime una contraseña para protegerlo.",
                            nombre);
    }
    cJSON_Delete(profiles);
    free(pf);
    free(alias);
    free(nombre);
    return result;
}

char *tool_proteger_perfil(const cJSON *a)
{
    char *password = str_trim(arg_str(a, "password"));
    if (!*password) {
        free(password);
        return xstrdup("Dime qué contraseña quieres poner.");
    }
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    const char *key = current_speaker();
    cJSON *p = cJSON_GetObjectItem(profiles, key);
    if (!cJSON_IsObject(p)) {
        cJSON_DeleteItemFromObject(profiles, key);
        p = cJSON_AddObjectToObject(profiles, key);
        cJSON_AddStringToObject(p, "nombre", key);
        cJSON *ap = cJSON_AddArrayToObject(p, "apodos");
        cJSON_AddItemToArray(ap, cJSON_CreateString(key));
    }
    char *h = hash_password(password);
    cJSON_DeleteItemFromObject(p, "password");
    cJSON_AddStringToObject(p, "password", h);
    json_save(pf, profiles);
    free(h);
    cJSON_Delete(profiles);
    free(pf);
    SecureZeroMemory(password, strlen(password));
    free(password);
    return xstrdup("Listo, tu perfil ahora pide contraseña para que otros entren.");
}

static char *obsidian_slug(const char *text)
{
    char *s = str_trim(text);
    for (char *p = s; *p; p++)
        if (strchr("<>:\"/\\|?*", *p) || (unsigned char)*p < 32) *p = ' ';
    char *t = str_trim(s);
    free(s);
    if (!*t) {
        free(t);
        return xstrdup("Sin titulo");
    }
    return t;
}

static char *capitalize(const char *s)
{
    wchar_t *w = utf8_to_wide(s);
    size_t n = wcslen(w);
    if (n) {
        CharLowerBuffW(w, (DWORD)n);
        CharUpperBuffW(w, 1);
    }
    char *r = wide_to_utf8(w);
    free(w);
    return r;
}

/* La bóveda que elegiste en Configuración; si no, la de Obsidian en esta PC. */
static wchar_t *find_obsidian_vault(void)
{
    char *mine = config_obsidian_vault();
    wchar_t *w = *mine ? utf8_to_wide(mine) : NULL;
    free(mine);
    if (w && dir_exists(w)) return w;
    free(w);
    return boveda_detect();
}

static bool write_text(const wchar_t *dir, const char *name, const char *content)
{
    char *fname = str_printf("%s.md", name);
    wchar_t *wn = utf8_to_wide(fname);
    wchar_t *path = path_join(dir, wn);
    bool ok = write_file_atomic(path, content, strlen(content));
    free(path);
    free(wn);
    free(fname);
    return ok;
}

char *tool_exportar_a_obsidian(const cJSON *a)
{
    const char *nota = arg_str(a, "nota");
    wchar_t *vault = find_obsidian_vault();
    if (!vault) return xstrdup("No encontré ningún vault de Obsidian en esta máquina.");
    char *display = profile_display_name(current_speaker());
    char *person = obsidian_slug(display);
    wchar_t *wperson = utf8_to_wide(person);
    wchar_t *jdir = path_join(vault, L"Sokari");
    wchar_t *pdir = path_join(jdir, wperson);
    wchar_t *ddir = path_join(pdir, L"Datos");

    cJSON *all, *facts = facts_load(&all);
    char *r;
    if (!facts || !cJSON_GetArraySize(facts)) {
        r = str_printf("%s todavía no tiene datos guardados para exportar.", display);
    } else if (!ensure_dir(ddir)) {
        r = xstrdup("No pude escribir en el vault de Obsidian.");
    } else {
        StrBuf links;
        sb_init(&links);
        int count = 0;
        bool ok = true;
        cJSON *f;
        cJSON_ArrayForEach(f, facts)
        {
            char *valor = cJSON_IsString(f) ? xstrdup(f->valuestring) : cJSON_PrintUnformatted(f);
            char *titulo = capitalize(f->string);
            char *slug = obsidian_slug(titulo);
            char *content = str_printf("# %s\n\n- [[%s]]: %s\n", titulo, person, valor);
            ok = write_text(ddir, slug, content) && ok;
            sb_appendf(&links, "%s- [[%s]]: %s", links.len ? "\n" : "", slug, valor);
            count++;
            free(content);
            free(slug);
            free(titulo);
            free(valor);
        }
        StrBuf main;
        sb_init(&main);
        sb_appendf(&main, "# %s\n\n%s", display, links.data);
        if (!str_is_blank(nota)) {
            char *t = str_trim(nota);
            sb_appendf(&main, "\n\n%s\n", t);
            free(t);
        }
        ok = write_text(pdir, person, main.data) && ok;
        r = ok ? str_printf("Listo, exporté %d dato(s) de %s a Obsidian.", count, display)
               : xstrdup("No pude escribir en el vault de Obsidian.");
        sb_free(&main);
        sb_free(&links);
    }
    cJSON_Delete(all);
    free(ddir);
    free(pdir);
    free(jdir);
    free(wperson);
    free(person);
    free(display);
    free(vault);
    return r;
}

static char *spoken_datetime(double epoch)
{
    static const char *DAYS[] = {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
    static const char *MONTHS[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                                   "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
    time_t t = (time_t)epoch;
    struct tm tmv;
    if (localtime_s(&tmv, &t) != 0) return xstrdup("");
    return str_printf("el %s %d de %s a las %d:%02d", DAYS[tmv.tm_wday], tmv.tm_mday, MONTHS[tmv.tm_mon], tmv.tm_hour,
                      tmv.tm_min);
}

char *tool_crear_recordatorio(const cJSON *a)
{
    char *texto = str_trim(arg_str(a, "texto"));
    char *cuando = str_trim(arg_str(a, "cuando"));
    char *iso = str_trim(arg_str(a, "cuando_iso"));
    if (!*texto) {
        free(texto);
        free(cuando);
        free(iso);
        return xstrdup("No entendí qué quieres que recuerde.");
    }
    double when = 0;
    if (*iso && !parse_iso_local(iso, &when)) {
        free(iso);
        iso = xstrdup("");
    }
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    cJSON *items = cJSON_GetObjectItem(data, current_speaker());
    if (!cJSON_IsArray(items)) {
        cJSON_DeleteItemFromObject(data, current_speaker());
        items = cJSON_AddArrayToObject(data, current_speaker());
    }
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "texto", texto);
    cJSON_AddStringToObject(it, "cuando", cuando);
    cJSON_AddStringToObject(it, "cuando_iso", iso);
    cJSON_AddNumberToObject(it, "creado", now_epoch());
    cJSON_AddFalseToObject(it, "avisado");
    cJSON_AddItemToArray(items, it);
    json_save(rf, data);
    cJSON_Delete(data);
    free(rf);
    char *r;
    if (*iso) {
        char *sp = spoken_datetime(when);
        r = str_printf("Listo, te aviso %s.", sp);
        free(sp);
    } else if (*cuando) {
        r = str_printf("Listo, te lo voy a recordar para %s.", cuando);
    } else {
        r = xstrdup("Listo, te lo voy a recordar.");
    }
    free(texto);
    free(cuando);
    free(iso);
    return r;
}

char *tool_create_macro(const cJSON *a)
{
    char *name = str_trim(arg_str(a, "name"));
    const cJSON *steps = cJSON_GetObjectItem(a, "steps");
    cJSON *clean = cJSON_CreateArray();
    const cJSON *s;
    cJSON_ArrayForEach(s, steps)
    {
        const cJSON *tool = cJSON_GetObjectItem(s, "tool");
        const cJSON *args = cJSON_GetObjectItem(s, "args");
        if (!cJSON_IsString(tool) || !is_safe_step(tool->valuestring)) continue;
        cJSON *st = cJSON_CreateObject();
        cJSON_AddStringToObject(st, "tool", tool->valuestring);
        cJSON_AddItemToObject(st, "args", cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
        cJSON_AddItemToArray(clean, st);
    }
    int n = cJSON_GetArraySize(clean);
    if (!n || !*name) {
        cJSON_Delete(clean);
        free(name);
        return xstrdup("No pude crear el comando: ningún paso era válido.");
    }
    char *key = str_lower(name);
    wchar_t *cf = memory_file(L"commands.json");
    cJSON *macros = json_load_object(cf);
    cJSON_DeleteItemFromObject(macros, key);
    cJSON_AddItemToObject(macros, key, clean);
    json_save(cf, macros);
    cJSON_Delete(macros);
    free(cf);
    char *r = str_printf("Comando '%s' creado con %d paso(s).", name, n);
    free(key);
    free(name);
    return r;
}

char *tool_run_macro(const cJSON *a)
{
    char *name = str_trim(arg_str(a, "name"));
    char *key = str_lower(name);
    wchar_t *cf = memory_file(L"commands.json");
    cJSON *macros = json_load_object(cf);
    free(cf);
    cJSON *steps = cJSON_GetObjectItem(macros, key);
    char *r;
    if (!cJSON_IsArray(steps) || !cJSON_GetArraySize(steps)) {
        r = str_printf("No existe el comando '%s'.", name);
    } else {
        StrBuf sb;
        sb_init(&sb);
        cJSON *s;
        cJSON_ArrayForEach(s, steps)
        {
            cJSON *tool = cJSON_GetObjectItem(s, "tool");
            cJSON *args = cJSON_GetObjectItem(s, "args");
            if (!cJSON_IsString(tool) || !is_safe_step(tool->valuestring)) continue;
            char *aj = args ? cJSON_PrintUnformatted(args) : xstrdup("{}");
            char *out = run_tool(tool->valuestring, aj);
            sb_appendf(&sb, "%s%s", sb.len ? " " : "", out);
            free(out);
            free(aj);
            Sleep(150);
        }
        r = sb_steal(&sb);
        if (!r) r = xstrdup("Listo.");
    }
    cJSON_Delete(macros);
    free(key);
    free(name);
    return r;
}

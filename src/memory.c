#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "log.h"
#include "memory.h"
#include "util.h"

#define CALIBRATION_VERSION 2

static CRITICAL_SECTION g_state;
static char *g_speaker;

void state_lock(void)
{
    EnterCriticalSection(&g_state);
}

bool state_try_lock(unsigned timeout_ms)
{
    uint64_t deadline = GetTickCount64() + timeout_ms;
    do {
        if (TryEnterCriticalSection(&g_state)) return true;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return false;
}

void state_unlock(void)
{
    LeaveCriticalSection(&g_state);
}

void memory_init(void)
{
    InitializeCriticalSectionAndSpinCount(&g_state, 1000);
    g_speaker = xstrdup(DEFAULT_PROFILE);
    ensure_dir(g_paths.memory_dir);
}

wchar_t *memory_file(const wchar_t *name)
{
    return path_join(g_paths.memory_dir, name);
}

wchar_t *local_file(const wchar_t *name)
{
    return path_join(g_paths.local_dir, name);
}

cJSON *json_load_object(const wchar_t *path)
{
    char *text = read_file_all(path, NULL);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return cJSON_CreateObject();
    }
    return j;
}

bool json_save(const wchar_t *path, const cJSON *obj)
{
    char *text = cJSON_Print(obj);
    if (!text) return false;
    wchar_t *dir = path_dirname(path);
    ensure_dir(dir);
    free(dir);
    bool ok = write_file_atomic(path, text, strlen(text));
    if (!ok) {
        char *p = wide_to_utf8(path);
        log_msg("No pude guardar %s", p);
        free(p);
    }
    free(text);
    return ok;
}

const char *current_speaker(void)
{
    return g_speaker ? g_speaker : DEFAULT_PROFILE;
}

void set_current_speaker(const char *key)
{
    free(g_speaker);
    g_speaker = xstrdup(key && *key ? key : DEFAULT_PROFILE);
}

char *hash_password(const char *password)
{
    char *t = str_trim(password);
    char *h = sha256_hex(t);
    free(t);
    return h;
}

/* Al arrancar con un nombre configurado, se entra a ese perfil directo (o se
   crea). A diferencia de la versión anterior, NO marca como avisados sus
   recordatorios pendientes: esos se dicen en la primera conversación. */
void memory_identify_on_start(const char *user_name)
{
    char *name = str_trim(user_name);
    if (!*name) {
        free(name);
        return;
    }
    char *alias = str_lower(name);
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    const char *found = NULL;
    cJSON *p;
    cJSON_ArrayForEach(p, profiles)
    {
        if (!cJSON_IsObject(p)) continue;
        bool match = !strcmp(p->string, alias);
        cJSON *apodos = cJSON_GetObjectItem(p, "apodos");
        cJSON *a;
        cJSON_ArrayForEach(a, apodos)
        {
            if (cJSON_IsString(a) && str_eq_ci(a->valuestring, alias)) match = true;
        }
        if (match) {
            found = p->string;
            break;
        }
    }
    if (found) {
        set_current_speaker(found);
    } else {
        cJSON *np = cJSON_CreateObject();
        cJSON_AddStringToObject(np, "nombre", name);
        cJSON *ap = cJSON_AddArrayToObject(np, "apodos");
        cJSON_AddItemToArray(ap, cJSON_CreateString(alias));
        cJSON_AddNullToObject(np, "password");
        cJSON_AddItemToObject(profiles, alias, np);
        json_save(pf, profiles);
        set_current_speaker(alias);
    }
    cJSON_Delete(profiles);
    free(pf);
    free(alias);
    free(name);
}

void memory_persist(const char *role, const char *content)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "ts", now_epoch());
    cJSON_AddStringToObject(e, "role", role);
    cJSON_AddStringToObject(e, "content", content ? content : "");
    char *line = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    char *withnl = str_printf("%s\n", line);
    ensure_dir(g_paths.memory_dir);
    wchar_t *mf = memory_file(L"memoria.jsonl");
    append_file(mf, withnl, strlen(withnl));
    free(mf);
    free(withnl);
    free(line);
}

int memory_forget_since(double since)
{
    wchar_t *mf = memory_file(L"memoria.jsonl");
    char *text = read_file_all(mf, NULL);
    if (!text) {
        free(mf);
        return 0;
    }
    StrBuf keep;
    sb_init(&keep);
    int removed = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *e = cJSON_Parse(line);
        cJSON *ts = e ? cJSON_GetObjectItem(e, "ts") : NULL;
        if (cJSON_IsNumber(ts) && ts->valuedouble >= since) {
            removed++;
        } else {
            sb_append(&keep, line);
            sb_append(&keep, "\n");
        }
        cJSON_Delete(e);
    }
    bool ok = write_file_atomic(mf, keep.data ? keep.data : "", keep.len);
    sb_free(&keep);
    free(text);
    free(mf);
    return ok ? removed : -1;
}

cJSON *memory_recent_history(double window_seconds)
{
    cJSON *arr = cJSON_CreateArray();
    wchar_t *mf = memory_file(L"memoria.jsonl");
    char *text = read_file_all(mf, NULL);
    free(mf);
    if (!text) return arr;
    double cutoff = now_epoch() - window_seconds;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *e = cJSON_Parse(line);
        if (!e) continue;
        cJSON *ts = cJSON_GetObjectItem(e, "ts");
        cJSON *role = cJSON_GetObjectItem(e, "role");
        cJSON *content = cJSON_GetObjectItem(e, "content");
        if (cJSON_IsNumber(ts) && ts->valuedouble >= cutoff && cJSON_IsString(role) && cJSON_IsString(content) &&
            (!strcmp(role->valuestring, "user") || !strcmp(role->valuestring, "assistant"))) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_Delete(e);
    }
    free(text);
    return arr;
}

char *profile_display_name(const char *key)
{
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    free(pf);
    cJSON *p = cJSON_GetObjectItem(profiles, key);
    cJSON *n = cJSON_IsObject(p) ? cJSON_GetObjectItem(p, "nombre") : NULL;
    char *r = xstrdup(cJSON_IsString(n) ? n->valuestring : key);
    cJSON_Delete(profiles);
    return r;
}

/* Recordatorios con hora (cuando_iso) ya vencidos, de TODOS los perfiles: se
   avisan solos aunque nadie haya dicho "Hey Sokari". Se marcan como avisados
   al tomarlos para que no se repitan. */
int reminders_take_due(DueReminder **out)
{
    *out = NULL;
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    double now = now_epoch();
    int n = 0, cap = 0;
    DueReminder *list = NULL;
    bool changed = false;
    cJSON *items;
    cJSON_ArrayForEach(items, data)
    {
        if (!cJSON_IsArray(items)) continue;
        cJSON *it;
        cJSON_ArrayForEach(it, items)
        {
            cJSON *avisado = cJSON_GetObjectItem(it, "avisado");
            cJSON *due = cJSON_GetObjectItem(it, "cuando_iso");
            cJSON *texto = cJSON_GetObjectItem(it, "texto");
            if (cJSON_IsTrue(avisado) || !cJSON_IsString(due) || !*due->valuestring || !cJSON_IsString(texto))
                continue;
            double when;
            if (!parse_iso_local(due->valuestring, &when) || when > now) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 4;
                list = xrealloc(list, sizeof *list * (size_t)cap);
            }
            list[n].profile = xstrdup(items->string);
            list[n].texto = xstrdup(texto->valuestring);
            cJSON *tipo = cJSON_GetObjectItem(it, "tipo");
            list[n].alarm = cJSON_IsString(tipo) && !strcmp(tipo->valuestring, "alarma");
            n++;
            cJSON_DeleteItemFromObject(it, "avisado");
            cJSON_AddTrueToObject(it, "avisado");
            changed = true;
        }
    }
    if (changed) json_save(rf, data);
    cJSON_Delete(data);
    free(rf);
    *out = list;
    return n;
}

bool reminders_add(const char *profile, const char *texto, double when, bool alarm)
{
    time_t t = (time_t)when;
    struct tm tmv;
    if (localtime_s(&tmv, &t) != 0) return false;
    char iso[32];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S", &tmv);
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    cJSON *items = cJSON_GetObjectItem(data, profile);
    if (!cJSON_IsArray(items)) {
        cJSON_DeleteItemFromObject(data, profile);
        items = cJSON_AddArrayToObject(data, profile);
    }
    cJSON *it = cJSON_CreateObject();
    cJSON_AddStringToObject(it, "texto", texto);
    cJSON_AddStringToObject(it, "cuando", "");
    cJSON_AddStringToObject(it, "cuando_iso", iso);
    cJSON_AddNumberToObject(it, "creado", now_epoch());
    cJSON_AddFalseToObject(it, "avisado");
    if (alarm) cJSON_AddStringToObject(it, "tipo", "alarma");
    cJSON_AddItemToArray(items, it);
    bool ok = json_save(rf, data);
    cJSON_Delete(data);
    free(rf);
    return ok;
}

static bool pending_alarm(const cJSON *it, double *when)
{
    const cJSON *tipo = cJSON_GetObjectItem(it, "tipo"), *due = cJSON_GetObjectItem(it, "cuando_iso");
    return cJSON_IsString(tipo) && !strcmp(tipo->valuestring, "alarma") &&
           !cJSON_IsTrue(cJSON_GetObjectItem(it, "avisado")) && cJSON_IsString(due) &&
           parse_iso_local(due->valuestring, when);
}

int reminders_cancel_alarms(const char *profile)
{
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    cJSON *items = cJSON_GetObjectItem(data, profile);
    int n = 0;
    for (int i = cJSON_GetArraySize(items) - 1; i >= 0; i--) {
        double when;
        if (pending_alarm(cJSON_GetArrayItem(items, i), &when)) {
            cJSON_DeleteItemFromArray(items, i);
            n++;
        }
    }
    if (n) json_save(rf, data);
    cJSON_Delete(data);
    free(rf);
    return n;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int reminders_alarm_times(const char *profile, double **out)
{
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    cJSON *items = cJSON_GetObjectItem(data, profile);
    int n = 0;
    double *list = xcalloc((size_t)cJSON_GetArraySize(items) + 1, sizeof *list);
    const cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        double when;
        if (pending_alarm(it, &when)) list[n++] = when;
    }
    qsort(list, (size_t)n, sizeof *list, cmp_double);
    cJSON_Delete(data);
    free(rf);
    *out = list;
    return n;
}

void due_reminders_free(DueReminder *r, int n)
{
    for (int i = 0; i < n; i++) {
        free(r[i].profile);
        free(r[i].texto);
    }
    free(r);
}

/* Recordatorios sin avisar de un perfil (los que no tenían hora, o los que
   aún no llegaron no cuentan: solo los sin cuando_iso). Los marca avisados. */
char *reminders_take_pending_for(const char *profile)
{
    wchar_t *rf = memory_file(L"recordatorios.json");
    cJSON *data = json_load_object(rf);
    cJSON *items = cJSON_GetObjectItem(data, profile);
    StrBuf sb;
    sb_init(&sb);
    bool changed = false;
    cJSON *it;
    cJSON_ArrayForEach(it, items)
    {
        cJSON *avisado = cJSON_GetObjectItem(it, "avisado");
        cJSON *due = cJSON_GetObjectItem(it, "cuando_iso");
        cJSON *texto = cJSON_GetObjectItem(it, "texto");
        cJSON *cuando = cJSON_GetObjectItem(it, "cuando");
        if (cJSON_IsTrue(avisado) || !cJSON_IsString(texto)) continue;
        if (cJSON_IsString(due) && *due->valuestring) continue;
        if (sb.len) sb_append(&sb, "; ");
        sb_append(&sb, texto->valuestring);
        if (cJSON_IsString(cuando) && *cuando->valuestring) sb_appendf(&sb, " (%s)", cuando->valuestring);
        cJSON_DeleteItemFromObject(it, "avisado");
        cJSON_AddTrueToObject(it, "avisado");
        changed = true;
    }
    if (changed) json_save(rf, data);
    cJSON_Delete(data);
    free(rf);
    char *r = sb_steal(&sb);
    return r ? r : xstrdup("");
}

int reset_all_profile_passwords(void)
{
    state_lock();
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    int count = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, profiles)
    {
        if (!cJSON_IsObject(p)) continue;
        cJSON *pw = cJSON_GetObjectItem(p, "password");
        if (cJSON_IsString(pw) && *pw->valuestring) count++;
        cJSON_DeleteItemFromObject(p, "password");
        cJSON_AddNullToObject(p, "password");
    }
    json_save(pf, profiles);
    cJSON_Delete(profiles);
    free(pf);
    state_unlock();
    return count;
}

/* Contraseña puesta desde Configuración: protege el mismo perfil al que entra
   Sokari al arrancar (el del nombre configurado, o "default"). */
bool set_profile_password(const char *name, const char *password)
{
    state_lock();
    char *trimmed = str_trim(name);
    char *key = str_lower(*trimmed ? trimmed : DEFAULT_PROFILE);
    wchar_t *pf = memory_file(L"perfiles.json");
    cJSON *profiles = json_load_object(pf);
    cJSON *p = cJSON_GetObjectItem(profiles, key);
    if (!cJSON_IsObject(p)) {
        p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "nombre", *trimmed ? trimmed : DEFAULT_PROFILE);
        cJSON *ap = cJSON_AddArrayToObject(p, "apodos");
        cJSON_AddItemToArray(ap, cJSON_CreateString(key));
        cJSON_DeleteItemFromObject(profiles, key);
        cJSON_AddItemToObject(profiles, key, p);
    }
    cJSON_DeleteItemFromObject(p, "password");
    if (password && *password) {
        char *h = hash_password(password);
        cJSON_AddStringToObject(p, "password", h);
        free(h);
    } else {
        cJSON_AddNullToObject(p, "password");
    }
    bool ok = json_save(pf, profiles);
    cJSON_Delete(profiles);
    free(pf);
    free(key);
    free(trimmed);
    state_unlock();
    return ok;
}

bool load_calibration(int *threshold, int *runs)
{
    wchar_t *cf = memory_file(L"calibracion.json");
    cJSON *c = json_load_object(cf);
    free(cf);
    cJSON *v = cJSON_GetObjectItem(c, "version"), *t = cJSON_GetObjectItem(c, "threshold"),
          *r = cJSON_GetObjectItem(c, "runs");
    bool ok = cJSON_IsNumber(v) && v->valueint == CALIBRATION_VERSION && cJSON_IsNumber(t) && cJSON_IsNumber(r) &&
              r->valueint >= 0 && t->valuedouble > 0;
    if (ok) {
        *threshold = (int)t->valuedouble;
        *runs = r->valueint;
    }
    cJSON_Delete(c);
    return ok;
}

void save_calibration(int threshold, int runs)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddNumberToObject(c, "version", CALIBRATION_VERSION);
    cJSON_AddNumberToObject(c, "threshold", threshold);
    cJSON_AddNumberToObject(c, "runs", runs);
    wchar_t *cf = memory_file(L"calibracion.json");
    json_save(cf, c);
    free(cf);
    cJSON_Delete(c);
}

/* Actualizaciones en Linux. Sokari se instala con un paquete (.deb o .rpm)
   y cambiarlo pide tu contraseña, así que no se actualiza solo: revisa
   GitHub cada 6 horas y te avisa. «sokari --actualizar» (o Configuración)
   baja el paquete de este sistema, revisa que sea el publicado (tamaño y
   huella SHA-256 que da GitHub) y lo instala con apt o dnf. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "config.h"
#include "http.h"
#include "linux/linux.h"
#include "linux/proc.h"
#include "log.h"
#include "third_party/cJSON.h"
#include "update.h"
#include "util.h"

#define FIRST_CHECK_DELAY_MS (45 * 1000)
#define CHECK_INTERVAL_MS (6 * 3600 * 1000)

static void parse_version(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

bool linux_version_newer(const char *remote, const char *local)
{
    int a[3], b[3];
    parse_version(remote, a);
    parse_version(local, b);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

/* "deb" (Ubuntu), "rpm" (Fedora) o NULL si no se sabe con qué se instala aquí. */
const char *linux_package_kind(void)
{
    if (!access("/etc/debian_version", F_OK)) return "deb";
    if (!access("/etc/fedora-release", F_OK) || !access("/etc/redhat-release", F_OK)) return "rpm";
    return NULL;
}

void update_cleanup_old(void) {}

const cJSON *update_pick_asset(const cJSON *assets)
{
    const char *kind = linux_package_kind();
    if (!kind) return NULL;
    char *want = str_printf("Sokari.%s", kind);
    const cJSON *a, *found = NULL;
    cJSON_ArrayForEach(a, assets)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (!found && cJSON_IsString(name) && !strcmp(name->valuestring, want)) found = a;
    }
    free(want);
    return found;
}

typedef struct {
    char *tag, *url, *sha256;
    double size;
} Release;

static void release_free(Release *r)
{
    free(r->tag);
    free(r->url);
    free(r->sha256);
    memset(r, 0, sizeof *r);
}

static bool fetch_latest(Release *out, char **error)
{
    memset(out, 0, sizeof *out);
    HttpRequest req = {.method = "GET",
                       .url = "https://api.github.com/repos/" GITHUB_REPO "/releases/latest",
                       .headers = "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                       .timeout_ms = 20000};
    HttpResponse r = http_request(&req);
    if (r.status != 200) {
        *error = r.error ? str_printf("no pude consultar GitHub: %s", r.error)
                         : str_printf("GitHub respondió %d", r.status);
        http_response_free(&r);
        return false;
    }
    cJSON *j = cJSON_Parse(r.body);
    http_response_free(&r);
    const cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    const cJSON *asset = update_pick_asset(cJSON_GetObjectItem(j, "assets"));
    if (cJSON_IsString(tag)) out->tag = xstrdup(tag->valuestring);
    if (asset) {
        const cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
        const cJSON *size = cJSON_GetObjectItem(asset, "size");
        const cJSON *digest = cJSON_GetObjectItem(asset, "digest");
        if (cJSON_IsString(url)) out->url = xstrdup(url->valuestring);
        if (cJSON_IsNumber(size)) out->size = size->valuedouble;
        if (cJSON_IsString(digest) && str_starts_with(digest->valuestring, "sha256:"))
            out->sha256 = xstrdup(digest->valuestring + 7);
    }
    cJSON_Delete(j);
    if (!out->tag) {
        *error = xstrdup("GitHub no dijo cuál es la última versión");
        release_free(out);
        return false;
    }
    return true;
}

/* Que sea el paquete publicado: su tamaño, su huella y que empiece como un .deb o un .rpm. */
bool linux_package_verify(const char *path, double size, const char *sha256, const char *kind, char **error)
{
    size_t n = 0;
    wchar_t *w = utf8_to_wide(path);
    char *data = read_file_all(w, &n);
    free(w);
    bool magic = data && (!strcmp(kind, "deb") ? n > 8 && !memcmp(data, "!<arch>\n", 8)
                                               : n > 4 && !memcmp(data, "\xed\xab\xee\xdb", 4));
    bool ok = magic && n > 100000 && (size <= 0 || (double)n == size);
    if (!ok) {
        *error = str_printf("lo que bajé no es un paquete .%s válido", kind);
    } else if (!sha256) {
        ok = false;
        *error = xstrdup("GitHub no publicó la huella del paquete, así que no lo instalo");
    } else {
        unsigned char sum[32];
        sha256_bytes(data, n, sum);
        char *hex = hex_encode(sum, sizeof sum);
        ok = !strcasecmp(hex, sha256);
        free(hex);
        if (!ok) *error = xstrdup("el paquete no coincide con la huella publicada en GitHub");
    }
    free(data);
    return ok;
}

static volatile LONG g_installed;

bool linux_update_installed(void)
{
    return InterlockedCompareExchange(&g_installed, 0, 0) != 0;
}

char *linux_update_notice(const char *tag)
{
    return str_printf("Hay una versión nueva de Sokari (%s). Dale a «Instalar» (pide tu contraseña); también está en "
                      "el menú de Sokari → «" LINUX_UPDATE_LABEL "».",
                      tag);
}

char *update_check_now(void)
{
    Release rel;
    char *error = NULL;
    if (!fetch_latest(&rel, &error)) {
        char *r = str_printf("No pude revisar si hay versión nueva: %s.", error);
        free(error);
        return r;
    }
    char *r = NULL;
    const char *kind = linux_package_kind();
    if (!linux_version_newer(rel.tag, SOKARI_VERSION)) {
        r = str_printf("Ya tienes la última versión (%s).", SOKARI_VERSION);
    } else if (!kind || !rel.url) {
        r = str_printf("Hay una versión nueva (%s), pero no para este sistema: bájala de "
                       "https://github.com/" GITHUB_REPO "/releases/latest",
                       rel.tag);
    } else {
        ensure_dir(g_paths.update_dir);
        char *dir = wide_to_utf8(g_paths.update_dir);
        char *path = str_printf("%s/Sokari.%s", dir, kind);
        wchar_t *wpath = utf8_to_wide(path);
        HttpRequest req = {.method = "GET", .url = rel.url, .timeout_ms = 300000, .download_to = wpath};
        HttpResponse resp = http_request(&req);
        bool got = resp.status == 200 && !resp.error;
        if (!got) {
            r = str_printf("No pude bajar la versión %s: %s.", rel.tag,
                           resp.error ? resp.error : "GitHub no la entregó");
        } else if (!linux_package_verify(path, rel.size, rel.sha256, kind, &error)) {
            r = str_printf("No instalé la versión %s: %s.", rel.tag, error);
            free(error);
            unlink(path);
        } else {
            /* Pide tu contraseña; apt y dnf resuelven lo que haga falta. */
            const char *deb[] = {"pkexec", "apt-get", "install", "-y", path, NULL};
            const char *rpm[] = {"pkexec", "dnf", "install", "-y", path, NULL};
            int code = -1;
            free(proc_run(!strcmp(kind, "deb") ? deb : rpm, NULL, 0, 10 * 60 * 1000, 1 << 20, NULL, &code));
            if (code == 0) {
                r = str_printf("Listo: instalé Sokari %s. Ciérrame (Salir) y vuelve a abrirme para usarla.", rel.tag);
                log_msg("Actualización: instalé %s.", rel.tag);
                InterlockedExchange(&g_installed, 1);
            } else {
                r = str_printf("No se instaló la versión %s (¿cancelaste la contraseña?). El paquete quedó en %s.",
                               rel.tag, path);
            }
        }
        http_response_free(&resp);
        free(wpath);
        free(path);
        free(dir);
    }
    release_free(&rel);
    return r;
}

static DWORD WINAPI background(LPVOID arg)
{
    Sleep(FIRST_CHECK_DELAY_MS);
    char *told = NULL;
    for (;;) {
        Release rel;
        char *error = NULL;
        if (fetch_latest(&rel, &error)) {
            if (linux_version_newer(rel.tag, SOKARI_VERSION) && (!told || strcmp(told, rel.tag))) {
                free(told);
                told = xstrdup(rel.tag);
                char *msg = linux_update_notice(rel.tag);
                if (ui_active()) ui_post_notify_button("version-nueva", "Sokari", msg, "Instalar", "app.actualizar");
                else app_notify("Sokari", msg);
                free(msg);
            }
            release_free(&rel);
        } else {
            log_msg("Actualización: %s.", error);
            free(error);
        }
        Sleep(CHECK_INTERVAL_MS);
    }
    return 0;
}

void update_start_background(void)
{
    if (getenv("SOKARI_SIN_DESCARGAS")) return;
    HANDLE t = CreateThread(NULL, 0, background, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

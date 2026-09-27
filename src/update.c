/* Auto-actualización del .exe sin scripts: Windows no deja sobreescribir un
   .exe que está corriendo, pero sí renombrarlo. Se renombra el actual a
   <nombre>.exe.old, se pone el nuevo en su lugar, se lanza, y el nuevo borra
   el .old al arrancar. Solo se aplica con Sokari en reposo. El archivo
   conserva su nombre (los accesos directos y el inicio con Windows apuntan
   ahí). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "github.h"
#include "http.h"
#include "log.h"
#include "third_party/cJSON.h"
#include "update.h"
#include "util.h"

#define FIRST_CHECK_DELAY_MS (45 * 1000)
#define CHECK_INTERVAL_MS (6 * 3600 * 1000)

static volatile LONG g_busy;

static void parse_version(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static bool is_newer(const char *remote, const char *local)
{
    int a[3], b[3];
    parse_version(remote, a);
    parse_version(local, b);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static wchar_t *old_path(void)
{
    wchar_t *exe = exe_path();
    size_t n = wcslen(exe) + 8;
    wchar_t *p = xmalloc(sizeof(wchar_t) * n);
    swprintf(p, n, L"%ls.old", exe);
    free(exe);
    return p;
}

void update_cleanup_old(void)
{
    wchar_t *old = old_path();
    for (int i = 0; i < 20 && file_exists(old); i++) {
        if (DeleteFileW(old)) break;
        Sleep(250);
    }
    free(old);
}

static char *file_sha256(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char digest[32];
    char *r = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        unsigned char buf[1 << 16];
        DWORD got;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) BCryptHashData(h, buf, got, 0);
        if (BCryptFinishHash(h, digest, sizeof digest, 0) == 0) r = hex_encode(digest, sizeof digest);
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return r;
}

typedef struct {
    char *tag;
    char *url;
    char *sha256;
    double size;
} Release;

static void release_free(Release *r)
{
    free(r->tag);
    free(r->url);
    free(r->sha256);
    memset(r, 0, sizeof *r);
}

/* La última versión y su Sokari.exe: por la API de GitHub o, si no deja (403),
   por la página del release (ver github.h). */
static bool fetch_latest(Release *out, char **error)
{
    memset(out, 0, sizeof *out);
    GhRelease g;
    if (!gh_latest("Sokari.exe", &g, error)) return false;
    out->tag = g.tag;
    out->url = g.url;
    out->sha256 = g.sha256;
    out->size = g.size;
    if (!out->url) {
        *error = xstrdup("la última versión en GitHub no trae Sokari.exe");
        release_free(out);
        return false;
    }
    return true;
}

const cJSON *update_pick_asset(const cJSON *assets)
{
    return gh_pick_asset(assets, "Sokari.exe");
}

static bool download_verified(const Release *rel, const wchar_t *dest, char **error)
{
    ensure_dir(g_paths.update_dir);
    HttpRequest req = {.method = "GET", .url = rel->url, .timeout_ms = 120000, .download_to = dest};
    HttpResponse r = http_request(&req);
    bool ok = r.status == 200 && !r.error;
    if (!ok) *error = r.error ? xstrdup(r.error) : str_printf("la descarga respondió %d", r.status);
    http_response_free(&r);
    if (!ok) return false;

    WIN32_FILE_ATTRIBUTE_DATA info;
    GetFileAttributesExW(dest, GetFileExInfoStandard, &info);
    double size = (double)(((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow);
    char head[2] = {0};
    HANDLE f = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    if (f != INVALID_HANDLE_VALUE) {
        ReadFile(f, head, 2, &got, NULL);
        CloseHandle(f);
    }
    if (got != 2 || head[0] != 'M' || head[1] != 'Z' || size < 100000 || (rel->size > 0 && size != rel->size)) {
        *error = xstrdup("el archivo descargado no es un ejecutable válido");
        DeleteFileW(dest);
        return false;
    }
    if (rel->sha256) {
        char *sum = file_sha256(dest);
        bool match = sum && !_stricmp(sum, rel->sha256);
        free(sum);
        if (!match) {
            *error = xstrdup("la descarga no coincide con la huella publicada en GitHub");
            DeleteFileW(dest);
            return false;
        }
    }
    return true;
}

static bool apply_update(const wchar_t *new_exe, char **error)
{
    wchar_t *exe = exe_path();
    wchar_t *old = old_path();
    DeleteFileW(old);
    bool ok = false;
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) {
        *error = str_printf("no pude reemplazar el .exe (error %lu); ¿está en una carpeta protegida?",
                            (unsigned long)GetLastError());
    } else if (!MoveFileExW(new_exe, exe, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        *error = str_printf("no pude poner la versión nueva (error %lu)", (unsigned long)GetLastError());
        MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);
    } else {
        size_t n = wcslen(exe) + 64;
        wchar_t *cmd = xmalloc(sizeof(wchar_t) * n);
        swprintf(cmd, n, L"\"%ls\" --wait-for-pid %lu --updated", exe, (unsigned long)GetCurrentProcessId());
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            ok = true;
        } else {
            *error = xstrdup("no pude arrancar la versión nueva");
            MoveFileExW(exe, new_exe, MOVEFILE_REPLACE_EXISTING);
            MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING);
        }
        free(cmd);
    }
    free(old);
    free(exe);
    return ok;
}

/* Devuelve un mensaje para el usuario, o NULL si no había nada nuevo y
   quiet es true. Si se aplicó una versión nueva, Sokari se reinicia solo. */
static char *check_and_update(bool quiet)
{
    if (InterlockedExchange(&g_busy, 1)) return quiet ? NULL : xstrdup("Ya estoy revisando actualizaciones.");
    char *error = NULL, *msg = NULL;
    Release rel;
    if (!fetch_latest(&rel, &error)) {
        log_msg("Actualización: %s", error);
        msg = quiet ? NULL : str_printf("No pude revisar actualizaciones: %s.", error);
        free(error);
        InterlockedExchange(&g_busy, 0);
        return msg;
    }
    if (!is_newer(rel.tag, SOKARI_VERSION)) {
        msg = quiet ? NULL : str_printf("Ya tienes la última versión (%s).", SOKARI_VERSION);
        release_free(&rel);
        InterlockedExchange(&g_busy, 0);
        return msg;
    }
    log_msg("Hay una versión nueva: %s (tengo %s). Descargando...", rel.tag, SOKARI_VERSION);
    wchar_t *dest = path_join(g_paths.update_dir, L"Sokari_nuevo.exe");
    if (!download_verified(&rel, dest, &error)) {
        log_msg("Actualización: %s", error);
        msg = str_printf("Hay una versión nueva (%s) pero no pude bajarla: %s.", rel.tag, error);
        free(error);
    } else {
        for (int i = 0; i < 600 && app_get_state() != JV_IDLE; i++) Sleep(1000);
        char *text = str_printf("Actualizando a la versión %s, vuelvo en un segundo.", rel.tag);
        app_notify("Sokari", text);
        free(text);
        Sleep(1500);
        if (apply_update(dest, &error)) {
            log_msg("Actualización a %s aplicada, reiniciando.", rel.tag);
            app_request_quit();
            msg = str_printf("Actualizando a %s…", rel.tag);
        } else {
            log_msg("Actualización: %s", error);
            msg = str_printf("Hay una versión nueva (%s) pero no pude instalarla sola: %s. Bájala de "
                             "github.com/" GITHUB_REPO "/releases.",
                             rel.tag, error);
            app_notify("Sokari", msg);
            free(error);
        }
    }
    free(dest);
    release_free(&rel);
    InterlockedExchange(&g_busy, 0);
    return msg;
}

static DWORD WINAPI background(LPVOID arg)
{
    Sleep(FIRST_CHECK_DELAY_MS);
    for (;;) {
        char *msg = check_and_update(true);
        if (msg) app_notify("Sokari", msg);
        free(msg);
        Sleep(CHECK_INTERVAL_MS);
    }
    return 0;
}

void update_start_background(void)
{
    HANDLE t = CreateThread(NULL, 0, background, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

char *update_check_now(void)
{
    return check_and_update(false);
}

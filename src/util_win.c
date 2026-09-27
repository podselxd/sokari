/* Archivos, rutas, relojes, azar y texto ancho con las APIs de Windows. Lo
   que no depende del sistema está en util.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

void fatal_error(const char *msg, int code)
{
    /* Sin pedir memoria: puede ser justo lo que se acabó. */
    wchar_t w[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 512)) wcscpy(w, L"Sokari tuvo un error y se va a cerrar.");
    MessageBoxW(NULL, w, L"Sokari", MB_ICONERROR);
    ExitProcess((UINT)code);
}

wchar_t *utf8_to_wide(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return xwcsdup(L"");
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char *wide_n_to_utf8(const wchar_t *w, int len)
{
    if (!w || len == 0) return xstrdup("");
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *s = xmalloc((size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
    s[n] = 0;
    if (len < 0) s[n - 1] = 0;
    return s;
}

char *wide_to_utf8(const wchar_t *w)
{
    return wide_n_to_utf8(w ? w : L"", -1);
}

char *str_lower(const char *s)
{
    wchar_t *w = utf8_to_wide(s);
    DWORD n = (DWORD)wcslen(w);
    if (n) CharLowerBuffW(w, n);
    char *r = wide_to_utf8(w);
    free(w);
    return r;
}

char *read_file_all(const wchar_t *path, size_t *out_len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (LONGLONG)512 * 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }
    size_t n = (size_t)size.QuadPart;
    char *buf = xmalloc(n + 1);
    size_t got = 0;
    while (got < n) {
        DWORD chunk = 0;
        DWORD want = (DWORD)((n - got) > 0x10000000 ? 0x10000000 : (n - got));
        if (!ReadFile(h, buf + got, want, &chunk, NULL) || chunk == 0) break;
        got += chunk;
    }
    CloseHandle(h);
    buf[got] = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 2);
        got -= 3;
    }
    if (out_len) *out_len = got;
    return buf;
}

static bool write_whole(HANDLE h, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        DWORD chunk = (DWORD)(len > 0x10000000 ? 0x10000000 : len), wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, NULL) || wrote == 0) return false;
        p += wrote;
        len -= wrote;
    }
    return true;
}

/* Escribe primero a un .tmp y después lo renombra encima: si Sokari se corta
   a mitad de escritura, el archivo original queda intacto en vez de corrupto. */
bool write_file_atomic(const wchar_t *path, const void *data, size_t len)
{
    size_t n = wcslen(path);
    wchar_t *tmp = xmalloc((n + 8) * sizeof(wchar_t));
    swprintf(tmp, n + 8, L"%ls.tmp", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(tmp);
        return false;
    }
    bool ok = write_whole(h, data, len);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) {
        ok = false;
        for (int attempt = 0; attempt < 10 && !ok; attempt++) {
            ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (!ok) Sleep(50);
        }
    }
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}

bool append_file(const wchar_t *path, const void *data, size_t len)
{
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = write_whole(h, data, len);
    CloseHandle(h);
    return ok;
}

bool file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensure_dir(const wchar_t *path)
{
    if (dir_exists(path)) return true;
    int r = SHCreateDirectoryExW(NULL, path, NULL);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **dir_list(const wchar_t *dir, const char *suffix, int *n)
{
    *n = 0;
    wchar_t *pattern = path_join(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char **out = NULL;
    size_t sl = strlen(suffix);
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char *name = wide_to_utf8(fd.cFileName);
        size_t nl = strlen(name);
        if (nl > sl && !_stricmp(name + nl - sl, suffix) && *n < 500) {
            out = xrealloc(out, sizeof *out * (size_t)(*n + 1));
            out[(*n)++] = name;
        } else {
            free(name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (*n) qsort(out, (size_t)*n, sizeof *out, cmp_names);
    return out;
}

wchar_t *path_join(const wchar_t *a, const wchar_t *b)
{
    size_t na = wcslen(a), nb = wcslen(b);
    bool sep = na && a[na - 1] != L'\\' && a[na - 1] != L'/';
    wchar_t *r = xmalloc((na + nb + 2) * sizeof(wchar_t));
    memcpy(r, a, na * sizeof(wchar_t));
    size_t pos = na;
    if (sep) r[pos++] = L'\\';
    memcpy(r + pos, b, (nb + 1) * sizeof(wchar_t));
    return r;
}

wchar_t *exe_path(void)
{
    DWORD cap = MAX_PATH;
    for (;;) {
        wchar_t *buf = xmalloc(cap * sizeof(wchar_t));
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        if (n && n < cap) return buf;
        free(buf);
        if (cap > 32768) return xwcsdup(L"Sokari.exe");
        cap *= 2;
    }
}

wchar_t *exe_dir(void)
{
    wchar_t *p = exe_path();
    wchar_t *d = path_dirname(p);
    free(p);
    return d;
}

wchar_t *expand_env(const wchar_t *s)
{
    DWORD n = ExpandEnvironmentStringsW(s, NULL, 0);
    if (!n) return xwcsdup(s);
    wchar_t *r = xmalloc(n * sizeof(wchar_t));
    ExpandEnvironmentStringsW(s, r, n);
    return r;
}

double now_epoch(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    return (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
}

uint64_t now_ms(void)
{
    return GetTickCount64();
}

char *format_epoch_local(double ts, const char *fmt)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    if (localtime_s(&tmv, &t) != 0) return xstrdup("");
    char buf[128];
    size_t n = strftime(buf, sizeof buf, fmt, &tmv);
    return xstrndup(buf, n);
}

void random_bytes(void *buf, size_t n)
{
    if (BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        MessageBoxW(NULL, L"No pude generar números aleatorios seguros.", L"Sokari", MB_ICONERROR);
        ExitProcess(4);
    }
}

char *sha256_hex(const char *text)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32] = {0};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
            BCryptHashData(hash, (PUCHAR)text, (ULONG)strlen(text), 0);
            BCryptFinishHash(hash, digest, sizeof digest, 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex_encode(digest, sizeof digest);
}

bool civil_to_epoch(int Y, int M, int D, int h, int m, int sec, bool utc, double *out_epoch)
{
    SYSTEMTIME st = {.wYear = (WORD)Y, .wMonth = (WORD)M, .wDay = (WORD)D, .wHour = (WORD)h, .wMinute = (WORD)m,
                     .wSecond = (WORD)sec};
    FILETIME ft;
    if (utc) {
        if (!SystemTimeToFileTime(&st, &ft)) return false;
        ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
        *out_epoch = (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
        return true;
    }
    SYSTEMTIME st_utc;
    if (!TzSpecificLocalTimeToSystemTime(NULL, &st, &st_utc) || !SystemTimeToFileTime(&st_utc, &ft)) return false;
    ULARGE_INTEGER u = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    *out_epoch = (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
    return true;
}

bool copy_file(const wchar_t *src, const wchar_t *dst, bool overwrite)
{
    return CopyFileW(src, dst, !overwrite) != 0;
}

bool move_file(const wchar_t *from, const wchar_t *to)
{
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
}

long long file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return -1;
    return (long long)(((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow);
}

void console_write(const char *utf8)
{
    wchar_t *w = utf8_to_wide(utf8);
    DWORD wrote;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        if (!WriteConsoleW(out, w, (DWORD)wcslen(w), &wrote, NULL)) WriteFile(out, utf8, (DWORD)strlen(utf8), &wrote, NULL);
    }
    free(w);
}

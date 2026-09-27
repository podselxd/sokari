/* util.h en Linux: archivos, rutas, relojes, azar, SHA-256 y texto ancho. Las
   rutas llegan como wchar_t (igual que en Windows) y aquí se pasan a UTF-8,
   que es lo que entiende Linux. Todo lo que Sokari escribe queda solo para tu
   usuario (0600 y carpetas 0700): ahí están tus keys y tu memoria. */
#include <windows.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

void fatal_error(const char *msg, int code)
{
    fprintf(stderr, "Sokari: %s\n", msg);
    _exit(code);
}

/* ------------------------------------------------------ texto ancho --- */

/* UTF-8 a UTF-32 sin depender del locale; lo que no es UTF-8 válido queda
   como U+FFFD. */
wchar_t *utf8_to_wide(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s);
    wchar_t *w = xmalloc((n + 1) * sizeof(wchar_t));
    size_t k = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p, cp;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0;
        for (int i = 1; ok && i < len; i++) ok = (p[i] & 0xC0) == 0x80;
        if (!ok) {
            w[k++] = 0xFFFD;
            p++;
            continue;
        }
        if (len == 1) cp = c;
        else if (len == 2) cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        else if (len == 3) cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        else cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        /* Formas largas de más, sustitutos y fuera de rango: inválidos. */
        bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
        w[k++] = (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) ? 0xFFFD : (wchar_t)cp;
        p += len;
    }
    w[k] = 0;
    return w;
}

char *wide_n_to_utf8(const wchar_t *w, int len)
{
    if (!w || len == 0) return xstrdup("");
    size_t n = len < 0 ? wcslen(w) : (size_t)len;
    char *s = xmalloc(n * 4 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned cp = (unsigned)w[i];
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        if (cp < 0x80) {
            s[k++] = (char)cp;
        } else if (cp < 0x800) {
            s[k++] = (char)(0xC0 | (cp >> 6));
            s[k++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s[k++] = (char)(0xE0 | (cp >> 12));
            s[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[k++] = (char)(0x80 | (cp & 0x3F));
        } else {
            s[k++] = (char)(0xF0 | (cp >> 18));
            s[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[k++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    s[k] = 0;
    return s;
}

char *wide_to_utf8(const wchar_t *w)
{
    return wide_n_to_utf8(w ? w : L"", -1);
}

char *str_lower(const char *s)
{
    wchar_t *w = utf8_to_wide(s);
    for (wchar_t *p = w; *p; p++) *p = lx_towlower(*p);
    char *r = wide_to_utf8(w);
    free(w);
    return r;
}

/* ---------------------------------------------------------- archivos --- */

char *read_file_all(const wchar_t *path, size_t *out_len)
{
    char *p = wide_to_utf8(path);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    free(p);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || S_ISDIR(st.st_mode) || st.st_size > (off_t)512 * 1024 * 1024) {
        close(fd);
        return NULL;
    }
    size_t cap = (size_t)st.st_size, got = 0;
    char *buf = xmalloc(cap + 1);
    for (;;) {
        if (got == cap) { /* archivos que crecen o de /proc: se lee hasta el final */
            if (cap >= (size_t)512 * 1024 * 1024) break;
            cap = cap ? cap * 2 : 4096;
            buf = xrealloc(buf, cap + 1);
        }
        ssize_t r = read(fd, buf + got, cap - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    if (got >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 2);
        got -= 3;
    }
    if (out_len) *out_len = got;
    return buf;
}

static bool write_whole(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        len -= (size_t)w;
    }
    return true;
}

/* Primero a un .tmp y después se renombra encima: si Sokari se corta a mitad
   de escritura, el archivo original queda intacto. */
bool write_file_atomic(const wchar_t *path, const void *data, size_t len)
{
    char *p = wide_to_utf8(path);
    char *tmp = str_printf("%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write_whole(fd, data, len) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) unlink(tmp);
    free(tmp);
    free(p);
    return ok;
}

bool append_file(const wchar_t *path, const void *data, size_t len)
{
    char *p = wide_to_utf8(path);
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    free(p);
    if (fd < 0) return false;
    bool ok = write_whole(fd, data, len);
    close(fd);
    return ok;
}

static int stat_w(const wchar_t *path, struct stat *st)
{
    char *p = wide_to_utf8(path);
    int r = stat(p, st);
    free(p);
    return r;
}

bool file_exists(const wchar_t *path)
{
    struct stat st;
    return !stat_w(path, &st) && !S_ISDIR(st.st_mode);
}

bool dir_exists(const wchar_t *path)
{
    struct stat st;
    return !stat_w(path, &st) && S_ISDIR(st.st_mode);
}

long long file_size(const wchar_t *path)
{
    struct stat st;
    return stat_w(path, &st) ? -1 : (long long)st.st_size;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

char **dir_list(const wchar_t *dir, const char *suffix, int *n)
{
    *n = 0;
    char *p = wide_to_utf8(dir);
    DIR *d = opendir(p);
    char **out = NULL;
    size_t sl = strlen(suffix);
    struct dirent *e;
    while (d && (e = readdir(d)) && *n < 500) {
        size_t nl = strlen(e->d_name);
        if (nl <= sl || strcasecmp(e->d_name + nl - sl, suffix)) continue;
        char *full = str_printf("%s/%s", p, e->d_name);
        struct stat st;
        bool regular = !lstat(full, &st) && S_ISREG(st.st_mode);
        free(full);
        if (!regular) continue;
        out = xrealloc(out, sizeof *out * (size_t)(*n + 1));
        out[(*n)++] = xstrdup(e->d_name);
    }
    if (d) closedir(d);
    free(p);
    if (*n) qsort(out, (size_t)*n, sizeof *out, cmp_names);
    return out;
}

bool ensure_dir(const wchar_t *path)
{
    if (dir_exists(path)) return true;
    char *p = wide_to_utf8(path);
    bool ok = true;
    for (char *s = p + 1; ok && *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        ok = !mkdir(p, 0700) || errno == EEXIST;
        *s = '/';
    }
    if (ok) ok = !mkdir(p, 0700) || errno == EEXIST;
    free(p);
    return ok && dir_exists(path);
}

bool copy_file(const wchar_t *src, const wchar_t *dst, bool overwrite)
{
    char *s = wide_to_utf8(src), *d = wide_to_utf8(dst);
    int in = open(s, O_RDONLY | O_CLOEXEC);
    int out = in < 0 ? -1 : open(d, O_WRONLY | O_CREAT | O_CLOEXEC | (overwrite ? O_TRUNC : O_EXCL), 0600);
    bool ok = in >= 0 && out >= 0;
    char buf[65536];
    while (ok) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            ok = r == 0;
            break;
        }
        ok = write_whole(out, buf, (size_t)r);
    }
    if (in >= 0) close(in);
    if (out >= 0) {
        close(out);
        if (!ok) unlink(d);
    }
    free(s);
    free(d);
    return ok;
}

bool move_file(const wchar_t *from, const wchar_t *to)
{
    char *f = wide_to_utf8(from), *t = wide_to_utf8(to);
    bool ok = rename(f, t) == 0;
    int err = errno;
    free(f);
    free(t);
    /* Entre discos rename no puede: se copia y se borra el original. */
    if (!ok && err == EXDEV && copy_file(from, to, true)) {
        char *g = wide_to_utf8(from);
        ok = unlink(g) == 0;
        free(g);
    }
    return ok;
}

void console_write(const char *utf8)
{
    fputs(utf8, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------- rutas --- */

wchar_t *path_join(const wchar_t *a, const wchar_t *b)
{
    size_t na = wcslen(a), nb = wcslen(b);
    bool sep = na && a[na - 1] != L'/';
    wchar_t *r = xmalloc((na + nb + 2) * sizeof(wchar_t));
    memcpy(r, a, na * sizeof(wchar_t));
    size_t pos = na;
    if (sep) r[pos++] = L'/';
    memcpy(r + pos, b, (nb + 1) * sizeof(wchar_t));
    return r;
}

wchar_t *exe_path(void)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return xwcsdup(L"sokari");
    buf[n] = 0;
    return utf8_to_wide(buf);
}

wchar_t *exe_dir(void)
{
    wchar_t *p = exe_path();
    wchar_t *d = path_dirname(p);
    free(p);
    return d;
}

/* ~ al principio, $VAR, ${VAR} y también %VAR% (como en Windows). */
wchar_t *expand_env(const wchar_t *s)
{
    char *in = wide_to_utf8(s);
    StrBuf sb;
    sb_init(&sb);
    const char *p = in;
    if (p[0] == '~' && (p[1] == '/' || !p[1])) {
        const char *home = getenv("HOME");
        sb_append(&sb, home ? home : "");
        p++;
    }
    while (*p) {
        const char *start = NULL, *end = NULL;
        size_t skip = 0;
        if (p[0] == '$' && p[1] == '{' && (end = strchr(p + 2, '}'))) start = p + 2, skip = 1;
        else if (p[0] == '%' && (end = strchr(p + 1, '%')) && end > p + 1) start = p + 1, skip = 1;
        else if (p[0] == '$' && (p[1] == '_' || (p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z'))) {
            start = p + 1;
            end = start;
            while (*end == '_' || (*end >= 'A' && *end <= 'Z') || (*end >= 'a' && *end <= 'z') ||
                   (*end >= '0' && *end <= '9'))
                end++;
        }
        if (!start) {
            sb_append_char(&sb, *p++);
            continue;
        }
        char *name = xstrndup(start, (size_t)(end - start));
        const char *v = getenv(name);
        /* %USERPROFILE% de las rutas de Windows: tu carpeta personal. */
        if (!v && !strcmp(name, "USERPROFILE")) v = getenv("HOME");
        if (v) sb_append(&sb, v);
        else sb_append_n(&sb, p, (size_t)(end - p) + skip);
        free(name);
        p = end + skip;
    }
    wchar_t *r = utf8_to_wide(sb.data ? sb.data : "");
    sb_free(&sb);
    free(in);
    return r;
}

/* ------------------------------------------------------------ tiempo --- */

double now_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

uint64_t now_ms(void)
{
    return GetTickCount64();
}

char *format_epoch_local(double ts, const char *fmt)
{
    time_t t = (time_t)ts;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return xstrdup("");
    char buf[128];
    size_t n = strftime(buf, sizeof buf, fmt, &tmv);
    return xstrndup(buf, n);
}

bool civil_to_epoch(int Y, int M, int D, int h, int m, int sec, bool utc, double *out_epoch)
{
    struct tm t = {.tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = sec};
    if (utc) {
        time_t r = timegm(&t);
        if (r == (time_t)-1) return false;
        *out_epoch = (double)r;
        return true;
    }
    t.tm_isdst = -1; /* que la libc decida si ese día hay horario de verano */
    time_t r = mktime(&t);
    if (r == (time_t)-1) return false;
    *out_epoch = (double)r;
    return true;
}

/* ------------------------------------------------------ azar y SHA-256 --- */

void random_bytes(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) fatal_error("No pude generar números aleatorios seguros.", 4);
        p += r;
        n -= (size_t)r;
    }
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char b[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
    }
    h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void sha256_bytes(const void *data, size_t n, unsigned char out[32])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const unsigned char *p = data;
    size_t left = n;
    while (left >= 64) {
        sha256_block(h, p);
        p += 64;
        left -= 64;
    }
    unsigned char last[128] = {0};
    memcpy(last, p, left);
    last[left] = 0x80;
    size_t blocks = left < 56 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t i = 0; i < blocks; i++) sha256_block(h, last + i * 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h[i];
    }
}

char *sha256_hex(const char *text)
{
    unsigned char digest[32];
    sha256_bytes(text, strlen(text), digest);
    return hex_encode(digest, sizeof digest);
}

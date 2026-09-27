#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "github.h"
#include "http.h"
#include "log.h"
#include "util.h"

static char *g_api, *g_web;

void gh_set_bases(const char *api, const char *web)
{
    free(g_api);
    free(g_web);
    g_api = api ? xstrdup(api) : NULL;
    g_web = web ? xstrdup(web) : NULL;
}

static const char *api_base(void)
{
    return g_api ? g_api : "https://api.github.com";
}

static const char *web_base(void)
{
    return g_web ? g_web : "https://github.com";
}

void gh_release_free(GhRelease *r)
{
    free(r->tag);
    free(r->url);
    free(r->sha256);
    memset(r, 0, sizeof *r);
}

const cJSON *gh_pick_asset(const cJSON *assets, const char *name)
{
    const cJSON *a;
    if (!name) return NULL;
    cJSON_ArrayForEach(a, assets)
    {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, name)) return a;
    }
    return NULL;
}

/* vX.Y.Z (o X.Y.Z, o con un cuarto número): nada más, porque con él se arma
   la dirección de la descarga. */
static bool tag_ok(const char *t)
{
    const char *p = t;
    if (*p == 'v') p++;
    int parts = 0;
    for (;;) {
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
        parts++;
        if (!*p) break;
        if (*p++ != '.') return false;
    }
    return parts >= 2 && parts <= 4 && strlen(t) < 24;
}

char *gh_tag_from_location(const char *location)
{
    static const char MARK[] = "/releases/tag/";
    const char *p = location ? strstr(location, MARK) : NULL;
    if (!p) return NULL;
    char *tag = xstrdup(p + sizeof MARK - 1);
    tag[strcspn(tag, "?#/")] = 0;
    if (tag_ok(tag)) return tag;
    free(tag);
    return NULL;
}

char *gh_parse_sha256(const char *text)
{
    const char *p = text ? text : "";
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)p[i])) return NULL;
    if (isxdigit((unsigned char)p[64])) return NULL; /* más largo: no es un sha256 */
    char *hex = xmalloc(65);
    for (int i = 0; i < 64; i++) hex[i] = (char)tolower((unsigned char)p[i]);
    hex[64] = 0;
    return hex;
}

static bool from_api(const char *asset, GhRelease *out, char **error)
{
    char *url = str_printf("%s/repos/%s/releases/latest", api_base(), GITHUB_REPO);
    HttpRequest req = {.method = "GET",
                       .url = url,
                       .headers = "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                       .timeout_ms = 20000};
    HttpResponse r = http_request(&req);
    free(url);
    if (r.status != 200) {
        *error = r.error                                   ? str_printf("no pude consultar la API de GitHub: %s", r.error)
                 : r.status == 403 || r.status == 429 ? str_printf("la API de GitHub no dejó consultar (%d: su límite de "
                                                                  "consultas para tu red)",
                                                                  r.status)
                                                      : str_printf("la API de GitHub respondió %d", r.status);
        http_response_free(&r);
        return false;
    }
    cJSON *j = cJSON_Parse(r.body);
    http_response_free(&r);
    const cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    if (cJSON_IsString(tag)) out->tag = xstrdup(tag->valuestring);
    const cJSON *a = gh_pick_asset(cJSON_GetObjectItem(j, "assets"), asset);
    if (a) {
        const cJSON *u = cJSON_GetObjectItem(a, "browser_download_url");
        const cJSON *size = cJSON_GetObjectItem(a, "size");
        const cJSON *digest = cJSON_GetObjectItem(a, "digest");
        if (cJSON_IsString(u)) out->url = xstrdup(u->valuestring);
        if (cJSON_IsNumber(size)) out->size = size->valuedouble;
        if (cJSON_IsString(digest) && str_starts_with(digest->valuestring, "sha256:"))
            out->sha256 = gh_parse_sha256(digest->valuestring + 7);
    }
    cJSON_Delete(j);
    if (!out->tag) {
        *error = xstrdup("GitHub no dijo cuál es la última versión");
        gh_release_free(out);
        return false;
    }
    return true;
}

static bool from_page(const char *asset, GhRelease *out, char **error)
{
    char *url = str_printf("%s/%s/releases/latest", web_base(), GITHUB_REPO);
    HttpRequest req = {.method = "GET", .url = url, .timeout_ms = 20000, .max_bytes = 64 * 1024, .no_redirects = true};
    HttpResponse r = http_request(&req);
    free(url);
    char *tag = r.status >= 300 && r.status < 400 ? gh_tag_from_location(r.location) : NULL;
    if (!tag) {
        *error = r.error ? str_printf("no pude abrir la página de GitHub: %s", r.error)
                         : str_printf("la página de GitHub respondió %d sin la versión", r.status);
        http_response_free(&r);
        return false;
    }
    http_response_free(&r);
    out->tag = tag;
    out->por_pagina = true;
    if (!asset) return true;
    out->url = str_printf("%s/%s/releases/download/%s/%s", web_base(), GITHUB_REPO, tag, asset);
    char *surl = str_printf("%s.sha256", out->url);
    HttpRequest sreq = {.method = "GET", .url = surl, .timeout_ms = 20000, .max_bytes = 4096};
    HttpResponse s = http_request(&sreq);
    free(surl);
    char *hex = s.status == 200 ? gh_parse_sha256(s.body) : NULL;
    http_response_free(&s);
    if (!hex) {
        *error = str_printf("la versión %s no trae la huella de %s (%s.sha256) para revisar la descarga", tag, asset,
                            asset);
        gh_release_free(out);
        return false;
    }
    out->sha256 = hex;
    return true;
}

bool gh_latest(const char *asset, GhRelease *out, char **error)
{
    memset(out, 0, sizeof *out);
    char *api_error = NULL, *page_error = NULL;
    if (from_api(asset, out, &api_error)) return true;
    if (from_page(asset, out, &page_error)) {
        log_msg("Actualizaciones: %s; lo revisé en la página del release (%s).", api_error, out->tag);
        free(api_error);
        return true;
    }
    *error = str_printf("%s, y la página del release tampoco: %s", api_error, page_error);
    free(api_error);
    free(page_error);
    return false;
}

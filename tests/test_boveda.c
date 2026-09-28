/* La memoria en Obsidian (src/boveda.c) y los gustos para siempre
   (src/gustos.c): sin bóveda todo sigue igual; con bóveda, sus gustos, tus
   datos, tus pendientes y su música se escriben como notas, lo que cambias en
   una nota manda (con respaldo), la primera vez se juntan, una nota que no se
   entiende se respalda antes de reescribirse, y los nombres con acentos
   funcionan. Los gustos para siempre: a lo más 10, no salen con el tope de
   40 ni con «borra la memoria», no cambian aunque se lo pidan y se vuelven
   así solos al sentirlos 3 días distintos. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/cJSON.h"
#include "boveda.h"
#include "config.h"
#include "gustos.h"
#include "memory.h"
#include "radio.h"
#include "skills.h"
#include "tools.h"
#include "util.h"

static int g_fail, g_total;

static void check(bool ok, const char *what)
{
    g_total++;
    printf("%s %s\n", ok ? "ok   " : "FALLA", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

/* Vacía una carpeta (solo sus archivos). */
static void empty_dir(const wchar_t *dir)
{
    int n = 0;
    char **names = dir_list(dir, "", &n);
    for (int i = 0; i < n; i++) {
        wchar_t *w = utf8_to_wide(names[i]), *p = path_join(dir, w);
        DeleteFileW(p);
        free(p);
        free(w);
        free(names[i]);
    }
    free(names);
}

static wchar_t *sub(const wchar_t *dir, const wchar_t *name)
{
    wchar_t *p = path_join(dir, name);
    ensure_dir(p);
    empty_dir(p);
    return p;
}

static char *gusto(const char *cosa, const char *nivel, bool siempre, const char *soltar)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "cosa", cosa);
    cJSON_AddStringToObject(a, "nivel", nivel);
    if (siempre) cJSON_AddTrueToObject(a, "para_siempre");
    if (soltar) cJSON_AddStringToObject(a, "soltar", soltar);
    char *r = tool_anotar_gusto(a);
    cJSON_Delete(a);
    return r;
}

static char *tool2(char *(*fn)(const cJSON *), const char *k1, const char *v1, const char *k2, const char *v2)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, k1, v1);
    if (k2) cJSON_AddStringToObject(a, k2, v2);
    char *r = fn(a);
    cJSON_Delete(a);
    return r;
}

static char *note_text(BovedaKind k, const char *who)
{
    wchar_t *p = boveda_note_path(k, who);
    char *t = p ? read_file_all(p, NULL) : NULL;
    free(p);
    return t;
}

/* Cambia la nota como lo harías en Obsidian: reemplaza from por to. */
static bool edit_note(BovedaKind k, const char *who, const char *from, const char *to)
{
    wchar_t *p = boveda_note_path(k, who);
    char *t = p ? read_file_all(p, NULL) : NULL;
    char *at = t ? strstr(t, from) : NULL;
    bool ok = false;
    if (at) {
        StrBuf sb;
        sb_init(&sb);
        sb_append_n(&sb, t, (size_t)(at - t));
        sb_append(&sb, to);
        sb_append(&sb, at + strlen(from));
        ok = write_file_atomic(p, sb.data, sb.len);
        sb_free(&sb);
    }
    free(t);
    free(p);
    return ok;
}

static bool write_note(BovedaKind k, const char *who, const char *text)
{
    wchar_t *p = boveda_note_path(k, who);
    bool ok = p && write_file_atomic(p, text, strlen(text));
    free(p);
    return ok;
}

static bool file_has(const wchar_t *dir, const wchar_t *name, const char *text)
{
    wchar_t *p = path_join(dir, name);
    char *t = read_file_all(p, NULL);
    bool ok = t && (!text || strstr(t, text));
    free(t);
    free(p);
    return ok;
}

/* ¿Hay en la carpeta un archivo que empiece con prefix y traiga text? */
static bool some_file_has(const wchar_t *dir, const char *prefix, const char *text)
{
    int n = 0;
    char **names = dir_list(dir, "", &n);
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (!found && str_starts_with(names[i], prefix)) {
            wchar_t *w = utf8_to_wide(names[i]);
            found = file_has(dir, w, text);
            free(w);
        }
        free(names[i]);
    }
    free(names);
    return found;
}

static bool has(const char *s, const char *what)
{
    return s && strstr(s, what);
}

static void set_vault(const wchar_t *dir)
{
    char *u = dir ? wide_to_utf8(dir) : xstrdup("");
    config_set_obsidian_vault(u);
    free(u);
}

int wmain(void)
{
    SetConsoleOutputCP(CP_UTF8);
    paths_init();
    /* Todo en una carpeta de prueba: nunca tu memoria ni tu config de verdad. */
    wchar_t *exe = exe_dir(), *root = sub(exe, L"prueba_boveda");
    wchar_t *local = sub(root, L"local"), *mem = sub(root, L"memoria");
    wchar_t *vault = sub(root, L"Bóveda Ñandú"), *vault2 = sub(root, L"Otra bóveda");
    wchar_t *notes = sub(vault, L"Sokari"), *notes2 = sub(vault2, L"Sokari");
    wchar_t *backups = sub(mem, L"respaldos");
    g_paths.local_dir = local;
    g_paths.memory_dir = mem;
    g_paths.config_file = path_join(local, L"config.env");
    memory_init();
    gustos_set_today("2026-09-01");

    /* ----------------------------------------------- gustos para siempre --- */
    char *r = gusto("la lluvia", "encanta", true, NULL);
    char *s = gustos_resumen();
    check(has(s, "Tus gustos para siempre") && has(s, "te encanta: la lluvia"),
          "un gusto para siempre va primero en lo que se le recuerda");
    free(s), free(r);
    r = gusto("la lluvia", "desagrada", false, NULL);
    s = gustos_resumen();
    check(has(r, "No cambió") && has(s, "te encanta: la lluvia") && !has(s, "desagrada: la lluvia"),
          "un gusto para siempre no cambia aunque se lo pidan (y ella lo sabe)");
    free(s), free(r);
    for (int i = 0; i < 45; i++) {
        char c[32];
        snprintf(c, sizeof c, "cosa %d", i);
        free(gusto(c, "gusta", false, NULL));
    }
    s = gustos_resumen();
    check(has(s, "la lluvia") && !has(s, "cosa 4,") && has(s, "cosa 5,") && has(s, "cosa 44"),
          "con el tope de 40 salen los normales más viejos, nunca uno para siempre");
    free(s);
    for (int i = 0; i < 11; i++) {
        char c[32];
        snprintf(c, sizeof c, "tema %d", i);
        r = gusto(c, "encanta", true, NULL);
        if (i == 9) check(has(r, "suelta uno"), "el once para siempre no entra: tiene que soltar uno");
        free(r);
    }
    s = gustos_describe();
    check(has(s, "Para siempre (10 de 10)"), "a lo más 10 para siempre");
    free(s);
    free(gusto("tema 9", "encanta", true, "tema 0"));
    s = gustos_resumen();
    char *para = s ? xstrndup(s, strstr(s, "Tus demás") ? (size_t)(strstr(s, "Tus demás") - s) : strlen(s)) : NULL;
    check(has(para, "tema 9") && !has(para, "tema 0"), "soltando uno, entra el nuevo");
    free(para), free(s);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "periodo", "todo");
    free(tool_borrar_memoria_reciente(a));
    cJSON_Delete(a);
    s = gustos_describe();
    check(has(s, "Para siempre (10 de 10)") && has(s, "la lluvia"), "«borra la memoria» no toca sus gustos");
    free(s);
    check(gustos_soltar_para_siempre() == 10, "«Soltar sus gustos para siempre» los vuelve normales");
    s = gustos_describe();
    check(s && !has(s, "Para siempre"), "…y ya no hay ninguno para siempre");
    free(s);
    /* Tres días distintos sintiendo lo mismo: para siempre solo. */
    free(gusto("el café de olla", "encanta", false, NULL));
    free(gusto("el café de olla", "encanta", false, NULL));
    gustos_set_today("2026-09-02");
    free(gusto("el café de olla", "encanta", false, NULL));
    s = gustos_describe();
    bool two = s && !has(s, "Para siempre");
    free(s);
    gustos_set_today("2026-09-05");
    free(gusto("el café de olla", "encanta", false, NULL));
    s = gustos_describe();
    check(two && has(s, "Para siempre (1 de 10): le encanta: el café de olla"),
          "sentirlo en 3 días distintos lo vuelve para siempre (no 2, ni 3 veces el mismo día)");
    free(s);
    /* Gustos de la 2.8.0 (sin días ni para siempre): se leen igual. */
    wchar_t *gp = path_join(mem, L"gustos.json");
    const char *old = "[{\"cosa\":\"el mar\",\"nivel\":\"encanta\"},{\"cosa\":\"el tráfico\",\"nivel\":\"desagrada\"}]";
    write_file_atomic(gp, old, strlen(old));
    s = gustos_resumen();
    r = gusto("el mar", "encanta", false, NULL);
    char *g = read_file_all(gp, NULL);
    check(has(s, "te encanta: el mar") && has(s, "te desagrada: el tráfico") && has(g, "dias"),
          "los gustos de antes se leen y siguen creciendo");
    free(g), free(r), free(s);

    /* ----------------------------------------------------- sin bóveda --- */
    check(!boveda_dir(), "sin bóveda elegida no hay notas");
    set_vault(NULL);
    free(gusto("los gatos", "gusta", false, NULL));
    check(!file_has(notes, L"Gustos de Sokari.md", NULL), "sin bóveda, nada se escribe en Obsidian");

    /* ----------------------------------------------------- con bóveda --- */
    set_vault(vault);
    boveda_sync_all();
    char *t = note_text(BOVEDA_GUSTOS, NULL);
    check(has(t, "## Le encanta\n- el mar") && has(t, "## Le gusta\n- los gatos") && has(t, "## Le desagrada\n- el tráfico"),
          "al elegir la bóveda se escriben sus gustos como nota");
    free(t);
    free(gusto("el rock de los 80", "encanta", true, NULL));
    t = note_text(BOVEDA_GUSTOS, NULL);
    check(has(t, "- el rock de los 80 #para-siempre"), "lo nuevo llega a la nota (para siempre, con su etiqueta)");
    free(t);
    /* La editas en Obsidian: quitas uno, agregas otro y cambias uno de nivel. */
    edit_note(BOVEDA_GUSTOS, NULL, "- los gatos\n", "- los gatos\n- las tortas ahogadas\n");
    edit_note(BOVEDA_GUSTOS, NULL, "## Le desagrada\n- el tráfico\n", "## Le desagrada\n- los lunes #para-siempre\n");
    s = gustos_resumen();
    g = read_file_all(gp, NULL);
    check(has(s, "tortas ahogadas") && !has(s, "tráfico") && has(s, "te desagrada: los lunes") &&
              has(g, "tortas ahogadas") && !has(g, "tráfico"),
          "lo que cambias en la nota manda (y queda en gustos.json)");
    check(has(s, "Tus gustos para siempre") && has(s, "los lunes"), "#para-siempre en la nota lo vuelve para siempre");
    check(some_file_has(backups, "gustos-", "tráfico"), "antes de tomar tus cambios, respalda gustos.json");
    free(g), free(s);
    t = note_text(BOVEDA_GUSTOS, NULL);
    check(has(t, "# Gustos de Sokari") && has(t, "- las tortas ahogadas"), "y la nota queda en orden");
    free(t);

    /* Una nota que no se entiende: no se toma; se respalda antes de reescribirla. */
    write_note(BOVEDA_GUSTOS, NULL, "hola, esto lo escribí yo\n");
    s = gustos_resumen();
    check(has(s, "el mar") && has(s, "los lunes"), "una nota que no se entiende no le borra sus gustos");
    free(s);
    free(gusto("el mariachi", "gusta", false, NULL));
    t = note_text(BOVEDA_GUSTOS, NULL);
    check(has(t, "- el mariachi") && some_file_has(backups, "Gustos de Sokari-", "lo escribí yo"),
          "…y antes de reescribirla, la respalda tal cual");
    free(t);

    /* Tus datos, con un perfil con acentos. */
    wchar_t *pf = path_join(mem, L"perfiles.json");
    const char *profiles = "{\"josé\":{\"nombre\":\"José Ñúñez\",\"apodos\":[\"josé\"],\"password\":null}}";
    write_file_atomic(pf, profiles, strlen(profiles));
    set_current_speaker("josé");
    free(tool2(tool_guardar_dato, "clave", "Color favorito", "valor", "azul"));
    t = note_text(BOVEDA_DATOS, "José Ñúñez");
    check(has(t, "# Lo que Sokari sabe de José Ñúñez") && has(t, "- color favorito: azul") &&
              file_has(notes, L"Datos de José Ñúñez.md", NULL),
          "tus datos van a «Datos de José Ñúñez.md» (acentos y ñ)");
    free(t);
    edit_note(BOVEDA_DATOS, "José Ñúñez", "- color favorito: azul\n", "- color favorito: verde\n- mascota: Firulais\n");
    r = tool2(tool_recordar, "query", "color favorito", NULL, NULL);
    char *r2 = tool2(tool_recordar, "query", "mascota", NULL, NULL);
    check(has(r, "verde") && has(r2, "Firulais"), "lo que corriges o agregas en la nota es lo que recuerda");
    free(r2), free(r);

    /* Tus pendientes: marcas uno en Obsidian y lo tacha. */
    const SkillInfo *which = NULL;
    bool end = false;
    free(skills_try("anota comprar leche", &which, &end));
    t = note_text(BOVEDA_PENDIENTES, "José Ñúñez");
    check(has(t, "- [ ] comprar leche"), "tus pendientes van como casillas");
    free(t);
    edit_note(BOVEDA_PENDIENTES, "José Ñúñez", "- [ ] comprar leche\n", "- [x] comprar leche\n- [ ] pagar la luz\n");
    r = skills_try("qué tengo pendiente", &which, &end);
    check(has(r, "pagar la luz") && !has(r, "leche"), "marcarlo hecho en la nota lo tacha; lo que agregas, se anota");
    free(r);
    set_current_speaker(DEFAULT_PROFILE);

    /* Su música: favoritos y playlists de ida y vuelta; el historial solo se ve. */
    wchar_t *mp = path_join(mem, L"musica.json");
    const char *music = "{\"favoritos\":[{\"nombre\":\"Rock FM\",\"url\":\"http://rock.example/stream\"}],"
                        "\"historial\":[{\"nombre\":\"Jazz [HD]\",\"url\":\"https://jazz.example/a\"}],"
                        "\"playlists\":{\"estudiar\":[{\"nombre\":\"Lofi\",\"url\":\"https://lofi.example/s\"}]}}";
    write_file_atomic(mp, music, strlen(music));
    radio_sync();
    t = note_text(BOVEDA_MUSICA, NULL);
    check(has(t, "## Favoritos\n- [Rock FM](http://rock.example/stream)") && has(t, "## Playlist: estudiar\n- [Lofi]") &&
              has(t, "- [Jazz (HD)](https://jazz.example/a)"),
          "su música va como enlaces (favoritos, playlists e historial)");
    free(t);
    edit_note(BOVEDA_MUSICA, NULL, "- [Rock FM](http://rock.example/stream)\n",
              "- [Radio Z](https://z.example/stream)\n- [mala](javascript:alert(1))\n\n## Playlist: correr\n"
              "- https://run.example/live\n");
    edit_note(BOVEDA_MUSICA, NULL, "- [Jazz (HD)](https://jazz.example/a)", "");
    radio_sync();
    char *m = read_file_all(mp, NULL);
    check(has(m, "z.example") && !has(m, "rock.example") && has(m, "\"correr\"") && has(m, "run.example") &&
              !has(m, "javascript") && has(m, "jazz.example"),
          "quitas y agregas estaciones y playlists en la nota (solo http/https; el historial no se toca)");
    free(m);

    /* Otra bóveda que ya tiene una nota de sus gustos (otra PC): se juntan. */
    set_vault(vault2);
    const char *theirs = "# Gustos de Sokari\n\n## Le encanta\n- los perros\n- el mar #para-siempre\n\n## Le gusta\n\n## Le desagrada\n";
    write_note(BOVEDA_GUSTOS, NULL, theirs);
    s = gustos_resumen();
    t = note_text(BOVEDA_GUSTOS, NULL);
    check(has(s, "los perros") && has(s, "tortas ahogadas") && has(t, "- los perros") && has(t, "- el mariachi") &&
              file_has(notes2, L"Gustos de Sokari.md", NULL),
          "la primera vez con una nota que ya existía, se juntan (no se pierde nada de ninguna)");
    free(t), free(s);

    /* La bóveda ya no existe: sigue con sus archivos, sin tronar. */
    wchar_t *gone = path_join(root, L"no existe");
    set_vault(gone);
    check(!boveda_dir(), "si la bóveda ya no existe, no hay notas");
    r = gusto("los tacos", "encanta", false, NULL);
    s = gustos_resumen();
    check(has(r, "Anotado") && has(s, "los tacos"), "…y sus gustos siguen en su carpeta");
    free(s), free(r);

    /* Lo que escribe y lee, tal cual (sin archivos). */
    cJSON *d = cJSON_Parse("[{\"cosa\":\"a\",\"nivel\":\"gusta\"}]");
    cJSON *p = boveda_parse(BOVEDA_GUSTOS, "sin títulos\n- algo\n", d);
    check(!p, "una nota sin títulos de gustos no se toma como gustos");
    cJSON_Delete(p);
    char *md = boveda_render(BOVEDA_GUSTOS, NULL, d);
    p = boveda_parse(BOVEDA_GUSTOS, md, d);
    check(p && cJSON_Compare(p, d, true), "lo que escribe lo vuelve a leer igual");
    cJSON_Delete(p), free(md), cJSON_Delete(d);

    set_vault(NULL);
    free(gone), free(mp), free(pf), free(gp);
    printf("\n%d/%d pruebas pasaron\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}

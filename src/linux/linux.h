#ifndef SOKARI_LINUX_LINUX_H
#define SOKARI_LINUX_LINUX_H

/* Lo propio de la versión de Linux que usan otros archivos de src/linux/ y
   las pruebas. */

#include <stdbool.h>
#include <stddef.h>

/* util_linux.c */
void sha256_bytes(const void *data, size_t n, unsigned char out[32]);

/* app_linux.c: ¿se pidió cerrar Sokari (palabra de apagado, menú)? */
bool app_quit_requested(void);

/* tts_linux.c: del catálogo de voces de Piper (voices.json), la de México que
   se va a bajar y lo que debe medir y dar su MD5. */
bool tts_pick_catalog_voice(const char *catalog_json, char **key, char **onnx_path, char **onnx_md5, long long *onnx_size,
                            char **json_path, char **json_md5, long long *json_size);

/* audio_linux.c: el volumen de la salida predeterminada, en % (puede pasar
   de 100 si alguien la subió de más) y el silencio. */
bool system_volume_get(int *percent, bool *muted);
bool system_volume_set(int percent);
bool system_mute_set(bool mute);

/* ui_linux.c: la ventana. ui_run la abre (y se queda hasta Salir); lo demás
   lo usa app_linux.c para mandarle lo que pasa desde cualquier hilo. */
int ui_run(int argc, char **argv);
bool ui_active(void);
bool ui_own_window_active(void);
void ui_post_state(int state);
void ui_post_level(float level);
void ui_post_subtitle(bool from_user, const char *text);
void ui_post_status(const char *text);
void ui_post_notify(const char *title, const char *text);
/* Un aviso con un botón que corre una acción de la app ("app.actualizar");
   uno nuevo con el mismo id reemplaza al anterior. */
void ui_post_notify_button(const char *id, const char *title, const char *text, const char *button,
                           const char *action);
void ui_post_quit(void);
/* Sokari va a abrir o usar otra ventana (en pantalla completa se aparta). */
void ui_post_yield(void);
/* Para las pruebas (necesita pantalla): qué falla en la barra de la ventana, o NULL. */
char *ui_window_problems(void);

/* autostart_linux.c: cómo se llama a este Sokari ("sokari" o su ruta, heap)
   y el atajo Ctrl+Alt+J de GNOME (false si no se pudo o no es GNOME). */
char *linux_self_command(void);
bool linux_hotkey_ensure(void);

/* update_linux.c: con qué se instala aquí ("deb", "rpm" o NULL), si la
   versión remote ("v2.5.1") es más nueva que local, y si el paquete bajado es
   el publicado: su tipo, su tamaño (<= 0: no se sabe) y su huella SHA-256 en
   hexadecimal (NULL: no se instala). */
const char *linux_package_kind(void);
bool linux_version_newer(const char *remote, const char *local);
bool linux_package_verify(const char *path, double size, const char *sha256, const char *kind, char **error);
/* Cómo se llama el botón (en el menú y en Configuración) y el aviso de versión
   nueva, que lo nombra (heap). */
#define LINUX_UPDATE_LABEL "Buscar actualizaciones"
char *linux_update_notice(const char *tag);
/* ¿La última revisión instaló una versión nueva? (para ofrecer reiniciar) */
bool linux_update_installed(void);

/* MD5 en hexadecimal (heap): el que publica el catálogo de voces de Piper. */
char *tts_md5_hex(const void *data, size_t n);
/* "es_MX-ald-medium" -> "Ald (México, Piper)" (heap). */
char *tts_piper_voice_name(const char *key);

#endif

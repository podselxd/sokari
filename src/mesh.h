#ifndef SOKARI_MESH_H
#define SOKARI_MESH_H

#include <stdbool.h>

/* Procesa un comando de texto que llegó por la malla. Lo implementa el
   agente; origen es el nombre del dispositivo que la mandó (o su IP si no
   está registrado). Devuelve la respuesta (heap) o NULL si Sokari está
   ocupado. */
typedef char *(*MeshHandler)(const char *comando, const char *origen);

/* Arranca un hilo que levanta el servidor en cuanto Tailscale está conectado
   (aunque se conecte después de abrir Sokari) y lo mueve si cambia de IP. */
bool mesh_start(MeshHandler handler);
void mesh_stop(void);
char *mesh_tailscale_ip(void);
/* IP donde esta PC está recibiendo órdenes ahora, o NULL. */
char *mesh_listening_ip(void);
bool tailscale_installed(void);
bool mesh_host_allowed(const char *host);
/* ¿Es una IPv4 de Tailscale (100.64.0.0/10)? */
bool mesh_is_tailscale_v4(const unsigned char b[4]);

/* Tus dispositivos registrados (dispositivos.json): nombre -> dirección de
   Tailscale. */
typedef struct {
    char *name;
    char *host;
} MeshDevice;
int mesh_devices(MeshDevice **out);
/* dispositivos.json tal cual ({"nombre": "host"}), para quien necesite más
   que la lista. */
struct cJSON *mesh_devices_load(void);
void mesh_devices_free(MeshDevice *list, int n);
bool mesh_device_set(const char *name, const char *host); /* false si no es de Tailscale */
bool mesh_device_remove(const char *name);
char *mesh_device_name_for_ip(const char *ip);
/* ¿La frase habla de otra de tus PCs? NULL si no; "" si sí pero no se sabe
   cuál (ninguna registrada, o varias y no dijo el nombre); si no, su nombre
   registrado (heap). Entiende "Chloe" por "cloe" y "mi laptop" o "la otra
   compu" cuando solo tienes una. */
char *mesh_device_mentioned(const char *text);
/* El nombre registrado del dispositivo que se dijo así ("Chloe", "mi
   laptop"), o NULL si no hay uno claro. */
char *mesh_resolve_device(const char *spoken);

/* Prueba un dispositivo con una orden vacía: dice si contesta y si el secreto
   coincide, sin que allá se haga nada. */
typedef enum {
    MESH_OK,
    MESH_BUSY,
    MESH_BAD_SECRET,
    MESH_NO_SOKARI, /* la PC está pero nadie escucha en el puerto: Sokari cerrado o sin su servidor */
    MESH_NO_ANSWER, /* apagada, sin Tailscale o bloqueada por el firewall */
    MESH_NOT_FOUND,
    MESH_BAD_HOST,
    MESH_ERROR,
} MeshProbe;
MeshProbe mesh_probe(const char *host);
const char *mesh_probe_text(MeshProbe p);
/* Prueba y, si no contesta, averigua por qué (¿Tailscale llega?, ¿esa PC te
   ha mandado algo?, ¿tiene «Allow incoming connections» apagado?) y dice qué
   hacer y en qué PC. Heap. */
char *mesh_probe_report(const char *name, const char *host);
/* ¿El firewall de Windows deja entrar las órdenes? 1 sí, 0 no, -1 sin revisar.
   mesh_firewall_ok dice lo último que se revisó; mesh_firewall_check lo
   revisa ahora (unos cientos de milisegundos). */
int mesh_firewall_ok(void);
int mesh_firewall_check(void);

/* Las otras PCs de tu red de Tailscale donde puede estar Sokari (nombre e
   IP), leídas de "tailscale status --json": las de Windows, y las de Linux
   solo si Sokari ya les contesta (un servidor con Linux no cuenta). Si no
   encuentra ninguna, *why (heap, si no es NULL) dice por qué, en palabras
   para el usuario. */
int tailscale_windows_peers(MeshDevice **out, char **why);
/* La parte que lee ese JSON (aparte para poder probarla sin Tailscale): las
   PCs con Windows. */
int tailscale_parse_peers(const char *json, MeshDevice **out);

/* Lo que "tailscale status --json" dice de esta PC. */
typedef struct {
    char *state;   /* "Running" si está conectado */
    char *account; /* la cuenta con la que entraste (LoginName), o NULL */
    int peers;     /* otros dispositivos que ve tu red */
} TailscaleStatus;
bool tailscale_parse_status(const char *out, TailscaleStatus *st);
void tailscale_status_free(TailscaleStatus *st);
/* De "tailscale whois --json <ip>": la cuenta dueña de esa IP (heap) o NULL. */
char *tailscale_parse_whois_account(const char *out);
/* De "tailscale ping": 1 contestó, 0 no llegó, -1 no se sabe. */
int tailscale_parse_ping(const char *out);
/* De "tailscale debug prefs" o "whois --json": ¿«Allow incoming connections»
   está apagado (ShieldsUp)? */
bool tailscale_parse_shields_up(const char *out);

/* Revisa la malla paso a paso (Tailscale, tu cuenta, el servidor, el
   firewall, lo que ve tu red y cada PC registrada) y devuelve el reporte
   (heap), con ✓ o ✗ en cada renglón. Tarda unos segundos. */
char *mesh_diagnose(void);
/* «Detectar mis PCs»: registra las PCs de tu Tailscale que falten y dice qué
   hizo (heap). */
char *mesh_detect_devices(void);

/* Para las pruebas: escuchar en una IP cualquiera sin esperar a Tailscale, y
   mandar una orden a una IP ya revisada (*reply con la respuesta si hubo). */
bool mesh_listen_at(const char *ip, MeshHandler handler);
void mesh_listen_stop(void);
MeshProbe mesh_send_ip(const char *ip, const char *comando, int timeout_ms, char **reply);
/* Cuánto espera una orden antes de contestar «recibido» (de fábrica, 5 s). */
void mesh_set_ack_ms(int ms);
/* Permite el puerto de la malla en el firewall de Windows solo para tu red de
   Tailscale (pide permiso de administrador). */
bool mesh_allow_firewall(void);

#endif

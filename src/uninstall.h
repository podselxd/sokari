#ifndef SOKARI_UNINSTALL_H
#define SOKARI_UNINSTALL_H

/* Desinstalar Sokari («sokari --desinstalar»): pregunta, cierra la que está
   corriendo, quita el inicio automático, borra tus datos si lo pides y quita
   el programa. En Windows: uninstall_win.c (y aparece en Configuración de
   Windows → Aplicaciones instaladas); en Linux: linux/uninstall_linux.c (clic
   derecho en su ícono). Devuelve el código de salida. */
int uninstall_run(void);

/* Windows: anota a Sokari en «Aplicaciones instaladas» (con Desinstalar).
   En Linux no hace nada: ahí lo hace el paquete. */
void uninstall_register(void);

#endif

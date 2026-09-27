/* La ventana de Sokari en Linux: su barra de arriba (Hablar, el menú y la X)
   se ve. En la 2.7.0 nacía escondida y la ventana quedaba sin barra: sin
   menú y sin cómo moverla. Necesita pantalla; sin ella se omite (la prueba
   de GNOME, que sí tiene, la corre también). */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

#include "linux/linux.h"

int main(void)
{
    if (!gtk_init_check(NULL, NULL)) {
        printf("Sin pantalla: se omite (la prueba de GNOME la corre).\n");
        return 0;
    }
    char *problem = ui_window_problems();
    printf("%s la barra de la ventana se ve: Hablar, el menú y la X\n", problem ? "FALLA" : "ok   ");
    if (problem) printf("      (%s)\n", problem);
    free(problem);
    return problem ? 1 : 0;
}

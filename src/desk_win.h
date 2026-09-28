#ifndef SOKARI_DESK_WIN_H
#define SOKARI_DESK_WIN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "desk.h"

/* Llena v con lo que hay ahora en el escritorio (sin la ventana own). */
void desk_view_windows(DeskView *v, HWND own);

#endif

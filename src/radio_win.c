/* La radio en Windows: Media Foundation (MFPlay) toca la dirección de la
   estación en sus propios hilos, sin ventana. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfplay.h>

#include "radio.h"
#include "util.h"

static IMFPMediaPlayer *g_player;
static SRWLOCK g_lock = SRWLOCK_INIT;

void radio_stop(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_player) {
        IMFPMediaPlayer_Shutdown(g_player);
        IMFPMediaPlayer_Release(g_player);
        g_player = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool radio_play(const char *url, char **why)
{
    radio_stop();
    CoInitializeEx(NULL, COINIT_MULTITHREADED); /* si ya estaba, no pasa nada */
    wchar_t *w = utf8_to_wide(url);
    IMFPMediaPlayer *p = NULL;
    HRESULT hr = MFPCreateMediaPlayer(w, TRUE, 0, NULL, NULL, &p);
    free(w);
    if (FAILED(hr) || !p) {
        if (why) *why = str_printf("Windows no pudo abrir la estación (0x%08lx)", (unsigned long)hr);
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_player = p;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

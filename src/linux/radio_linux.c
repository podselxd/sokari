/* La radio en Linux: un reproductor que ya tengas (mpv, ffplay, GStreamer o
   VLC), sin ventana, en segundo plano. */
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "linux/proc.h"
#include "radio.h"
#include "util.h"

static pid_t g_pid;

void radio_stop(void)
{
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        proc_finish(g_pid, 2000);
        g_pid = 0;
    }
}

bool radio_play(const char *url, char **why)
{
    radio_stop();
    static const char *const PLAYERS[][5] = {
        {"mpv", "--no-video", "--really-quiet", NULL, NULL},
        {"ffplay", "-nodisp", "-loglevel", "quiet", NULL},
        {"gst-play-1.0", "--quiet", NULL, NULL, NULL},
        {"cvlc", "--quiet", "--no-video", NULL, NULL},
    };
    for (size_t i = 0; i < sizeof PLAYERS / sizeof *PLAYERS; i++) {
        char *bin = proc_which(PLAYERS[i][0]);
        if (!bin) continue;
        free(bin);
        const char *argv[7] = {0};
        int n = 0;
        for (int k = 0; k < 5 && PLAYERS[i][k]; k++) argv[n++] = PLAYERS[i][k];
        argv[n++] = url;
        g_pid = proc_spawn(argv, NULL, NULL, NULL, NULL);
        if (g_pid > 0) return true;
    }
    if (why) *why = xstrdup("no tengo con qué tocarla: instala mpv (sudo apt install mpv)");
    return false;
}

/* Voz de Sokari con las voces nativas de Windows (SAPI 5), incluidas las
   "OneCore" (Raúl, Sabina) que SAPI no lista por defecto. Se sintetiza a un
   buffer en memoria en vez de mandarlo directo a los parlantes: así el volumen
   es propio de Sokari, se puede cortar al instante si lo interrumpes, y la
   esfera puede moverse con la voz. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <sapi.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "tts.h"
#include "util.h"

DEFINE_GUID(JV_SPDFID_WaveFormatEx, 0xC31ADBAE, 0x527F, 0x4ff5, 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C);

static const wchar_t *VOICE_CATEGORIES[] = {
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices",
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices",
};

/* Preferencia por defecto: voz masculina de México, la más parecida a la
   voz que usaba la versión en Python (es-MX-JorgeNeural). */
static const char *PREFERRED[] = {"Sabina", "Raul", "Jorge", "Spanish", "Español"};

static ISpVoice *g_voice;
static bool g_com;

int tts_list_voices(TtsVoice **out)
{
    int cap = 16, n = 0;
    TtsVoice *list = xcalloc((size_t)cap, sizeof *list);
    for (size_t c = 0; c < sizeof VOICE_CATEGORIES / sizeof *VOICE_CATEGORIES; c++) {
        ISpObjectTokenCategory *cat = NULL;
        IEnumSpObjectTokens *en = NULL;
        if (FAILED(CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL, &IID_ISpObjectTokenCategory,
                                    (void **)&cat)))
            continue;
        if (SUCCEEDED(ISpObjectTokenCategory_SetId(cat, VOICE_CATEGORIES[c], FALSE)) &&
            SUCCEEDED(ISpObjectTokenCategory_EnumTokens(cat, NULL, NULL, &en))) {
            ISpObjectToken *tok = NULL;
            ULONG fetched = 0;
            while (IEnumSpObjectTokens_Next(en, 1, &tok, &fetched) == S_OK && fetched) {
                LPWSTR id = NULL, desc = NULL;
                if (SUCCEEDED(ISpObjectToken_GetId(tok, &id)) &&
                    SUCCEEDED(ISpObjectToken_GetStringValue(tok, NULL, &desc))) {
                    char *name = wide_to_utf8(desc);
                    bool dup = false;
                    for (int i = 0; i < n; i++)
                        if (!strcmp(list[i].name, name)) dup = true;
                    if (!dup) {
                        if (n == cap) {
                            cap *= 2;
                            list = xrealloc(list, sizeof *list * (size_t)cap);
                        }
                        list[n].id = wide_to_utf8(id);
                        list[n].name = name;
                        n++;
                    } else {
                        free(name);
                    }
                }
                CoTaskMemFree(id);
                CoTaskMemFree(desc);
                ISpObjectToken_Release(tok);
            }
            IEnumSpObjectTokens_Release(en);
        }
        ISpObjectTokenCategory_Release(cat);
    }
    *out = list;
    return n;
}

void tts_free_voices(TtsVoice *v, int n)
{
    for (int i = 0; i < n; i++) {
        free(v[i].id);
        free(v[i].name);
    }
    free(v);
}

bool tts_set_voice(const char *id)
{
    if (!g_voice || !id || !*id) return false;
    ISpObjectToken *tok = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpObjectToken, NULL, CLSCTX_ALL, &IID_ISpObjectToken, (void **)&tok)))
        return false;
    wchar_t *wid = utf8_to_wide(id);
    bool ok = SUCCEEDED(ISpObjectToken_SetId(tok, NULL, wid, FALSE)) && SUCCEEDED(ISpVoice_SetVoice(g_voice, tok));
    free(wid);
    ISpObjectToken_Release(tok);
    return ok;
}

static void pick_default_voice(void)
{
    TtsVoice *voices;
    int n = tts_list_voices(&voices);
    for (size_t p = 0; p < sizeof PREFERRED / sizeof *PREFERRED; p++) {
        for (int i = 0; i < n; i++) {
            if (str_contains_ci(voices[i].name, PREFERRED[p]) && tts_set_voice(voices[i].id)) {
                log_msg("Voz: %s", voices[i].name);
                tts_free_voices(voices, n);
                return;
            }
        }
    }
    tts_free_voices(voices, n);
}

bool tts_init(const char *preferred_voice)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_com = SUCCEEDED(hr);
    if (FAILED(CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void **)&g_voice))) {
        log_msg("SAPI no está disponible: Sokari no va a poder hablar.");
        g_voice = NULL;
        return false;
    }
    if (!preferred_voice || !*preferred_voice || !tts_set_voice(preferred_voice)) pick_default_voice();
    return true;
}

void tts_shutdown(void)
{
    if (g_voice) ISpVoice_Release(g_voice);
    g_voice = NULL;
    if (g_com) CoUninitialize();
    g_com = false;
}

int16_t *tts_synthesize(const char *text, size_t *samples)
{
    *samples = 0;
    if (!g_voice || str_is_blank(text)) return NULL;
    IStream *mem = NULL;
    ISpStream *sp = NULL;
    int16_t *pcm = NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &mem))) return NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL, &IID_ISpStream, (void **)&sp))) goto done;
    WAVEFORMATEX fmt = {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 1, .nSamplesPerSec = TTS_RATE,
                        .wBitsPerSample = 16, .nBlockAlign = 2, .nAvgBytesPerSec = TTS_RATE * 2};
    if (FAILED(ISpStream_SetBaseStream(sp, mem, &JV_SPDFID_WaveFormatEx, &fmt))) goto done;
    if (FAILED(ISpVoice_SetOutput(g_voice, (IUnknown *)sp, TRUE))) goto done;

    wchar_t *w = utf8_to_wide(text);
    HRESULT hr = ISpVoice_Speak(g_voice, w, SPF_DEFAULT | SPF_IS_NOT_XML | SPF_PURGEBEFORESPEAK, NULL);
    free(w);
    ISpVoice_SetOutput(g_voice, NULL, TRUE);
    if (FAILED(hr)) {
        log_msg("SAPI no pudo sintetizar (0x%08lx).", (unsigned long)hr);
        goto done;
    }
    HGLOBAL hg = NULL;
    STATSTG st;
    if (SUCCEEDED(GetHGlobalFromStream(mem, &hg)) && SUCCEEDED(IStream_Stat(mem, &st, STATFLAG_NONAME))) {
        size_t bytes = (size_t)st.cbSize.QuadPart;
        void *src = GlobalLock(hg);
        if (src && bytes >= 2) {
            pcm = xmalloc(bytes);
            memcpy(pcm, src, bytes);
            *samples = bytes / 2;
        }
        if (src) GlobalUnlock(hg);
    }
done:
    if (sp) ISpStream_Release(sp);
    if (mem) IStream_Release(mem);
    return pcm;
}

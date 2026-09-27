/* Desinstalar Sokari en Windows (ver uninstall.h). Sokari no tiene
   instalador: es su .exe, sus datos y el inicio automático. Se anota en
   Configuración de Windows → Aplicaciones instaladas para que ahí aparezca
   «Desinstalar», que corre «Sokari.exe --desinstalar». */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autostart.h"
#include "config.h"
#include "log.h"
#include "ui.h"
#include "uninstall.h"
#include "util.h"

#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Sokari"

static void set_str(HKEY k, const wchar_t *name, const wchar_t *value)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void set_dword(HKEY k, const wchar_t *name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v);
}

void uninstall_register(void)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    wchar_t cmd[MAX_PATH + 32], dir[MAX_PATH];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --desinstalar", exe);
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    set_str(k, L"DisplayName", L"Sokari");
    set_str(k, L"DisplayVersion", SOKARI_VERSION_W);
    set_str(k, L"Publisher", L"podselxd");
    set_str(k, L"DisplayIcon", exe);
    set_str(k, L"InstallLocation", dir);
    set_str(k, L"UninstallString", cmd);
    set_dword(k, L"NoModify", 1);
    set_dword(k, L"NoRepair", 1);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &a)) set_dword(k, L"EstimatedSize", a.nFileSizeLow / 1024);
    RegCloseKey(k);
}

/* Solo carpetas que se llaman Sokari (las de sus datos), nunca otra. */
static void remove_tree(const wchar_t *dir)
{
    size_t n = dir ? wcslen(dir) : 0;
    if (n < 8 || _wcsicmp(dir + n - 7, L"\\Sokari")) return;
    wchar_t *from = xcalloc(n + 2, sizeof(wchar_t)); /* doble NUL al final */
    wcscpy(from, dir);
    SHFILEOPSTRUCTW op = {0};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
    free(from);
}

int uninstall_run(void)
{
    if (MessageBoxW(NULL,
                    L"¿Desinstalar Sokari?\n\nSe cierra, deja de abrirse con Windows y se borra el programa.",
                    L"Desinstalar Sokari", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return 0;
    bool wipe = MessageBoxW(NULL,
                            L"¿Borrar también tus datos (configuración, memoria, notas y skills)?\n\nSi dices que "
                            L"no, se quedan por si lo vuelves a instalar.",
                            L"Desinstalar Sokari", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    /* La que está abierta se cierra sola. */
    HWND other = FindWindowW(SOKARI_MSG_CLASS, NULL);
    if (other) PostMessageW(other, WM_APP_QUIT, 0, 0);
    for (int i = 0; i < 80 && FindWindowW(SOKARI_MSG_CLASS, NULL); i++) Sleep(100);
    autostart_set(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    if (wipe) {
        remove_tree(g_paths.local_dir);
        remove_tree(g_paths.memory_dir);
    }
    /* El .exe se borra cuando este ya se cerró. */
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 96];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        swprintf(cmd, MAX_PATH + 96, L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\"", exe);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    log_msg("Desinstalado%s.", wipe ? " (con tus datos)" : "");
    MessageBoxW(NULL,
                wipe ? L"Listo: Sokari se desinstaló y se borraron tus datos."
                     : L"Listo: Sokari se desinstaló. Tus datos se quedaron por si lo vuelves a instalar.",
                L"Sokari", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// Pipette for Windows — tiny launcher. © 2026 VITIRAN (Vaibhav Tiwary). MIT licence.
// Writes the bundled Pipette app to %LOCALAPPDATA%\Pipette\Pipette.html (so autosave survives)
// and opens it in the default browser. The app itself is the same file as the web version.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

extern const unsigned char pipette_html[];
extern const unsigned int pipette_html_len;

static void fail(const wchar_t *msg) { MessageBoxW(NULL, msg, L"Pipette", MB_OK | MB_ICONERROR); }

int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show) {
    wchar_t base[MAX_PATH], dir[MAX_PATH], file[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) { fail(L"Could not find your AppData folder."); return 1; }
    wcscpy(dir, base); wcscat(dir, L"\\Pipette");
    CreateDirectoryW(dir, NULL);
    wcscpy(file, dir); wcscat(file, L"\\Pipette.html");
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, pipette_html, pipette_html_len, &written, NULL) || written != pipette_html_len) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        fail(L"Could not write Pipette to your AppData folder."); return 1;
    }
    CloseHandle(f);
    if ((INT_PTR)ShellExecuteW(NULL, L"open", file, NULL, NULL, SW_SHOWNORMAL) <= 32) { fail(L"Could not open Pipette in your web browser."); return 1; }
    return 0;
}

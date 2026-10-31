// Pipette for Windows — a real application window around the Pipette web app.
// © 2026 VITIRAN (Vaibhav Tiwary). MIT licence.
//
// The statistics and graphs live in Pipette.html (the same file as the browser and Mac versions).
// This program gives it its own window, menus, Save/Open dialogs, clipboard and autosave, using
// Microsoft Edge WebView2 — the web engine built into Windows 10 and 11. Microsoft's small
// WebView2Loader.dll (redistributable, from the WebView2 SDK) is bundled inside the .exe.
// Build: see build.sh (zig cc, no Visual Studio needed).

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_VERSION L"1.3"
#define IDR_HTML 101
#define IDR_LOADER 102
#define WM_APP_REQUEST (WM_APP + 1)
#define WV2_DOWNLOAD L"https://go.microsoft.com/fwlink/p/?LinkId=2124703"
#define SITE L"https://vaibhavtiwaryvib.github.io/vaibhavtiwary/pipette/"

// ---------------------------------------------------------------------------------------------
// Minimal WebView2 COM access: call methods by their position in the interface (from WebView2.idl).
#define VT(o) (*(void ***)(o))
#define RELEASE(o) do { if (o) ((ULONG(STDMETHODCALLTYPE *)(void *))VT(o)[2])(o); } while (0)
typedef HRESULT(STDMETHODCALLTYPE *F_P)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *F_PP)(void *, void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *F_B)(void *, BOOL);
typedef HRESULT(STDMETHODCALLTYPE *F_0)(void *);
typedef HRESULT(STDMETHODCALLTYPE *F_RECT)(void *, RECT);
typedef HRESULT(STDMETHODCALLTYPE *F_D)(void *, double);
typedef HRESULT(STDMETHODCALLTYPE *F_PD)(void *, double *);
typedef HRESULT(STDMETHODCALLTYPE *F_I)(void *, int);
typedef HRESULT(STDMETHODCALLTYPE *F_HWNDP)(void *, HWND, void *);
typedef HRESULT(STDMETHODCALLTYPE *F_CreateEnv)(PCWSTR, PCWSTR, void *, void *);
typedef struct { INT64 value; } EventToken;

// ICoreWebView2Environment
#define ENV_CreateController 3
// ICoreWebView2Controller
#define CTL_put_Bounds 6
#define CTL_get_ZoomFactor 7
#define CTL_put_ZoomFactor 8
#define CTL_MoveFocus 12
#define CTL_NotifyParentWindowPositionChanged 23
#define CTL_get_CoreWebView2 25
// ICoreWebView2
#define WV_get_Settings 3
#define WV_Navigate 5
#define WV_add_NavigationStarting 7
#define WV_add_NavigationCompleted 15
#define WV_add_ProcessFailed 25
#define WV_AddScriptOnCreated 27
#define WV_ExecuteScript 29
#define WV_Reload 31
#define WV_PostWebMessageAsJson 32
#define WV_add_WebMessageReceived 34
#define WV_add_NewWindowRequested 44
// ICoreWebView2Settings
#define SET_put_IsStatusBarEnabled 10
#define SET_put_IsZoomControlEnabled 18
#define SET3_put_AreBrowserAcceleratorKeysEnabled 24 // ICoreWebView2Settings3
static const GUID IID_Settings3 = { 0xfdb5ab74, 0xaf33, 0x4854, { 0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba } };
// event args
#define MSG_TryGetString 5
#define NAV_get_Uri 3
#define NAV_put_Cancel 8
#define NEWWIN_get_Uri 3
#define NEWWIN_put_Handled 7

// Our callback objects: IUnknown + Invoke. They live for the whole program, so reference counting is a formality.
typedef struct { void **vtbl; LONG ref; } Handler;
static HRESULT STDMETHODCALLTYPE H_QueryInterface(Handler *h, REFIID riid, void **out) { *out = h; InterlockedIncrement(&h->ref); return S_OK; }
static ULONG STDMETHODCALLTYPE H_AddRef(Handler *h) { return InterlockedIncrement(&h->ref); }
static ULONG STDMETHODCALLTYPE H_Release(Handler *h) { return InterlockedDecrement(&h->ref); }
#define HANDLER(name, fn) static void *name##_vt[4] = { (void *)H_QueryInterface, (void *)H_AddRef, (void *)H_Release, (void *)fn }; static Handler name = { name##_vt, 1 };

// ---------------------------------------------------------------------------------------------
static HWND g_hwnd;
static void *g_ctl, *g_wv;
static BOOL g_ready, g_closing, g_allowClose;
static WCHAR g_dataDir[MAX_PATH], g_appDir[MAX_PATH], g_htmlPath[MAX_PATH], g_autosave[MAX_PATH];
static WCHAR g_project[MAX_PATH * 2];     // the .pipette file this project came from / was last saved to
static WCHAR g_pendingPath[MAX_PATH * 2]; // a .pipette file being opened (becomes g_project once the page accepts it)
static WCHAR g_startFile[MAX_PATH * 2];   // a file given on the command line (double-click in Explorer)

enum { M_NEWTABLE = 100, M_NEWPROJECT, M_OPEN, M_IMPORT, M_SAVE, M_SAVEAS, M_EXIT,
       M_UNDO, M_REDO, M_SELECTALL, M_DELETE, M_EXCLUDE, M_ZOOMIN, M_ZOOMOUT, M_ZOOM0, M_SITE, M_CITE, M_ABOUT };

// ---------- small helpers ----------
static char *to_utf8(const WCHAR *w, int *len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n ? n : 1); if (!n) { s[0] = 0; } else WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    if (len) *len = n ? n - 1 : 0;
    return s;
}
static WCHAR *from_utf8(const char *s, int len) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    WCHAR *w = malloc((n + 1) * sizeof(WCHAR)); MultiByteToWideChar(CP_UTF8, 0, s, len, w, n); w[n] = 0;
    return w;
}
static BYTE *read_file(const WCHAR *path, DWORD *size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0; BYTE *b = malloc(n + 1);
    if (!ReadFile(f, b, n, &got, NULL) || got != n) { CloseHandle(f); free(b); return NULL; }
    CloseHandle(f); b[n] = 0; *size = n; return b;
}
// write to a temporary file first, then swap it in, so a crash never leaves a half-written project
static BOOL write_file(const WCHAR *path, const void *data, DWORD size) {
    WCHAR tmp[MAX_PATH * 2 + 8]; wcscpy(tmp, path); wcscat(tmp, L".tmp");
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD put = 0; BOOL ok = WriteFile(f, data, size, &put, NULL) && put == size; CloseHandle(f);
    if (!ok) { DeleteFileW(tmp); return FALSE; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp); return FALSE; }
    return TRUE;
}
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static WCHAR *b64_encode(const BYTE *d, DWORD n) {
    DWORD m = 4 * ((n + 2) / 3); WCHAR *o = malloc((m + 1) * sizeof(WCHAR)), *p = o;
    for (DWORD i = 0; i < n; i += 3) {
        DWORD v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *p++ = B64[v >> 18 & 63]; *p++ = B64[v >> 12 & 63];
        *p++ = i + 1 < n ? B64[v >> 6 & 63] : L'='; *p++ = i + 2 < n ? B64[v & 63] : L'=';
    }
    *p = 0; return o;
}
static BYTE *b64_decode(const WCHAR *s, DWORD *outLen) {
    size_t n = wcslen(s); BYTE *o = malloc(n / 4 * 3 + 4); DWORD len = 0, v = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        WCHAR c = s[i]; int x;
        if (c >= L'A' && c <= L'Z') x = c - L'A'; else if (c >= L'a' && c <= L'z') x = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') x = c - L'0' + 52; else if (c == L'+') x = 62; else if (c == L'/') x = 63; else continue;
        v = v << 6 | x; bits += 6;
        if (bits >= 8) { bits -= 8; o[len++] = (BYTE)(v >> bits & 0xFF); }
    }
    *outLen = len; return o;
}
// "text" as a JSON / JavaScript string literal
static WCHAR *json_str(const WCHAR *s) {
    size_t n = wcslen(s); WCHAR *o = malloc((n * 6 + 3) * sizeof(WCHAR)), *p = o; *p++ = L'"';
    for (size_t i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (c == L'"' || c == L'\\') { *p++ = L'\\'; *p++ = c; }
        else if (c == L'\n') { *p++ = L'\\'; *p++ = L'n'; }
        else if (c == L'\r') { *p++ = L'\\'; *p++ = L'r'; }
        else if (c == L'\t') { *p++ = L'\\'; *p++ = L't'; }
        else if (c < 0x20 || c == 0x2028 || c == 0x2029) { swprintf(p, 7, L"\\u%04x", c); p += 6; }
        else *p++ = c;
    }
    *p++ = L'"'; *p = 0; return o;
}
// the page sends header fields percent-encoded (encodeURIComponent)
static WCHAR *pct_decode(const WCHAR *s, size_t n) {
    char *b = malloc(n + 1); size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == L'%' && i + 2 < n) { WCHAR h[3] = { s[i + 1], s[i + 2], 0 }; b[k++] = (char)wcstol(h, NULL, 16); i += 2; }
        else b[k++] = (char)s[i];
    }
    WCHAR *w = from_utf8(b, (int)k); free(b); return w;
}
static const WCHAR *base_name(const WCHAR *path) { const WCHAR *a = wcsrchr(path, L'\\'), *b = wcsrchr(path, L'/'); if (b > a) a = b; return a ? a + 1 : path; }
static BOOL has_ext(const WCHAR *path, const WCHAR *ext) { const WCHAR *d = wcsrchr(path, L'.'); return d && _wcsicmp(d + 1, ext) == 0; }

static void run_script(const WCHAR *js);

// ---------- talking to the page ----------
static void reply_json(int id, const WCHAR *json) {
    if (!g_wv || !id) return;
    size_t n = wcslen(json) + 64; WCHAR *m = malloc(n * sizeof(WCHAR));
    swprintf(m, n, L"{\"id\":%d,\"r\":%s}", id, json);
    ((F_P)VT(g_wv)[WV_PostWebMessageAsJson])(g_wv, m); free(m);
}
static void reply_str(int id, const WCHAR *s) { WCHAR *j = json_str(s); reply_json(id, j); free(j); }

// Requests that show a dialog are handled from the window's message loop, not inside the WebView2 callback.
typedef struct { int id; WCHAR cmd[32], ext[16], mode[16]; WCHAR *name, *data; BOOL as; } Request;

static BOOL ask_save_path(const WCHAR *suggested, const WCHAR *ext, WCHAR *out) {
    WCHAR filter[256]; size_t k = 0;
    const WCHAR *label = _wcsicmp(ext, L"pipette") == 0 ? L"Pipette project" : _wcsicmp(ext, L"pdf") == 0 ? L"PDF document" :
                         _wcsicmp(ext, L"svg") == 0 ? L"SVG image" : L"Image";
    k += swprintf(filter + k, 120, L"%ls (*.%ls)", label, ext) + 1;
    k += swprintf(filter + k, 40, L"*.%ls", ext) + 1;
    filter[k] = 0;
    wcsncpy(out, suggested, MAX_PATH * 2 - 1); out[MAX_PATH * 2 - 1] = 0;
    for (WCHAR *p = out; *p; p++) if (wcschr(L"\\/:*?\"<>|", *p)) *p = L'_';
    WCHAR dir[MAX_PATH * 2] = L"";
    if (g_project[0]) { wcscpy(dir, g_project); WCHAR *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = filter; ofn.lpstrFile = out; ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = ext; ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn);
}

static void open_path(const WCHAR *path, const WCHAR *mode) {
    if (!g_ready) { wcsncpy(g_startFile, path, MAX_PATH * 2 - 1); return; }
    DWORD n = 0; BYTE *bytes = read_file(path, &n);
    if (!bytes) { MessageBoxW(g_hwnd, L"This file could not be opened.", L"Pipette", MB_OK | MB_ICONWARNING); return; }
    if (wcscmp(mode, L"open") == 0 && (has_ext(path, L"pipette") || has_ext(path, L"json"))) wcscpy(g_pendingPath, path); else g_pendingPath[0] = 0;
    WCHAR *b64 = b64_encode(bytes, n), *name = json_str(base_name(path)); free(bytes);
    size_t len = wcslen(b64) + wcslen(name) + 96; WCHAR *js = malloc(len * sizeof(WCHAR));
    swprintf(js, len, L"window.__pipetteOpen(%ls,'%ls','%ls')", name, b64, mode);
    run_script(js); free(js); free(b64); free(name);
}
static void show_open(BOOL import) {
    WCHAR path[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner = g_hwnd; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrFilter = import ? L"GraphPad Prism files (*.pzfx, *.prism)\0*.pzfx;*.prism;*.pzf\0\0"
                             : L"Pipette projects and Prism files\0*.pipette;*.pzfx;*.prism;*.pzf;*.json\0Pipette project (*.pipette)\0*.pipette\0GraphPad Prism file (*.pzfx, *.prism)\0*.pzfx;*.prism;*.pzf\0\0";
    ofn.lpstrTitle = import ? L"Import Prism tables into this project" : L"Open";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) open_path(path, import ? L"import" : L"open");
}

static void handle_request(Request *r) {
    if (wcscmp(r->cmd, L"saveProject") == 0) {
        WCHAR path[MAX_PATH * 2];
        if (g_project[0] && !r->as) wcscpy(path, g_project);
        else {
            WCHAR sug[MAX_PATH * 2]; wcsncpy(sug, g_project[0] ? base_name(g_project) : r->name, MAX_PATH * 2 - 10); sug[MAX_PATH * 2 - 10] = 0;
            if (!has_ext(sug, L"pipette")) wcscat(sug, L".pipette");
            if (!ask_save_path(sug, L"pipette", path)) { reply_json(r->id, L"{\"status\":\"cancel\"}"); return; }
        }
        int len; char *u = to_utf8(r->data, &len);
        BOOL ok = write_file(path, u, len); free(u);
        if (!ok) { reply_json(r->id, L"{\"status\":\"error\"}"); return; }
        wcscpy(g_project, path); SHAddToRecentDocs(SHARD_PATHW, path);
        WCHAR *nm = json_str(base_name(path)); size_t n = wcslen(nm) + 40; WCHAR *j = malloc(n * sizeof(WCHAR));
        swprintf(j, n, L"{\"status\":\"ok\",\"name\":%ls}", nm); reply_json(r->id, j); free(j); free(nm);
    } else if (wcscmp(r->cmd, L"save") == 0) { // exported graph
        WCHAR path[MAX_PATH * 2];
        if (!ask_save_path(r->name, r->ext[0] ? r->ext : L"png", path)) { reply_str(r->id, L"cancel"); return; }
        DWORD n; BYTE *b = b64_decode(r->data, &n); BOOL ok = write_file(path, b, n); free(b);
        reply_str(r->id, ok ? L"ok" : L"error");
    } else if (wcscmp(r->cmd, L"open") == 0) {
        reply_str(r->id, L"ok"); show_open(wcscmp(r->mode, L"import") == 0);
    }
}

static void set_clipboard_text(const WCHAR *s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard(); size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) { memcpy(GlobalLock(h), s, n); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}
static WCHAR *get_clipboard_text(void) {
    WCHAR *out = NULL;
    if (OpenClipboard(g_hwnd)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) { const WCHAR *p = GlobalLock(h); if (p) out = _wcsdup(p); GlobalUnlock(h); }
        CloseClipboard();
    }
    return out ? out : _wcsdup(L"");
}

// message format from the page: id|cmd|name|ext|mode|as|edited  (percent-encoded) "\n" data
static void on_page_message(const WCHAR *msg) {
    const WCHAR *nl = wcschr(msg, L'\n'); if (!nl) return;
    WCHAR *f[7] = { 0 }; const WCHAR *p = msg;
    for (int i = 0; i < 7; i++) {
        const WCHAR *e = p; while (e < nl && *e != L'|') e++;
        f[i] = pct_decode(p, e - p); p = e < nl ? e + 1 : nl;
    }
    const WCHAR *data = nl + 1, *cmd = f[1];
    int id = _wtoi(f[0]);
    if (wcscmp(cmd, L"autosave") == 0) {
        int len; char *u = to_utf8(data, &len);
        if (len > 2) write_file(g_autosave, u, len);
        free(u); reply_str(id, L"ok");
    } else if (wcscmp(cmd, L"saveProject") == 0 || wcscmp(cmd, L"save") == 0 || wcscmp(cmd, L"open") == 0) {
        Request *r = calloc(1, sizeof *r);
        r->id = id; wcsncpy(r->cmd, cmd, 31); wcsncpy(r->ext, f[3], 15); wcsncpy(r->mode, f[4], 15);
        r->name = _wcsdup(f[2][0] ? f[2] : L"Untitled"); r->data = _wcsdup(data); r->as = f[5][0] == L'1';
        PostMessageW(g_hwnd, WM_APP_REQUEST, 0, (LPARAM)r);
    } else if (wcscmp(cmd, L"forgetFile") == 0) {
        g_project[0] = 0; reply_str(id, L"ok");
    } else if (wcscmp(cmd, L"openedFile") == 0) {
        if (g_pendingPath[0]) { wcscpy(g_project, g_pendingPath); SHAddToRecentDocs(SHARD_PATHW, g_project); g_pendingPath[0] = 0; }
        reply_str(id, L"ok");
    } else if (wcscmp(cmd, L"copyText") == 0) {
        set_clipboard_text(data); reply_str(id, L"ok");
    } else if (wcscmp(cmd, L"readText") == 0) {
        WCHAR *t = get_clipboard_text(); reply_str(id, t); free(t);
    } else if (wcscmp(cmd, L"title") == 0) {
        WCHAR t[600]; swprintf(t, 600, L"%ls%ls — Pipette", f[6][0] == L'1' ? L"• " : L"", f[2][0] ? f[2] : L"Untitled");
        SetWindowTextW(g_hwnd, t); reply_str(id, L"ok");
    } else if (wcscmp(cmd, L"quitResult") == 0) {
        reply_str(id, L"ok");
        if (wcscmp(f[2], L"cancel") != 0) { g_allowClose = TRUE; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
        else g_closing = FALSE;
    } else {
        reply_json(id, L"null");
    }
    for (int i = 0; i < 7; i++) free(f[i]);
}

// ---------- WebView2 callbacks ----------
static HRESULT STDMETHODCALLTYPE OnScriptDone(Handler *h, HRESULT hr, LPCWSTR result) { return S_OK; }
HANDLER(hScriptDone, OnScriptDone)
static void run_script(const WCHAR *js) { if (g_wv) ((F_PP)VT(g_wv)[WV_ExecuteScript])(g_wv, (void *)js, &hScriptDone); }

static HRESULT STDMETHODCALLTYPE OnWebMessage(Handler *h, void *sender, void *args) {
    LPWSTR s = NULL;
    if (SUCCEEDED(((F_P)VT(args)[MSG_TryGetString])(args, &s)) && s) { on_page_message(s); CoTaskMemFree(s); }
    return S_OK;
}
HANDLER(hWebMessage, OnWebMessage)

static BOOL is_internal(const WCHAR *uri) { return !_wcsnicmp(uri, L"file:", 5) || !_wcsnicmp(uri, L"about:", 6) || !_wcsnicmp(uri, L"data:", 5) || !_wcsnicmp(uri, L"blob:", 5); }
static void open_external(const WCHAR *uri) {
    if (!_wcsnicmp(uri, L"https:", 6) || !_wcsnicmp(uri, L"http:", 5) || !_wcsnicmp(uri, L"mailto:", 7))
        ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
}
// Links to websites / e-mail open in your normal browser or mail program; the app itself stays put.
static HRESULT STDMETHODCALLTYPE OnNavStarting(Handler *h, void *sender, void *args) {
    LPWSTR uri = NULL;
    if (SUCCEEDED(((F_P)VT(args)[NAV_get_Uri])(args, &uri)) && uri) {
        if (!is_internal(uri)) { ((F_B)VT(args)[NAV_put_Cancel])(args, TRUE); open_external(uri); }
        else if (g_ready && !_wcsnicmp(uri, L"file:", 5) && !wcsstr(uri, L"Pipette.html")) ((F_B)VT(args)[NAV_put_Cancel])(args, TRUE); // a file dropped on the window
        CoTaskMemFree(uri);
    }
    return S_OK;
}
HANDLER(hNavStarting, OnNavStarting)
static HRESULT STDMETHODCALLTYPE OnNewWindow(Handler *h, void *sender, void *args) {
    LPWSTR uri = NULL;
    ((F_B)VT(args)[NEWWIN_put_Handled])(args, TRUE);
    if (SUCCEEDED(((F_P)VT(args)[NEWWIN_get_Uri])(args, &uri)) && uri) { open_external(uri); CoTaskMemFree(uri); }
    return S_OK;
}
HANDLER(hNewWindow, OnNewWindow)
static HRESULT STDMETHODCALLTYPE OnNavCompleted(Handler *h, void *sender, void *args) {
    g_ready = TRUE;
    if (g_startFile[0]) { WCHAR p[MAX_PATH * 2]; wcscpy(p, g_startFile); g_startFile[0] = 0; open_path(p, L"open"); }
    return S_OK;
}
HANDLER(hNavCompleted, OnNavCompleted)
static HRESULT STDMETHODCALLTYPE OnProcessFailed(Handler *h, void *sender, void *args) {
    g_ready = FALSE; if (g_wv) ((F_0)VT(g_wv)[WV_Reload])(g_wv); // autosave lets the welcome screen restore unsaved work
    return S_OK;
}
HANDLER(hProcessFailed, OnProcessFailed)

// Runs in the page before Pipette starts: makes the Mac bridge API (webkit.messageHandlers) work on Windows too.
static const WCHAR *SHIM =
L"(() => {"
L" const pend = new Map(); let n = 0;"
L" const enc = v => encodeURIComponent(v == null ? '' : String(v));"
L" window.chrome.webview.addEventListener('message', e => { const m = e.data; const f = m && pend.get(m.id); if (f) { pend.delete(m.id); f(m.r); } });"
L" async function post(m) {"
L"  if (m.cmd === 'copyImage') { try { const b = await (await fetch('data:image/png;base64,' + m.data)).blob(); await navigator.clipboard.write([new ClipboardItem({ 'image/png': b })]); return 'ok'; } catch (e) { return 'error'; } }"
L"  return new Promise(res => { const id = ++n; pend.set(id, res);"
L"   window.chrome.webview.postMessage([id, m.cmd, m.name, m.ext, m.mode, m.as ? 1 : 0, m.edited ? 1 : 0].map(enc).join('|') + '\\n' + (m.data == null ? '' : String(m.data))); });"
L" }"
L" window.webkit = { messageHandlers: { pipette: { postMessage: post } } };"
L" window.__pipetteNative = 'windows';"
L" window.__pipetteOpen = (name, b64, mode) => {"
L"  if (/\\.(pzfx|prism|pzf)$/i.test(name)) return Pipette.importBytes(name, b64, mode);"
L"  const bin = atob(b64), u = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);"
L"  Pipette.openText(name, new TextDecoder().decode(u)).then(r => { if (r === 'ok') post({ cmd: 'openedFile' }); });"
L" };"
L" window.__pipetteCmd = c => { const r = Pipette.command(c); if (r === 'native' && ['undo', 'redo', 'selectAll'].includes(c)) document.execCommand(c); };"
L" window.__pipetteQuit = () => Pipette.quitCheck().then(r => post({ cmd: 'quitResult', name: r }), () => post({ cmd: 'quitResult', name: 'ok' }));"
L"})();";

static void fallback_to_browser(const WCHAR *why) {
    WCHAR msg[1024];
    swprintf(msg, 1024, L"%ls\n\nPipette needs the Microsoft Edge WebView2 Runtime, which is built into Windows 11 and up-to-date Windows 10.\n\n"
             L"Yes = download it from Microsoft (free), then start Pipette again.\nNo = open Pipette in your web browser for now.", why);
    int r = MessageBoxW(g_hwnd, msg, L"Pipette", MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (r == IDYES) ShellExecuteW(NULL, L"open", WV2_DOWNLOAD, NULL, NULL, SW_SHOWNORMAL);
    else if (r == IDNO) ShellExecuteW(NULL, L"open", g_htmlPath, NULL, NULL, SW_SHOWNORMAL);
    g_allowClose = TRUE; PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

static void resize_view(void) {
    if (!g_ctl) return;
    RECT rc; GetClientRect(g_hwnd, &rc); ((F_RECT)VT(g_ctl)[CTL_put_Bounds])(g_ctl, rc);
}

static WCHAR *file_url(const WCHAR *path) {
    size_t n = wcslen(path); WCHAR *u = malloc((n * 3 + 16) * sizeof(WCHAR)), *p = u;
    wcscpy(p, L"file:///"); p += 8;
    for (size_t i = 0; i < n; i++) {
        WCHAR c = path[i];
        if (c == L'\\') *p++ = L'/';
        else if (c == L' ' || c == L'#' || c == L'%' || c == L'?') { swprintf(p, 4, L"%%%02X", c); p += 3; }
        else *p++ = c;
    }
    *p = 0; return u;
}

static HRESULT STDMETHODCALLTYPE OnControllerCreated(Handler *h, HRESULT hr, void *ctl) {
    if (FAILED(hr) || !ctl) { fallback_to_browser(L"The Pipette window could not be created."); return S_OK; }
    g_ctl = ctl; ((ULONG(STDMETHODCALLTYPE *)(void *))VT(ctl)[1])(ctl); // keep it
    ((F_P)VT(ctl)[CTL_get_CoreWebView2])(ctl, &g_wv);
    void *settings = NULL;
    if (SUCCEEDED(((F_P)VT(g_wv)[WV_get_Settings])(g_wv, &settings)) && settings) {
        ((F_B)VT(settings)[SET_put_IsStatusBarEnabled])(settings, FALSE);
        ((F_B)VT(settings)[SET_put_IsZoomControlEnabled])(settings, TRUE);
        // no browser shortcuts like F5 (reload would throw away unsaved work); editing keys (Ctrl+C/V/Z) still work
        void *s3 = NULL;
        if (SUCCEEDED(((HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, void **))VT(settings)[0])(settings, &IID_Settings3, &s3)) && s3) {
            ((F_B)VT(s3)[SET3_put_AreBrowserAcceleratorKeysEnabled])(s3, FALSE); RELEASE(s3);
        }
        RELEASE(settings);
    }
    EventToken tok;
    ((F_PP)VT(g_wv)[WV_add_WebMessageReceived])(g_wv, &hWebMessage, &tok);
    ((F_PP)VT(g_wv)[WV_add_NavigationStarting])(g_wv, &hNavStarting, &tok);
    ((F_PP)VT(g_wv)[WV_add_NavigationCompleted])(g_wv, &hNavCompleted, &tok);
    ((F_PP)VT(g_wv)[WV_add_NewWindowRequested])(g_wv, &hNewWindow, &tok);
    ((F_PP)VT(g_wv)[WV_add_ProcessFailed])(g_wv, &hProcessFailed, &tok);
    ((F_PP)VT(g_wv)[WV_AddScriptOnCreated])(g_wv, (void *)SHIM, &hScriptDone);
    // hand the last autosaved project to the page, so the welcome screen can offer to restore unsaved work
    DWORD n = 0; BYTE *saved = read_file(g_autosave, &n);
    if (saved && n > 2) {
        WCHAR *b64 = b64_encode(saved, n); size_t len = wcslen(b64) + 200; WCHAR *js = malloc(len * sizeof(WCHAR));
        swprintf(js, len, L"try { window.__pipetteAutosave = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob('%ls'), c => c.charCodeAt(0)))); } catch (e) { }", b64);
        ((F_PP)VT(g_wv)[WV_AddScriptOnCreated])(g_wv, js, &hScriptDone); free(js); free(b64);
    }
    free(saved);
    resize_view();
    WCHAR *url = file_url(g_htmlPath); ((F_P)VT(g_wv)[WV_Navigate])(g_wv, url); free(url);
    ((F_I)VT(g_ctl)[CTL_MoveFocus])(g_ctl, 0);
    return S_OK;
}
HANDLER(hControllerCreated, OnControllerCreated)
static HRESULT STDMETHODCALLTYPE OnEnvironmentCreated(Handler *h, HRESULT hr, void *env) {
    if (FAILED(hr) || !env) { fallback_to_browser(L"The Microsoft Edge WebView2 Runtime was not found."); return S_OK; }
    ((F_HWNDP)VT(env)[ENV_CreateController])(env, g_hwnd, &hControllerCreated);
    return S_OK;
}
HANDLER(hEnvironmentCreated, OnEnvironmentCreated)

// ---------- set-up ----------
static BOOL extract_resource(int id, const WCHAR *path, BOOL onlyIfMissing) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA); if (!r) return FALSE;
    DWORD size = SizeofResource(NULL, r); const void *data = LockResource(LoadResource(NULL, r));
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) && a.nFileSizeLow == size && (onlyIfMissing || id != IDR_HTML)) return TRUE;
    if (write_file(path, data, size)) return TRUE;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; // in use by another Pipette window: keep the copy that is there
}
static void set_reg(HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *value) {
    HKEY k; if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR))); RegCloseKey(k);
}
// .pipette files open in Pipette when double-clicked (for this Windows user only; no admin rights needed)
static void register_file_type(void) {
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 16], icon[MAX_PATH + 8];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(cmd, MAX_PATH + 16, L"\"%ls\" \"%%1\"", exe); swprintf(icon, MAX_PATH + 8, L"%ls,0", exe);
    WCHAR cur[MAX_PATH + 16] = L""; DWORD sz = sizeof cur;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\VITIRAN.Pipette.Project\\shell\\open\\command", NULL, RRF_RT_REG_SZ, NULL, cur, &sz) == ERROR_SUCCESS && wcscmp(cur, cmd) == 0) return;
    set_reg(HKEY_CURRENT_USER, L"Software\\Classes\\.pipette", NULL, L"VITIRAN.Pipette.Project");
    set_reg(HKEY_CURRENT_USER, L"Software\\Classes\\VITIRAN.Pipette.Project", NULL, L"Pipette Project");
    set_reg(HKEY_CURRENT_USER, L"Software\\Classes\\VITIRAN.Pipette.Project\\DefaultIcon", NULL, icon);
    set_reg(HKEY_CURRENT_USER, L"Software\\Classes\\VITIRAN.Pipette.Project\\shell\\open\\command", NULL, cmd);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static HMENU build_menu(void) {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, M_NEWTABLE, L"New &Data Table…");
    AppendMenuW(file, MF_STRING, M_NEWPROJECT, L"&New Project");
    AppendMenuW(file, MF_STRING, M_OPEN, L"&Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, M_IMPORT, L"&Import Prism Tables into This Project…\tCtrl+Shift+O");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, M_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, M_SAVEAS, L"Save &As…\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, M_EXIT, L"E&xit");
    AppendMenuW(edit, MF_STRING, M_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, M_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, M_SELECTALL, L"Select &All\tCtrl+A");
    AppendMenuW(edit, MF_STRING, M_DELETE, L"&Delete Selected Rows or Columns\tDel");
    AppendMenuW(edit, MF_STRING, M_EXCLUDE, L"E&xclude / Include Selected Values\tCtrl+E");
    AppendMenuW(view, MF_STRING, M_ZOOMIN, L"Zoom &In\tCtrl++");
    AppendMenuW(view, MF_STRING, M_ZOOMOUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(view, MF_STRING, M_ZOOM0, L"&Actual Size\tCtrl+0");
    AppendMenuW(help, MF_STRING, M_SITE, L"Pipette &Website");
    AppendMenuW(help, MF_STRING, M_CITE, L"How to &Cite Pipette…");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, M_ABOUT, L"&About Pipette");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}
static void page_cmd(const WCHAR *c) {
    if (!g_ready) return;
    WCHAR js[96]; swprintf(js, 96, L"window.__pipetteCmd('%ls')", c); run_script(js);
}
static void zoom(double step) {
    if (!g_ctl) return;
    double z = 1; ((F_PD)VT(g_ctl)[CTL_get_ZoomFactor])(g_ctl, &z);
    z = step == 0 ? 1 : z + step; if (z < 0.5) z = 0.5; if (z > 2.5) z = 2.5;
    ((F_D)VT(g_ctl)[CTL_put_ZoomFactor])(g_ctl, z);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: resize_view(); return 0;
    case WM_MOVE: if (g_ctl) ((F_0)VT(g_ctl)[CTL_NotifyParentWindowPositionChanged])(g_ctl); return 0;
    case WM_SETFOCUS: if (g_ctl) ((F_I)VT(g_ctl)[CTL_MoveFocus])(g_ctl, 0); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)l; mm->ptMinTrackSize.x = 900; mm->ptMinTrackSize.y = 560; return 0; }
    case WM_APP_REQUEST: { Request *r = (Request *)l; handle_request(r); free(r->name); free(r->data); free(r); return 0; }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case M_NEWTABLE: page_cmd(L"newTable"); break;
        case M_NEWPROJECT: page_cmd(L"newProject"); break;
        case M_OPEN: show_open(FALSE); break;
        case M_IMPORT: show_open(TRUE); break;
        case M_SAVE: page_cmd(L"save"); break;
        case M_SAVEAS: page_cmd(L"saveAs"); break;
        case M_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); break;
        case M_UNDO: page_cmd(L"undo"); break;
        case M_REDO: page_cmd(L"redo"); break;
        case M_SELECTALL: page_cmd(L"selectAll"); break;
        case M_DELETE: page_cmd(L"deleteRows"); break;
        case M_EXCLUDE: page_cmd(L"exclude"); break;
        case M_ZOOMIN: zoom(0.1); break;
        case M_ZOOMOUT: zoom(-0.1); break;
        case M_ZOOM0: zoom(0); break;
        case M_SITE: ShellExecuteW(NULL, L"open", SITE, NULL, NULL, SW_SHOWNORMAL); break;
        case M_CITE: page_cmd(L"cite"); break;
        case M_ABOUT: MessageBoxW(h, L"Pipette " APP_VERSION L"\nStatistics & graphs for lab data.\n\nFree and open source (MIT licence) — published by VITIRAN.\n" SITE, L"About Pipette", MB_OK | MB_ICONINFORMATION); break;
        }
        return 0;
    case WM_CLOSE:
        // Ask the page first: it shows "Save changes?" if there is unsaved work.
        if (g_allowClose || !g_ready || !g_wv) { DestroyWindow(h); return 0; }
        if (g_closing) {
            if (MessageBoxW(h, L"Pipette is waiting for an answer about saving. Close anyway?", L"Pipette", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(h);
            return 0;
        }
        g_closing = TRUE; run_script(L"window.__pipetteQuit()");
        return 0;
    case WM_DESTROY:
        if (g_ctl) { ((F_0)VT(g_ctl)[24])(g_ctl); } // ICoreWebView2Controller::Close
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show) {
    typedef BOOL(WINAPI * SetDpiCtx)(HANDLE);
    SetDpiCtx sdc = (SetDpiCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (sdc) sdc((HANDLE)-4); // per-monitor v2 (the manifest says the same; this covers older loaders)
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // %LOCALAPPDATA%\Pipette — the app files, the web engine's profile, and the autosave
    WCHAR local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) GetTempPathW(MAX_PATH, local);
    swprintf(g_dataDir, MAX_PATH, L"%ls\\Pipette", local); CreateDirectoryW(g_dataDir, NULL);
    swprintf(g_appDir, MAX_PATH, L"%ls\\app-" APP_VERSION, g_dataDir); CreateDirectoryW(g_appDir, NULL);
    swprintf(g_htmlPath, MAX_PATH, L"%ls\\Pipette.html", g_appDir);
    swprintf(g_autosave, MAX_PATH, L"%ls\\autosave.pipette", g_dataDir);
    WCHAR loader[MAX_PATH], profile[MAX_PATH];
    swprintf(loader, MAX_PATH, L"%ls\\WebView2Loader.dll", g_appDir);
    swprintf(profile, MAX_PATH, L"%ls\\WebView2", g_dataDir);
    if (!extract_resource(IDR_HTML, g_htmlPath, FALSE) || !extract_resource(IDR_LOADER, loader, TRUE)) {
        MessageBoxW(NULL, L"Pipette could not write its files to your AppData folder.", L"Pipette", MB_OK | MB_ICONERROR); return 1;
    }
    register_file_type();

    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) wcsncpy(g_startFile, argv[1], MAX_PATH * 2 - 1);
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"PipetteWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = CreateSolidBrush(RGB(245, 246, 248));
    RegisterClassExW(&wc);
    UINT dpi = 96; typedef UINT(WINAPI * GetDpiSys)(void);
    GetDpiSys gds = (GetDpiSys)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"); if (gds) dpi = gds();
    int W = MulDiv(1280, dpi, 96), H = MulDiv(820, dpi, 96);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Pipette", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, build_menu(), inst, NULL);
    ShowWindow(g_hwnd, show); UpdateWindow(g_hwnd);

    HMODULE lib = LoadLibraryW(loader);
    F_CreateEnv create = lib ? (F_CreateEnv)(void *)GetProcAddress(lib, "CreateCoreWebView2EnvironmentWithOptions") : NULL;
    if (!create || FAILED(create(NULL, profile, NULL, &hEnvironmentCreated))) fallback_to_browser(L"The Pipette window could not start its web engine.");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return 0;
}

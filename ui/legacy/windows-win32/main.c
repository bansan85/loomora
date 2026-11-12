/* Loomora probe UI: plain Win32 in C. An input field and a label showing the
   greeting from the C++ backend. No logic here beyond UTF-16 <-> UTF-8. */
#define UNICODE
#define _UNICODE
#include "../../../core/include/loomora.h"
#include <stdlib.h>
#include <windows.h>

#define ID_EDIT 101
#define MAX_CHARS 1000

static HWND g_edit, g_label;

static void update_greeting(void) {
    WCHAR wtext[MAX_CHARS + 1];
    char utf8[MAX_CHARS * 4 + 1];
    uint8_t out[MAX_CHARS * 4 + 16];
    WCHAR wout[MAX_CHARS + 16];
    int wlen = GetWindowTextW(g_edit, wtext, MAX_CHARS + 1);
    int n8 = wlen ? WideCharToMultiByte(CP_UTF8, 0, wtext, wlen, utf8, sizeof utf8, NULL, NULL) : 0;
    int32_t n = loomora_greet(utf8, (size_t)n8, out, sizeof out);
    if (n < 0) {
        SetWindowTextW(g_label, L"(erreur)");
        return;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, (const char *)out, n, wout, MAX_CHARS + 15);
    wout[n] = L'\0';
    SetWindowTextW(g_label, wout);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Coucou",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 12, 360,
                                 24, hwnd, (HMENU)ID_EDIT, NULL, NULL);
        g_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 48, 360, 60, hwnd,
                                  NULL, NULL, NULL);
        SendMessageW(g_edit, EM_LIMITTEXT, MAX_CHARS, 0);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(g_label, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        update_greeting();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EDIT && HIWORD(wp) == EN_CHANGE)
            update_greeting();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc = {0};
    HWND hwnd;
    MSG m;
    (void)prev;
    (void)cmd;
    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        MessageBoxW(NULL, L"Version ABI incompatible", L"Loomora", MB_ICONERROR);
        return 1;
    }
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LoomoraWin";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, L"LoomoraWin", L"Loomora", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 400, 170, NULL, NULL, inst, NULL);
    ShowWindow(hwnd, show);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

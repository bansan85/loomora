// Loomora Legacy UI for Windows: plain Win32 API in C++11, no MFC, no common controls.
//
// One EDIT control pre-filled with "Coucou" (limited to 1,000 characters) and one STATIC control
// showing the greeting returned by the C++ backend. This file only creates the window, runs the
// message loop and converts between UTF-16 (Windows) and UTF-8 (backend); it builds no greeting
// itself. Only Win32 functions that exist on Windows 2000 are used (Unicode "W" entry points).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <stdint.h>
#include <string>

#include "loomora.h"
#include "resource.h"

namespace {

const int kMaxChars = 1000;
const int kMargin = 12;
const int kEditHeight = 24;
const wchar_t kWindowClass[] = L"LoomoraWin";

HWND g_edit = NULL;
HWND g_greeting = NULL;

std::wstring LoadText(UINT id) {
    wchar_t buffer[256];
    const int n = LoadStringW(GetModuleHandleW(NULL), id, buffer, 256);
    return std::wstring(buffer, n > 0 ? n : 0);
}

std::string ToUtf8(const std::wstring &text) {
    if (text.empty())
        return std::string();
    const int size = static_cast<int>(text.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), size, NULL, 0, NULL, NULL);
    std::string utf8(n > 0 ? n : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), size, &utf8[0], n, NULL, NULL);
    return utf8;
}

std::wstring ToUtf16(const char *utf8, size_t length) {
    if (length == 0)
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(length), NULL, 0);
    std::wstring text(n > 0 ? n : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(length), &text[0], n);
    return text;
}

std::wstring EditText() {
    const int length = GetWindowTextLengthW(g_edit);
    std::wstring text(length > 0 ? length + 1 : 1, L'\0');
    const int copied = GetWindowTextW(g_edit, &text[0], static_cast<int>(text.size()));
    text.resize(copied > 0 ? copied : 0);
    return text;
}

// Asks the backend for the greeting of the EDIT content and shows it in the STATIC control.
void UpdateGreeting() {
    std::wstring shown;
    try {
        const std::string input = ToUtf8(EditText());
        std::string out(input.size() + 64, '\0'); // "Bonjour " + input
        const int32_t n = loomora_greet(input.c_str(), input.size(),
                                        reinterpret_cast<uint8_t *>(&out[0]), out.size());
        shown = n < 0 ? LoadText(IDS_GREETING_ERROR) : ToUtf16(out.data(), static_cast<size_t>(n));
    } catch (...) { // no C++ exception may cross a window procedure
        shown = LoadText(IDS_GREETING_ERROR);
    }
    SetWindowTextW(g_greeting, shown.c_str());
}

void Layout(int width, int height) {
    const int inner = width - 2 * kMargin;
    const int top = 2 * kMargin + kEditHeight;
    MoveWindow(g_edit, kMargin, kMargin, inner > 0 ? inner : 0, kEditHeight, TRUE);
    MoveWindow(g_greeting, kMargin, top, inner > 0 ? inner : 0,
               height > top + kMargin ? height - top - kMargin : 0, TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const std::wstring initial = LoadText(IDS_DEFAULT_INPUT);
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                 window, NULL, NULL, NULL);
        g_greeting = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0,
                                     0, window, NULL, NULL, NULL);
        if (g_edit == NULL || g_greeting == NULL)
            return -1; // creation of the window fails
        SendMessageW(g_edit, EM_LIMITTEXT, kMaxChars, 0);
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(g_greeting, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        UpdateGreeting();
        return 0;
    }
    case WM_SIZE:
        Layout(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_edit);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == g_edit && HIWORD(wparam) == EN_CHANGE)
            UpdateGreeting();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        MessageBoxW(NULL, LoadText(IDS_ABI_MISMATCH).c_str(), LoadText(IDS_TITLE).c_str(),
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSW windowClass = WNDCLASSW();
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassW(&windowClass) == 0)
        return 1;

    const HWND window =
        CreateWindowExW(0, kWindowClass, LoadText(IDS_TITLE).c_str(), WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 420, 200, NULL, NULL, instance, NULL);
    if (window == NULL)
        return 1;
    ShowWindow(window, show);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

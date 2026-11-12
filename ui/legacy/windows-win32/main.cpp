// Loomora Legacy UI for Windows: plain Win32 API in C++11, no MFC, no common controls.
//
// One EDIT control pre-filled with "Coucou" (limited to 1,000 characters) and one STATIC control
// showing the greeting returned by the C++ backend, plus a second STATIC control with the
// end-of-life warning when the backend says this Windows is no longer maintained. This file only
// creates the window, runs the message loop and converts between UTF-16 (Windows) and UTF-8
// (backend); it builds no greeting itself. Only Win32 functions that exist on Windows 2000 are
// imported (Unicode "W" entry points); newer features are loaded at run time (dynfeatures.cpp).
// Probe mode is in probe.cpp, the Windows version in sysinfo.cpp.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <stdint.h>
#include <string.h>
#include <string>

#include "dynfeatures.h"
#include "loomora.h"
#include "probe.h"
#include "resource.h"
#include "sysinfo.h"

namespace {

const int kMaxChars = 1000;
// Lengths at 96 DPI; dynfeatures::Scale adapts them to the DPI of the system.
const int kMargin = 12;
const int kEditHeight = 24;
const int kBannerHeight = 56;
const int kWindowWidth = 420;
const int kWindowHeight = 200;
const wchar_t kWindowClass[] = L"LoomoraWin";

HWND g_edit = NULL;
HWND g_greeting = NULL;
HWND g_banner = NULL; // end-of-life warning, NULL when the system is maintained
std::wstring g_initialInput;

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

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(length > 0 ? length + 1 : 1, L'\0');
    const int copied = GetWindowTextW(window, &text[0], static_cast<int>(text.size()));
    text.resize(copied > 0 ? copied : 0);
    return text;
}

// At most kMaxChars UTF-16 units (what EM_LIMITTEXT allows a user to type, which does not apply
// to text set by the program), never cutting a surrogate pair in two.
std::wstring Truncated(std::wstring text) {
    if (text.size() > static_cast<size_t>(kMaxChars)) {
        text.resize(kMaxChars);
        const unsigned last = static_cast<unsigned>(text[kMaxChars - 1]);
        if (last >= 0xD800 && last <= 0xDBFF)
            text.resize(kMaxChars - 1);
    }
    return text;
}

// Asks the backend for the greeting of the EDIT content and shows it in the STATIC control.
void UpdateGreeting() {
    std::wstring shown;
    try {
        const std::string input = ToUtf8(WindowText(g_edit));
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
    using dynfeatures::Scale;
    const int margin = Scale(kMargin);
    const int inner = width > 2 * margin ? width - 2 * margin : 0;
    int top = margin;
    if (g_banner != NULL) {
        MoveWindow(g_banner, margin, top, inner, Scale(kBannerHeight), TRUE);
        top += Scale(kBannerHeight) + margin;
    }
    MoveWindow(g_edit, margin, top, inner, Scale(kEditHeight), TRUE);
    top += Scale(kEditHeight) + margin;
    MoveWindow(g_greeting, margin, top, inner, height > top + margin ? height - top - margin : 0,
               TRUE);
}

// The end-of-life warning of the backend, shown above the field; it does not block anything.
void CreateBanner(HWND window) {
    if (!sysinfo::IsEndOfLife())
        return;
    const char *warning = loomora_eol_warning();
    const std::wstring text = ToUtf16(warning, strlen(warning));
    g_banner = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0,
                               0, window, NULL, NULL, NULL);
    if (g_banner != NULL)
        SendMessageW(g_banner, WM_SETFONT, reinterpret_cast<WPARAM>(dynfeatures::Font()), TRUE);
}

// Probe mode ends here: the line is built from what the controls hold now.
void FinishProbe(HWND window) {
    probe::Emit(ToUtf8(WindowText(g_edit)), ToUtf8(WindowText(g_greeting)), g_banner != NULL);
    DestroyWindow(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const HFONT font = dynfeatures::Font();
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_initialInput.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                 window, NULL, NULL, NULL);
        g_greeting = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0,
                                     0, window, NULL, NULL, NULL);
        if (g_edit == NULL || g_greeting == NULL)
            return -1; // creation of the window fails
        SendMessageW(g_edit, EM_LIMITTEXT, kMaxChars, 0);
        SendMessageW(g_edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(g_greeting, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        CreateBanner(window);
        UpdateGreeting();
        dynfeatures::StyleWindow(window);
        return 0;
    }
    case WM_SIZE:
        Layout(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_edit);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        // Dark theme of the system; the colours of the system otherwise (high contrast included).
        const HBRUSH brush =
            dynfeatures::ControlColors(reinterpret_cast<HDC>(wparam), message == WM_CTLCOLOREDIT);
        if (brush != NULL)
            return reinterpret_cast<LRESULT>(brush);
        break;
    }
    case WM_PAINT: {
        const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
        probe::MarkDisplayed(); // the first paint of the window; later calls are ignored
        return result;
    }
    case WM_TIMER:
        if (wparam == probe::kTimerId)
            FinishProbe(window);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lparam) == g_edit && HIWORD(wparam) == EN_CHANGE)
            UpdateGreeting();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(probe::ExitStatus());
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    probe::Start();
    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        MessageBoxW(NULL, LoadText(IDS_ABI_MISMATCH).c_str(), LoadText(IDS_TITLE).c_str(),
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    const probe::Options options = probe::ParseCommandLine();
    if (options.badUsage) {
        probe::ReportUsage();
        return 2;
    }
    g_initialInput = Truncated(options.hasInput ? options.input : LoadText(IDS_DEFAULT_INPUT));

    dynfeatures::Init();

    WNDCLASSW windowClass = WNDCLASSW();
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = dynfeatures::BackgroundBrush();
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassW(&windowClass) == 0)
        return 1;

    const HWND window =
        CreateWindowExW(0, kWindowClass, LoadText(IDS_TITLE).c_str(), WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, dynfeatures::Scale(kWindowWidth),
                        dynfeatures::Scale(kWindowHeight), NULL, NULL, instance, NULL);
    if (window == NULL)
        return 1;
    ShowWindow(window, show);
    if (options.enabled)
        probe::ScheduleExit(window);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    dynfeatures::Shutdown();
    return static_cast<int>(message.wParam);
}

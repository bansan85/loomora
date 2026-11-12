#include "dynfeatures.h"

#include <psapi.h>

#include "dynload.h"

namespace dynfeatures {

namespace {

const COLORREF kDarkBackground = RGB(32, 32, 32);
const COLORREF kDarkEdit = RGB(48, 48, 48);
const COLORREF kDarkText = RGB(240, 240, 240); // contrast 14:1 on both backgrounds

std::string g_disabled;
int g_dpi = 96;
bool g_dark = false;
HFONT g_font = NULL;
bool g_ownFont = false;
HBRUSH g_backgroundBrush = NULL;
HBRUSH g_editBrush = NULL;

void Disable(const char *id) {
    const std::string list = "," + g_disabled + ",";
    if (list.find(std::string(",") + id + ",") != std::string::npos)
        return; // already recorded
    if (!g_disabled.empty())
        g_disabled += ',';
    g_disabled += id;
}

// DPI awareness: SetProcessDPIAware exists since Windows Vista. Before that there is no DPI
// virtualization, a program already sees the real DPI, so nothing is lost where it is absent.
// Only the awareness of the primary monitor at start-up is requested (the "system" level): a
// window dragged to a monitor of another DPI is stretched by the system, not redrawn.
void EnableDpiAwareness() {
    typedef BOOL(WINAPI * SetAwareFn)(void);
    const SetAwareFn setAware =
        dynload::Resolve<SetAwareFn>(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (setAware != NULL)
        setAware();
    else
        Disable(LOOMORA_FEATURE_DPI_AWARENESS);

    const HDC screen = GetDC(NULL);
    if (screen != NULL) {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        if (dpi > 0)
            g_dpi = dpi;
        ReleaseDC(NULL, screen);
    }
}

// SPI_GETHIGHCONTRAST through SystemParametersInfoW (looked up at run time like the rest). When a
// high contrast scheme is on, the system colours are left alone: no dark theme of our own.
bool HighContrastOn() {
    typedef BOOL(WINAPI * ParametersFn)(UINT, UINT, PVOID, UINT);
    const ParametersFn parameters =
        dynload::Resolve<ParametersFn>(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoW");
    if (parameters == NULL) {
        Disable(LOOMORA_FEATURE_HIGH_CONTRAST_QUERY);
        return false;
    }
    HIGHCONTRASTW contrast = HIGHCONTRASTW();
    contrast.cbSize = sizeof(contrast);
    return parameters(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != 0 &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// Reads the "apps use light theme" switch of the user (Windows 10 and later). Returns false when
// the value does not exist: the system has no dark theme setting for applications.
bool ReadAppsUseLightTheme(bool *light) {
    typedef LONG(WINAPI * OpenFn)(HKEY, LPCWSTR, DWORD, REGSAM, HKEY *);
    typedef LONG(WINAPI * QueryFn)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    typedef LONG(WINAPI * CloseFn)(HKEY);

    const HMODULE advapi = dynload::LoadSystemLibrary(L"advapi32.dll");
    const OpenFn open = dynload::Resolve<OpenFn>(advapi, "RegOpenKeyExW");
    const QueryFn query = dynload::Resolve<QueryFn>(advapi, "RegQueryValueExW");
    const CloseFn close = dynload::Resolve<CloseFn>(advapi, "RegCloseKey");
    bool found = false;
    if (open != NULL && query != NULL && close != NULL) {
        HKEY key = NULL;
        if (open(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                 KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            DWORD type = 0;
            DWORD value = 0;
            DWORD size = sizeof(value);
            if (query(key, L"AppsUseLightTheme", NULL, &type, reinterpret_cast<LPBYTE>(&value),
                      &size) == ERROR_SUCCESS &&
                type == REG_DWORD) {
                *light = value != 0;
                found = true;
            }
            close(key);
        }
    }
    if (advapi != NULL)
        FreeLibrary(advapi);
    return found;
}

void CreateFontAndBrushes() {
    const HGDIOBJ stock = GetStockObject(DEFAULT_GUI_FONT);
    g_font = static_cast<HFONT>(stock);
    LOGFONTW description = LOGFONTW();
    if (g_dpi != 96 && GetObjectW(stock, sizeof(description), &description) > 0) {
        description.lfHeight = MulDiv(description.lfHeight, g_dpi, 96);
        description.lfWidth = 0;
        const HFONT scaled = CreateFontIndirectW(&description);
        if (scaled != NULL) {
            g_font = scaled;
            g_ownFont = true;
        }
    }
    if (g_dark) {
        g_backgroundBrush = CreateSolidBrush(kDarkBackground);
        g_editBrush = CreateSolidBrush(kDarkEdit);
        if (g_backgroundBrush == NULL || g_editBrush == NULL)
            g_dark = false; // no brush: keep the system colours
    }
}

} // namespace

void Init() {
    EnableDpiAwareness();
    const bool highContrast = HighContrastOn();
    bool light = true;
    if (!ReadAppsUseLightTheme(&light))
        Disable(LOOMORA_FEATURE_DARK_MODE);
    else
        g_dark = !light && !highContrast;
    CreateFontAndBrushes();
}

void Shutdown() {
    if (g_ownFont)
        DeleteObject(g_font);
    if (g_backgroundBrush != NULL)
        DeleteObject(g_backgroundBrush);
    if (g_editBrush != NULL)
        DeleteObject(g_editBrush);
    g_font = NULL;
    g_ownFont = false;
    g_backgroundBrush = NULL;
    g_editBrush = NULL;
}

int Scale(int pixels) { return MulDiv(pixels, g_dpi, 96); }

HFONT Font() { return g_font; }

HBRUSH BackgroundBrush() {
    return g_dark ? g_backgroundBrush : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
}

HBRUSH ControlColors(HDC dc, bool edit) {
    if (!g_dark)
        return NULL;
    SetTextColor(dc, kDarkText);
    SetBkColor(dc, edit ? kDarkEdit : kDarkBackground);
    return edit ? g_editBrush : g_backgroundBrush;
}

void StyleWindow(HWND window) {
    typedef HRESULT(WINAPI * SetAttributeFn)(HWND, DWORD, const void *, DWORD);
    if (!g_dark)
        return;
    const HMODULE dwm = dynload::LoadSystemLibrary(L"dwmapi.dll");
    const SetAttributeFn setAttribute =
        dynload::Resolve<SetAttributeFn>(dwm, "DwmSetWindowAttribute");
    if (setAttribute != NULL) {
        const BOOL on = TRUE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 (Windows 10 version 2004 and later, Windows 11) and
        // was 19 in version 1809; the system refuses the one it does not know.
        if (FAILED(setAttribute(window, 20, &on, sizeof(on))))
            setAttribute(window, 19, &on, sizeof(on));
    }
    if (dwm != NULL)
        FreeLibrary(dwm);
}

long long PeakMemoryKb() {
    typedef BOOL(WINAPI * MemoryInfoFn)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
    const HMODULE psapi = dynload::LoadSystemLibrary(L"psapi.dll");
    const MemoryInfoFn memoryInfo = dynload::Resolve<MemoryInfoFn>(psapi, "GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS counters = PROCESS_MEMORY_COUNTERS();
    counters.cb = sizeof(counters);
    const bool ok = memoryInfo != NULL && memoryInfo(GetCurrentProcess(), &counters, counters.cb);
    if (psapi != NULL)
        FreeLibrary(psapi);
    if (!ok) {
        Disable(LOOMORA_FEATURE_PEAK_MEMORY);
        return -1;
    }
    return static_cast<long long>(counters.PeakWorkingSetSize / 1024);
}

const std::string &DisabledFeatures() { return g_disabled; }

} // namespace dynfeatures

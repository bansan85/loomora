// Features newer than Windows 2000 (dark mode, DPI awareness, high contrast query, message font,
// process memory measurement). Every one of them is looked up at run time (GetProcAddress /
// LoadLibrary) and skipped when the system does not have it; a skipped feature is added to the list
// of disabled features, which probe mode copies into the probe record. Each id below is declared in
// docs/implementation/validation/fallbacks.json under the macro that names it.
#ifndef LOOMORA_WIN32_DYNFEATURES_H
#define LOOMORA_WIN32_DYNFEATURES_H

#include <windows.h>

#include <string>

// Ids reported in "disabled_features".
#define LOOMORA_FEATURE_DARK_MODE "dark-mode"
#define LOOMORA_FEATURE_DPI_AWARENESS "dpi-awareness"
#define LOOMORA_FEATURE_HIGH_CONTRAST_QUERY "high-contrast-query"
#define LOOMORA_FEATURE_MESSAGE_FONT "message-font"
#define LOOMORA_FEATURE_PEAK_MEMORY "peak-memory"

namespace dynfeatures {

// Call once before any window is created: switches DPI awareness on, queries the high contrast
// setting and the dark theme of applications, and prepares the font and the brushes.
void Init();

// Releases what Init created.
void Shutdown();

// Pixels for a length given at 96 DPI.
int Scale(int pixels);

// The font of the controls: the message font of the system (Segoe UI 9 pt since Vista), or the
// stock GUI font scaled to the DPI of the system when the message font cannot be read.
HFONT Font();

// Brush for the class background of the main window (the system face colour, or the dark one).
HBRUSH BackgroundBrush();

// Answer to WM_CTLCOLOREDIT (edit = true) or WM_CTLCOLORSTATIC (edit = false): the brush for the
// dark theme, after setting the text and background colours of `dc`; NULL when the dark theme is
// not in use, in which case the default window procedure paints the control.
HBRUSH ControlColors(HDC dc, bool edit);

// Dark title bar for a window (Windows 10 version 1809 and later); a no-op otherwise.
void StyleWindow(HWND window);

// Peak working set of the process in kB, or -1 (and "peak-memory" disabled) when psapi.dll or
// GetProcessMemoryInfo is not there.
long long PeakMemoryKb();

// Comma-separated ids of the features skipped so far ("" if none), as loomora_probe_record wants.
const std::string &DisabledFeatures();

} // namespace dynfeatures

#endif

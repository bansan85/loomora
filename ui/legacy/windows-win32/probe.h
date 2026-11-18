// Probe mode of the Windows Legacy UI (ui-contract.md): --probe and --probe-input <utf8>.
//
// The Windows GUI executable has no console, so the probe line is written to loomora-probe.json
// next to the executable and to OutputDebugString. The line itself is built by the backend
// (loomora_probe_record); this file only measures, converts and writes.
#ifndef LOOMORA_WIN32_PROBE_H
#define LOOMORA_WIN32_PROBE_H

#include <windows.h>

#include <string>

namespace probe {

struct Options {
    bool enabled;       // --probe was given
    bool hasInput;      // --probe-input was given
    std::wstring input; // its value (UTF-16, as the command line carries it)
    bool badUsage;      // malformed options: missing value, input without --probe, invalid text,
                        // or several of --probe, --validate-project and --roundtrip-project
    int projectMode;    // 0, or the project_probe::Mode of --validate-project / --roundtrip-project
    std::wstring projectPath; // the path given to that option (UTF-16, as the command line carries it)
};

// Identifier of the timer that ends probe mode (WM_TIMER wParam).
const UINT_PTR kTimerId = 1;

// Records the launch time; call it first thing in WinMain.
void Start();

// Reads --probe and --probe-input <text> from the command line of the process. Unknown arguments
// are ignored. An input that is not valid UTF-16 (an unpaired surrogate, so not convertible to
// UTF-8) is bad usage.
Options ParseCommandLine();

// Tells where the usage is (OutputDebugString: there is no console); the caller exits with 2.
void ReportUsage();

// Records the first display; call it after the first paint of the main window. Further calls are
// ignored.
void MarkDisplayed();

// Arms the 2-second timer that ends probe mode; the window procedure calls Emit on WM_TIMER.
void ScheduleExit(HWND window);

// Builds the probe line with loomora_probe_record and writes it to loomora-probe.json (next to
// the executable) and to OutputDebugString. `input` is what the EDIT field holds, `message` what
// the STATIC control displays, both UTF-8.
void Emit(const std::string &input, const std::string &message, bool eolWarningShown);

// 0, or 1 if the probe line could not be built or written.
int ExitStatus();

// Project mode (contracts/ui-validate-project.md): runs --validate-project or --roundtrip-project
// (options.projectMode != 0) with the backend, writes the one result line to
// loomora-validate-project.txt next to the executable (UTF-8, one line ending in LF) and to OutputDebugString, and
// returns the exit status of the program: 0 valid, 1 error (or a line that could not be written),
// 2 the file cannot be read. No window is created.
int RunProject(const Options &options);

} // namespace probe

#endif

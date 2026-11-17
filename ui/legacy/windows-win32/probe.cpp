#include "probe.h"

#include <stdint.h>
#include <vector>

#include "dynfeatures.h"
#include "loomora.h"
#include "project_probe.hpp"
#include "sysinfo.h"

namespace probe {

namespace {

const UINT kExitDelayMs = 2000;
const wchar_t kProbeFileName[] = L"loomora-probe.json";
const wchar_t kProjectFileName[] = L"loomora-validate-project.txt";

LARGE_INTEGER g_launch = LARGE_INTEGER();
LARGE_INTEGER g_displayed = LARGE_INTEGER(); // zero until the first paint
DWORD g_launchTicks = 0;                     // fallback clock if QueryPerformanceCounter fails
DWORD g_displayedTicks = 0;
bool g_displayedMarked = false;
bool g_counterWorks = false;
LARGE_INTEGER g_frequency = LARGE_INTEGER();
int g_exitStatus = 0;

// The command line as a list of arguments, with the rules of the Microsoft C runtime: the program
// name is up to the first space or the closing quote (no escapes); later on, 2n backslashes and a
// quote give n backslashes and toggle the quoting, 2n+1 and a quote give n backslashes and a
// literal quote; backslashes not followed by a quote are literal.
std::vector<std::wstring> SplitCommandLine(const wchar_t *p) {
    std::vector<std::wstring> args;
    std::wstring name;
    if (*p == L'"') {
        for (++p; *p != L'\0' && *p != L'"'; ++p)
            name += *p;
        if (*p != L'\0')
            ++p;
    } else {
        for (; *p != L'\0' && *p != L' ' && *p != L'\t'; ++p)
            name += *p;
    }
    args.push_back(name);

    for (;;) {
        while (*p == L' ' || *p == L'\t')
            ++p;
        if (*p == L'\0')
            break;
        std::wstring arg;
        bool quoted = false;
        while (*p != L'\0' && (quoted || (*p != L' ' && *p != L'\t'))) {
            size_t slashes = 0;
            while (*p == L'\\') {
                ++slashes;
                ++p;
            }
            if (*p == L'"') {
                arg.append(slashes / 2, L'\\');
                if (slashes % 2 == 1)
                    arg += L'"';
                else
                    quoted = !quoted;
                ++p;
            } else if (slashes > 0) {
                arg.append(slashes, L'\\');
            } else {
                arg += *p++;
            }
        }
        args.push_back(arg);
    }
    return args;
}

// True if every UTF-16 unit belongs to a well-formed sequence: no unpaired surrogate.
bool IsWellFormedUtf16(const std::wstring &text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned unit = static_cast<unsigned>(text[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            const bool paired = i + 1 < text.size() &&
                                static_cast<unsigned>(text[i + 1]) >= 0xDC00 &&
                                static_cast<unsigned>(text[i + 1]) <= 0xDFFF;
            if (!paired)
                return false;
            ++i;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

// The UTF-8 form of a well-formed UTF-16 text.
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

// Milliseconds between two readings of the launch clock.
uint32_t ElapsedMs() {
    if (!g_displayedMarked)
        return 0;
    if (g_counterWorks) {
        const LONGLONG ticks = g_displayed.QuadPart - g_launch.QuadPart;
        return ticks > 0 ? static_cast<uint32_t>(ticks * 1000 / g_frequency.QuadPart) : 0;
    }
    return g_displayedTicks - g_launchTicks;
}

// Appends `value` as exactly `digits` decimal digits.
void Append(std::string &text, unsigned value, int digits) {
    std::string number(static_cast<size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i, value /= 10)
        number[static_cast<size_t>(i)] = static_cast<char>('0' + value % 10);
    text += number;
}

// "YYYY-MM-DDTHH:MM:SSZ", the current UTC time.
std::string TimestampUtc() {
    SYSTEMTIME now = SYSTEMTIME();
    GetSystemTime(&now);
    std::string text;
    Append(text, now.wYear, 4);
    text += '-';
    Append(text, now.wMonth, 2);
    text += '-';
    Append(text, now.wDay, 2);
    text += 'T';
    Append(text, now.wHour, 2);
    text += ':';
    Append(text, now.wMinute, 2);
    text += ':';
    Append(text, now.wSecond, 2);
    text += 'Z';
    return text;
}

// Full path of the file `name`: next to the executable.
bool FilePathNextToExecutable(const wchar_t *name, std::wstring *path) {
    std::vector<wchar_t> buffer(MAX_PATH * 4);
    const DWORD n = GetModuleFileNameW(NULL, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size())
        return false; // failed or truncated
    path->assign(&buffer[0], n);
    const size_t slash = path->find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;
    path->resize(slash + 1);
    *path += name;
    return true;
}

bool WriteFileNextToExecutable(const wchar_t *name, const std::string &text) {
    std::wstring path;
    if (!FilePathNextToExecutable(name, &path))
        return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, NULL) &&
                    written == text.size();
    return CloseHandle(file) && ok;
}

} // namespace

void Start() {
    g_counterWorks = QueryPerformanceFrequency(&g_frequency) && g_frequency.QuadPart > 0 &&
                     QueryPerformanceCounter(&g_launch);
    g_launchTicks = GetTickCount();
}

Options ParseCommandLine() {
    Options options = Options();
    const std::vector<std::wstring> args = SplitCommandLine(GetCommandLineW());
    int projectOptions = 0; // --validate-project and --roundtrip-project given, with their repeats
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--probe") {
            options.enabled = true;
        } else if (args[i] == L"--probe-input") {
            if (i + 1 < args.size()) {
                options.hasInput = true;
                options.input = args[++i];
            } else {
                options.badUsage = true;
            }
        } else if (args[i] == L"--validate-project" || args[i] == L"--roundtrip-project") {
            ++projectOptions;
            options.projectMode = args[i] == L"--validate-project" ? project_probe::kValidate
                                                                   : project_probe::kRoundtrip;
            if (i + 1 < args.size())
                options.projectPath = args[++i];
            else
                options.badUsage = true;
        }
    }
    // The input override is only meaningful in probe mode, and must be convertible to UTF-8.
    if (options.hasInput && (!options.enabled || !IsWellFormedUtf16(options.input)))
        options.badUsage = true;
    // The three modes exclude each other, and the path must be convertible to UTF-8.
    if (projectOptions > 1 || (projectOptions == 1 && (options.enabled || options.hasInput)) ||
        (projectOptions == 1 && !IsWellFormedUtf16(options.projectPath)))
        options.badUsage = true;
    return options;
}

void ReportUsage() {
    OutputDebugStringA("usage: loomora.exe [--probe [--probe-input <utf8>] | --validate-project <path> |"
                       " --roundtrip-project <path>]\n");
}

void MarkDisplayed() {
    if (g_displayedMarked)
        return;
    g_displayedMarked = true;
    g_displayedTicks = GetTickCount();
    if (g_counterWorks)
        QueryPerformanceCounter(&g_displayed);
}

void ScheduleExit(HWND window) {
    if (SetTimer(window, kTimerId, kExitDelayMs, NULL) == 0)
        g_exitStatus = 1; // no timer: probe mode could never end by itself
}

void Emit(const std::string &input, const std::string &message, bool eolWarningShown) {
    try {
        // Measured first: the memory measurement may add "peak-memory" to the disabled features.
        const long long rssKb = dynfeatures::PeakMemoryKb();
        const std::string platform = sysinfo::PlatformId();
        const std::string timestamp = TimestampUtc();
        const std::string &disabled = dynfeatures::DisabledFeatures();
        const uint32_t displayMs = ElapsedMs();
        const int32_t displayed = g_displayedMarked ? 1 : 0;

        // First call with no room measures, second one fills the buffer (the line is ASCII).
        const size_t needed = loomora_probe_record(
            platform.c_str(), LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE, input.c_str(),
            message.c_str(), displayed, displayMs, rssKb, eolWarningShown ? 1 : 0, disabled.c_str(),
            timestamp.c_str(), NULL, 0);
        std::string line(needed, '\0');
        if (needed == 0 ||
            loomora_probe_record(platform.c_str(), LOOMORA_UI_LEGACY, LOOMORA_CORE_NATIVE,
                                 input.c_str(), message.c_str(), displayed, displayMs, rssKb,
                                 eolWarningShown ? 1 : 0, disabled.c_str(), timestamp.c_str(),
                                 reinterpret_cast<uint8_t *>(&line[0]), needed) != needed) {
            OutputDebugStringA("loomora: the backend could not build the probe record\n");
            g_exitStatus = 1;
            return;
        }
        line += '\n';
        OutputDebugStringA(line.c_str());
        if (!WriteFileNextToExecutable(kProbeFileName, line)) {
            OutputDebugStringA("loomora: cannot write loomora-probe.json next to the executable\n");
            g_exitStatus = 1;
        }
    } catch (...) { // out of memory: no C++ exception may cross a window procedure
        g_exitStatus = 1;
    }
}

int ExitStatus() { return g_exitStatus; }

int RunProject(const Options &options) {
    std::string line;
    std::string diagnostic;
    int status = 2;
    try {
        const std::string path = ToUtf8(options.projectPath);
        status = project_probe::Run(static_cast<project_probe::Mode>(options.projectMode), path,
                                    &line, &diagnostic);
        if (!diagnostic.empty())
            OutputDebugStringA(("loomora: " + diagnostic + "\n").c_str());
        if (line.empty())
            return status; // nothing to print: the status says why
        line += '\n';
        OutputDebugStringA(line.c_str());
        if (!WriteFileNextToExecutable(kProjectFileName, line)) {
            OutputDebugStringA(
                "loomora: cannot write loomora-validate-project.txt next to the executable\n");
            if (status == 0)
                status = 1;
        }
    } catch (...) { // out of memory
        return 2;
    }
    return status;
}

} // namespace probe

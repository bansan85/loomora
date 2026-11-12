/* runw.c - guest-side helper of tools/run_vectors.py for an old Windows guest (Windows 2000).
 *
 * Why it exists: the old VirtualBox Guest Additions do not pass command-line arguments to a guest
 * program, and Windows Script Host 5.1 (WshShell.Run) converts the command line to the ANSI code
 * page (every character outside it becomes '?') and expands %VARIABLES%. This program starts the
 * Loomora executable with the exact Unicode command line instead, through CreateProcessW.
 *
 *   runw.exe          reads cmdline.txt next to itself (the whole command line, UTF-16LE with or
 *                     without a byte order mark, no line parsing: tabs and newlines are kept),
 *                     deletes loomora-probe.json, starts the process in this folder, waits for it
 *                     and prints, as ASCII lines on standard output:
 *                       LOOMORA-EXIT <exit status of the process>
 *                       LOOMORA-WALLMS <milliseconds from CreateProcessW to the exit, as counted
 *                                      by GetTickCount: a resolution of 10 to 16 ms>
 *                       LOOMORA-PROBE <first line of loomora-probe.json>   (if the file exists)
 *                       LOOMORA-LEFTOVER <number of loomora.exe processes still alive; they are
 *                                        terminated>   (-1 if they could not be counted)
 *   runw.exe --nowait the same start, but does not wait (a window left open on the desktop for a
 *                     screenshot and typed input): prints LOOMORA-STARTED <process id> and exits;
 *                     the new process inherits no handle, so the guest run returns at once
 *   runw.exe --kill   terminates every loomora.exe and every other runw.exe and prints
 *                       LOOMORA-KILLED <count>
 *
 * Only kernel32.dll functions that Windows 2000 exports; no C runtime, no console API. Built by
 * tools/guest/build_runw.sh with the MinGW-w64 i686 compiler (run by tools/run_vectors.py when
 * out/windows-legacy/guest/runw.exe is missing).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#define PROBE_NAME L"loomora-probe.json"
#define CMDLINE_NAME L"cmdline.txt"
#define MAX_CMD 32767
#define MAX_PROBE (1 << 17)

static WCHAR g_dir[2 * MAX_PATH];
static WCHAR g_path[2 * MAX_PATH + 32];
static WCHAR g_cmd[MAX_CMD + 2];
static char g_probe[MAX_PROBE + 1];
static STARTUPINFOW g_si;
static PROCESS_INFORMATION g_pi;
static HANDLE g_out;

static void put(const char *text, DWORD length) {
    DWORD written = 0;
    WriteFile(g_out, text, length, &written, NULL);
}

static void put_str(const char *text) {
    DWORD n = 0;
    while (text[n] != '\0')
        ++n;
    put(text, n);
}

static void put_tagged_number(const char *tag, long value) {
    char digits[16];
    int n = 0;
    unsigned long v = value < 0 ? (unsigned long)(-value) : (unsigned long)value;
    put_str(tag);
    if (value < 0)
        put_str("-");
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
        put(&digits[--n], 1);
    put_str("\r\n");
}

static int cut_after_last_backslash(WCHAR *path) {
    DWORD i = 0, last = 0;
    for (; path[i] != L'\0'; ++i)
        if (path[i] == L'\\')
            last = i;
    if (last == 0)
        return 0;
    path[last + 1] = L'\0';
    return 1;
}

static void join(WCHAR *target, const WCHAR *directory, const WCHAR *name) {
    DWORD i = 0, j = 0;
    for (; directory[i] != L'\0'; ++i)
        target[i] = directory[i];
    for (; name[j] != L'\0'; ++j)
        target[i + j] = name[j];
    target[i + j] = L'\0';
}

/* Counts (and terminates) the processes named loomora.exe, plus runw.exe other than this one when
 * kill_helpers is set. Returns -1 if the process list cannot be read. */
static long terminate_named(int kill_helpers) {
    PROCESSENTRY32 entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    long count = 0;
    BOOL more;
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    entry.dwSize = sizeof(entry);
    for (more = Process32First(snapshot, &entry); more; more = Process32Next(snapshot, &entry)) {
        int target = lstrcmpiA(entry.szExeFile, "loomora.exe") == 0 ||
                     (kill_helpers && lstrcmpiA(entry.szExeFile, "runw.exe") == 0 &&
                      entry.th32ProcessID != GetCurrentProcessId());
        if (target) {
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            ++count;
            if (process != NULL) {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        }
    }
    CloseHandle(snapshot);
    return count;
}

/* True if the command line of runw.exe contains `option` (ASCII, for example "--kill"). */
static int has_option(const char *option) {
    const WCHAR *p = GetCommandLineW();
    for (; *p != L'\0'; ++p) {
        DWORD i = 0;
        while (option[i] != '\0' && p[i] == (WCHAR)option[i])
            ++i;
        if (option[i] == '\0')
            return 1;
    }
    return 0;
}

static int run(void) {
    HANDLE file;
    DWORD got = 0, i;
    DWORD exit_code = 0;
    DWORD started;
    const WCHAR *text;
    DWORD chars;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (has_option("--kill")) {
        put_tagged_number("LOOMORA-KILLED ", terminate_named(1));
        return 0;
    }
    if (GetModuleFileNameW(NULL, g_dir, 2 * MAX_PATH) == 0 || !cut_after_last_backslash(g_dir)) {
        put_str("LOOMORA-RUNERROR cannot find the folder of runw.exe\r\n");
        return 2;
    }

    join(g_path, g_dir, CMDLINE_NAME);
    file = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        put_str("LOOMORA-RUNERROR cannot open cmdline.txt\r\n");
        return 2;
    }
    if (!ReadFile(file, g_cmd, MAX_CMD * sizeof(WCHAR), &got, NULL)) {
        CloseHandle(file);
        put_str("LOOMORA-RUNERROR cannot read cmdline.txt\r\n");
        return 2;
    }
    CloseHandle(file);
    chars = got / sizeof(WCHAR);
    g_cmd[chars] = L'\0';
    text = g_cmd;
    if (chars > 0 && g_cmd[0] == 0xFEFF) { /* byte order mark */
        text = g_cmd + 1;
        --chars;
    }
    if (chars == 0) {
        put_str("LOOMORA-RUNERROR cmdline.txt is empty\r\n");
        return 2;
    }
    if (text != g_cmd)
        for (i = 0; i <= chars; ++i) /* CreateProcessW wants a writable buffer */
            g_cmd[i] = text[i];

    join(g_path, g_dir, PROBE_NAME);
    DeleteFileW(g_path);

    g_si.cb = sizeof(g_si);
    started = GetTickCount();
    if (!CreateProcessW(NULL, g_cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &g_si, &g_pi)) {
        put_tagged_number("LOOMORA-RUNERROR CreateProcessW failed, error ", (long)GetLastError());
        return 2;
    }
    if (has_option("--nowait")) {
        put_tagged_number("LOOMORA-STARTED ", (long)g_pi.dwProcessId);
        CloseHandle(g_pi.hThread);
        CloseHandle(g_pi.hProcess);
        return 0;
    }
    WaitForSingleObject(g_pi.hProcess, INFINITE);
    GetExitCodeProcess(g_pi.hProcess, &exit_code);
    started = GetTickCount() - started;
    CloseHandle(g_pi.hThread);
    CloseHandle(g_pi.hProcess);
    put_tagged_number("LOOMORA-EXIT ", (long)exit_code);
    put_tagged_number("LOOMORA-WALLMS ", (long)started);

    file = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        got = 0;
        if (ReadFile(file, g_probe, MAX_PROBE, &got, NULL)) {
            DWORD end = 0;
            while (end < got && g_probe[end] != '\r' && g_probe[end] != '\n')
                ++end;
            put_str("LOOMORA-PROBE ");
            put(g_probe, end);
            put_str("\r\n");
        }
        CloseHandle(file);
    }
    put_tagged_number("LOOMORA-LEFTOVER ", terminate_named(0));
    return 0;
}

void start(void) { ExitProcess((UINT)run()); }

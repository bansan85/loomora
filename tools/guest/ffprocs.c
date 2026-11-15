/* ffprocs.c - guest-side helper of tools/run_vectors.py (WebFirefoxVmLauncher) for Windows 2000.
 *
 * Windows 2000 has no tasklist or taskkill, and WMI (Windows Script Host) answered "unspecified
 * error" in the guest of the Firefox 10 run, so this counts the firefox.exe processes with the
 * tool help library of kernel32.dll.
 *
 *   ffprocs.exe         prints FIREFOX-PROCESSES <count>   (-1: the process list cannot be read)
 *   ffprocs.exe --kill  the same, then terminates each of them and prints FIREFOX-KILLED <count>
 *
 * Only kernel32.dll functions that Windows 2000 exports; no C runtime. Built by
 * tools/guest/build_ffprocs.sh with the MinGW-w64 i686 compiler (run by tools/run_vectors.py when
 * out/windows-legacy/guest/ffprocs.exe is missing).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

static HANDLE g_out;

static void put_str(const char *text) {
    DWORD n = 0, written = 0;
    while (text[n] != '\0')
        ++n;
    WriteFile(g_out, text, n, &written, NULL);
}

static void put_tagged_number(const char *tag, long value) {
    char digits[16];
    DWORD written = 0;
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
        WriteFile(g_out, &digits[--n], 1, &written, NULL);
    put_str("\r\n");
}

/* Counts the processes named firefox.exe and terminates them when `kill` is set; -1 if the process
 * list cannot be read. */
static long scan(int kill) {
    PROCESSENTRY32 entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    long count = 0;
    BOOL more;
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    entry.dwSize = sizeof(entry);
    for (more = Process32First(snapshot, &entry); more; more = Process32Next(snapshot, &entry)) {
        if (lstrcmpiA(entry.szExeFile, "firefox.exe") == 0) {
            ++count;
            if (kill) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (process != NULL) {
                    TerminateProcess(process, 1);
                    CloseHandle(process);
                }
            }
        }
    }
    CloseHandle(snapshot);
    return count;
}

static int has_kill_option(void) {
    const WCHAR *p = GetCommandLineW();
    const char *option = "--kill";
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
    long count;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (has_kill_option()) {
        count = scan(1);
        put_tagged_number("FIREFOX-PROCESSES ", count);
        put_tagged_number("FIREFOX-KILLED ", count < 0 ? 0 : count);
    } else {
        put_tagged_number("FIREFOX-PROCESSES ", scan(0));
    }
    return 0;
}

void start(void) { ExitProcess((UINT)run()); }

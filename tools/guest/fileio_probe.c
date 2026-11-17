/* fileio_probe.c - spike S-F3 of spec 002: the file operations the .loomora I/O layer
 * (core/src/format/io/file_win32.cpp) relies on, measured on the system under test.
 *
 *   (a) a reader opened with FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, then the file
 *       replaced from a second handle, by four methods:
 *         MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)   Windows 2000
 *         rename the old file away, move the new one in, delete the old   Windows 2000
 *         ReplaceFileW                                                    Windows XP (found by GetProcAddress)
 *         SetFileInformationByHandle(FileRenameInfoEx, POSIX semantics)   Windows 10 1809 (GetProcAddress)
 *       for each: the result, the Windows error, whether the reader still reads the complete old
 *       content, whether the target then holds the new content. A reader without
 *       FILE_SHARE_DELETE is also tried with MoveFileExW.
 *   (b) a 5 GiB offset set with SetFilePointer and its high word on a sparse file (never
 *       SetFilePointerEx), including the offsets whose low word is 0xFFFFFFFF (the value of
 *       INVALID_SET_FILE_POINTER) and 0x80000000 (negative as a LONG); skipped with a message
 *       when the file system cannot make the file.
 *   (c) a UTF-8 path with non-BMP characters converted with MultiByteToWideChar(CP_UTF8): the
 *       UTF-16 produced (surrogate pairs expected), a file created with it, its name read back
 *       from the directory; the flags MB_ERR_INVALID_CHARS (Vista and later) and a malformed
 *       byte are tried too, for information.
 *
 * Usage: fileio_probe [directory]    (default: the current directory; everything it creates is
 * in a sub-folder it removes at the end). Output: one line per observation, "S-F3 <part> ...",
 * and a final "S-F3 done". Exit status 0 when it ran to the end, whatever the observations.
 *
 * Plain C89, kernel32 and the C runtime only, so it builds with the MinGW-w64 Windows 2000
 * toolchain (-D_WIN32_WINNT=0x0500) and with clang or MSVC on a recent Windows. Newer functions
 * are looked up with GetProcAddress. Not part of the product: tooling that produced evidence.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE 0x000900C4
#endif
#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#endif
#ifndef MB_ERR_INVALID_CHARS
#define MB_ERR_INVALID_CHARS 0x00000008
#endif

typedef unsigned long long u64;

static WCHAR g_root[MAX_PATH * 2]; /* the probe's own folder, with a trailing backslash */

/* ---- small helpers ----------------------------------------------------------------------- */

static void wpath(WCHAR *out, const WCHAR *name) {
    lstrcpyW(out, g_root);
    lstrcatW(out, name);
}

static int write_new(const WCHAR *path, const char *data, DWORD len) {
    DWORD wrote = 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!WriteFile(h, data, len, &wrote, NULL) || wrote != len) {
        CloseHandle(h);
        return 0;
    }
    return CloseHandle(h) != 0;
}

/* Reads up to `cap` bytes of the whole file through the open handle `h` from its start. */
static DWORD read_all_handle(HANDLE h, char *buf, DWORD cap) {
    DWORD got = 0;
    if (SetFilePointer(h, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return 0;
    if (!ReadFile(h, buf, cap, &got, NULL))
        return 0;
    return got;
}

static int file_has(const WCHAR *path, const char *expect) {
    char buf[64];
    DWORD got;
    DWORD n = (DWORD)strlen(expect);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    got = read_all_handle(h, buf, sizeof buf);
    CloseHandle(h);
    ok = got == n && memcmp(buf, expect, n) == 0;
    return ok;
}

static void report_os(void) {
    OSVERSIONINFOA v;
    WCHAR vol[MAX_PATH];
    WCHAR fsname[MAX_PATH];
    DWORD serial = 0, maxlen = 0, flags = 0;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (GetVersionExA(&v))
        printf("S-F3 system: Windows %lu.%lu build %lu %s\n", (unsigned long)v.dwMajorVersion,
               (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber, v.szCSDVersion);
    /* GetVersionExA reports 6.2 on Windows 8.1 and later without a manifest: RtlGetVersion does not lie. */
    {
        typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        RtlGetVersion_t rgv = nt ? (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion") : NULL;
        OSVERSIONINFOW w;
        memset(&w, 0, sizeof w);
        w.dwOSVersionInfoSize = sizeof w;
        if (rgv != NULL && rgv(&w) == 0)
            printf("S-F3 system (RtlGetVersion): Windows %lu.%lu build %lu\n", (unsigned long)w.dwMajorVersion,
                   (unsigned long)w.dwMinorVersion, (unsigned long)w.dwBuildNumber);
    }
    (void)vol;
    {
        /* GetVolumePathNameW is Windows XP: found at run time on the floor. */
        HMODULE k = GetModuleHandleA("kernel32.dll");
        typedef BOOL(WINAPI * GetVolumePathNameW_t)(LPCWSTR, LPWSTR, DWORD);
        GetVolumePathNameW_t fn = k ? (GetVolumePathNameW_t)GetProcAddress(k, "GetVolumePathNameW") : NULL;
        WCHAR root[MAX_PATH];
        if (fn != NULL && fn(g_root, root, MAX_PATH) &&
            GetVolumeInformationW(root, NULL, 0, &serial, &maxlen, &flags, fsname, MAX_PATH))
            printf("S-F3 filesystem: %ls (max component %lu)\n", fsname, (unsigned long)maxlen);
        else if (g_root[1] == L':') {
            root[0] = g_root[0];
            root[1] = L':';
            root[2] = L'\\';
            root[3] = 0;
            if (GetVolumeInformationW(root, NULL, 0, &serial, &maxlen, &flags, fsname, MAX_PATH))
                printf("S-F3 filesystem: %ls (max component %lu)\n", fsname, (unsigned long)maxlen);
        }
    }
}

/* ---- (a) replacing a file a reader holds open -------------------------------------------- */

typedef BOOL(WINAPI *ReplaceFileW_t)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
typedef BOOL(WINAPI *SetFileInformationByHandle_t)(HANDLE, int, LPVOID, DWORD);

#define NO_READER 0xFFFFFFFFu

enum { M_MOVEEX, M_RENAME_AWAY, M_REPLACEFILE, M_POSIX_RENAME };

static const char *method_name(int m) {
    switch (m) {
    case M_MOVEEX: return "MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)";
    case M_RENAME_AWAY: return "rename-old-away, move-new-in, delete-old";
    case M_REPLACEFILE: return "ReplaceFileW";
    default: return "SetFileInformationByHandle(FileRenameInfoEx, POSIX semantics)";
    }
}

/* FILE_RENAME_INFO_EX, declared here: the SDK of the floor does not have it. */
typedef struct {
    DWORD Flags;
    HANDLE RootDirectory;
    DWORD FileNameLength; /* bytes */
    WCHAR FileName[MAX_PATH * 2];
} RenameInfoEx;

/* Returns 1 when the replacement succeeded, 0 otherwise (and *err holds the Windows error);
 * -1 when the method does not exist on this system. */
static int do_replace(int method, const WCHAR *temp, const WCHAR *target, DWORD *err) {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    *err = 0;
    if (method == M_MOVEEX) {
        if (MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 1;
        *err = GetLastError();
        return 0;
    }
    if (method == M_RENAME_AWAY) {
        WCHAR old[MAX_PATH * 2];
        lstrcpyW(old, target);
        lstrcatW(old, L".old");
        if (!MoveFileExW(target, old, 0)) {
            *err = GetLastError();
            return 0;
        }
        if (!MoveFileExW(temp, target, MOVEFILE_WRITE_THROUGH)) {
            *err = GetLastError();
            MoveFileExW(old, target, 0);
            return 0;
        }
        if (!DeleteFileW(old)) {
            DWORD e = GetLastError();
            printf("S-F3 a   (the old file could not be deleted: Windows error %lu)\n", (unsigned long)e);
        }
        return 1;
    }
    if (method == M_REPLACEFILE) {
        ReplaceFileW_t fn = k ? (ReplaceFileW_t)GetProcAddress(k, "ReplaceFileW") : NULL;
        if (fn == NULL)
            return -1;
        if (fn(target, temp, NULL, 0, NULL, NULL))
            return 1;
        *err = GetLastError();
        return 0;
    }
    {
        SetFileInformationByHandle_t fn =
            k ? (SetFileInformationByHandle_t)GetProcAddress(k, "SetFileInformationByHandle") : NULL;
        RenameInfoEx info;
        HANDLE h;
        BOOL ok;
        if (fn == NULL)
            return -1;
        h = CreateFileW(temp, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            *err = GetLastError();
            return 0;
        }
        memset(&info, 0, sizeof info);
        info.Flags = 0x1 | 0x2; /* FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS */
        info.FileNameLength = (DWORD)(lstrlenW(target) * sizeof(WCHAR));
        lstrcpyW(info.FileName, target);
        ok = fn(h, 22 /* FileRenameInfoEx */, &info, (DWORD)sizeof info);
        if (!ok)
            *err = GetLastError();
        CloseHandle(h);
        return ok ? 1 : 0;
    }
}

static void scenario_a(int method, DWORD reader_share, const char *label) {
    WCHAR target[MAX_PATH * 2], temp[MAX_PATH * 2];
    char buf[64];
    HANDLE reader;
    DWORD err = 0, got;
    int r, old_ok, new_ok;
    wpath(target, L"t.bin");
    wpath(temp, L"t.bin.tmp-0");
    DeleteFileW(target);
    DeleteFileW(temp);
    if (!write_new(target, "OLD-CONTENT", 11) || !write_new(temp, "NEW-CONTENT", 11)) {
        printf("S-F3 a %s: cannot create the files (error %lu)\n", label, (unsigned long)GetLastError());
        return;
    }
    reader = reader_share == NO_READER
                 ? CreateFileW(target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : CreateFileW(target, GENERIC_READ, reader_share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (reader != INVALID_HANDLE_VALUE && reader_share == NO_READER) {
        CloseHandle(reader); /* baseline: the reader is closed before the replace */
        reader = CreateFileW(target, GENERIC_WRITE | GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (reader != INVALID_HANDLE_VALUE) {
            CloseHandle(reader);
            reader = INVALID_HANDLE_VALUE;
            r = do_replace(method, temp, target, &err);
            printf("S-F3 a %s | %s: %s (Windows error %lu); target holds the new content: %s\n", label,
                   method_name(method), r == 1 ? "replaced" : "FAILED", (unsigned long)err,
                   file_has(target, "NEW-CONTENT") ? "yes" : "NO");
            DeleteFileW(target);
            DeleteFileW(temp);
            return;
        }
    }
    if (reader == INVALID_HANDLE_VALUE) {
        printf("S-F3 a %s: cannot open the reader (error %lu)\n", label, (unsigned long)GetLastError());
        return;
    }
    r = do_replace(method, temp, target, &err);
    if (r < 0) {
        printf("S-F3 a %s | %s: not available on this system\n", label, method_name(method));
    } else {
        got = read_all_handle(reader, buf, sizeof buf);
        old_ok = got == 11 && memcmp(buf, "OLD-CONTENT", 11) == 0;
        CloseHandle(reader);
        reader = INVALID_HANDLE_VALUE;
        new_ok = file_has(target, "NEW-CONTENT");
        if (r == 1)
            printf("S-F3 a %s | %s: replaced; reader still reads the complete old file: %s; target holds the new content: %s\n",
                   label, method_name(method), old_ok ? "yes" : "NO", new_ok ? "yes" : "NO");
        else
            printf("S-F3 a %s | %s: FAILED, Windows error %lu; reader reads old: %s; target unchanged: %s\n",
                   label, method_name(method), (unsigned long)err, old_ok ? "yes" : "NO",
                   file_has(target, "OLD-CONTENT") ? "yes" : "NO");
    }
    if (reader != INVALID_HANDLE_VALUE)
        CloseHandle(reader);
    DeleteFileW(target);
    DeleteFileW(temp);
    {
        WCHAR old[MAX_PATH * 2];
        lstrcpyW(old, target);
        lstrcatW(old, L".old");
        DeleteFileW(old);
    }
}

static void part_a(void) {
    const DWORD all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    int m;
    scenario_a(M_MOVEEX, NO_READER, "no reader open (baseline)");
    scenario_a(M_MOVEEX, 0, "reader share=none");
    for (m = M_MOVEEX; m <= M_POSIX_RENAME; ++m)
        scenario_a(m, all, "reader share=READ|WRITE|DELETE");
    scenario_a(M_MOVEEX, FILE_SHARE_READ | FILE_SHARE_WRITE, "reader share=READ|WRITE (no DELETE)");
}

/* ---- (b) 5 GiB offset with SetFilePointer and its high word -------------------------------- */

static int seek_hw(HANDLE h, u64 pos) {
    LONG high = (LONG)(pos >> 32);
    DWORD low;
    SetLastError(NO_ERROR); /* the low word may be 0xFFFFFFFF on success: GetLastError tells */
    low = SetFilePointer(h, (LONG)(pos & 0xFFFFFFFFu), &high, FILE_BEGIN);
    return !(low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR);
}

static void part_b(void) {
    WCHAR path[MAX_PATH * 2];
    HANDLE h;
    DWORD ret = 0, wrote = 0, size_high = 0, size_low;
    const u64 offsets[3] = {5ull << 30, 0x1FFFFFFFFull, 0x180000000ull};
    int i;
    wpath(path, L"sparse.bin");
    DeleteFileW(path);
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("S-F3 b cannot create the file (error %lu): skipped\n", (unsigned long)GetLastError());
        return;
    }
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL))
        printf("S-F3 b FSCTL_SET_SPARSE refused (error %lu): the file system may not be sparse\n",
               (unsigned long)GetLastError());
    for (i = 0; i < 3; ++i) {
        char buf[4];
        DWORD got = 0;
        int okw = seek_hw(h, offsets[i]) && WriteFile(h, "END!", 4, &wrote, NULL) && wrote == 4;
        if (!okw) {
            printf("S-F3 b offset 0x%llx: write failed (error %lu): %s\n", offsets[i],
                   (unsigned long)GetLastError(),
                   GetLastError() == ERROR_DISK_FULL || GetLastError() == ERROR_FILE_TOO_LARGE ||
                           GetLastError() == ERROR_INVALID_PARAMETER ? "the file system cannot (e.g. FAT32 limit 4 GiB)" : "unexpected");
            continue;
        }
        memset(buf, 0, sizeof buf);
        if (seek_hw(h, offsets[i]) && ReadFile(h, buf, 4, &got, NULL) && got == 4 && memcmp(buf, "END!", 4) == 0)
            printf("S-F3 b offset 0x%llx via SetFilePointer high word: write and read back ok\n", offsets[i]);
        else
            printf("S-F3 b offset 0x%llx via SetFilePointer high word: read back FAILED (error %lu)\n",
                   offsets[i], (unsigned long)GetLastError());
    }
    size_low = GetFileSize(h, &size_high);
    printf("S-F3 b GetFileSize: high=%lu low=0x%lx\n", (unsigned long)size_high, (unsigned long)size_low);
    CloseHandle(h);
    DeleteFileW(path);
}

/* ---- (c) UTF-8 path with non-BMP characters ------------------------------------------------- */

static void part_c(void) {
    /* "pr" e-acute (U+00E9), U+1F600 GRINNING FACE (non-BMP), U+65E5, U+1D11E MUSICAL SYMBOL G CLEF, ".txt" */
    static const char utf8[] = "pr\xC3\xA9-\xF0\x9F\x98\x80-\xE6\x97\xA5-\xF0\x9D\x84\x9E.txt";
    static const WCHAR expect[] = {'p', 'r', 0x00E9, '-', 0xD83D, 0xDE00, '-', 0x65E5, '-', 0xD834, 0xDD1E, '.', 't', 'x', 't', 0};
    WCHAR wide[64];
    WCHAR full[MAX_PATH * 2];
    int n, same;
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int found = 0;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 64);
    same = n == (int)(sizeof expect / sizeof expect[0]) && memcmp(wide, expect, sizeof expect) == 0;
    printf("S-F3 c MultiByteToWideChar(CP_UTF8, 0): %d UTF-16 units, surrogate pairs correct: %s\n", n,
           same ? "yes" : "NO");
    if (!same) {
        int i;
        printf("S-F3 c   got:");
        for (i = 0; i < n && i < 20; ++i)
            printf(" %04X", (unsigned)wide[i]);
        printf("\n");
    }
    SetLastError(0);
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, 64);
    printf("S-F3 c MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS): %s (Windows error %lu)\n",
           n > 0 ? "supported" : "refused", n > 0 ? 0ul : (unsigned long)GetLastError());
    {
        static const char bad[] = "a\xFF" "b";
        WCHAR out[8];
        SetLastError(0);
        n = MultiByteToWideChar(CP_UTF8, 0, bad, -1, out, 8);
        printf("S-F3 c a malformed byte with flags 0: %d units, second unit U+%04X (Windows error %lu): "
               "the product validates UTF-8 itself\n", n, n > 1 ? (unsigned)out[1] : 0u, (unsigned long)GetLastError());
    }
    wpath(full, L"");
    lstrcatW(full, expect);
    if (write_new(full, "x", 1)) {
        WCHAR pat[MAX_PATH * 2];
        wpath(pat, L"pr*");
        f = FindFirstFileW(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (lstrcmpW(fd.cFileName, expect) == 0)
                    found = 1;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        printf("S-F3 c file created through the converted path; its name read back from the directory is identical: %s\n",
               found ? "yes" : "NO");
        DeleteFileW(full);
    } else {
        printf("S-F3 c file creation with the converted path FAILED (Windows error %lu)\n",
               (unsigned long)GetLastError());
    }
}

/* ---- main --------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    WCHAR base[MAX_PATH * 2];
    char tail[32];
    DWORD len;
    (void)argv;
    if (argc >= 2) {
        if (MultiByteToWideChar(CP_ACP, 0, argv[1], -1, base, MAX_PATH * 2) == 0) {
            printf("S-F3 bad directory argument\n");
            return 1;
        }
    } else {
        lstrcpyW(base, L".");
    }
    len = GetFullPathNameW(base, MAX_PATH * 2, g_root, NULL);
    if (len == 0 || len >= MAX_PATH * 2 - 40) {
        printf("S-F3 cannot resolve the directory\n");
        return 1;
    }
    if (g_root[lstrlenW(g_root) - 1] != L'\\')
        lstrcatW(g_root, L"\\");
    sprintf(tail, "fileio-probe-%lu", (unsigned long)GetTickCount());
    {
        WCHAR wtail[40];
        int i;
        for (i = 0; tail[i] != 0; ++i)
            wtail[i] = (WCHAR)tail[i];
        wtail[i] = 0;
        lstrcatW(g_root, wtail);
    }
    if (!CreateDirectoryW(g_root, NULL)) {
        printf("S-F3 cannot create the probe folder (Windows error %lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    lstrcatW(g_root, L"\\");
    report_os();
    part_a();
    part_b();
    part_c();
    g_root[lstrlenW(g_root) - 1] = 0;
    RemoveDirectoryW(g_root);
    printf("S-F3 done\n");
    return 0;
}

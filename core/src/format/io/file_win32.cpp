// The file source and the file sink on Windows (Windows 2000 and later, x86, x64, ARM64).
// #if-guarded: it compiles to nothing elsewhere, where file_posix.cpp defines the same factories.
// Built for _WIN32_WINNT=0x0500, so only functions of Windows 2000 are called unconditionally:
// no ReplaceFile (XP), no GetFileSizeEx / SetFilePointerEx on the Windows 2000 path (the seek
// then goes through SetFilePointer and its high word, LOOMORA_FMT_WIN2000_SEEK in config.hpp),
// no MB_ERR_INVALID_CHARS (Vista).
//
//   source   CreateFileW(GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE): it
//            never writes and lets another process replace or delete the file, so a save that
//            replaces the file while a reader is open succeeds and the reader keeps reading the
//            complete old file.
//   sink     `<target>.tmp-<random>` created with CREATE_NEW in the target's directory,
//            FlushFileBuffers, then MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),
//            temporary file deleted on failure, abort or destruction. When a reader holds the
//            target open MoveFileExW is refused (measured, S-F3): see WinSink::replace_target.
//            When the target is a symbolic link (FR-028) it is followed where the system can tell
//            (GetFinalPathNameByHandleW, Windows Vista and later, found at run time): the file the
//            link leads to is replaced, in its own directory, and the link stays a link. A link
//            whose target is missing is followed too, reading the link text with DeviceIoControl
//            (FSCTL_GET_REPARSE_POINT): the save creates the file the link names, as POSIX does.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include "format/io/file.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"
#include "internal/config.hpp"
#include "loomora.h"
#include "loomora_project.h"

namespace loomora {
namespace format {

namespace {

// A transfer never asks for more than this in one call (DWORD length, and old systems fail very
// large synchronous transfers over a network).
const std::size_t kMaxChunk = 32u * 1024u * 1024u;

std::string win_error_text(const std::string &what, const std::string &path, DWORD error) {
    char number[16];
    std::size_t n = 0;
    char digits[16];
    DWORD v = error;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < sizeof digits);
    for (std::size_t i = 0; i < n; ++i)
        number[i] = digits[n - 1 - i];
    number[n] = '\0';
    return what + " '" + path + "': Windows error " + number;
}

// Strict UTF-8 check (MultiByteToWideChar would turn malformed bytes into U+FFFD, and a path with
// a replacement character could name another file): no overlong form, no surrogate, nothing above
// U+10FFFF, no NUL.
bool valid_utf8(const std::string &s) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0)
            return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra;
        std::uint32_t cp;
        std::uint32_t minimum;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
            cp = c & 0x1Fu;
            minimum = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            cp = c & 0x0Fu;
            minimum = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            cp = c & 0x07u;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (n - i - 1 < extra)
            return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

// UTF-8 to UTF-16. A path too long for MAX_PATH is made absolute and given the \\?\ prefix (the
// long path form exists since Windows 2000). Throws LOOMORA_ERR_IO for a malformed path.
std::wstring to_native(const std::string &utf8) {
    if (utf8.empty() || !valid_utf8(utf8))
        throw FormatError(LOOMORA_ERR_IO, "the path is empty or not valid UTF-8");
    if (utf8.size() > 0x7FFFFFFF)
        throw FormatError(LOOMORA_ERR_IO, "the path is too long");
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0)
        throw FormatError(LOOMORA_ERR_IO, "the path cannot be converted to UTF-16");
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n) != n)
        throw FormatError(LOOMORA_ERR_IO, "the path cannot be converted to UTF-16");

    // 240 leaves room for the ".tmp-" suffix of the sink's temporary file.
    if (w.size() >= 240 && w.compare(0, 4, L"\\\\?\\") != 0) {
        std::wstring full(w.size() + 300, L'\0');
        DWORD got = GetFullPathNameW(w.c_str(), static_cast<DWORD>(full.size()), &full[0], nullptr);
        if (got >= full.size()) {
            full.assign(static_cast<std::size_t>(got) + 1, L'\0');
            got = GetFullPathNameW(w.c_str(), static_cast<DWORD>(full.size()), &full[0], nullptr);
        }
        if (got == 0 || got >= full.size())
            throw FormatError(LOOMORA_ERR_IO, "the path cannot be made absolute");
        full.resize(got);
        if (full.compare(0, 2, L"\\\\") == 0)
            w = L"\\\\?\\UNC\\" + full.substr(2);
        else
            w = L"\\\\?\\" + full;
    }
    return w;
}

// Moves the file pointer of `h` to the 64-bit offset `pos`. Returns false on failure.
bool seek64(HANDLE h, std::uint64_t pos) {
#if LOOMORA_FMT_WIN2000_SEEK
    LONG high = static_cast<LONG>(pos >> 32);
    SetLastError(NO_ERROR); // a low word of 0xFFFFFFFF is a valid result: GetLastError decides
    const DWORD low = SetFilePointer(h, static_cast<LONG>(pos & 0xFFFFFFFFu), &high, FILE_BEGIN);
    return !(low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR);
#else
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(pos);
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
#endif
}

typedef DWORD(WINAPI *FinalPathFn)(HANDLE, LPWSTR, DWORD, DWORD);
typedef BOOL(WINAPI *DeviceIoControlFn)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

// A kernel32 function by name, or null. Through a generic function pointer type: GCC's
// -Wcast-function-type accepts void (*)().
void (*kernel_function(const char *name))() {
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    const FARPROC proc = kernel != nullptr ? GetProcAddress(kernel, name) : nullptr;
    return reinterpret_cast<void (*)()>(proc);
}

// The path of the file `h` is open on, in the `\\?\` form. False when the system cannot tell.
bool final_path_of(FinalPathFn final_path, HANDLE h, std::wstring &out) {
    out.assign(static_cast<std::size_t>(MAX_PATH) + 1, L'\0');
    DWORD n = final_path(h, &out[0], static_cast<DWORD>(out.size()), 0 /* VOLUME_NAME_DOS */);
    if (n >= out.size()) { // too short: n is the size needed, with the terminating NUL
        out.assign(static_cast<std::size_t>(n), L'\0');
        n = final_path(h, &out[0], static_cast<DWORD>(out.size()), 0);
    }
    if (n == 0 || n >= out.size())
        return false;
    out.resize(n);
    return true;
}

// The final path of `path` when the file (or the file its link leads to) exists. No access wanted,
// only the name; the open follows the links.
bool final_path_of_file(FinalPathFn final_path, const std::wstring &path, std::wstring &out) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = final_path_of(final_path, h, out);
    CloseHandle(h);
    return ok;
}

// `path` as an absolute path in the `\\?\` form (which Win32 does not normalise: no "." or "..").
bool long_absolute(const std::wstring &path, std::wstring &out) {
    if (path.compare(0, 4, L"\\\\?\\") == 0) {
        out = path;
        return true;
    }
    std::wstring full(path.size() + 300, L'\0');
    DWORD got = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), &full[0], nullptr);
    if (got >= full.size()) {
        full.assign(static_cast<std::size_t>(got) + 1, L'\0');
        got = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), &full[0], nullptr);
    }
    if (got == 0 || got >= full.size())
        return false;
    full.resize(got);
    out = full.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + full.substr(2) : L"\\\\?\\" + full;
    return true;
}

// Removes the "." and ".." components of a `\\?\` path, which the system leaves alone in that form.
// False for a path it does not understand.
bool normalize_long(const std::wstring &path, std::wstring &out) {
    if (path.compare(0, 4, L"\\\\?\\") != 0)
        return false;
    // The root is kept as it is: `\\?\C:`, `\\?\UNC\server\share`, `\\?\Volume{...}`.
    const std::size_t root_components = path.compare(4, 4, L"UNC\\") == 0 ? 3 : 1;
    std::size_t pos = 4;
    for (std::size_t k = 0; k < root_components; ++k) {
        pos = path.find(L'\\', pos);
        if (pos == std::wstring::npos)
            return false;
        if (k + 1 < root_components)
            ++pos;
    }
    const std::wstring root = path.substr(0, pos);
    std::vector<std::wstring> parts;
    std::size_t at = pos + 1;
    while (at <= path.size()) {
        std::size_t next = path.find(L'\\', at);
        if (next == std::wstring::npos)
            next = path.size();
        const std::wstring part = path.substr(at, next - at);
        if (part == L"..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        at = next + 1;
    }
    if (parts.empty())
        return false;
    out = root;
    for (std::size_t i = 0; i < parts.size(); ++i)
        out += L"\\" + parts[i];
    return true;
}

// The place a symbolic link `link` (an absolute `\\?\` path) leads to, as an absolute `\\?\` path, read
// from the link itself (FSCTL_GET_REPARSE_POINT on a handle that does not follow it), so that it works
// when the target is missing. False when `link` is not a symbolic link, or on any failure: a junction
// (IO_REPARSE_TAG_MOUNT_POINT) is not followed. DeviceIoControl is found at run time like the other
// functions here, so the import table stays what the Windows 2000 check allows.
bool read_link(FinalPathFn final_path, const std::wstring &link, std::wstring &target) {
    DeviceIoControlFn control = reinterpret_cast<DeviceIoControlFn>(kernel_function("DeviceIoControl"));
    if (control == nullptr)
        return false;
    // Not in the headers of the Windows 2000 floor, nor the REPARSE_DATA_BUFFER of a symbolic link.
    const DWORD kFsctlGetReparsePoint = 0x000900A8;
    const DWORD kReparseTagSymlink = 0xA000000C;
    const DWORD kSymlinkFlagRelative = 0x1;
    const std::size_t kHeader = 20; // tag, data length, reserved, four name offsets and lengths, flags
    HANDLE h = CreateFileW(link.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<unsigned char> buffer(16u * 1024u, 0); // MAXIMUM_REPARSE_DATA_BUFFER_SIZE
    DWORD got = 0;
    const BOOL ok = control(h, kFsctlGetReparsePoint, nullptr, 0, &buffer[0], static_cast<DWORD>(buffer.size()),
                            &got, nullptr);
    CloseHandle(h);
    if (!ok || got < kHeader)
        return false;
    DWORD tag = 0;
    DWORD flags = 0;
    WORD substitute_offset = 0;
    WORD substitute_length = 0;
    std::memcpy(&tag, &buffer[0], sizeof tag);
    std::memcpy(&substitute_offset, &buffer[8], sizeof substitute_offset);
    std::memcpy(&substitute_length, &buffer[10], sizeof substitute_length);
    std::memcpy(&flags, &buffer[16], sizeof flags);
    if (tag != kReparseTagSymlink || substitute_length == 0 || (substitute_offset & 1) != 0 ||
        (substitute_length & 1) != 0 || kHeader + substitute_offset + substitute_length > got)
        return false;
    std::wstring text(substitute_length / sizeof(WCHAR), L'\0');
    std::memcpy(&text[0], &buffer[kHeader + substitute_offset], substitute_length);

    if ((flags & kSymlinkFlagRelative) == 0) {
        // The substitute name of an absolute link is an NT path: `\??\C:\dir\file`.
        if (text.compare(0, 4, L"\\??\\") != 0)
            return false;
        return normalize_long(L"\\\\?\\" + text.substr(4), target);
    }
    // Relative to the directory of the link, resolved on disk (that directory may itself be reached
    // through a link). A rooted or drive-relative text is not handled.
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == L'/')
            text[i] = L'\\';
    if (text[0] == L'\\' || (text.size() > 1 && text[1] == L':'))
        return false;
    const std::size_t slash = link.rfind(L'\\');
    if (slash == std::wstring::npos)
        return false;
    std::wstring directory = link.substr(0, slash);
    std::wstring real_directory;
    if (final_path_of_file(final_path, directory, real_directory))
        directory = real_directory;
    return normalize_long(directory + L"\\" + text, target);
}

// FR-028: the path of the file a save to `wpath` replaces, as POSIX does: when `wpath` is a symbolic
// link (a reparse point), the file at the end of the chain of links, in the `\\?\` form, where a link
// whose target is missing leads to the path the save creates; otherwise `wpath` unchanged: not a link,
// a junction, a loop or a chain of more than 40 links, any failure, or a system without
// GetFinalPathNameByHandleW (Windows 2000 and XP, which have no symbolic links either). The functions
// are found with GetProcAddress, never imported, so the binary still loads on the Windows 2000 floor.
std::wstring follow_link(const std::wstring &wpath) {
    const int kMaxHops = 40;
    const DWORD attributes = GetFileAttributesW(wpath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
        return wpath;
    const FinalPathFn final_path = reinterpret_cast<FinalPathFn>(kernel_function("GetFinalPathNameByHandleW"));
    if (final_path == nullptr)
        return wpath;
    std::wstring current = wpath;
    for (int hop = 0; hop <= kMaxHops; ++hop) {
        // The end of the chain exists: the system resolves the whole of it, every link included.
        std::wstring resolved;
        if (final_path_of_file(final_path, current, resolved))
            return resolved;
        // Otherwise one link at a time, up to the missing file the last link names.
        const DWORD a = GetFileAttributesW(current.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            std::wstring absolute;
            return hop > 0 && long_absolute(current, absolute) ? absolute : wpath;
        }
        std::wstring link;
        std::wstring target;
        if (!long_absolute(current, link) || !read_link(final_path, link, target))
            return wpath;
        current = target;
    }
    return wpath;
}

class WinSource : public Source {
  public:
    WinSource(HANDLE h, std::uint64_t size) : h_(h), size_(size) {
#if LOOMORA_FMT_WIN2000_SEEK
        InitializeCriticalSection(&lock_);
#endif
    }
    ~WinSource() override {
        CloseHandle(h_);
#if LOOMORA_FMT_WIN2000_SEEK
        DeleteCriticalSection(&lock_);
#endif
    }

    std::uint64_t size() const override { return size_; }

    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t len) const override {
        if (len == 0 || offset >= size_)
            return 0;
        if (size_ - offset < len)
            len = static_cast<std::size_t>(size_ - offset);
        char *out = static_cast<char *>(buf);
        std::size_t done = 0;
        while (done < len) {
            const DWORD want = static_cast<DWORD>(len - done < kMaxChunk ? len - done : kMaxChunk);
            DWORD got = 0;
            if (!read_chunk(offset + done, out + done, want, got))
                throw FormatError(LOOMORA_ERR_IO, win_error_text("read failed", "", GetLastError()));
            if (got == 0)
                break; // the file shrank under us: a short read, the caller sees a truncation
            done += got;
        }
        return done;
    }

  private:
#if LOOMORA_FMT_WIN2000_SEEK
    // Windows 2000 path: seek with the high word, then read, under a lock (the file pointer is
    // shared by every call on the handle).
    bool read_chunk(std::uint64_t offset, void *buf, DWORD want, DWORD &got) const {
        struct Guard {
            CRITICAL_SECTION *cs;
            explicit Guard(CRITICAL_SECTION *c) : cs(c) { EnterCriticalSection(cs); }
            ~Guard() { LeaveCriticalSection(cs); }
        } guard(&lock_);
        return seek64(h_, offset) && ReadFile(h_, buf, want, &got, nullptr) != 0;
    }
    mutable CRITICAL_SECTION lock_;
#else
    // The offset travels in an OVERLAPPED on this synchronous handle: no shared file position,
    // so concurrent calls do not disturb each other (the equivalent of pread).
    bool read_chunk(std::uint64_t offset, void *buf, DWORD want, DWORD &got) const {
        OVERLAPPED ov;
        ov.Internal = 0;
        ov.InternalHigh = 0;
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ov.hEvent = nullptr;
        return ReadFile(h_, buf, want, &got, &ov) != 0;
    }
#endif

    HANDLE h_;
    std::uint64_t size_;
};

std::string random_suffix() {
    static volatile LONG counter = 0;
    int local = 0;
    std::uint64_t x = static_cast<std::uint64_t>(GetTickCount()) * 6364136223846793005ull;
    x ^= static_cast<std::uint64_t>(GetCurrentProcessId()) << 20;
    x ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&local));
    x += static_cast<std::uint64_t>(InterlockedIncrement(&counter)) *
         1442695040888963407ull;
    LARGE_INTEGER qpc;
    if (QueryPerformanceCounter(&qpc))
        x ^= static_cast<std::uint64_t>(qpc.QuadPart);
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string s;
    for (int i = 0; i < 8; ++i) {
        s += digits[x % 36];
        x /= 36;
    }
    return s;
}

class WinSink : public Sink {
  public:
    WinSink(const std::string &target, const std::string &temp, const std::wstring &wtarget,
            const std::wstring &wtemp, HANDLE h)
        : target_(target), temp_(temp), wtarget_(wtarget), wtemp_(wtemp), h_(h), pos_(0),
          file_pos_(0), state_(kOpen) {}
    ~WinSink() override { abort(); }

    void write(const void *data, std::size_t len) override {
        require_open();
        if (should_fail_write(pos_, len))
            fail("write failed (injected)");
        write_at(pos_, data, len);
        pos_ += len;
    }

    std::uint64_t position() const override { return pos_; }

    std::string leftover_file() const override { return leftover_; }

    void patch_at(std::uint64_t offset, const void *data, std::size_t len) override {
        require_open();
        if (offset > pos_ || len > pos_ - offset)
            throw FormatError(LOOMORA_ERR_INTERNAL, "patch outside the bytes already written");
        write_at(offset, data, len);
    }

    void commit() override {
        require_open();
        if (should_fail_flush())
            fail("flush failed (injected)");
        if (!FlushFileBuffers(h_))
            fail(win_error_text("flush failed on", temp_, GetLastError()));
        const BOOL closed = CloseHandle(h_);
        const DWORD close_error = GetLastError();
        h_ = INVALID_HANDLE_VALUE; // the handle is gone whatever CloseHandle says
        if (!closed)
            fail(win_error_text("close failed on", temp_, close_error));
        if (should_fail_rename())
            fail("rename failed (injected)");
        const DWORD error = replace_target();
        if (error != ERROR_SUCCESS)
            fail(win_error_text("rename failed to", target_, error));
        state_ = kCommitted;
    }

    void abort() noexcept override {
        if (state_ != kOpen)
            return;
        state_ = kAborted;
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (DeleteFileW(wtemp_.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
                break;
            Sleep(20);
        }
    }

  private:
    enum State { kOpen, kCommitted, kAborted };

    void require_open() const {
        if (state_ != kOpen)
            throw FormatError(LOOMORA_ERR_INTERNAL, "the sink is already committed or aborted");
    }

    // Aborts (deletes the temporary file) and throws LOOMORA_ERR_IO: the target is untouched.
    void fail(const std::string &message) {
        abort();
        throw FormatError(LOOMORA_ERR_IO, message);
    }

    // Replaces the target with the closed temporary file; returns 0 or the Windows error.
    //
    // 1. MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH): atomic, exists on Windows
    //    2000. Measured (spike S-F3, Windows 11 NTFS): it fails with ERROR_ACCESS_DENIED when
    //    another handle is open on the target, even one opened with FILE_SHARE_DELETE, which is
    //    exactly what a use-mode reader is. Then, when the target is not read-only:
    // 2. a rename with POSIX semantics (SetFileInformationByHandle, FileRenameInfoEx; atomic;
    //    found at run time, Windows 10 1809 and later on NTFS);
    // 3. the target is renamed aside, the temporary file renamed to the target, the old file
    //    deleted (every system; a short window where the target has no file, closed again by
    //    renaming the old file back when the second step fails). ReplaceFile is not used.
    // A virus scanner may hold a file for a moment: a failure is retried a few times. The tests skip
    // step 2 (should_skip_posix_rename) to run step 3 on a system that has step 2.
    DWORD replace_target() {
        DWORD error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (MoveFileExW(wtemp_.c_str(), wtarget_.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return ERROR_SUCCESS;
            error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
                return error;
            const DWORD attributes = GetFileAttributesW(wtarget_.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) != 0 ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return error; // nothing to take over, or a file the user protected
            if ((!should_skip_posix_rename() && posix_rename()) || rename_aside())
                return ERROR_SUCCESS;
            Sleep(20);
        }
        return error;
    }

    bool posix_rename() {
        typedef BOOL(WINAPI * SetInfoFn)(HANDLE, int, LPVOID, DWORD);
        const HMODULE kernel = GetModuleHandleA("kernel32.dll");
        const FARPROC proc = kernel != nullptr ? GetProcAddress(kernel, "SetFileInformationByHandle") : nullptr;
        if (proc == nullptr)
            return false;
        // Through a generic function pointer type: GCC's -Wcast-function-type accepts void (*)().
        SetInfoFn set_info = reinterpret_cast<SetInfoFn>(reinterpret_cast<void (*)()>(proc));
        std::wstring full(wtarget_.size() + 300, L'\0');
        DWORD got = GetFullPathNameW(wtarget_.c_str(), static_cast<DWORD>(full.size()), &full[0], nullptr);
        if (got == 0 || got >= full.size())
            return false;
        full.resize(got);
        HANDLE h = CreateFileW(wtemp_.c_str(), DELETE | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        // FILE_RENAME_INFO_EX, declared here: the headers of the Windows 2000 floor lack it.
        struct RenameInfoEx {
            DWORD flags;
            HANDLE root;
            DWORD length; // bytes of the name
            WCHAR name[1];
        };
        const DWORD kReplaceIfExists = 0x1;
        const DWORD kPosixSemantics = 0x2;
        const int kFileRenameInfoEx = 22;
        const std::size_t header = offsetof(RenameInfoEx, name);
        std::vector<char> buffer(header + full.size() * sizeof(WCHAR) + sizeof(WCHAR), 0);
        RenameInfoEx *info = reinterpret_cast<RenameInfoEx *>(&buffer[0]);
        info->flags = kReplaceIfExists | kPosixSemantics;
        info->root = nullptr;
        info->length = static_cast<DWORD>(full.size() * sizeof(WCHAR));
        std::memcpy(buffer.data() + header, full.c_str(), full.size() * sizeof(WCHAR));
        const BOOL ok = set_info(h, kFileRenameInfoEx, info, static_cast<DWORD>(buffer.size()));
        CloseHandle(h);
        return ok != 0;
    }

    bool rename_aside() {
        const std::wstring aside = wtarget_ + L".old-" + std::wstring(suffix_.begin(), suffix_.end());
        if (!MoveFileExW(wtarget_.c_str(), aside.c_str(), 0))
            return false;
        if (!MoveFileExW(wtemp_.c_str(), wtarget_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            MoveFileExW(aside.c_str(), wtarget_.c_str(), 0); // put the old file back
            return false;
        }
        // The reader that holds the old file open (FILE_SHARE_DELETE) keeps reading it; the name
        // disappears now or when the last handle closes. A delete that keeps failing (an antivirus
        // scanning the file, a reader that did not share delete) must not stay unnoticed: the file
        // is named by leftover_file() and the save reports it (FR-028). The save itself succeeded.
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (!should_fail_delete_aside() &&
                (DeleteFileW(aside.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND))
                return true;
            Sleep(20);
        }
        leftover_ = target_ + ".old-" + suffix_;
        return true;
    }

    void write_at(std::uint64_t offset, const void *data, std::size_t len) {
        if (len == 0)
            return;
        if (file_pos_ != offset) {
            if (!seek64(h_, offset))
                fail(win_error_text("seek failed on", temp_, GetLastError()));
            file_pos_ = offset;
        }
        const char *p = static_cast<const char *>(data);
        std::size_t done = 0;
        while (done < len) {
            const DWORD want = static_cast<DWORD>(len - done < kMaxChunk ? len - done : kMaxChunk);
            DWORD wrote = 0;
            if (!WriteFile(h_, p + done, want, &wrote, nullptr) || wrote == 0)
                fail(win_error_text("write failed on", temp_, GetLastError()));
            done += wrote;
            file_pos_ += wrote;
        }
    }

    std::string target_;
    std::string temp_;
    std::wstring wtarget_;
    std::wstring wtemp_;
    std::string suffix_ = random_suffix(); // names the aside file of rename_aside()
    std::string leftover_; // the aside file that rename_aside() could not delete ("" when none)
    HANDLE h_;
    std::uint64_t pos_;
    std::uint64_t file_pos_; // where the file pointer of h_ is
    State state_;
};

} // namespace

std::unique_ptr<Source> open_file_source(const std::string &utf8_path) {
    const std::wstring wpath = to_native(utf8_path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw FormatError(LOOMORA_ERR_IO, win_error_text("cannot open", utf8_path, GetLastError()));
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        throw FormatError(LOOMORA_ERR_IO, "'" + utf8_path + "' is not a regular file");
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#if LOOMORA_FMT_NO_LARGE_FILES
    if (size > 0x7FFFFFFFull) {
        CloseHandle(h);
        throw FormatError(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM,
                          "'" + utf8_path + "' is over 2 GiB, which this platform cannot address");
    }
#endif
    try {
        return std::unique_ptr<Source>(new WinSource(h, size));
    } catch (...) {
        CloseHandle(h);
        throw;
    }
}

std::unique_ptr<Sink> create_file_sink(const std::string &utf8_target_path) {
    const std::wstring wpath = to_native(utf8_target_path);
    // A symbolic link is followed: the real file is replaced, in its own directory, and the link stays.
    const std::wstring wtarget = follow_link(wpath);
    const bool linked = wtarget != wpath;
    // CREATE_NEW: never open a temporary file another process (or a stale run) already made.
    // No sharing: nobody else reads or deletes it while it is written.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string suffix = random_suffix();
        const std::string temp = utf8_target_path + ".tmp-" + suffix;
        // Beside the real file when a link was followed (its path is already in the long form).
        const std::wstring wtemp =
            linked ? wtarget + L".tmp-" + std::wstring(suffix.begin(), suffix.end()) : to_native(temp);
        HANDLE h = CreateFileW(wtemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            throw FormatError(LOOMORA_ERR_IO, win_error_text("cannot create", temp, error));
        }
        try {
            return std::unique_ptr<Sink>(new WinSink(utf8_target_path, temp, wtarget, wtemp, h));
        } catch (...) {
            CloseHandle(h);
            DeleteFileW(wtemp.c_str());
            throw;
        }
    }
    throw FormatError(LOOMORA_ERR_IO,
                      "cannot find an unused temporary file name for '" + utf8_target_path + "'");
}

} // namespace format
} // namespace loomora

#endif // _WIN32

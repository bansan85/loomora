// The file source and the file sink of format/io/file.hpp (file_posix.cpp, file_win32.cpp):
// atomic save (temporary file in the same directory, then replace), failure injected at every
// write, flush and rename step (SC-008), a reader opened before the replace, a read-only file,
// non-ASCII paths (accents, emoji), offsets above 4 GiB. The helpers below use the platform API
// directly (not the code under test) to create directories, list them and read files back.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "../../include/loomora.h"
#include "format/io/file.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"

using namespace loomora::format;

namespace {

typedef std::vector<std::uint8_t> Bytes;

template <class F> int code_thrown_by(F fn) {
    try {
        fn();
    } catch (...) {
        return code_of_current_exception();
    }
    return 0;
}

// "é" (U+00E9), "à" (U+00E0), a CJK character (U+65E5) and an emoji outside the BMP (U+1F600).
const char kAccents[] = "\xC3\xA9t\xC3\xA9-\xC3\xA0-\xE6\x97\xA5";
const char kEmoji[] = "\xF0\x9F\x98\x80";

#ifdef _WIN32

std::wstring wide(const std::string &utf8) {
    if (utf8.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

std::string narrow(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

const char kSep = '\\';

bool make_dir(const std::string &path) { return CreateDirectoryW(wide(path).c_str(), nullptr) != 0; }
bool remove_dir(const std::string &path) { return RemoveDirectoryW(wide(path).c_str()) != 0; }

bool remove_file(const std::string &path) {
    const std::wstring w = wide(path);
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(w.c_str()) != 0;
}

bool set_read_only(const std::string &path, bool read_only) {
    return SetFileAttributesW(wide(path).c_str(),
                              read_only ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0;
}

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(wide(dir + "\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do {
        const std::string name = narrow(data.cFileName);
        if (name != "." && name != "..")
            names.push_back(name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return names;
}

unsigned long process_id() { return GetCurrentProcessId(); }

// A symbolic link `link` whose text is `text` (relative to the link directory); false when the
// system or the account cannot create one (Windows 2000 to XP have none; Vista to 10 need a privilege
// or the developer mode). Called through GetProcAddress, as the library does for what old systems lack.
bool make_symlink(const std::string &text, const std::string &link) {
    typedef BOOLEAN(WINAPI * CreateSymbolicLinkWFn)(LPCWSTR, LPCWSTR, DWORD);
    const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const CreateSymbolicLinkWFn fn =
        k32 == nullptr ? nullptr : reinterpret_cast<CreateSymbolicLinkWFn>(
                                       reinterpret_cast<void *>(GetProcAddress(k32, "CreateSymbolicLinkW")));
    if (fn == nullptr)
        return false;
    // 0x2: SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (developer mode).
    return fn(wide(link).c_str(), wide(text).c_str(), 0x2) != 0 || fn(wide(link).c_str(), wide(text).c_str(), 0) != 0;
}

bool is_symlink(const std::string &path) {
    const DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// A sparse file of `size` bytes with `marker` at its end; false when the file system cannot.
bool make_sparse_file(const std::string &path, std::uint64_t size, const char *marker, std::size_t len) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD ret = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr); // best effort
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(size - len);
    DWORD written = 0;
    const bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0 &&
                    WriteFile(h, marker, static_cast<DWORD>(len), &written, nullptr) != 0 && written == len;
    CloseHandle(h);
    return ok;
}

#else

const char kSep = '/';

bool make_dir(const std::string &path) { return mkdir(path.c_str(), 0777) == 0; }
bool remove_dir(const std::string &path) { return rmdir(path.c_str()) == 0; }
bool remove_file(const std::string &path) {
    chmod(path.c_str(), 0644);
    return unlink(path.c_str()) == 0;
}
bool set_read_only(const std::string &path, bool read_only) {
    return chmod(path.c_str(), read_only ? 0444 : 0644) == 0;
}

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
    DIR *d = opendir(dir.c_str());
    if (d == nullptr)
        return names;
    while (struct dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..")
            names.push_back(name);
    }
    closedir(d);
    return names;
}

unsigned long process_id() { return static_cast<unsigned long>(getpid()); }

// A symbolic link `link` whose text is `text` (relative to the link directory, or absolute).
bool make_symlink(const std::string &text, const std::string &link) { return symlink(text.c_str(), link.c_str()) == 0; }

bool is_symlink(const std::string &path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

bool make_sparse_file(const std::string &path, std::uint64_t size, const char *marker, std::size_t len) {
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = sizeof(off_t) >= 8 && ftruncate(fd, static_cast<off_t>(size)) == 0 &&
              pwrite(fd, marker, len, static_cast<off_t>(size - len)) == static_cast<ssize_t>(len);
    close(fd);
    return ok;
}

#endif

// Whole-file helpers on the C library: UTF-8 paths on POSIX, wide paths on Windows.
FILE *open_c(const std::string &path, const char *mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool write_file(const std::string &path, const Bytes &bytes) {
    FILE *f = open_c(path, "wb");
    if (f == nullptr)
        return false;
    const bool ok = bytes.empty() || std::fwrite(&bytes[0], 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

bool read_file(const std::string &path, Bytes &out) {
    FILE *f = open_c(path, "rb");
    if (f == nullptr)
        return false;
    out.clear();
    unsigned char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool exists(const std::string &path) {
    FILE *f = open_c(path, "rb");
    if (f == nullptr)
        return false;
    std::fclose(f);
    return true;
}

// Deterministic content: `n` bytes derived from `seed`.
Bytes make_bytes(unsigned seed, std::size_t n) {
    Bytes b(n);
    std::uint32_t x = 2463534242u + seed;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = static_cast<std::uint8_t>(x >> 8);
    }
    return b;
}

// A scratch directory under the working directory (the build directory under ctest), removed with
// what it contains when the test ends.
class ScratchDir {
  public:
    ScratchDir(const std::string &leaf = "scratch") {
        root_ = "loomora-fileio-test-" + std::to_string(process_id());
        make_dir(root_);
        path_ = root_ + kSep + leaf;
        REQUIRE(make_dir(path_));
    }
    ~ScratchDir() {
        const std::vector<std::string> names = list_dir(path_);
        for (std::size_t i = 0; i < names.size(); ++i)
            remove_file(path_ + kSep + names[i]);
        remove_dir(path_);
        remove_dir(root_);
    }
    const std::string &path() const { return path_; }
    std::string file(const std::string &name) const { return path_ + kSep + name; }

  private:
    std::string root_;
    std::string path_;
};

// A save of several chunks, a patch of the first four bytes and a commit, as the ZIP writer does.
// `fail` receives the sink so a test may keep it; the sink is destroyed (aborted) on an exception.
void save_file(const std::string &target, unsigned seed) {
    std::unique_ptr<Sink> sink = create_file_sink(target);
    const Bytes a = make_bytes(seed, 1000);
    const Bytes b = make_bytes(seed + 1, 5000);
    const Bytes c = make_bytes(seed + 2, 1);
    sink->write(&a[0], a.size());
    sink->write(&b[0], b.size());
    sink->write(&c[0], c.size());
    sink->patch_at(0, "LOOM", 4);
    sink->commit();
}

Bytes saved_bytes(unsigned seed) {
    Bytes all = make_bytes(seed, 1000);
    const Bytes b = make_bytes(seed + 1, 5000);
    const Bytes c = make_bytes(seed + 2, 1);
    all.insert(all.end(), b.begin(), b.end());
    all.insert(all.end(), c.begin(), c.end());
    std::memcpy(&all[0], "LOOM", 4);
    return all;
}

Bytes read_all(const Source &src) {
    Bytes out(static_cast<std::size_t>(src.size()));
    if (!out.empty()) {
        REQUIRE(read_exact(src, 0, &out[0], out.size()));
    }
    return out;
}

// Counts the steps of a save and fails the `fail_at`-th (1-based, 0 = none) of one kind.
struct StepCounter {
    int writes;
    int flushes;
    int renames;
    int fail_write_at;
    int fail_flush_at;
    int fail_rename_at;
    StepCounter() : writes(0), flushes(0), renames(0), fail_write_at(0), fail_flush_at(0), fail_rename_at(0) {}
};

bool on_write(void *user, std::uint64_t, std::size_t) {
    StepCounter *c = static_cast<StepCounter *>(user);
    return ++c->writes == c->fail_write_at;
}
bool on_flush(void *user) {
    StepCounter *c = static_cast<StepCounter *>(user);
    return ++c->flushes == c->fail_flush_at;
}
bool on_rename(void *user) {
    StepCounter *c = static_cast<StepCounter *>(user);
    return ++c->renames == c->fail_rename_at;
}

void install(StepCounter &c) {
    Hooks &h = hooks();
    h.user = &c;
    h.fail_write = on_write;
    h.fail_flush = on_flush;
    h.fail_rename = on_rename;
}

// The only entry of `dir` is `name` (no temporary file left), or the directory is empty when
// `name` is empty.
bool only_entry_is(const ScratchDir &dir, const std::string &name) {
    const std::vector<std::string> names = list_dir(dir.path());
    if (name.empty())
        return names.empty();
    return names.size() == 1 && names[0] == name;
}

} // namespace

TEST_CASE("file sink writes a temporary file beside the target, commit publishes it", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("project.loomora");

    std::unique_ptr<Sink> sink = create_file_sink(target);
    REQUIRE(sink->position() == 0u);
    sink->write("0123456789", 10);
    sink->write("abc", 3);
    REQUIRE(sink->position() == 13u);
    sink->patch_at(2, "XY", 2);
    REQUIRE(sink->position() == 13u);

    // Before commit: the target does not exist, one temporary file `<target>.tmp-<random>` does.
    REQUIRE_FALSE(exists(target));
    const std::vector<std::string> during = list_dir(dir.path());
    REQUIRE(during.size() == 1u);
    const std::string prefix = "project.loomora.tmp-";
    REQUIRE(during[0].size() > prefix.size());
    REQUIRE(during[0].compare(0, prefix.size(), prefix) == 0);

    sink->commit();
    REQUIRE(only_entry_is(dir, "project.loomora"));
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(std::string(got.begin(), got.end()) == "01XY456789abc");
}

TEST_CASE("file sink replaces an existing target only at commit", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("project.loomora");
    const Bytes old_bytes = make_bytes(1, 777);
    REQUIRE(write_file(target, old_bytes));

    std::unique_ptr<Sink> sink = create_file_sink(target);
    sink->write("new", 3);
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == old_bytes); // untouched while the sink is open
    sink->commit();
    REQUIRE(read_file(target, got));
    REQUIRE(std::string(got.begin(), got.end()) == "new");
    REQUIRE(only_entry_is(dir, "project.loomora"));
}

TEST_CASE("file sink save of several chunks and a patch", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    save_file(target, 7);
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == saved_bytes(7));
    REQUIRE(only_entry_is(dir, "p.loomora"));
}

TEST_CASE("file sink abort and destruction remove the temporary file", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    const Bytes old_bytes = make_bytes(2, 300);
    REQUIRE(write_file(target, old_bytes));

    {
        std::unique_ptr<Sink> sink = create_file_sink(target);
        sink->write("abc", 3);
        REQUIRE(list_dir(dir.path()).size() == 2u);
        sink->abort();
        sink->abort(); // idempotent
        REQUIRE(only_entry_is(dir, "p.loomora"));
        REQUIRE(code_thrown_by([&] { sink->write("x", 1); }) == LOOMORA_ERR_INTERNAL);
        REQUIRE(code_thrown_by([&] { sink->patch_at(0, "x", 1); }) == LOOMORA_ERR_INTERNAL);
        REQUIRE(code_thrown_by([&] { sink->commit(); }) == LOOMORA_ERR_INTERNAL);
    }
    {
        std::unique_ptr<Sink> sink = create_file_sink(target);
        sink->write("abc", 3);
        REQUIRE(list_dir(dir.path()).size() == 2u);
    } // destroyed without commit: aborts
    REQUIRE(only_entry_is(dir, "p.loomora"));
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == old_bytes);
}

TEST_CASE("file sink misuse after commit and patch outside the written range", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    std::unique_ptr<Sink> sink = create_file_sink(target);
    sink->write("0123456789", 10);
    REQUIRE(code_thrown_by([&] { sink->patch_at(9, "AB", 2); }) == LOOMORA_ERR_INTERNAL);
    REQUIRE(code_thrown_by([&] { sink->patch_at(11, "A", 1); }) == LOOMORA_ERR_INTERNAL);
    sink->patch_at(10, "", 0); // an empty patch at the end is in range
    sink->commit();
    REQUIRE(code_thrown_by([&] { sink->write("x", 1); }) == LOOMORA_ERR_INTERNAL);
    REQUIRE(code_thrown_by([&] { sink->commit(); }) == LOOMORA_ERR_INTERNAL);
    sink->abort(); // after commit: nothing to discard, the target stays
    REQUIRE(exists(target));
}

TEST_CASE("file sink: creation fails with LOOMORA_ERR_IO when the directory is missing", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("missing-dir") + kSep + "p.loomora";
    REQUIRE(code_thrown_by([&] { create_file_sink(target); }) == LOOMORA_ERR_IO);
    REQUIRE(code_thrown_by([&] { create_file_sink(""); }) == LOOMORA_ERR_IO);
}

TEST_CASE("a failure at every write, flush and rename step keeps the old file", "[format][io][file][sc008]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    const Bytes old_bytes = make_bytes(100, 4321);

    // First a clean run to count the steps.
    StepCounter counted;
    {
        REQUIRE(write_file(target, old_bytes));
        loomora::format::ScopedHooks scope;
        install(counted);
        save_file(target, 1);
    }
    REQUIRE(counted.writes == 3);
    REQUIRE(counted.flushes >= 1);
    REQUIRE(counted.renames >= 1);
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == saved_bytes(1));

    struct Kind {
        const char *name;
        int steps;
    };
    const Kind kinds[] = {{"write", counted.writes}, {"flush", counted.flushes}, {"rename", counted.renames}};

    // With an existing target and then without one.
    for (int existing = 1; existing >= 0; --existing) {
        for (std::size_t k = 0; k < 3; ++k) {
            for (int step = 1; step <= kinds[k].steps; ++step) {
                INFO(kinds[k].name << " step " << step << (existing ? " (existing target)" : " (new target)"));
                remove_file(target);
                if (existing)
                    REQUIRE(write_file(target, old_bytes));
                StepCounter c;
                if (k == 0)
                    c.fail_write_at = step;
                else if (k == 1)
                    c.fail_flush_at = step;
                else
                    c.fail_rename_at = step;
                int code;
                {
                    loomora::format::ScopedHooks scope;
                    install(c);
                    code = code_thrown_by([&] { save_file(target, 2); });
                }
                REQUIRE(code == LOOMORA_ERR_IO);
                if (existing) {
                    REQUIRE(read_file(target, got));
                    REQUIRE(got == old_bytes); // byte-identical
                    REQUIRE(only_entry_is(dir, "p.loomora"));
                } else {
                    REQUIRE_FALSE(exists(target));
                    REQUIRE(only_entry_is(dir, ""));
                }
            }
        }
    }

    // And with the hooks off the same save works again (no state leaked by the failures).
    remove_file(target);
    save_file(target, 3);
    REQUIRE(read_file(target, got));
    REQUIRE(got == saved_bytes(3));
    REQUIRE(only_entry_is(dir, "p.loomora"));
}

TEST_CASE("a failed step aborts the sink: the temporary file is gone before destruction", "[format][io][file][sc008]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    StepCounter c;
    c.fail_rename_at = 1;
    loomora::format::ScopedHooks scope;
    install(c);
    std::unique_ptr<Sink> sink = create_file_sink(target);
    sink->write("abc", 3);
    REQUIRE(code_thrown_by([&] { sink->commit(); }) == LOOMORA_ERR_IO);
    REQUIRE(only_entry_is(dir, "")); // the sink is still alive
    REQUIRE(code_thrown_by([&] { sink->write("x", 1); }) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("a reader opened before the replace reads the complete old file", "[format][io][file][sc008]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    const Bytes old_bytes = make_bytes(5, 70000);
    REQUIRE(write_file(target, old_bytes));

    std::unique_ptr<Source> reader = open_file_source(target);
    REQUIRE(reader->size() == old_bytes.size());
    Bytes head(100);
    REQUIRE(read_exact(*reader, 0, &head[0], head.size())); // partly read before the save

    save_file(target, 6); // replaces the target while the reader holds the old file open

    Bytes got = read_all(*reader);
    REQUIRE(got == old_bytes);
    REQUIRE(reader->size() == old_bytes.size());

    std::unique_ptr<Source> fresh = open_file_source(target);
    REQUIRE(read_all(*fresh) == saved_bytes(6));
    reader.reset();
    fresh.reset();
    REQUIRE(only_entry_is(dir, "p.loomora"));
}

TEST_CASE("opening a file for reading creates no temporary file and writes nothing", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    const Bytes bytes = make_bytes(8, 1234);
    REQUIRE(write_file(target, bytes));
    {
        std::unique_ptr<Source> src = open_file_source(target);
        REQUIRE(only_entry_is(dir, "p.loomora"));
        REQUIRE(read_all(*src) == bytes);
    }
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == bytes);
}

TEST_CASE("a read-only file opens", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("ro.loomora");
    const Bytes bytes = make_bytes(9, 2048);
    REQUIRE(write_file(target, bytes));
    REQUIRE(set_read_only(target, true));
    {
        std::unique_ptr<Source> src = open_file_source(target);
        REQUIRE(read_all(*src) == bytes);
    }
    REQUIRE(set_read_only(target, false));
}

TEST_CASE("file source read_at: short at the end, nothing past it, empty file", "[format][io][file]") {
    ScratchDir dir;
    const std::string target = dir.file("p.loomora");
    const Bytes bytes = make_bytes(10, 10);
    REQUIRE(write_file(target, bytes));
    std::unique_ptr<Source> src = open_file_source(target);
    REQUIRE(src->size() == 10u);
    std::uint8_t buf[8] = {0};
    REQUIRE(src->read_at(3, buf, 4) == 4u);
    REQUIRE(buf[0] == bytes[3]);
    REQUIRE(buf[3] == bytes[6]);
    REQUIRE(src->read_at(7, buf, 8) == 3u);
    REQUIRE(buf[2] == bytes[9]);
    REQUIRE(src->read_at(10, buf, 8) == 0u);
    REQUIRE(src->read_at(1000, buf, 8) == 0u);
    REQUIRE(src->read_at(0, buf, 0) == 0u);
    REQUIRE(src->read_at(0xFFFFFFFFFFFFFFFFull, buf, 8) == 0u);
    REQUIRE_FALSE(read_exact(*src, 5, buf, 8));
    REQUIRE(read_exact(*src, 2, buf, 8));
    src.reset();

    const std::string empty = dir.file("empty.loomora");
    REQUIRE(write_file(empty, Bytes()));
    std::unique_ptr<Source> e = open_file_source(empty);
    REQUIRE(e->size() == 0u);
    REQUIRE(e->read_at(0, buf, 8) == 0u);
}

TEST_CASE("file source: a missing file or a directory is LOOMORA_ERR_IO", "[format][io][file]") {
    ScratchDir dir;
    REQUIRE(code_thrown_by([&] { open_file_source(dir.file("absent.loomora")); }) == LOOMORA_ERR_IO);
    REQUIRE(code_thrown_by([&] { open_file_source(dir.path()); }) == LOOMORA_ERR_IO);
    REQUIRE(code_thrown_by([&] { open_file_source(""); }) == LOOMORA_ERR_IO);
    REQUIRE(only_entry_is(dir, ""));
}

TEST_CASE("file source and sink with a non-ASCII path (accents, emoji)", "[format][io][file][unicode]") {
    // The directory and the file names both carry accents, a CJK character and an emoji.
    const std::string leaf = std::string("dir-") + kAccents + "-" + kEmoji;
    ScratchDir dir(leaf);
    const std::string name = std::string("projet-") + kAccents + "-" + kEmoji + ".loomora";
    const std::string target = dir.file(name);

    save_file(target, 11);
    REQUIRE(only_entry_is(dir, name));
    Bytes got;
    REQUIRE(read_file(target, got));
    REQUIRE(got == saved_bytes(11));

    std::unique_ptr<Source> src = open_file_source(target);
    REQUIRE(read_all(*src) == saved_bytes(11));

    // Replace it, with a reader open on the old one, and fail a save: the names stay intact.
    save_file(target, 12);
    REQUIRE(read_all(*src) == saved_bytes(11));
    {
        StepCounter c;
        c.fail_rename_at = 1;
        loomora::format::ScopedHooks scope;
        install(c);
        REQUIRE(code_thrown_by([&] { save_file(target, 13); }) == LOOMORA_ERR_IO);
    }
    src.reset();
    REQUIRE(only_entry_is(dir, name));
    REQUIRE(read_file(target, got));
    REQUIRE(got == saved_bytes(12));
}

// FR-028: a save through a symbolic link replaces the file the link points to (in that file's own
// directory) and the link stays a link. `links` holds the link, `real` the file; the link texts are
// relative to the link directory.
namespace {

std::string up_to(const std::string &leaf, const std::string &name) {
    return std::string("..") + kSep + leaf + kSep + name;
}

} // namespace

TEST_CASE("file sink through a symbolic link replaces the target and keeps the link", "[format][io][file][symlink]") {
    ScratchDir link_dir("links");
    ScratchDir real_dir("real");
    const std::string real = real_dir.file("p.loomora");
    const std::string link = link_dir.file("p.loomora");
    const Bytes old_bytes = make_bytes(200, 999);
    REQUIRE(write_file(real, old_bytes));
    if (!make_symlink(up_to("real", "p.loomora"), link)) {
        WARN("this system or account cannot create a symbolic link: skipped");
        return;
    }

    save_file(link, 21);

    REQUIRE(is_symlink(link)); // still a link, not a regular file
    REQUIRE_FALSE(is_symlink(real));
    Bytes got;
    REQUIRE(read_file(real, got)); // the file it points to has the new content
    REQUIRE(got == saved_bytes(21));
    REQUIRE(read_file(link, got));
    REQUIRE(got == saved_bytes(21));
    REQUIRE(only_entry_is(link_dir, "p.loomora")); // no temporary file in either directory
    REQUIRE(only_entry_is(real_dir, "p.loomora"));
}

TEST_CASE("file sink through a link to a missing file creates that file", "[format][io][file][symlink]") {
    ScratchDir link_dir("links");
    ScratchDir real_dir("real");
    const std::string link = link_dir.file("p.loomora");
    if (!make_symlink(up_to("real", "new.loomora"), link)) {
        WARN("this system or account cannot create a symbolic link: skipped");
        return;
    }

    save_file(link, 22);

    REQUIRE(is_symlink(link));
    Bytes got;
    REQUIRE(read_file(real_dir.file("new.loomora"), got));
    REQUIRE(got == saved_bytes(22));
    REQUIRE(only_entry_is(link_dir, "p.loomora"));
    REQUIRE(only_entry_is(real_dir, "new.loomora"));
}

TEST_CASE("file sink follows a chain of links to the last file", "[format][io][file][symlink]") {
    ScratchDir link_dir("links");
    ScratchDir real_dir("real");
    const std::string real = real_dir.file("p.loomora");
    REQUIRE(write_file(real, make_bytes(201, 500)));
    // first -> second (same directory) -> real (other directory)
    if (!make_symlink(up_to("real", "p.loomora"), link_dir.file("second"))) {
        WARN("this system or account cannot create a symbolic link: skipped");
        return;
    }
    REQUIRE(make_symlink("second", link_dir.file("first")));

    save_file(link_dir.file("first"), 23);

    REQUIRE(is_symlink(link_dir.file("first")));
    REQUIRE(is_symlink(link_dir.file("second")));
    Bytes got;
    REQUIRE(read_file(real, got));
    REQUIRE(got == saved_bytes(23));
    REQUIRE(list_dir(link_dir.path()).size() == 2u);
    REQUIRE(only_entry_is(real_dir, "p.loomora"));
}

TEST_CASE("a failure while saving through a link leaves the old file and the link intact",
          "[format][io][file][symlink][sc008]") {
    ScratchDir link_dir("links");
    ScratchDir real_dir("real");
    const std::string real = real_dir.file("p.loomora");
    const std::string link = link_dir.file("p.loomora");
    const Bytes old_bytes = make_bytes(202, 1234);
    REQUIRE(write_file(real, old_bytes));
    if (!make_symlink(up_to("real", "p.loomora"), link)) {
        WARN("this system or account cannot create a symbolic link: skipped");
        return;
    }

    for (int k = 0; k < 3; ++k) {
        INFO((k == 0 ? "write fails" : k == 1 ? "flush fails" : "rename fails"));
        StepCounter c;
        if (k == 0)
            c.fail_write_at = 2;
        else if (k == 1)
            c.fail_flush_at = 1;
        else
            c.fail_rename_at = 1;
        int code;
        {
            loomora::format::ScopedHooks scope;
            install(c);
            code = code_thrown_by([&] { save_file(link, 24); });
        }
        REQUIRE(code == LOOMORA_ERR_IO);
        REQUIRE(is_symlink(link));
        Bytes got;
        REQUIRE(read_file(real, got));
        REQUIRE(got == old_bytes);
        REQUIRE(only_entry_is(link_dir, "p.loomora"));
        REQUIRE(only_entry_is(real_dir, "p.loomora"));
    }
}

#ifndef _WIN32
TEST_CASE("file sink through an absolute link, a link to a directory and a loop of links",
          "[format][io][file][symlink]") {
    ScratchDir link_dir("links");
    ScratchDir real_dir("real");
    char cwd[4096];
    REQUIRE(getcwd(cwd, sizeof cwd) != nullptr);
    const std::string base = std::string(cwd) + "/";

    // An absolute link text, and a link in the middle of the path (a link to a directory).
    REQUIRE(write_file(real_dir.file("p.loomora"), make_bytes(203, 100)));
    REQUIRE(make_symlink(base + real_dir.file("p.loomora"), link_dir.file("abs")));
    save_file(link_dir.file("abs"), 25);
    REQUIRE(is_symlink(link_dir.file("abs")));
    Bytes got;
    REQUIRE(read_file(real_dir.file("p.loomora"), got));
    REQUIRE(got == saved_bytes(25));

    REQUIRE(make_symlink(base + real_dir.path(), link_dir.file("dir")));
    save_file(link_dir.file("dir") + "/p.loomora", 26);
    REQUIRE_FALSE(is_symlink(real_dir.file("p.loomora")));
    REQUIRE(read_file(real_dir.file("p.loomora"), got));
    REQUIRE(got == saved_bytes(26));
    REQUIRE(only_entry_is(real_dir, "p.loomora"));

    // A loop of links: LOOMORA_ERR_IO, nothing created.
    REQUIRE(make_symlink("loop-b", link_dir.file("loop-a")));
    REQUIRE(make_symlink("loop-a", link_dir.file("loop-b")));
    REQUIRE(code_thrown_by([&] { create_file_sink(link_dir.file("loop-a")); }) == LOOMORA_ERR_IO);
    REQUIRE(list_dir(link_dir.path()).size() == 4u); // abs, dir, loop-a, loop-b
    REQUIRE(only_entry_is(real_dir, "p.loomora"));
}
#endif

TEST_CASE("file source addresses offsets above 4 GiB", "[format][io][file][large]") {
    ScratchDir dir;
    const std::string target = dir.file("sparse.bin");
    const std::uint64_t size = (5ull << 30) + 16; // 5 GiB and 16 bytes
    const char marker[] = "LOOMORA!";
    if (!make_sparse_file(target, size, marker, 8)) {
        WARN("the file system of the test directory cannot make a sparse file above 4 GiB: skipped");
        remove_file(target);
        return;
    }
    std::unique_ptr<Source> src = open_file_source(target);
    REQUIRE(src->size() == size);
    char buf[8] = {0};
    REQUIRE(src->read_at(size - 8, buf, 8) == 8u);
    REQUIRE(std::memcmp(buf, marker, 8) == 0);
    REQUIRE(src->read_at(5ull << 30, buf, 8) == 8u); // inside the hole: zeros
    REQUIRE(buf[0] == 0);
    // The offsets around 2^32 (a low word of 0xFFFFFFFF is not an error) and 2^31.
    REQUIRE(src->read_at(0xFFFFFFFFull, buf, 8) == 8u);
    REQUIRE(src->read_at(0x7FFFFFFFull, buf, 8) == 8u);
    REQUIRE(src->read_at(0x100000000ull, buf, 8) == 8u);
    REQUIRE(src->read_at(size, buf, 8) == 0u);
    src.reset();
    REQUIRE(remove_file(target));
}

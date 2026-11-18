// Small file-system helpers for the tests that save to disk (ASCII paths only): a scratch directory
// under the working directory, listing, whole-file read and write. Uses the platform API directly,
// not the code under test.
#ifndef LOOMORA_TESTS_FORMAT_FS_TEST_UTIL_HPP
#define LOOMORA_TESTS_FORMAT_FS_TEST_UTIL_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fstest {

typedef std::vector<std::uint8_t> Bytes;

#ifdef _WIN32
const char kSep = '\\';
inline bool make_dir(const std::string &p) { return CreateDirectoryA(p.c_str(), nullptr) != 0; }
inline bool remove_dir(const std::string &p) { return RemoveDirectoryA(p.c_str()) != 0; }
inline bool remove_file(const std::string &p) { return DeleteFileA(p.c_str()) != 0; }
inline unsigned long process_id() { return GetCurrentProcessId(); }
inline std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE)
        return names;
    do {
        const std::string n = data.cFileName;
        if (n != "." && n != "..")
            names.push_back(n);
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return names;
}
#else
const char kSep = '/';
inline bool make_dir(const std::string &p) { return mkdir(p.c_str(), 0777) == 0; }
inline bool remove_dir(const std::string &p) { return rmdir(p.c_str()) == 0; }
inline bool remove_file(const std::string &p) { return unlink(p.c_str()) == 0; }
inline unsigned long process_id() { return static_cast<unsigned long>(getpid()); }
inline std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
    DIR *d = opendir(dir.c_str());
    if (d == nullptr)
        return names;
    while (struct dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..")
            names.push_back(n);
    }
    closedir(d);
    return names;
}
#endif

inline bool write_file(const std::string &path, const Bytes &bytes) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = bytes.empty() || std::fwrite(&bytes[0], 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

inline bool read_file(const std::string &path, Bytes &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
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

// A directory removed with its files when the object goes out of scope.
class ScratchDir {
  public:
    explicit ScratchDir(const std::string &tag) {
        root_ = "loomora-" + tag + "-test-" + std::to_string(process_id());
        make_dir(root_);
        path_ = root_ + kSep + "d";
        make_dir(path_);
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
    std::vector<std::string> names() const { return list_dir(path_); }

  private:
    ScratchDir(const ScratchDir &);
    ScratchDir &operator=(const ScratchDir &);
    std::string root_;
    std::string path_;
};

} // namespace fstest

#endif

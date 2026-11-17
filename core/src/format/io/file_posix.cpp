// The file source and the file sink on POSIX systems (Linux, macOS, Android, iOS, the BSDs).
// #if-guarded: it compiles to nothing on Windows, where file_win32.cpp defines the same factories.
//
//   source   plain open(O_RDONLY), pread with 64-bit offsets, so concurrent reads never share a
//            file position; a save that renames over the file leaves the descriptor on the old
//            inode, so a reader opened before the save keeps reading the complete old file.
//   sink     `<target>.tmp-<random>` created with O_EXCL in the target's directory, fsync
//            (F_FULLFSYNC on macOS), rename over the target, temporary file unlinked on failure,
//            abort or destruction. The hooks of testing.hpp fail a write, the flush or the rename.
#if !defined(_WIN32)

// 64-bit file offsets for off_t on the 32-bit targets (glibc, bionic from API 21); must come
// before the first system header. Android API 9 to 20 have no such mapping: they use the explicit
// *64 calls under LOOMORA_FMT_LSEEK64 (internal/config.hpp).
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "format/io/file.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"
#include "internal/config.hpp"
#include "loomora.h"
#include "loomora_project.h"

namespace loomora {
namespace format {

namespace {

#if LOOMORA_FMT_LSEEK64
typedef off64_t file_off_t;
inline ssize_t pread_at(int fd, void *buf, std::size_t len, file_off_t off) {
    return pread64(fd, buf, len, off);
}
inline ssize_t pwrite_at(int fd, const void *buf, std::size_t len, file_off_t off) {
    return pwrite64(fd, buf, len, off);
}
inline int fstat_of(int fd, struct stat64 *st) { return fstat64(fd, st); }
typedef struct stat64 StatBuf;
#else
typedef off_t file_off_t;
inline ssize_t pread_at(int fd, void *buf, std::size_t len, file_off_t off) {
    return pread(fd, buf, len, off);
}
inline ssize_t pwrite_at(int fd, const void *buf, std::size_t len, file_off_t off) {
    return pwrite(fd, buf, len, off);
}
inline int fstat_of(int fd, struct stat *st) { return fstat(fd, st); }
typedef struct stat StatBuf;
#endif

#ifdef O_CLOEXEC
const int kCloexec = O_CLOEXEC;
#else
const int kCloexec = 0;
#endif

// A source is opened without waiting: a plain open of a named pipe blocks until a writer appears,
// so a path naming a FIFO would hang before the "not a regular file" check. The flag changes
// nothing for a regular file, which is the only kind that is kept.
#ifdef O_NONBLOCK
const int kNonblock = O_NONBLOCK;
#else
const int kNonblock = 0;
#endif

// One read or write call never asks for more than this: some systems fail a transfer over
// 2 GiB with EINVAL.
const std::size_t kMaxChunk = 1u << 30;

std::string errno_text(const std::string &what, const std::string &path) {
    return what + " '" + path + "': " + std::strerror(errno);
}

class PosixSource : public Source {
  public:
    PosixSource(int fd, std::uint64_t size) : fd_(fd), size_(size) {}
    ~PosixSource() override { ::close(fd_); }

    std::uint64_t size() const override { return size_; }

    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t len) const override {
        if (len == 0 || offset >= size_)
            return 0;
        if (size_ - offset < len)
            len = static_cast<std::size_t>(size_ - offset);
        std::uint8_t *out = static_cast<std::uint8_t *>(buf);
        std::size_t done = 0;
        while (done < len) {
            const std::size_t want = len - done < kMaxChunk ? len - done : kMaxChunk;
            const ssize_t got = pread_at(fd_, out + done, want, static_cast<file_off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                throw FormatError(LOOMORA_ERR_IO, std::string("read failed: ") + std::strerror(errno));
            }
            if (got == 0)
                break; // the file shrank under us: a short read, the caller sees a truncation
            done += static_cast<std::size_t>(got);
        }
        return done;
    }

  private:
    int fd_;
    std::uint64_t size_;
};

// A name suffix that differs between processes, calls and runs: time, process id, a counter and
// the address of a local, mixed; uniqueness is guaranteed by O_EXCL, not by this value.
std::string random_suffix() {
    static std::uint32_t counter = 0;
    int local = 0;
    std::uint64_t x = static_cast<std::uint64_t>(std::time(nullptr)) * 6364136223846793005ull;
    x ^= static_cast<std::uint64_t>(::getpid()) << 20;
    x ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&local));
    x += static_cast<std::uint64_t>(++counter) * 1442695040888963407ull;
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

class PosixSink : public Sink {
  public:
    PosixSink(const std::string &target, const std::string &temp, int fd)
        : target_(target), temp_(temp), fd_(fd), pos_(0), state_(kOpen) {}
    ~PosixSink() override { abort(); }

    void write(const void *data, std::size_t len) override {
        require_open();
        if (should_fail_write(pos_, len))
            fail("write failed (injected)");
        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        std::size_t done = 0;
        while (done < len) {
            const std::size_t want = len - done < kMaxChunk ? len - done : kMaxChunk;
            const ssize_t n = pwrite_at(fd_, p + done, want, static_cast<file_off_t>(pos_ + done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fail(errno_text("write failed on", temp_));
            }
            if (n == 0)
                fail("write failed on '" + temp_ + "': no progress");
            done += static_cast<std::size_t>(n);
        }
        pos_ += len;
    }

    std::uint64_t position() const override { return pos_; }

    void patch_at(std::uint64_t offset, const void *data, std::size_t len) override {
        require_open();
        if (offset > pos_ || len > pos_ - offset)
            throw FormatError(LOOMORA_ERR_INTERNAL, "patch outside the bytes already written");
        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        std::size_t done = 0;
        while (done < len) {
            const ssize_t n = pwrite_at(fd_, p + done, len - done, static_cast<file_off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fail(errno_text("write failed on", temp_));
            }
            if (n == 0)
                fail("write failed on '" + temp_ + "': no progress");
            done += static_cast<std::size_t>(n);
        }
    }

    void commit() override {
        require_open();
        if (should_fail_flush())
            fail("flush failed (injected)");
        if (!flush_to_disk())
            fail(errno_text("flush failed on", temp_));
        copy_mode_of_target();
        if (::close(fd_) != 0) {
            fd_ = -1;
            fail(errno_text("close failed on", temp_));
        }
        fd_ = -1;
        if (should_fail_rename())
            fail("rename failed (injected)");
        if (::rename(temp_.c_str(), target_.c_str()) != 0)
            fail(errno_text("rename failed to", target_));
        state_ = kCommitted;
        sync_directory();
    }

    void abort() noexcept override {
        if (state_ != kOpen)
            return;
        state_ = kAborted;
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        ::unlink(temp_.c_str());
    }

  private:
    enum State { kOpen, kCommitted, kAborted };

    void require_open() const {
        if (state_ != kOpen)
            throw FormatError(LOOMORA_ERR_INTERNAL, "the sink is already committed or aborted");
    }

    // Aborts (removes the temporary file) and throws LOOMORA_ERR_IO: the target is untouched.
    void fail(const std::string &message) {
        abort();
        throw FormatError(LOOMORA_ERR_IO, message);
    }

    bool flush_to_disk() {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
        // fsync on macOS only reaches the drive's cache; F_FULLFSYNC asks the drive to flush it.
        // Some file systems (network shares) refuse it: fall back to fsync there.
        if (::fcntl(fd_, F_FULLFSYNC) == 0)
            return true;
#endif
        int r;
        do {
            r = ::fsync(fd_);
        } while (r != 0 && errno == EINTR);
        return r == 0;
    }

    // The replacement keeps the permission bits of the file it replaces (best effort).
    void copy_mode_of_target() {
        struct stat st;
        if (::stat(target_.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            (void)::fchmod(fd_, st.st_mode & 07777);
    }

    // Makes the rename itself durable (best effort: some file systems refuse to sync a directory).
    void sync_directory() {
        std::string dir = ".";
        const std::string::size_type slash = target_.rfind('/');
        if (slash != std::string::npos)
            dir = slash == 0 ? "/" : target_.substr(0, slash);
        const int dfd = ::open(dir.c_str(), O_RDONLY | kCloexec);
        if (dfd >= 0) {
            (void)::fsync(dfd);
            ::close(dfd);
        }
    }

    std::string target_;
    std::string temp_;
    int fd_;
    std::uint64_t pos_;
    State state_;
};

} // namespace

std::unique_ptr<Source> open_file_source(const std::string &utf8_path) {
    int fd;
    do {
        fd = ::open(utf8_path.c_str(), O_RDONLY | kCloexec | kNonblock);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        throw FormatError(LOOMORA_ERR_IO, errno_text("cannot open", utf8_path));
    StatBuf st;
    if (fstat_of(fd, &st) != 0) {
        const std::string message = errno_text("cannot stat", utf8_path);
        ::close(fd);
        throw FormatError(LOOMORA_ERR_IO, message);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        throw FormatError(LOOMORA_ERR_IO, "'" + utf8_path + "' is not a regular file");
    }
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
#if LOOMORA_FMT_NO_LARGE_FILES
    if (size > 0x7FFFFFFFull) {
        ::close(fd);
        throw FormatError(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM,
                          "'" + utf8_path + "' is over 2 GiB, which this platform cannot address");
    }
#endif
    try {
        return std::unique_ptr<Source>(new PosixSource(fd, size));
    } catch (...) {
        ::close(fd);
        throw;
    }
}

std::unique_ptr<Sink> create_file_sink(const std::string &utf8_target_path) {
    if (utf8_target_path.empty())
        throw FormatError(LOOMORA_ERR_IO, "empty target path");
    // O_EXCL: never open a temporary file another process (or a stale run) already made.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string temp = utf8_target_path + ".tmp-" + random_suffix();
        const int fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | kCloexec, 0666);
        if (fd < 0) {
            if (errno == EEXIST || errno == EINTR)
                continue;
            throw FormatError(LOOMORA_ERR_IO, errno_text("cannot create", temp));
        }
        try {
            return std::unique_ptr<Sink>(new PosixSink(utf8_target_path, temp, fd));
        } catch (...) {
            ::close(fd);
            ::unlink(temp.c_str());
            throw;
        }
    }
    throw FormatError(LOOMORA_ERR_IO, "cannot find an unused temporary file name for '" + utf8_target_path + "'");
}

} // namespace format
} // namespace loomora

#endif // !_WIN32

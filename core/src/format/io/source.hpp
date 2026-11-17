// A random-access, read-only byte source: the one way the ZIP reader (and so the whole reader)
// gets at the bytes of a project, whether they come from a file or from a caller's buffer.
// Implementations: MemorySource (memory_io.hpp), the file source of file.hpp.
#ifndef LOOMORA_FORMAT_IO_SOURCE_HPP
#define LOOMORA_FORMAT_IO_SOURCE_HPP

#include <cstddef>
#include <cstdint>

namespace loomora {
namespace format {

class Source {
  public:
    virtual ~Source() {}

    // Total size in bytes, fixed for the life of the source.
    virtual std::uint64_t size() const = 0;

    // Reads up to `len` bytes starting at `offset` into `buf` and returns how many were read:
    // `len` unless the range crosses the end (then fewer), 0 at or after the end or when len is 0.
    // Throws FormatError(LOOMORA_ERR_IO) when the underlying read fails. Thread safe for read-only
    // sources: concurrent calls do not disturb each other (a file source uses pread or its
    // equivalent, never a shared file position).
    virtual std::size_t read_at(std::uint64_t offset, void *buf, std::size_t len) const = 0;
};

// Reads exactly `len` bytes at `offset`; false when the source ends before (a truncated file),
// in which case the content of `buf` is unspecified. Throws like Source::read_at.
bool read_exact(const Source &source, std::uint64_t offset, void *buf, std::size_t len);

} // namespace format
} // namespace loomora

#endif

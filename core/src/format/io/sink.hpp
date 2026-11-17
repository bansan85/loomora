// A sequential byte sink with the one random access the ZIP writer needs: patching bytes already
// written (the sizes and CRC of a local header, known only after the entry is written; no data
// descriptors). Implementations: MemorySink and CallbackSink (memory_io.hpp), the file sink of
// file.hpp (temporary file, then atomic replace).
//
// Life cycle: write()* and patch_at()*, then exactly one of commit() (publish the result: the
// file replaces its target, the callback receives the bytes) or abort() (discard everything: the
// temporary file is removed). After commit or abort, write, patch_at and commit throw
// FormatError(LOOMORA_ERR_INTERNAL) (a misuse); abort is idempotent and never throws, so it is safe
// in a catch block and in a destructor. A sink destroyed without commit() aborts.
#ifndef LOOMORA_FORMAT_IO_SINK_HPP
#define LOOMORA_FORMAT_IO_SINK_HPP

#include <cstddef>
#include <cstdint>

namespace loomora {
namespace format {

class Sink {
  public:
    virtual ~Sink() {}

    // Appends `len` bytes. Throws FormatError(LOOMORA_ERR_IO) on a write failure (disk full...),
    // LOOMORA_ERR_NO_MEMORY when a memory buffer cannot grow.
    virtual void write(const void *data, std::size_t len) = 0;

    // Number of bytes written so far (the offset the next write lands at).
    virtual std::uint64_t position() const = 0;

    // Overwrites `len` bytes at `offset` with `data`; the range must lie within the bytes already
    // written (offset + len <= position()), otherwise FormatError(LOOMORA_ERR_INTERNAL). Does not
    // change position().
    virtual void patch_at(std::uint64_t offset, const void *data, std::size_t len) = 0;

    // Publishes the written bytes (flush to stable storage and atomic rename for a file). Throws
    // FormatError(LOOMORA_ERR_IO) on failure; the sink is then aborted, and the previous content of
    // a replaced target is untouched.
    virtual void commit() = 0;

    // Discards everything written. Idempotent, never throws.
    virtual void abort() noexcept = 0;
};

} // namespace format
} // namespace loomora

#endif

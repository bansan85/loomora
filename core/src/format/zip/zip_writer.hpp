// The deterministic ZIP writer of the .loomora format (contracts/project-format.md, section 1.1).
//
// Every header field is fixed: version made by 0x0314, version needed 20 (45 when the entry uses
// ZIP64), general flags 0 (bit 11 only for a non-ASCII name), DOS time 0x0000 and date 0x0021,
// external attributes 0x81A40000, no extra field but ZIP64, no comment, no data descriptor (the
// sink patches the CRC-32 and the sizes of the local header once the entry is written). Entries
// go in the order of the calls (the canonical order is the caller's). ZIP64 is used only when a
// size or an offset reaches limits().zip64_size_threshold / zip64_offset_threshold, or the entry
// count limits().zip64_count_threshold (the thresholds of the classic format by default).
//
// The first entry must be `mimetype` (add_mimetype): stored, offset 0, no extra field.
//
// Deflate uses zlib with raw deflate, level 6, 15-bit window, memLevel 8, default strategy, and a
// fixed output buffer, so the bytes do not depend on how the data is chunked. The ZIP64 decision of
// a deflate entry is made before its compressed size is known: it uses ZIP64 when the uncompressed
// size, or zlib's worst-case compressed size for it, reaches the size threshold.
//
// A misuse (bad name, duplicate name, no mimetype first, wrong size declared) throws
// FormatError(LOOMORA_ERR_INTERNAL); write failures of the sink propagate (LOOMORA_ERR_IO,
// LOOMORA_ERR_NO_MEMORY), cancellation through the Progress is LOOMORA_ERR_CANCELLED. After an
// exception the writer is unusable: abort the sink. The writer never commits the sink.
#ifndef LOOMORA_FORMAT_ZIP_ZIP_WRITER_HPP
#define LOOMORA_FORMAT_ZIP_ZIP_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "format/io/progress.hpp"
#include "format/io/sink.hpp"
#include "format/io/source.hpp"
#include "format/zip/zip_reader.hpp"

namespace loomora {
namespace format {

class ZipWriter {
  public:
    // `sink` (and `progress`, which may be null) must outlive the writer.
    explicit ZipWriter(Sink &sink, Progress *progress = nullptr);
    ~ZipWriter();

    // The first entry: "mimetype", stored, with the fixed content.
    void add_mimetype();

    // Whole entries from memory or from a source (streamed through a fixed buffer).
    void add_stored(const std::string &name, const void *data, std::size_t len);
    void add_stored(const std::string &name, const Source &source);
    void add_deflate(const std::string &name, const void *data, std::size_t len);
    void add_deflate(const std::string &name, const Source &source);

    // Copy-through: `size` compressed bytes read from `source` at `offset` are written as they are,
    // with the given method (0 or 8), CRC-32 and uncompressed size, nothing is recompressed.
    void add_raw(const std::string &name, std::uint16_t method, std::uint32_t crc32,
                 std::uint64_t compressed_size, std::uint64_t size, const Source &source,
                 std::uint64_t offset);
    // The same for an entry of a reader (under a name chosen by the caller, usually entry.name).
    void add_raw(const std::string &name, const ZipReader &reader, const ZipEntry &entry);

    // Streaming producer: `uncompressed_size` must be the exact total of the write() calls
    // (checked by end_entry). method is 0 (stored) or 8 (deflate).
    void begin_entry(const std::string &name, std::uint16_t method, std::uint64_t uncompressed_size);
    void write(const void *data, std::size_t len);
    void end_entry();

    // Writes the central directory and the end records. Throws if `mimetype` was not added.
    void finish();

    std::size_t entry_count() const { return count_; }

  private:
    struct Impl;
    ZipWriter(const ZipWriter &);
    ZipWriter &operator=(const ZipWriter &);
    std::unique_ptr<Impl> impl_;
    std::size_t count_;
};

} // namespace format
} // namespace loomora

#endif

// The ZIP reader of the .loomora format (contracts/project-format.md, section 1): methods 0
// (stored) and 8 (deflate) only, ZIP64 whenever present, the central directory authoritative and
// each local header checked against it, every name and layout rule of the contract, streaming
// decompression through a fixed buffer, raw access to the compressed bytes (copy-through).
//
// Every rejection is a FormatError carrying an Issue with one of these codes (the C code in
// parentheses is the LOOMORA_ERR_* of the exception):
//   zip.not_zip        no end-of-central-directory record (NOT_A_PROJECT)
//   zip.mimetype       first entry is not a stored "mimetype" at offset 0, with no extra field and
//                      the exact content, or is missing (NOT_A_PROJECT)
//   zip.comment        an archive comment is present
//   zip.multidisk      a multi-disk archive
//   zip.central        the central directory is truncated, malformed or inconsistent with the
//                      end record (counts, sizes, signatures)
//   zip.zip64          ZIP64 structures missing or inconsistent
//   zip.method         a compression method other than 0 and 8
//   zip.encrypted      an encryption flag (traditional, strong or masked headers)
//   zip.bad_name       an entry name that breaks the rules of the contract
//   zip.duplicate_name two names equal after ASCII case folding
//   zip.symlink        a Unix mode that is a symbolic link, a device, a socket or a pipe
//   zip.local_header   a local header that disagrees with the central directory
//   zip.overlap        two entries overlap
//   zip.bounds         an entry that lies (partly) outside the file or after the central directory
//   zip.inflate        a deflate stream that is not valid
//   entry.size         the data does not have the declared size (stored size mismatch, too short or
//                      too long)
//   entry.crc          the CRC-32 of the data differs from the declared one
//   entry.bomb         the ratio guard of FR-026 (LOOMORA_ERR_BOMB)
// All the others are LOOMORA_ERR_CORRUPT. Cancellation through the Progress is
// LOOMORA_ERR_CANCELLED, an allocation that fails LOOMORA_ERR_NO_MEMORY, a failed read
// LOOMORA_ERR_IO. Every allocation is sized from a value already checked against the archive size
// (the content of an entry is never allocated from its declared size).
#ifndef LOOMORA_FORMAT_ZIP_ZIP_READER_HPP
#define LOOMORA_FORMAT_ZIP_ZIP_READER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "format/io/progress.hpp"
#include "format/io/source.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {

// The first entry of every .loomora file.
extern const char kMimetypeName[];     // "mimetype"
extern const char kMimetypeContent[];  // "application/vnd.loomora+zip"
extern const std::size_t kMimetypeSize; // 27

// Checks an entry name against the rules of the contract (UTF-8, at most limits().entry_name_max_bytes
// bytes, relative, '/' separator, no empty, "." or ".." component, no leading '/', no '\', no ':'
// in the first component, no control character; one trailing '/' marks a directory). Returns true
// when the name is acceptable, otherwise false and, when `why` is not null, the reason.
bool is_valid_entry_name(const std::string &name, std::string *why = nullptr);

// The ASCII case folding used to detect duplicate names.
std::string fold_entry_name(const std::string &name);

struct ZipEntry {
    std::string name;
    std::uint16_t method;           // 0 stored, 8 deflate
    std::uint32_t crc32;            // declared CRC-32 of the uncompressed data
    std::uint64_t compressed_size;
    std::uint64_t size;             // uncompressed size
    std::uint64_t local_offset;     // offset of the local header
    std::uint64_t data_offset;      // offset of the first byte of compressed data
    bool has_descriptor;            // general flag bit 3 (data descriptor, accepted, never read)

    ZipEntry() : method(0), crc32(0), compressed_size(0), size(0), local_offset(0), data_offset(0),
                 has_descriptor(false) {}
};

class ZipReader;

// A sequential, verified read of one entry. read() returns 0 only at the end, after the size, the
// compressed length and the CRC-32 have been checked. Not copyable.
class ZipEntryStream {
  public:
    ~ZipEntryStream();

    // Up to `len` bytes of uncompressed data (fewer only at the end); 0 at the end.
    std::size_t read(void *buf, std::size_t len);
    // Uncompressed bytes delivered so far.
    std::uint64_t produced() const { return produced_; }

  private:
    friend class ZipReader;
    struct Impl;
    ZipEntryStream(const ZipReader &reader, const ZipEntry &entry, Progress *progress);
    ZipEntryStream(const ZipEntryStream &);
    ZipEntryStream &operator=(const ZipEntryStream &);

    std::unique_ptr<Impl> impl_;
    std::uint64_t produced_;
};

class ZipReader {
  public:
    // `source` must outlive the reader. Nothing is read before open().
    explicit ZipReader(const Source &source) : source_(source), cd_offset_(0), opened_(false) {}

    // Validates the whole container: end record, central directory, every entry against the
    // contract, every local header against the central directory, the layout (overlap, bounds),
    // the ratio guard and the `mimetype` entry. Throws FormatError (see the codes above).
    // `progress` may be null; it is advanced by the bytes of central and local headers read.
    void open(Progress *progress = nullptr);

    // The entries in central directory order, directory entries (a name ending with '/') left out.
    const std::vector<ZipEntry> &entries() const { return entries_; }
    // The entry of that exact name, or null.
    const ZipEntry *find(const std::string &name) const;

    std::uint64_t archive_size() const { return source_.size(); }
    std::uint64_t central_directory_offset() const { return cd_offset_; }

    // Streams the uncompressed content through a fixed buffer, verifying size and CRC-32.
    std::unique_ptr<ZipEntryStream> open_entry(const ZipEntry &entry, Progress *progress = nullptr) const;

    // The whole uncompressed content. The vector grows with the data actually produced (never
    // sized from the declared size); an entry larger than `max_bytes` throws entry.size.
    std::vector<std::uint8_t> read_all(const ZipEntry &entry, std::uint64_t max_bytes,
                                       Progress *progress = nullptr) const;

    // Compressed bytes [offset, offset + len) of the entry, clamped to its compressed size: returns
    // how many were read (0 at or after the end). Does not verify anything.
    std::size_t read_raw(const ZipEntry &entry, std::uint64_t offset, void *buf, std::size_t len) const;

    // Same on the uncompressed content of a STORED entry (throws LOOMORA_ERR_INTERNAL for another
    // method). Does not verify the CRC-32.
    std::size_t read_range(const ZipEntry &entry, std::uint64_t offset, void *buf, std::size_t len) const;

    const Source &source() const { return source_; }

  private:
    void read_end_record(std::uint64_t *cd_offset, std::uint64_t *cd_size, std::uint64_t *count);
    void read_central_directory(std::uint64_t cd_offset, std::uint64_t cd_size, std::uint64_t count,
                                Progress *progress);
    void check_local_headers_and_layout(Progress *progress);
    void check_mimetype();

    const Source &source_;
    std::vector<ZipEntry> entries_;      // files only
    std::vector<ZipEntry> dirs_;         // the directory entries, for the layout check only
    std::uint64_t cd_offset_;
    std::size_t first_extra_len_ = 0; // extra field length of the local header of entries_[0]
    bool first_is_file_ = false;      // the first central record is a file, not a directory
    bool opened_;
};

} // namespace format
} // namespace loomora

#endif

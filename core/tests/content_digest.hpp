// The cross-platform golden of a .loomora archive (FR-004): the digest of its DECOMPRESSED content.
//
// The compressed bytes of a deflated entry depend on the zlib build (the macOS and iOS modern builds
// link the SDK zlib), so the golden of a reference project is not the SHA-256 of the archive. It is
// the SHA-256 of the text made of one line per entry, in central directory (archive) order:
//
//   <name> <uncompressed size, 16 lowercase hex digits> <SHA-256 of the uncompressed bytes, 64 hex digits>\n
//
// The fixed widths keep the fields apart. The compressed bytes, the compressed sizes and the offsets
// are left out. The reader of the format checks the size and the CRC-32 of every entry while it
// decompresses it, so a corrupt archive gives no digest. Test code only: it adds no library API.
#ifndef LOOMORA_TESTS_CONTENT_DIGEST_HPP
#define LOOMORA_TESTS_CONTENT_DIGEST_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "format/io/memory_io.hpp"
#include "format/sha256.hpp"
#include "format/zip/zip_reader.hpp"

namespace content_digest {

inline void put_hex(std::string &out, std::uint64_t value, int digits) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i)
        out += kHex[(value >> (4 * i)) & 0xF];
}

// The digest of the archive in `data`, or an empty string when it cannot be read.
inline std::string of_archive(const std::uint8_t *data, std::size_t len) {
    try {
        loomora::format::MemorySource source(data, len);
        loomora::format::ZipReader reader(source);
        reader.open();
        std::string text;
        for (const loomora::format::ZipEntry &entry : reader.entries()) {
            const std::vector<std::uint8_t> content = reader.read_all(entry, entry.size);
            text += entry.name;
            text += ' ';
            put_hex(text, entry.size, 16);
            text += ' ';
            text += loomora::format::sha256_hex(content.empty() ? 0 : &content[0], content.size());
            text += '\n';
        }
        return loomora::format::sha256_hex(text.data(), text.size());
    } catch (const std::exception &) {
        return std::string();
    }
}

} // namespace content_digest

#endif

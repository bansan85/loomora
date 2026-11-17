// SHA-256 (FIPS 180-4), streaming. Input is read byte by byte (no alignment or byte-order
// assumption), so the digest is the same on every host. Used for the `entries` of the manifest.
#ifndef LOOMORA_FORMAT_SHA256_HPP
#define LOOMORA_FORMAT_SHA256_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace loomora {
namespace format {

class Sha256 {
  public:
    Sha256() { reset(); }

    // Starts a new digest.
    void reset();
    // Adds `len` bytes (any length, any alignment; `len` 0 is allowed with any pointer).
    void update(const void *data, std::size_t len);
    // Ends the digest and writes the 32 bytes. The object must be reset() before it is reused.
    void finish(std::uint8_t out[32]);
    // finish() as 64 lowercase hexadecimal characters.
    std::string hex_digest();

  private:
    void compress(const std::uint8_t *block);

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffered_;
    std::uint64_t total_; // bytes processed so far
};

// Lowercase hexadecimal text of `len` bytes.
std::string to_hex(const std::uint8_t *bytes, std::size_t len);

// The digest of one buffer, as 64 lowercase hexadecimal characters.
std::string sha256_hex(const void *data, std::size_t len);

} // namespace format
} // namespace loomora

#endif

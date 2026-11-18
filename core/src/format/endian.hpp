// Byte-wise little-endian reads and writes (never a pointer cast: no alignment or host byte-order
// assumption) and the LEB128 varint of the sequence codec (u32, at most 5 bytes, minimal encoding).
#ifndef LOOMORA_FORMAT_ENDIAN_HPP
#define LOOMORA_FORMAT_ENDIAN_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace loomora {
namespace format {

inline std::uint16_t read_u16(const unsigned char *p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}
inline std::uint32_t read_u32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t read_u64(const unsigned char *p) {
    return static_cast<std::uint64_t>(read_u32(p)) | (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
}

inline void write_u16(unsigned char *p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
}
inline void write_u32(unsigned char *p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
}
inline void write_u64(unsigned char *p, std::uint64_t v) {
    write_u32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    write_u32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

// IEEE 754 binary32 through its 32-bit pattern (memcpy, never a pointer cast). The format needs
// float to be IEEE 754 binary32 (checked below); every supported target is.
static_assert(sizeof(float) == 4, "float must be binary32");
inline float read_f32(const unsigned char *p) {
    const std::uint32_t bits = read_u32(p);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
inline void write_f32(unsigned char *p, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    write_u32(p, bits);
}
// True when the binary32 pattern is not Infinity or NaN (exponent field not all ones).
inline bool f32_is_finite_bits(const unsigned char *p) {
    return (read_u32(p) & 0x7F800000u) != 0x7F800000u;
}

// LEB128: 7 bits per byte, low group first, bit 7 = continuation. Writes at most 5 bytes into
// `out` and returns the count.
inline std::size_t write_varint_u32(unsigned char *out, std::uint32_t v) {
    std::size_t n = 0;
    while (v >= 0x80u) {
        out[n++] = static_cast<unsigned char>((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    out[n++] = static_cast<unsigned char>(v);
    return n;
}

// Reads one varint from `p[0..len)`. Returns the bytes consumed, or 0 when the data is truncated,
// longer than 5 bytes, over 2^32 - 1, or not minimal (a last byte of 0 after the first byte).
inline std::size_t read_varint_u32(const unsigned char *p, std::size_t len, std::uint32_t *value) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < len && i < 5; ++i) {
        v |= static_cast<std::uint64_t>(p[i] & 0x7Fu) << (7 * i);
        if ((p[i] & 0x80u) == 0) {
            if (i > 0 && p[i] == 0)
                return 0;  // non-minimal
            if (v > 0xFFFFFFFFull)
                return 0;
            *value = static_cast<std::uint32_t>(v);
            return i + 1;
        }
    }
    return 0;
}

}  // namespace format
}  // namespace loomora

#endif

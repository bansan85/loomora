// Byte-wise little-endian codecs and the minimal LEB128 varint (independent of host byte order).
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>

#include "format/endian.hpp"

using namespace loomora::format;

TEST_CASE("little-endian integers decode from fixed bytes", "[endian]") {
    const unsigned char b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    CHECK(read_u16(b) == 0x0201u);
    CHECK(read_u32(b) == 0x04030201u);
    CHECK(read_u64(b) == 0x0807060504030201ull);
}

TEST_CASE("little-endian integers encode to fixed bytes", "[endian]") {
    unsigned char b[8];
    write_u16(b, 0xBEEF);
    CHECK(b[0] == 0xEF);
    CHECK(b[1] == 0xBE);
    write_u32(b, 0xDEADBEEFu);
    CHECK(b[0] == 0xEF);
    CHECK(b[3] == 0xDE);
    write_u64(b, 0x0102030405060708ull);
    CHECK(b[0] == 0x08);
    CHECK(b[7] == 0x01);
}

TEST_CASE("binary32 round trips through its IEEE 754 bit pattern", "[endian]") {
    unsigned char b[4];
    write_f32(b, 1.0f);
    CHECK(b[0] == 0x00);
    CHECK(b[1] == 0x00);
    CHECK(b[2] == 0x80);
    CHECK(b[3] == 0x3F);
    CHECK(read_f32(b) == 1.0f);
    write_f32(b, -2.5f);
    CHECK(b[3] == 0xC0);
    CHECK(read_f32(b) == -2.5f);
    const unsigned char inf[4] = {0x00, 0x00, 0x80, 0x7F};
    CHECK(f32_is_finite_bits(inf) == false);
    CHECK(f32_is_finite_bits(b) == true);
}

TEST_CASE("varint u32 is minimal, at most 5 bytes", "[endian]") {
    unsigned char b[8];
    CHECK(write_varint_u32(b, 0) == 1);
    CHECK(b[0] == 0x00);
    CHECK(write_varint_u32(b, 127) == 1);
    CHECK(b[0] == 0x7F);
    CHECK(write_varint_u32(b, 128) == 2);
    CHECK(b[0] == 0x80);
    CHECK(b[1] == 0x01);
    CHECK(write_varint_u32(b, 0xFFFFFFFFu) == 5);
    CHECK(b[4] == 0x0F);
    std::uint32_t v = 0;
    CHECK(read_varint_u32(b, 5, &v) == 5);
    CHECK(v == 0xFFFFFFFFu);
}

TEST_CASE("varint u32 rejects non-minimal, too long and truncated encodings", "[endian]") {
    std::uint32_t v = 7;
    const unsigned char nonminimal[2] = {0x80, 0x00};  // 0 written in 2 bytes
    CHECK(read_varint_u32(nonminimal, 2, &v) == 0);
    const unsigned char nonminimal2[3] = {0xFF, 0x80, 0x00};
    CHECK(read_varint_u32(nonminimal2, 3, &v) == 0);
    const unsigned char toolong[6] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    CHECK(read_varint_u32(toolong, 6, &v) == 0);
    const unsigned char overflow[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};  // above 2^32 - 1
    CHECK(read_varint_u32(overflow, 5, &v) == 0);
    const unsigned char truncated[2] = {0x80, 0x80};
    CHECK(read_varint_u32(truncated, 2, &v) == 0);
    CHECK(read_varint_u32(truncated, 0, &v) == 0);
}

TEST_CASE("varint u32 round trip on boundary values", "[endian]") {
    const std::uint32_t values[] = {0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455, 268435456, 0xFFFFFFFFu};
    for (std::size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        unsigned char b[8];
        const std::size_t n = write_varint_u32(b, values[i]);
        std::uint32_t v = 0;
        CHECK(read_varint_u32(b, n, &v) == n);
        CHECK(v == values[i]);
    }
}

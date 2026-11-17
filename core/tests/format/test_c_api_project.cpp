// The three library functions of loomora_project.h: loomora_format_version,
// loomora_format_limit, loomora_uuid4_from_random.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>

#include "../../include/loomora.h"

TEST_CASE("loomora_format_version reports 1.0 and returns 1", "[format][c-api]") {
    int32_t major = -1;
    int32_t minor = -1;
    REQUIRE(loomora_format_version(&major, &minor) == 1u);
    REQUIRE(major == 1);
    REQUIRE(minor == 0);
    // Either pointer may be null.
    REQUIRE(loomora_format_version(nullptr, nullptr) == 1u);
    major = -1;
    REQUIRE(loomora_format_version(&major, nullptr) == 1u);
    REQUIRE(major == 1);
}

TEST_CASE("loomora_format_limit gives the thresholds of the data model", "[format][c-api]") {
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_NAILS_PER_LAYOUT) == 100000u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_STEPS_PER_RESULT) == 5000000u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_TRIANGLES_PER_MESH) == 5000000u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_DECOMPRESSED_TOTAL) == 256u * 1024u * 1024u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_ITEMS_PER_KIND) == 1000u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_BOMB_RATIO) == 250u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_THUMB_MAX_SIDE) == 256u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_THUMB_MAX_BYTES) == 64u * 1024u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_JSON_DEPTH) == 64u);
    REQUIRE(loomora_format_limit(LOOMORA_LIMIT_ENTRY_NAME_LENGTH) == 512u);
}

TEST_CASE("loomora_format_limit gives 0 for an unknown selector", "[format][c-api]") {
    REQUIRE(loomora_format_limit(0) == 0u);
    REQUIRE(loomora_format_limit(-1) == 0u);
    REQUIRE(loomora_format_limit(1000) == 0u);
}

TEST_CASE("loomora_uuid4_from_random builds a lowercase canonical UUID v4", "[format][c-api]") {
    const std::uint8_t zeros[16] = {0};
    char out[37];
    std::memset(out, 'x', sizeof out);
    REQUIRE(loomora_uuid4_from_random(zeros, out) == 0);
    REQUIRE(std::strcmp(out, "00000000-0000-4000-8000-000000000000") == 0);

    const std::uint8_t ones[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    REQUIRE(loomora_uuid4_from_random(ones, out) == 0);
    REQUIRE(std::strcmp(out, "ffffffff-ffff-4fff-bfff-ffffffffffff") == 0);

    const std::uint8_t mixed[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                                    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    REQUIRE(loomora_uuid4_from_random(mixed, out) == 0);
    REQUIRE(std::strcmp(out, "12345678-9abc-4ef0-9122-334455667788") == 0);
    REQUIRE(std::strlen(out) == 36u);
}

TEST_CASE("loomora_uuid4_from_random rejects a null pointer", "[format][c-api]") {
    char out[37];
    const std::uint8_t zeros[16] = {0};
    REQUIRE(loomora_uuid4_from_random(nullptr, out) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_uuid4_from_random(zeros, nullptr) == LOOMORA_ERR_INVALID);
}

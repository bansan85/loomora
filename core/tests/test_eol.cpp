// loomora_is_eol and loomora_eol_warning against core/tests/vectors.json.
#include <catch2/catch.hpp>

#include <cstdint>
#include <string>

#include "../include/loomora.h"
#include "vectors.inc"

TEST_CASE("eol: every golden vector", "[eol]") {
    for (std::size_t i = 0; i < vectors::kEolVectorsCount; ++i) {
        const vectors::EolVector &v = vectors::kEolVectors[i];
        INFO("vector: " << v.id << " family " << v.family << " version " << v.major << "."
                        << v.minor);
        REQUIRE(loomora_is_eol(v.family, v.major, v.minor) == v.expected);
    }
}

TEST_CASE("eol: Web is never end of life", "[eol]") {
    const std::int32_t versions[] = {0, 1, 6, 9, 11, 130, 2147483647};
    for (std::size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i)
        REQUIRE(loomora_is_eol(LOOMORA_FAMILY_WEB, versions[i], 0) == 0);
}

TEST_CASE("eol: negative arguments give 0 in every family", "[eol]") {
    for (std::int32_t family = -2; family <= 8; ++family) {
        INFO("family " << family);
        REQUIRE(loomora_is_eol(family, -1, 0) == 0);
        REQUIRE(loomora_is_eol(family, 0, -1) == 0);
        REQUIRE(loomora_is_eol(family, -1, -1) == 0);
        REQUIRE(loomora_is_eol(family, -2147483647 - 1, -2147483647 - 1) == 0);
    }
    REQUIRE(loomora_is_eol(-1, 5, 1) == 0);
}

TEST_CASE("eol: result is only ever 0 or 1", "[eol]") {
    for (std::int32_t family = 0; family <= 7; ++family)
        for (std::int32_t major = 0; major <= 30; ++major) {
            const std::int32_t r = loomora_is_eol(family, major, 0);
            REQUIRE((r == 0 || r == 1));
        }
}

TEST_CASE("eol: the warning text is never NULL", "[eol]") {
    const char *warning = loomora_eol_warning();
    REQUIRE(warning != nullptr);
    REQUIRE(std::string(warning).size() > 0);
    REQUIRE(loomora_eol_warning() == warning); // static storage, stable pointer
}

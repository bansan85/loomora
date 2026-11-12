// loomora_greet against the golden vectors of core/tests/vectors.json.
#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "vectors.inc"

namespace {

const std::uint8_t kSentinel = 0xA5;

/* Calls loomora_greet with the vector's arguments on a sentinel-filled buffer. */
std::int32_t call(const vectors::GreetVector &v, std::vector<std::uint8_t> &buffer) {
    buffer.assign(v.capacity + 16, kSentinel);
    return loomora_greet(v.null_input ? nullptr : v.input.data(), v.input_len, buffer.data(),
                         v.capacity);
}

} // namespace

TEST_CASE("greet: every golden vector", "[greet]") {
    for (std::size_t i = 0; i < vectors::kGreetVectorsCount; ++i) {
        const vectors::GreetVector &v = vectors::kGreetVectors[i];
        INFO("vector: " << v.id);

        std::vector<std::uint8_t> buffer;
        const std::int32_t status = call(v, buffer);
        REQUIRE(status == v.expected_status);

        if (v.expected_status >= 0) {
            REQUIRE(static_cast<std::size_t>(status) == v.expected_output.size());
            // Exact bytes, and nothing written after them (no NUL, no overrun).
            REQUIRE(std::string(buffer.begin(), buffer.begin() + status) == v.expected_output);
            for (std::size_t k = static_cast<std::size_t>(status); k < buffer.size(); ++k)
                REQUIRE(buffer[k] == kSentinel);
        }
    }
}

TEST_CASE("greet: result is the prefix followed by the input", "[greet]") {
    const std::string prefix = vectors::kGreetPrefix;
    REQUIRE(prefix == "Bonjour ");

    const std::string input = "Coucou";
    std::vector<std::uint8_t> out(100);
    const std::int32_t status = loomora_greet(input.data(), input.size(), out.data(), out.size());
    REQUIRE(status == static_cast<std::int32_t>(prefix.size() + input.size()));
    REQUIRE(std::string(out.begin(), out.begin() + status) == prefix + input);
}

TEST_CASE("greet: limit of 4096 bytes", "[greet]") {
    std::vector<std::uint8_t> out(vectors::kGreetMaxInput + 100);

    const std::string at_limit(vectors::kGreetMaxInput, 'a');
    REQUIRE(loomora_greet(at_limit.data(), at_limit.size(), out.data(), out.size()) ==
            static_cast<std::int32_t>(8 + vectors::kGreetMaxInput));

    const std::string over_limit(vectors::kGreetMaxInput + 1, 'a');
    REQUIRE(loomora_greet(over_limit.data(), over_limit.size(), out.data(), out.size()) ==
            LOOMORA_ERR_TOO_LONG);
}

TEST_CASE("greet: error code values", "[greet]") {
    REQUIRE(LOOMORA_ERR_TOO_LONG == -1);
    REQUIRE(LOOMORA_ERR_CAPACITY == -2);
    REQUIRE(LOOMORA_ERR_INVALID == -3);
    REQUIRE(LOOMORA_ERR_INTERNAL == -4);
}

TEST_CASE("greet: null output buffer is rejected", "[greet]") {
    REQUIRE(loomora_greet("abc", 3, nullptr, 100) == LOOMORA_ERR_INVALID);
}

TEST_CASE("version: ABI version and compiler string", "[greet][version]") {
    REQUIRE(loomora_abi_version() == static_cast<std::uint32_t>(LOOMORA_ABI_VERSION));
    REQUIRE(loomora_build_compiler() != nullptr);
    REQUIRE(std::string(loomora_build_compiler()).size() > 0);
}

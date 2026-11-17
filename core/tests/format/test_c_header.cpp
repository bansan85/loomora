// The public header loomora_project.h (and loomora.h, which includes it): valid as C89 and C++98,
// and the ABI version stays 1 (owner decision 2026-11-17, contracts/c-api-project.md).
#include <catch2/catch.hpp>

#include "../../include/loomora.h"

extern "C" int loomora_header_c89_check(void);
extern "C" int loomora_header_cpp98_check(void);

TEST_CASE("loomora.h compiled as C89 works and the ABI version stays 1", "[format][header]") {
    REQUIRE(loomora_abi_version() == 1u);
    REQUIRE(LOOMORA_ABI_VERSION == 1);
    REQUIRE(loomora_header_c89_check() == 0);
}

TEST_CASE("loomora.h compiled as C++98 works", "[format][header]") {
    REQUIRE(loomora_header_cpp98_check() == 0);
}

TEST_CASE("the new error codes and kinds have the values of the contract", "[format][header]") {
    REQUIRE(LOOMORA_ERR_IO == -10);
    REQUIRE(LOOMORA_ERR_NOT_A_PROJECT == -11);
    REQUIRE(LOOMORA_ERR_CORRUPT == -12);
    REQUIRE(LOOMORA_ERR_UNSUPPORTED_VERSION == -13);
    REQUIRE(LOOMORA_ERR_UNSUPPORTED_FEATURE == -14);
    REQUIRE(LOOMORA_ERR_BOMB == -15);
    REQUIRE(LOOMORA_ERR_NO_MEMORY == -16);
    REQUIRE(LOOMORA_ERR_READ_ONLY_PROJECT == -17);
    REQUIRE(LOOMORA_ERR_NOT_FOUND == -18);
    REQUIRE(LOOMORA_ERR_CANCELLED == -19);
    REQUIRE(LOOMORA_ERR_HAS_DEPENDENTS == -20);
    REQUIRE(LOOMORA_ERR_INVALID_ITEM == -21);
    REQUIRE(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM == -22);
    REQUIRE(LOOMORA_KIND_FRAME == 1);
    REQUIRE(LOOMORA_KIND_LAYOUT == 2);
    REQUIRE(LOOMORA_KIND_IMAGE == 3);
    REQUIRE(LOOMORA_KIND_SPOOL == 4);
    REQUIRE(LOOMORA_KIND_RESULT == 5);
}

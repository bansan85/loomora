// Self-test of the .loomora format code: plain C++11 `main`, no test framework, so it can be
// cross-compiled and run on every target (the Catch2 tests of core/tests/format/ run on the host
// only). Each later story adds its checks here. Same output rule as selftest.cpp: through
// <cstdio> where LOOMORA_AVOID_IOSTREAM is defined (the MinGW-w64 toolchains of the Windows floors
// older than Vista), and no <iostream>, <thread>, <mutex> or <locale> in this file.
#include <cstdio>
#include <cstring>

#include "../include/loomora.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        ++g_failures;
        std::fputs("FAIL: ", stdout);
        std::fputs(what, stdout);
        std::fputs("\n", stdout);
    }
}

} // namespace

int main() {
    int32_t major = 0;
    int32_t minor = -1;
    check(loomora_abi_version() == 1u, "ABI version stays 1");
    check(loomora_format_version(&major, &minor) == 1u && major == 1 && minor == 0,
          "format version 1.0");
    const uint8_t zeros[16] = {0};
    char uuid[37];
    check(loomora_uuid4_from_random(zeros, uuid) == 0 &&
              std::strcmp(uuid, "00000000-0000-4000-8000-000000000000") == 0,
          "uuid4 from random");
    std::fputs(g_failures == 0 ? "format selftest: ok\n" : "format selftest: FAILED\n", stdout);
    return g_failures == 0 ? 0 : 1;
}

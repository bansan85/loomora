#include "../include/loomora.h"

#include <algorithm>
#include <new>
#include <string>

#include "errors.hpp"
#include "greet.hpp"

/* C binding: no exception ever crosses these functions, and no C++ type is
   exposed. Buffers belong to the caller; returned strings are static. */

extern "C" {

uint32_t loomora_abi_version(void) { return LOOMORA_ABI_VERSION; }

int32_t loomora_greet(const char *input, size_t input_len, uint8_t *out, size_t cap) {
    if (out == nullptr || (input == nullptr && input_len != 0))
        return LOOMORA_ERR_INVALID;

    try {
        const std::string greeting =
            loomora::greet(input_len == 0 ? std::string() : std::string(input, input_len));
        if (greeting.size() > cap)
            return LOOMORA_ERR_CAPACITY;

        std::copy(greeting.begin(), greeting.end(), out);
        return static_cast<int32_t>(greeting.size());
    } catch (const loomora::InputTooLong &) {
        return LOOMORA_ERR_TOO_LONG;
    } catch (const loomora::InvalidUtf8 &) {
        return LOOMORA_ERR_INVALID;
    } catch (...) { // std::bad_alloc or anything unexpected
        return LOOMORA_ERR_INTERNAL;
    }
}

const char *loomora_build_compiler(void) {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    return "msvc";
#else
    return "unknown";
#endif
}

} // extern "C"

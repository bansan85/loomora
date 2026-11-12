#include "version.hpp"

#include "../include/loomora.h"

#define LOOMORA_STRINGIFY_(x) #x
#define LOOMORA_STRINGIFY(x) LOOMORA_STRINGIFY_(x)

namespace loomora {

std::uint32_t abi_version() { return LOOMORA_ABI_VERSION; }

const char *build_compiler() {
    // clang and clang-cl also define __GNUC__ / _MSC_VER: test clang first.
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " LOOMORA_STRINGIFY(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

} // namespace loomora

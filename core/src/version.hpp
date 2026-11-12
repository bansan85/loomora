#ifndef LOOMORA_VERSION_HPP
#define LOOMORA_VERSION_HPP

#include <cstdint>

#include "internal/config.hpp"

namespace loomora {

/* ABI version of the C API (LOOMORA_ABI_VERSION of loomora.h). */
LOOMORA_NODISCARD std::uint32_t abi_version();

/* Identification of the compiler that built the backend, e.g. "gcc 8.3.0" or
   "clang 17.0.6": static, never null. */
LOOMORA_NODISCARD const char *build_compiler();

} // namespace loomora

#endif

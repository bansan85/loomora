#ifndef LOOMORA_EOL_HPP
#define LOOMORA_EOL_HPP

#include <cstdint>

#include "internal/config.hpp"

namespace loomora {

/* Operating-system families (values of the LOOMORA_FAMILY_* macros of loomora.h). */
enum Family : std::int32_t {
    kFamilyWindows = 1,
    kFamilyMacos = 2,
    kFamilyLinux = 3,
    kFamilyAndroid = 4,
    kFamilyIos = 5,
    kFamilyWeb = 6
};

/* True if the vendor no longer maintains the given system version (table as of
   2026-11-12, see eol.cpp). Any negative argument, an unknown family and the Web
   family give false. */
LOOMORA_NODISCARD bool is_eol(std::int32_t family, std::int32_t major, std::int32_t minor);

/* Non-blocking end-of-life warning (FR-013): UTF-8, static, never null. */
LOOMORA_NODISCARD const char *eol_warning();

} // namespace loomora

#endif

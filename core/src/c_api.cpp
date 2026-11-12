#include "../include/loomora.h"

#include <algorithm>
#include <new>
#include <string>

#include "eol.hpp"
#include "errors.hpp"
#include "greet.hpp"
#include "internal/config.hpp"
#include "probe.hpp"
#include "version.hpp"

/* C binding: a thin forwarding layer. No exception ever crosses these functions, and no
   C++ type is exposed. Buffers belong to the caller; returned strings are static. */

extern "C" {

uint32_t loomora_abi_version(void) { return loomora::abi_version(); }

int32_t loomora_greet(const char *input, size_t input_len, uint8_t *out, size_t cap) {
    if (LOOMORA_UNLIKELY(out == nullptr || (input == nullptr && input_len != 0)))
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

int32_t loomora_is_eol(int32_t family, int32_t major, int32_t minor) {
    try {
        return loomora::is_eol(family, major, minor) ? 1 : 0;
    } catch (...) { // cannot happen today; a safe value if the table ever allocates
        return 0;
    }
}

const char *loomora_eol_warning(void) { return loomora::eol_warning(); }

const char *loomora_build_compiler(void) { return loomora::build_compiler(); }

/* Returns the total length needed, or 0 when the record cannot be built: an unknown
   `ui_variant` or `core_kind`, or any internal failure (out of memory...). In that case
   nothing is written to `buf`, and a valid record is never empty, so 0 is unambiguous. */
size_t loomora_probe_record(const char *platform_id, int32_t ui_variant, int32_t core_kind,
                            const char *input, const char *displayed_text, int32_t displayed,
                            uint32_t display_ms, int64_t rss_kb, int32_t eol_warning_shown,
                            const char *disabled_features, const char *timestamp_utc, uint8_t *buf,
                            size_t cap) {
    try {
        return loomora::probe_record(platform_id, ui_variant, core_kind, input, displayed_text,
                                     displayed, display_ms, rss_kb, eol_warning_shown,
                                     disabled_features, timestamp_utc, buf, cap);
    } catch (...) {
        return 0;
    }
}

} // extern "C"

#ifndef LOOMORA_PROBE_HPP
#define LOOMORA_PROBE_HPP

#include <cstddef>
#include <cstdint>

namespace loomora {

/* Builds the ProbeResult JSON line (contracts/probe-result.schema.json): one line, ASCII
   only (non-ASCII characters as \uXXXX escapes), no trailing newline. Copies at most
   `cap` bytes into `buf` (no NUL) and returns the total length needed. Null strings are
   read as "" and a null `buf` is accepted when `cap` is 0.

   `ui_variant` is 1 (legacy) or 2 (modern) and `core_kind` 1 (cpp), 2 (cpp-wasm) or
   3 (cpp-js): any other value is a std::invalid_argument, as is any failure to build the
   record (std::bad_alloc...). The C boundary (c_api.cpp) turns every exception into 0. */
std::size_t probe_record(const char *platform_id, std::int32_t ui_variant, std::int32_t core_kind,
                         const char *input, const char *displayed_text, std::int32_t displayed,
                         std::uint32_t display_ms, std::int64_t rss_kb,
                         std::int32_t eol_warning_shown, const char *disabled_features,
                         const char *timestamp_utc, std::uint8_t *buf, std::size_t cap);

} // namespace loomora

#endif

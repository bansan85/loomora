#include "eol.hpp"

#include <cstddef>

namespace loomora {

namespace {

/* A system is end of life when its version is below the first one still maintained.
   Thresholds as of 2026-11-12; each one is cited in docs/implementation/validation/sources.json.
     Windows  NT version, Windows 11 passed as 11.0 (Windows 10 ended 2025-10-14)
     macOS    Apple patches the three latest releases (15, 26, 27): 14 and below are EOL
     iOS      18 and later are still patched, 17 and below are not
     Android  marketing version (not the API level): 14 and later are still patched
     Linux    kernel version: 5.10 is the oldest longterm branch still maintained
   The Web family has no entry: browsers are not covered (FR-013). */
struct Threshold {
    std::int32_t family;
    std::int32_t first_major; // first maintained version, as major.minor
    std::int32_t first_minor;
};

const Threshold kThresholds[] = {
    {kFamilyWindows, 11, 0}, {kFamilyMacos, 15, 0}, {kFamilyLinux, 5, 10},
    {kFamilyAndroid, 14, 0}, {kFamilyIos, 18, 0},
};

/* UTF-8; kept ASCII-only in the source so that it does not depend on the compiler's
   source character set. */
const char kEolWarning[] =
    "Warning: this operating system is no longer maintained by its vendor and no longer "
    "receives security updates. Loomora keeps working and opens no network connection.";

} // namespace

bool is_eol(std::int32_t family, std::int32_t major, std::int32_t minor) {
    if (major < 0 || minor < 0)
        return false;
    for (std::size_t i = 0; i < sizeof(kThresholds) / sizeof(kThresholds[0]); ++i) {
        const Threshold &t = kThresholds[i];
        if (t.family == family)
            return major < t.first_major || (major == t.first_major && minor < t.first_minor);
    }
    return false; // Web and unknown families
}

const char *eol_warning() { return kEolWarning; }

} // namespace loomora

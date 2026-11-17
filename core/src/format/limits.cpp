#include "format/limits.hpp"

#include <cstring>

namespace loomora {
namespace format {

namespace {
Limits g_limits;
}

Limits &limits() { return g_limits; }

void reset_limits() { g_limits = Limits(); }

bool set_limit_by_name(const char *name, std::uint64_t value) {
    if (name == nullptr)
        return false;
    if (std::strcmp(name, "warn_nails") == 0)
        g_limits.warn_nails_per_layout = value;
    else if (std::strcmp(name, "warn_steps") == 0)
        g_limits.warn_steps_per_result = value;
    else if (std::strcmp(name, "warn_triangles") == 0)
        g_limits.warn_triangles_per_mesh = value;
    else if (std::strcmp(name, "warn_total_bytes") == 0)
        g_limits.warn_decompressed_total = value;
    else if (std::strcmp(name, "warn_items") == 0)
        g_limits.warn_items_per_kind = value;
    else if (std::strcmp(name, "bomb_ratio") == 0)
        g_limits.bomb_ratio = value;
    else
        return false;
    return true;
}

} // namespace format
} // namespace loomora

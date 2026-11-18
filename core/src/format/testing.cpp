#include "format/testing.hpp"

namespace loomora {
namespace format {

namespace {
Hooks g_hooks;
}

Hooks &hooks() { return g_hooks; }

void reset_hooks() { g_hooks = Hooks(); }

} // namespace format
} // namespace loomora

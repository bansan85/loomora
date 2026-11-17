// Spool (data-model.md, "Spool"): one thread; all spools of a project are stored together in
// spools.json as a list, in the order of manifest.order.spools.
#ifndef LOOMORA_FORMAT_MODEL_SPOOL_HPP
#define LOOMORA_FORMAT_MODEL_SPOOL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "format/model/common.hpp"

namespace loomora {
namespace format {
namespace model {

enum Domain { kDomainExterior = 0, kDomainInterior = 1, kDomainBoth = 2 };

const char *to_string(Domain d);
bool parse_domain(const std::string &text, Domain &out);

// JSON key order: id, name, color, opacity, diameter_mm, available_length_mm, material, domain.
struct Spool {
    std::uint32_t id = 0;
    std::string name;                 // up to 256 bytes
    std::string color = "#000000";    // #rrggbb
    double opacity = 1;               // in (0, 1]
    double diameter_mm = 0;           // > 0
    bool has_available_length = false;
    double available_length_mm = 0;   // > 0, meaningful when has_available_length
    bool has_material = false;
    std::string material;             // free text, meaningful when has_material
    Domain domain = kDomainExterior;  // default exterior
};

void check_spool(const Spool &spool, const std::string &entry, Report &report);
// Every spool, plus the identifiers being unique (id.duplicate).
void check_spools(const std::vector<Spool> &spools, const std::string &entry, Report &report);

} // namespace model
} // namespace format
} // namespace loomora

#endif

#include "format/model/spool.hpp"

#include <algorithm>

namespace loomora {
namespace format {
namespace model {

namespace {
const char *const kDomainNames[] = {"exterior", "interior", "both"};
}

const char *to_string(Domain d) {
    const int i = static_cast<int>(d);
    return (i >= 0 && i < 3) ? kDomainNames[i] : "";
}

bool parse_domain(const std::string &text, Domain &out) {
    int i;
    if (!lookup_name(kDomainNames, 3, text, i))
        return false;
    out = static_cast<Domain>(i);
    return true;
}

void check_spool(const Spool &s, const std::string &entry, Report &report) {
    check_length(report, entry, kItemSpool, s.id, "name", s.name, kMaxNameBytes);
    if (!is_hex_color(s.color))
        add_value_error(report, entry, kItemSpool, s.id, "color must be #rrggbb");
    if (check_finite(report, entry, kItemSpool, s.id, "opacity", s.opacity) && !(s.opacity > 0 && s.opacity <= 1))
        add_value_error(report, entry, kItemSpool, s.id, "opacity must be greater than 0 and at most 1");
    check_positive(report, entry, kItemSpool, s.id, "diameter_mm", s.diameter_mm);
    if (s.has_available_length)
        check_positive(report, entry, kItemSpool, s.id, "available_length_mm", s.available_length_mm);
    if (static_cast<int>(s.domain) < 0 || static_cast<int>(s.domain) > 2)
        add_value_error(report, entry, kItemSpool, s.id, "domain must be exterior, interior or both");
}

void check_spools(const std::vector<Spool> &spools, const std::string &entry, Report &report) {
    std::vector<std::uint32_t> ids;
    ids.reserve(spools.size());
    for (std::size_t i = 0; i < spools.size(); ++i) {
        check_spool(spools[i], entry, report);
        ids.push_back(spools[i].id);
    }
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 1; i < ids.size(); ++i)
        if (ids[i] == ids[i - 1] && (i == 1 || ids[i] != ids[i - 2]))
            add_error(report, "id.duplicate", entry, kItemSpool, ids[i], "spool identifier used twice");
}

} // namespace model
} // namespace format
} // namespace loomora

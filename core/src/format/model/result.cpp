#include "format/model/result.hpp"

#include <algorithm>

namespace loomora {
namespace format {
namespace model {

namespace {

const char *const kProjectionNames[] = {"orthographic", "perspective"};

void check_vector3(const double v[3], const char *what, const Result &r, const std::string &entry, Report &report) {
    if (!is_finite(v[0]) || !is_finite(v[1]) || !is_finite(v[2]))
        add_error(report, "num.not_finite", entry, kItemResult, r.id, std::string(what) + " has a value that is not finite");
}

void check_view(const View &view, const Result &r, const std::string &entry, Report &report) {
    check_vector3(view.position, "view.position", r, entry, report);
    check_vector3(view.target, "view.target", r, entry, report);
    check_vector3(view.up, "view.up", r, entry, report);
    if (view.projection == kProjectionOrthographic) {
        check_positive(report, entry, kItemResult, r.id, "view.size", view.size);
    } else if (view.projection == kProjectionPerspective) {
        if (check_finite(report, entry, kItemResult, r.id, "view.fov_deg", view.fov_deg) &&
            !(view.fov_deg > 0 && view.fov_deg < 180))
            add_value_error(report, entry, kItemResult, r.id, "view.fov_deg must be greater than 0 and below 180");
    } else {
        add_value_error(report, entry, kItemResult, r.id, "view.projection must be orthographic or perspective");
    }
    if (check_finite(report, entry, kItemResult, r.id, "view.weight", view.weight) && view.weight < 0)
        add_value_error(report, entry, kItemResult, r.id, "view.weight must not be negative");
}

} // namespace

const char *to_string(Projection p) {
    const int i = static_cast<int>(p);
    return (i >= 0 && i < 2) ? kProjectionNames[i] : "";
}

bool parse_projection(const std::string &text, Projection &out) {
    int i;
    if (!lookup_name(kProjectionNames, 2, text, i))
        return false;
    out = static_cast<Projection>(i);
    return true;
}

void check_result(const Result &r, FrameKind frame_kind, Profile profile, const std::string &entry, Report &report) {
    check_length(report, entry, kItemResult, r.id, "name", r.name, kMaxNameBytes);
    if (!is_iso8601_utc(r.created_at))
        add_value_error(report, entry, kItemResult, r.id, "created_at must be an ISO 8601 UTC time (YYYY-MM-DDTHH:MM:SSZ)");

    if (r.images.empty()) {
        add_value_error(report, entry, kItemResult, r.id, "images must hold at least one image");
    } else if (frame_kind == kFrame2D) {
        bool any_view = false;
        for (std::size_t i = 0; i < r.images.size(); ++i)
            any_view = any_view || r.images[i].has_view;
        if (r.images.size() != 1)
            add_value_error(report, entry, kItemResult, r.id, "a result on a 2D frame has exactly one image");
        if (any_view)
            add_value_error(report, entry, kItemResult, r.id, "an image of a 2D frame has no view");
    } else {
        for (std::size_t i = 0; i < r.images.size(); ++i) {
            if (!r.images[i].has_view)
                add_value_error(report, entry, kItemResult, r.id, "an image of a 3D frame needs a view");
            else
                check_view(r.images[i].view, r, entry, report);
        }
    }

    if (r.spools.empty())
        add_value_error(report, entry, kItemResult, r.id, "the spool selection must hold at least one spool");
    std::vector<std::uint32_t> ids;
    ids.reserve(r.spools.size());
    for (std::size_t i = 0; i < r.spools.size(); ++i) {
        check_positive(report, entry, kItemResult, r.id, "spools[].length_mm", r.spools[i].length_mm);
        ids.push_back(r.spools[i].spool);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        add_value_error(report, entry, kItemResult, r.id, "a spool appears twice in the selection");

    check_entry_ref(report, entry, kItemResult, r.id, "thumbnail", r.thumbnail);
    if (r.has_settings) {
        check_entry_ref(report, entry, kItemResult, r.id, "settings", r.settings);
        if (profile == kProfileLight)
            add_value_error(report, entry, kItemResult, r.id, "a light project keeps no settings");
    }
}

} // namespace model
} // namespace format
} // namespace loomora

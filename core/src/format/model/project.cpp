#include "format/model/project.hpp"

#include <algorithm>

namespace loomora {
namespace format {
namespace model {

const char *const kFeatureSurfaceMesh = "surface_mesh";
const char *const kFeatureStringLevels = "string_levels";
const char *const kFeatureMultiViewTarget = "multi_view_target";

namespace {

const char *const kProfileNames[] = {"full", "light"};

std::string decimal(std::uint64_t v) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

bool parse_number(const std::string &s, std::size_t from, std::size_t to, std::uint32_t &out) {
    if (from >= to || to - from > 9)
        return false;
    std::uint32_t v = 0;
    for (std::size_t i = from; i < to; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + static_cast<std::uint32_t>(s[i] - '0');
    }
    out = v;
    return true;
}

void check_order(const std::vector<std::uint32_t> &order, std::uint64_t count, std::uint32_t next_id,
                 const char *kind, Report &report) {
    const std::string k(kind);
    if (count != order.size())
        add_value_error(report, "manifest.json", kItemNone, 0,
                        "counts." + k + " is " + decimal(count) + " but order." + k + " lists " + decimal(order.size()));
    std::vector<std::uint32_t> sorted(order);
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        add_error(report, "id.duplicate", "manifest.json", kItemNone, 0, "order." + k + " repeats an identifier");
    if (!sorted.empty() && sorted.back() >= next_id)
        add_value_error(report, "manifest.json", kItemNone, 0,
                        "next_id of " + k + " is not above the identifier " + decimal(sorted.back()));
}

} // namespace

const char *to_string(Profile p) {
    const int i = static_cast<int>(p);
    return (i >= 0 && i < 2) ? kProfileNames[i] : "";
}

bool parse_profile(const std::string &text, Profile &out) {
    int i;
    if (!lookup_name(kProfileNames, 2, text, i))
        return false;
    out = static_cast<Profile>(i);
    return true;
}

std::string version_text(const Version &v) { return decimal(v.major) + "." + decimal(v.minor); }

bool parse_version(const std::string &text, Version &out) {
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos)
        return false;
    Version v;
    if (!parse_number(text, 0, dot, v.major) || !parse_number(text, dot + 1, text.size(), v.minor))
        return false;
    out = v;
    return true;
}

bool version_above(const Version &a, const Version &b) {
    return a.major != b.major ? a.major > b.major : a.minor > b.minor;
}

void check_manifest(const Manifest &m, Report &report) {
    const std::string kEntry = "manifest.json";
    if (m.format.name != "loomora")
        add_value_error(report, kEntry, kItemNone, 0, "format.name must be loomora");
    Version format;
    format.major = m.format.major;
    format.minor = m.format.minor;
    if (version_above(m.min_reader, format))
        add_value_error(report, kEntry, kItemNone, 0, "min_reader is above the format version");
    if (!is_uuid4(m.project_id))
        add_value_error(report, kEntry, kItemNone, 0, "project_id must be a lowercase UUID v4");
    check_length(report, kEntry, kItemNone, 0, "title", m.title, kMaxTitleBytes);
    if (!is_iso8601_utc(m.created_at))
        add_value_error(report, kEntry, kItemNone, 0, "created_at must be an ISO 8601 UTC time");
    if (!is_iso8601_utc(m.modified_at))
        add_value_error(report, kEntry, kItemNone, 0, "modified_at must be an ISO 8601 UTC time");
    if (m.profile == kProfileLight && m.editable)
        add_value_error(report, kEntry, kItemNone, 0, "a light project is not editable");

    check_order(m.order.frames, m.counts.frames, m.next_id.frame, "frames", report);
    check_order(m.order.layouts, m.counts.layouts, m.next_id.layout, "layouts", report);
    check_order(m.order.images, m.counts.images, m.next_id.image, "images", report);
    check_order(m.order.spools, m.counts.spools, m.next_id.spool, "spools", report);
    check_order(m.order.results, m.counts.results, m.next_id.result, "results", report);

    std::vector<std::string> features(m.required_features);
    std::sort(features.begin(), features.end());
    if (std::adjacent_find(features.begin(), features.end()) != features.end())
        add_value_error(report, kEntry, kItemNone, 0, "required_features repeats a feature");
    for (std::size_t i = 0; i < m.required_features.size(); ++i) {
        const std::string &f = m.required_features[i];
        if (f != kFeatureSurfaceMesh && f != kFeatureStringLevels && f != kFeatureMultiViewTarget)
            add_value_error(report, kEntry, kItemNone, 0, "required_features names an unknown feature: " + f);
    }

    std::vector<std::string> paths;
    paths.reserve(m.entries.size());
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
        const ManifestEntry &e = m.entries[i];
        if (!is_sha256_hex(e.sha256))
            add_value_error(report, kEntry, kItemNone, 0, "entries[" + e.path + "].sha256 must be 64 lowercase hex characters");
        if (e.path == "mimetype" || e.path == "manifest.json")
            add_value_error(report, kEntry, kItemNone, 0, "entries must not list " + e.path);
        paths.push_back(e.path);
    }
    std::sort(paths.begin(), paths.end());
    if (std::adjacent_find(paths.begin(), paths.end()) != paths.end())
        add_error(report, "id.duplicate", kEntry, kItemNone, 0, "entries lists the same path twice");
}

} // namespace model
} // namespace format
} // namespace loomora

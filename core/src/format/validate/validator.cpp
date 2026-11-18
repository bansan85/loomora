#include "format/validate/validator.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "format/codec/image_meta.hpp"
#include "format/codec/mesh_codec.hpp"
#include "format/codec/sequence_codec.hpp"
#include "format/limits.hpp"
#include "format/project_reader.hpp"
#include "format/validate/geometry.hpp"

namespace loomora {
namespace format {

namespace {

const std::size_t kMaxIssuesPerRule = 100; // per layout and rule: a table of bad nails is not a report of bad nails

std::string num(std::uint64_t v) {
    if (v == 0)
        return "0";
    std::string s;
    while (v != 0) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
        v /= 10;
    }
    return s;
}

std::string real(double v) {
    // Short and locale free: millimetres with up to three decimals.
    const bool neg = v < 0;
    double a = neg ? -v : v;
    if (!(a < 1e15))
        return neg ? "-large" : "large";
    const std::uint64_t scaled = static_cast<std::uint64_t>(a * 1000 + 0.5);
    std::string s = num(scaled / 1000);
    const std::uint64_t frac = scaled % 1000;
    if (frac != 0) {
        std::string f = num(frac);
        while (f.size() < 3)
            f.insert(f.begin(), '0');
        while (!f.empty() && f[f.size() - 1] == '0')
            f.erase(f.size() - 1);
        s += "." + f;
    }
    return (neg && scaled != 0 ? "-" : "") + s;
}

void error(Report &r, const char *code, const std::string &entry, ItemKind kind, std::uint32_t id,
           const std::string &message) {
    r.add(kError, code, entry, message, kind, id);
}

void warning(Report &r, const char *code, const std::string &entry, ItemKind kind, std::uint32_t id,
             const std::string &message) {
    r.add(kWarning, code, entry, message, kind, id);
}

void warn_count(Report &r, const std::string &text, std::uint64_t count, std::uint64_t limit,
                const std::string &entry, ItemKind kind, std::uint32_t id) {
    if (count <= limit)
        return;
    warning(r, "warn.count", entry, kind, id, num(count) + " " + text + " (threshold " + num(limit) + ")");
}

const char *const kManifestEntry = "manifest.json";

struct Validator {
    const ProjectReader &rd;
    Report &report;
    const bool full; // every item could be read

    Validator(const ProjectReader &reader, Report &r) : rd(reader), report(r), full(reader.complete()) {}

    bool has_entry(const std::string &name) const { return rd.find_entry(name) != nullptr; }

    void missing_entry(const std::string &owner_entry, ItemKind kind, std::uint32_t id, const char *what,
                       const model::EntryRef &ref) {
        if (!ref.entry.empty() && !has_entry(ref.entry))
            error(report, "ref.missing", owner_entry, kind, id,
                  std::string(what) + " names the entry " + ref.entry + ", which is not in the archive");
    }

    // ---- time stamps ------------------------------------------------------------------------------
    void timestamps();
    void timestamp(const std::string &entry, ItemKind kind, std::uint32_t id, const char *field,
                   const std::string &value);

    // ---- frames ---------------------------------------------------------------------------------
    void frames();
    void frame_exclusions(const FrameRecord &fr);
    void frame_origin(const FrameRecord &fr);

    // ---- references and small items ---------------------------------------------------------------
    void references();
    void layout_references(const LayoutRecord &lr);
    void result_references(const ResultRecord &rr);
    void image_references(const ImageRecord &ir);
    void thumbnails();
    void thumbnail(const std::string &item_entry, ItemKind kind, std::uint32_t id, const std::string &entry);

    // ---- layouts and steps --------------------------------------------------------------------------
    void layouts();
    void layout(const LayoutRecord &lr);
    void nails_in_frame(const LayoutRecord &lr, const FrameRecord &fr, const model::NailTable &t);
    void steps(const ResultRecord &rr, const std::vector<std::uint32_t> &sorted_ids, bool dense);

    void thresholds();
};

// ---- frames -------------------------------------------------------------------------------------

void Validator::frames() {
    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = rd.frames().begin(); it != rd.frames().end();
         ++it) {
        frame_exclusions(it->second);
        frame_origin(it->second);
    }
}

void Validator::frame_exclusions(const FrameRecord &fr) {
    const model::Frame &f = fr.frame;
    if (f.exclusions.empty() || f.outline.polyline.size() < 3)
        return;
    const double tol = limits().nail_in_frame_tolerance_mm;
    const Polygon outline(f.outline.polyline);
    for (std::size_t i = 0; i < f.exclusions.size(); ++i) {
        const std::vector<model::Point> &p = f.exclusions[i].polyline;
        for (std::size_t k = 0; k < p.size(); ++k) {
            if (!outline.contains(p[k].x, p[k].y) && !outline.near_boundary(p[k].x, p[k].y, tol)) {
                error(report, "schema.value", fr.entry, kItemFrame, f.id,
                      "exclusion " + num(i) + " is not inside the outline (point " + num(k) + ")");
                break;
            }
        }
    }
}

void Validator::frame_origin(const FrameRecord &fr) {
    const model::Frame &f = fr.frame;
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    bool any = false;
    if (f.kind == model::kFrame3D && full && !fr.mesh_entry.empty()) {
        const ZipEntry *e = rd.find_entry(fr.mesh_entry);
        if (e != nullptr) {
            try {
                EntrySource src(rd.zip(), *e);
                MeshReader mr(src);
                const std::uint64_t total = mr.vertex_count();
                std::vector<float> buf(3 * 4096);
                for (std::uint64_t at = 0; at < total; at += 4096) {
                    const std::uint64_t n = std::min<std::uint64_t>(4096, total - at);
                    mr.read_positions(at, n, buf.data());
                    for (std::uint64_t v = 0; v < n; ++v)
                        for (int a = 0; a < 3; ++a) {
                            const double c = buf[3 * v + a];
                            if (!any && v == 0 && at == 0) {
                                // the first vertex starts the box (the three axes of it)
                                lo[a] = hi[a] = c;
                            } else {
                                lo[a] = std::min(lo[a], c);
                                hi[a] = std::max(hi[a], c);
                            }
                        }
                    any = true;
                }
            } catch (const FormatError &ex) {
                if (ex.code() != LOOMORA_ERR_CORRUPT)
                    throw;
                any = false; // the mesh is reported by the reader
            }
        }
    }
    if (!any && !f.outline.polyline.empty()) {
        const std::vector<model::Point> &p = f.outline.polyline;
        lo[0] = hi[0] = p[0].x;
        lo[1] = hi[1] = p[0].y;
        for (std::size_t i = 1; i < p.size(); ++i) {
            lo[0] = std::min(lo[0], p[i].x);
            hi[0] = std::max(hi[0], p[i].x);
            lo[1] = std::min(lo[1], p[i].y);
            hi[1] = std::max(hi[1], p[i].y);
        }
        lo[2] = hi[2] = 0;
        any = true;
    }
    if (!any)
        return;
    const double tol = limits().frame_origin_tolerance_mm;
    for (int a = 0; a < 3; ++a) {
        const double c = (lo[a] + hi[a]) / 2;
        if (std::fabs(c) > tol) {
            warning(report, "warn.origin_offset", fr.entry, kItemFrame, f.id,
                    std::string("the bounding box of the frame is centred at ") + "xyz"[a] + " = " + real(c) +
                        " mm, not at the origin (tolerance " + real(tol) + " mm)");
            return;
        }
    }
}

// ---- references -----------------------------------------------------------------------------------

void Validator::layout_references(const LayoutRecord &lr) {
    const model::Layout &l = lr.layout;
    if (full && rd.frame(l.frame) == nullptr) {
        error(report, "ref.missing", lr.entry, kItemLayout, l.id, "the layout names frame " + num(l.frame) + ", which does not exist");
    } else if (l.frame != lr.folder_frame && rd.frame(l.frame) != nullptr) {
        error(report, "ref.layout_of_other_frame", lr.entry, kItemLayout, l.id,
              "the layout is stored under frame " + num(lr.folder_frame) + " but names frame " + num(l.frame));
    }
    if (full && l.frame != lr.folder_frame && rd.frame(lr.folder_frame) == nullptr)
        error(report, "ref.missing", lr.entry, kItemLayout, l.id,
              "the layout is stored under frame " + num(lr.folder_frame) + ", which does not exist");
}

void Validator::result_references(const ResultRecord &rr) {
    const model::Result &r = rr.result;
    const LayoutRecord *lay = rd.layout(r.layout);
    if (full && rd.frame(r.frame) == nullptr)
        error(report, "ref.missing", rr.entry, kItemResult, r.id, "the result names frame " + num(r.frame) + ", which does not exist");
    if (lay == nullptr) {
        if (full)
            error(report, "ref.missing", rr.entry, kItemResult, r.id,
                  "the result names layout " + num(r.layout) + ", which does not exist");
    } else if (lay->layout.frame != r.frame && (!full || rd.frame(lay->layout.frame) != nullptr)) {
        // (a layout whose own frame does not exist is reported once, as ref.missing)
        error(report, "ref.layout_of_other_frame", rr.entry, kItemResult, r.id,
              "layout " + num(r.layout) + " belongs to frame " + num(lay->layout.frame) + ", the result is on frame " +
                  num(r.frame));
    }
    for (std::size_t i = 0; i < r.images.size(); ++i)
        if (full && rd.image(r.images[i].image) == nullptr)
            error(report, "ref.missing", rr.entry, kItemResult, r.id,
                  "the result uses image " + num(r.images[i].image) + ", which does not exist");
    for (std::size_t i = 0; i < r.spools.size(); ++i) {
        const SpoolRecord *sp = rd.spool(r.spools[i].spool);
        if (sp == nullptr) {
            if (full)
                error(report, "ref.missing", rr.entry, kItemResult, r.id,
                      "the spool selection names spool " + num(r.spools[i].spool) + ", which does not exist");
            continue;
        }
        if (sp->spool.has_available_length && r.spools[i].length_mm > sp->spool.available_length_mm)
            error(report, "spool.length_over_available", rr.entry, kItemResult, r.id,
                  "spool " + num(sp->spool.id) + ": " + real(r.spools[i].length_mm) + " mm selected, " +
                      real(sp->spool.available_length_mm) + " mm available");
    }
    if (full) {
        missing_entry(rr.entry, kItemResult, r.id, "thumbnail", r.thumbnail);
        if (r.has_settings)
            missing_entry(rr.entry, kItemResult, r.id, "settings", r.settings);
    }
}

void Validator::image_references(const ImageRecord &ir) {
    if (!full)
        return;
    const model::Image &im = ir.image;
    if (im.has_source)
        missing_entry(ir.entry, kItemImage, im.id, "source", im.source);
    if (im.has_target)
        missing_entry(ir.entry, kItemImage, im.id, "target", im.target);
    missing_entry(ir.entry, kItemImage, im.id, "thumbnail", im.thumbnail);
}

void Validator::references() {
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = rd.layouts().begin(); it != rd.layouts().end(); ++it)
        layout_references(it->second);
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = rd.results().begin(); it != rd.results().end(); ++it)
        result_references(it->second);
    for (std::map<std::uint32_t, ImageRecord>::const_iterator it = rd.images().begin(); it != rd.images().end(); ++it)
        image_references(it->second);
}

// ---- thumbnails ---------------------------------------------------------------------------------------

void Validator::thumbnail(const std::string &item_entry, ItemKind kind, std::uint32_t id, const std::string &entry) {
    const ZipEntry *e = rd.find_entry(entry);
    if (e == nullptr)
        return;
    const Limits &lim = limits();
    bool ok;
    if (e->size > lim.thumb_max_bytes) {
        ok = false;
    } else {
        try {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(e->size));
            const std::size_t got = bytes.empty() ? 0 : rd.read_entry(entry, 0, bytes.data(), bytes.size());
            ok = thumbnail_within_limits(bytes.data(), got);
        } catch (const FormatError &ex) {
            if (ex.code() != LOOMORA_ERR_CORRUPT)
                throw;
            return; // an unreadable entry is reported by the container pass
        }
    }
    if (!ok)
        warning(report, "warn.thumbnail_size", entry, kind, id,
                "the thumbnail is not a PNG within " + num(lim.thumb_max_side) + " x " + num(lim.thumb_max_side) +
                    " px and " + num(lim.thumb_max_bytes / 1024) + " KiB");
    (void)item_entry;
}

void Validator::thumbnails() {
    for (std::map<std::uint32_t, ImageRecord>::const_iterator it = rd.images().begin(); it != rd.images().end(); ++it)
        thumbnail(it->second.entry, kItemImage, it->first, it->second.image.thumbnail.entry);
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = rd.results().begin(); it != rd.results().end(); ++it)
        thumbnail(it->second.entry, kItemResult, it->first, it->second.result.thumbnail.entry);
}

// ---- layouts: nails against the frame, distances, steps -------------------------------------------------------

void Validator::nails_in_frame(const LayoutRecord &lr, const FrameRecord &fr, const model::NailTable &t) {
    const model::Layout &l = lr.layout;
    const model::Frame &f = fr.frame;
    const double tol = limits().nail_in_frame_tolerance_mm;
    std::size_t reported = 0, outside = 0;
    if (t.is_3d()) {
        if (!f.has_mesh)
            return;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t.triangle[i] < f.mesh.triangles)
                continue;
            ++outside;
            if (reported++ < kMaxIssuesPerRule)
                error(report, "nail.outside_frame", lr.nails_entry, kItemLayout, l.id,
                      "nail " + num(t.id[i]) + " is on triangle " + num(t.triangle[i]) + ", the mesh has " +
                          num(f.mesh.triangles));
        }
    } else {
        if (f.kind != model::kFrame2D || f.outline.polyline.size() < 3)
            return;
        const Polygon outline(f.outline.polyline);
        std::vector<Polygon> exclusions;
        for (std::size_t i = 0; i < f.exclusions.size(); ++i)
            if (f.exclusions[i].polyline.size() >= 3)
                exclusions.push_back(Polygon(f.exclusions[i].polyline));
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double x = t.x[i], y = t.y[i];
            const char *why = nullptr;
            if (!outline.contains(x, y) && !outline.near_boundary(x, y, tol)) {
                why = "is outside the outline";
            } else {
                for (std::size_t k = 0; k < exclusions.size(); ++k)
                    if (exclusions[k].contains(x, y) && !exclusions[k].near_boundary(x, y, tol)) {
                        why = "is inside an exclusion";
                        break;
                    }
            }
            if (why == nullptr)
                continue;
            ++outside;
            if (reported++ < kMaxIssuesPerRule)
                error(report, "nail.outside_frame", lr.nails_entry, kItemLayout, l.id,
                      "nail " + num(t.id[i]) + " at (" + real(x) + ", " + real(y) + ") " + why);
        }
    }
    if (outside > kMaxIssuesPerRule)
        error(report, "nail.outside_frame", lr.nails_entry, kItemLayout, l.id,
              num(outside - kMaxIssuesPerRule) + " more nails outside the frame are not listed");
}

void Validator::steps(const ResultRecord &rr, const std::vector<std::uint32_t> &sorted_ids, bool dense) {
    if (rr.sequence_entry.empty())
        return;
    const ZipEntry *e = rd.find_entry(rr.sequence_entry);
    if (e == nullptr)
        return;
    const model::Result &r = rr.result;
    std::uint64_t index = 0, bad_nails = 0, bad_spools = 0;
    try {
        EntrySource src(rd.zip(), *e);
        SequenceReader sr(src);
        SequenceStep st;
        while (sr.next(&st)) {
            const bool nail_ok = dense ? st.nail_id < sorted_ids.size()
                                       : std::binary_search(sorted_ids.begin(), sorted_ids.end(), st.nail_id);
            if (!nail_ok && bad_nails++ == 0)
                error(report, "ref.missing", rr.sequence_entry, kItemResult, r.id,
                      "step " + num(index) + " uses nail " + num(st.nail_id) + ", which is not a nail of layout " +
                          num(r.layout));
            if (st.spool_index >= r.spools.size() && bad_spools++ == 0)
                error(report, "ref.spool_not_selected", rr.sequence_entry, kItemResult, r.id,
                      "step " + num(index) + " uses spool index " + num(st.spool_index) + ", the selection has " +
                          num(r.spools.size()) + " spools");
            ++index;
        }
    } catch (const FormatError &ex) {
        if (ex.code() != LOOMORA_ERR_CORRUPT)
            throw;
        return; // reported by the container pass
    }
    if (bad_nails > 1)
        error(report, "ref.missing", rr.sequence_entry, kItemResult, r.id,
              num(bad_nails - 1) + " more steps use a nail the layout does not have");
    if (bad_spools > 1)
        error(report, "ref.spool_not_selected", rr.sequence_entry, kItemResult, r.id,
              num(bad_spools - 1) + " more steps use a spool outside the selection");
}

void Validator::layout(const LayoutRecord &lr) {
    const model::Layout &l = lr.layout;
    if (lr.nails_entry.empty())
        return;
    const FrameRecord *fr = rd.frame(l.frame);
    if (fr == nullptr)
        fr = rd.frame(lr.folder_frame);
    // The results that use this layout and have a sequence to check.
    std::vector<const ResultRecord *> users;
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = rd.results().begin(); it != rd.results().end();
         ++it)
        if (it->second.result.layout == l.id && !it->second.sequence_entry.empty())
            users.push_back(&it->second);
    model::NailTable t(fr != nullptr && fr->frame.kind == model::kFrame3D);
    try {
        rd.load_nails(l.id, t);
    } catch (const FormatError &ex) {
        if (ex.code() != LOOMORA_ERR_CORRUPT)
            throw;
        return; // reported by the container pass
    }
    if (fr != nullptr)
        nails_in_frame(lr, *fr, t);
    {
        std::size_t a = 0, b = 0;
        double d = 0;
        if (find_close_nails(t, limits().nail_overlap_tolerance_mm, a, b, d))
            warning(report, "warn.nail_distance", lr.nails_entry, kItemLayout, l.id,
                    "nails " + num(t.id[a]) + " and " + num(t.id[b]) + " are " + real(d) + " mm apart, closer than the sum of their radii (" +
                        real((t.diameter[a] + t.diameter[b]) / 2) + " mm)");
    }
    if (users.empty())
        return;
    std::vector<std::uint32_t> ids(t.id);
    std::sort(ids.begin(), ids.end());
    const bool dense = !ids.empty() && ids.back() == ids.size() - 1 &&
                       std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    for (std::size_t i = 0; i < users.size(); ++i)
        steps(*users[i], ids, dense);
}

void Validator::layouts() {
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = rd.layouts().begin(); it != rd.layouts().end(); ++it)
        layout(it->second);
}

// ---- thresholds ------------------------------------------------------------------------------------------------

// A time stamp that has the right shape but is not a real date and time (2026-02-31, second 60) still
// opens: the warning names the entry and the field.
void Validator::timestamp(const std::string &entry, ItemKind kind, std::uint32_t id, const char *field,
                          const std::string &value) {
    if (model::is_iso8601_utc(value) && !model::is_calendar_time(value))
        warning(report, "warn.timestamp", entry, kind, id,
                std::string(field) + " " + value + " is not a real date and time (checked: days of the month, hour, minute, second 00-59)");
}

void Validator::timestamps() {
    const model::Manifest &m = rd.manifest();
    timestamp(kManifestEntry, kItemNone, 0, "created_at", m.created_at);
    timestamp(kManifestEntry, kItemNone, 0, "modified_at", m.modified_at);
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = rd.results().begin(); it != rd.results().end();
         ++it)
        timestamp(it->second.entry, kItemResult, it->second.result.id, "created_at", it->second.result.created_at);
}

void Validator::thresholds() {
    ThresholdFacts facts;
    facts.frames = rd.frames().size();
    facts.layout_items = rd.layouts().size();
    facts.images = rd.images().size();
    facts.spools = rd.spools().size();
    facts.results = rd.results().size();
    const std::vector<ZipEntry> &entries = rd.zip().entries();
    for (std::size_t i = 0; i < entries.size(); ++i)
        facts.total_bytes += entries[i].size;
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = rd.layouts().begin(); it != rd.layouts().end(); ++it) {
        ThresholdFacts::Layout l;
        l.id = it->first;
        l.frame = it->second.layout.frame;
        l.nails = std::max(it->second.layout.nail_count, it->second.nails_parsed);
        facts.layouts.push_back(l);
    }
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = rd.results().begin(); it != rd.results().end(); ++it) {
        ThresholdFacts::Result r;
        r.id = it->first;
        r.steps = it->second.result.steps;
        facts.result_list.push_back(r);
    }
    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = rd.frames().begin(); it != rd.frames().end(); ++it)
        if (it->second.frame.has_mesh) {
            ThresholdFacts::Mesh m;
            m.frame = it->first;
            m.triangles = it->second.frame.mesh.triangles;
            facts.meshes.push_back(m);
        }
    check_thresholds(facts, report);
}

} // namespace

void check_thresholds(const ThresholdFacts &facts, Report &report) {
    const Limits &l = limits();
    const std::string manifest = kManifestEntry;
    warn_count(report, "frames", facts.frames, l.warn_items_per_kind, manifest, kItemNone, 0);
    warn_count(report, "layouts", facts.layout_items, l.warn_items_per_kind, manifest, kItemNone, 0);
    warn_count(report, "images", facts.images, l.warn_items_per_kind, manifest, kItemNone, 0);
    warn_count(report, "spools", facts.spools, l.warn_items_per_kind, manifest, kItemNone, 0);
    warn_count(report, "results", facts.results, l.warn_items_per_kind, manifest, kItemNone, 0);
    warn_count(report, "bytes in the project once decompressed", facts.total_bytes, l.warn_decompressed_total, "",
               kItemNone, 0);
    for (std::size_t i = 0; i < facts.layouts.size(); ++i) {
        const ThresholdFacts::Layout &x = facts.layouts[i];
        warn_count(report, "nails in layout " + num(x.id), x.nails, l.warn_nails_per_layout,
                   "frames/" + num(x.frame) + "/layouts/" + num(x.id) + "/nails.json", kItemLayout, x.id);
    }
    for (std::size_t i = 0; i < facts.result_list.size(); ++i) {
        const ThresholdFacts::Result &x = facts.result_list[i];
        warn_count(report, "steps in result " + num(x.id), x.steps, l.warn_steps_per_result,
                   "results/" + num(x.id) + "/sequence.bin", kItemResult, x.id);
    }
    for (std::size_t i = 0; i < facts.meshes.size(); ++i) {
        const ThresholdFacts::Mesh &x = facts.meshes[i];
        warn_count(report, "triangles in the mesh of frame " + num(x.frame), x.triangles, l.warn_triangles_per_mesh,
                   "frames/" + num(x.frame) + "/mesh.bin", kItemFrame, x.frame);
    }
}

void validate_semantic(const ProjectReader &reader, Report &report) {
    Validator v(reader, report);
    v.references();
    v.frames();
    if (v.full)
        v.layouts();
    v.thumbnails();
    v.timestamps();
    v.thresholds();
}

} // namespace format
} // namespace loomora

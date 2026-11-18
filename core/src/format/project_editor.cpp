#include "format/project_editor.hpp"

#include <algorithm>

#include "loomora.h"
#include "errors.hpp"
#include "format/codec/image_meta.hpp"
#include "format/codec/mesh_codec.hpp"
#include "format/io/memory_io.hpp"
#include "format/item_json.hpp"
#include "utf8.hpp"

namespace loomora {
namespace format {

namespace {

const std::uint32_t kNoMoreIds = 0xFFFFFFFFu;

std::string dec(std::uint64_t value) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

[[noreturn]] void invalid_item(const char *code, const std::string &entry, ItemKind kind, std::uint32_t id,
                               const std::string &message) {
    Issue issue;
    issue.severity = kError;
    issue.code = code;
    issue.entry = entry;
    issue.item_kind = kind;
    issue.item_id = id;
    issue.message = message;
    throw FormatError(LOOMORA_ERR_INVALID_ITEM, issue);
}

[[noreturn]] void not_found(const std::string &message) { throw FormatError(LOOMORA_ERR_NOT_FOUND, message); }

// Throws the first error of `report` as an invalid item.
void throw_if_errors(const Report &report) {
    for (std::size_t i = 0; i < report.count(); ++i)
        if (report.at(i).severity == kError)
            throw FormatError(LOOMORA_ERR_INVALID_ITEM, report.at(i));
}

void require_utf8(const std::string &text, const char *what) {
    try {
        decode_utf8(text);
    } catch (const InvalidUtf8 &) {
        throw FormatError(LOOMORA_ERR_INVALID, std::string(what) + " is not valid UTF-8");
    }
}

// The next identifier of a kind, without taking it (the counter moves once the item is staged).
std::uint32_t peek_id(std::uint32_t counter, const char *what) {
    if (counter == 0 || counter == kNoMoreIds)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, std::string("no identifier left for a ") + what);
    return counter;
}

std::vector<std::uint8_t> clean_thumbnail(const ByteSpan &thumb, ItemKind kind, std::uint32_t id,
                                          const std::string &entry) {
    if (!thumb.present())
        invalid_item("schema.missing", entry, kind, id, "a thumbnail (PNG) is required");
    std::vector<std::uint8_t> clean = strip_image_metadata(thumb.data, thumb.len, kImagePng);
    require_thumbnail(clean.data(), clean.size());
    return clean;
}


ItemKind item_kind_of(int kind) {
    if (kind < LOOMORA_KIND_FRAME || kind > LOOMORA_KIND_RESULT)
        not_found("unknown item kind " + dec(static_cast<std::uint64_t>(kind < 0 ? 0 : kind)));
    return static_cast<ItemKind>(kind);
}

bool shape_equal(const model::Shape &a, const model::Shape &b) {
    if (a.type != b.type || a.params_json != b.params_json || a.polyline.size() != b.polyline.size())
        return false;
    for (std::size_t i = 0; i < a.polyline.size(); ++i)
        if (a.polyline[i].x != b.polyline[i].x || a.polyline[i].y != b.polyline[i].y)
            return false;
    return true;
}

// The fields of a frame that results depend on (data-model.md, "Which changes count").
bool frame_geometry_differs(const model::Frame &a, const model::Frame &b) {
    if (a.width_mm != b.width_mm || a.height_mm != b.height_mm || a.depth_mm != b.depth_mm ||
        a.surface != b.surface || a.body != b.body || !shape_equal(a.outline, b.outline) ||
        a.exclusions.size() != b.exclusions.size())
        return true;
    for (std::size_t i = 0; i < a.exclusions.size(); ++i)
        if (!shape_equal(a.exclusions[i], b.exclusions[i]))
            return true;
    return false;
}

bool same_view(const model::View &a, const model::View &b) {
    for (int i = 0; i < 3; ++i)
        if (a.position[i] != b.position[i] || a.target[i] != b.target[i] || a.up[i] != b.up[i])
            return false;
    return a.projection == b.projection && a.size == b.size && a.fov_deg == b.fov_deg && a.weight == b.weight;
}

bool same_images(const std::vector<model::ResultImage> &a, const std::vector<model::ResultImage> &b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].image != b[i].image || a[i].has_view != b[i].has_view ||
            (a[i].has_view && !same_view(a[i].view, b[i].view)))
            return false;
    return true;
}

bool same_spools(const std::vector<model::ResultSpool> &a, const std::vector<model::ResultSpool> &b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].spool != b[i].spool || a[i].length_mm != b[i].length_mm)
            return false;
    return true;
}

// The nails of two tables compared by identifier (order and labels do not count): same identifiers,
// and every field of every nail equal.
bool nails_differ(const model::NailTable &a, const model::NailTable &b) {
    if (a.is_3d() != b.is_3d() || a.size() != b.size())
        return true;
    std::map<std::uint32_t, std::size_t> pos;
    for (std::size_t i = 0; i < a.size(); ++i)
        pos[a.id[i]] = i;
    for (std::size_t j = 0; j < b.size(); ++j) {
        std::map<std::uint32_t, std::size_t>::const_iterator it = pos.find(b.id[j]);
        if (it == pos.end())
            return true;
        const std::size_t i = it->second;
        if (a.x[i] != b.x[j] || a.y[i] != b.y[j] || a.diameter[i] != b.diameter[j] || a.kind[i] != b.kind[j])
            return true;
        if (a.is_3d()) {
            if (a.z[i] != b.z[j] || a.length[i] != b.length[j] || a.side[i] != b.side[j] ||
                a.triangle[i] != b.triangle[j] || a.u[i] != b.u[j] || a.v[i] != b.v[j])
                return true;
            for (int k = 0; k < 3; ++k)
                if (a.axis[3 * i + static_cast<std::size_t>(k)] != b.axis[3 * j + static_cast<std::size_t>(k)])
                    return true;
        }
    }
    return false;
}

// Encodes a mesh given by the caller into the bytes of mesh.bin (checked while it is written).
void encode_mesh(const MeshInput &m, std::vector<std::uint8_t> &out, const std::string &entry, std::uint32_t frame) {
    if (m.vertices < 3 || m.triangles < 1)
        invalid_item("schema.value", entry, kItemFrame, frame, "a mesh needs at least 3 vertices and 1 triangle");
    if (m.vertices > 0xFFFFFFFFull)
        invalid_item("schema.value", entry, kItemFrame, frame, "a mesh holds at most 4,294,967,295 vertices");
    if (m.xyz == nullptr || m.abc == nullptr || (m.normals_flag && m.normals == nullptr) ||
        (m.uvs_flag && m.uvs == nullptr))
        invalid_item("schema.missing", entry, kItemFrame, frame, "the mesh arrays the flags announce are missing");
    MemorySink sink;
    MeshWriter w(sink, m.vertices, m.triangles, m.normals_flag, m.uvs_flag);
    const std::size_t vertices = static_cast<std::size_t>(m.vertices);
    const std::size_t triangles = static_cast<std::size_t>(m.triangles);
    w.write_positions(m.xyz, vertices);
    w.write_indices(m.abc, triangles);
    if (m.normals_flag)
        w.write_normals(m.normals, vertices);
    if (m.uvs_flag)
        w.write_uvs(m.uvs, vertices);
    w.finish();
    sink.commit();
    out = sink.take();
}

// Checks a frame that may still wait for its mesh (surface mesh before frame_set_mesh).
void check_staged_frame(const model::Frame &f, const std::string &entry) {
    model::Frame copy = f;
    if (copy.kind == model::kFrame3D && copy.surface == model::kSurfaceMesh && !copy.has_mesh)
        copy.has_mesh = true;
    Report report;
    model::check_frame(copy, entry, report);
    throw_if_errors(report);
}

std::string entry_extension(const std::string &entry) {
    const std::size_t dot = entry.find_last_of('.');
    return dot == std::string::npos ? std::string() : entry.substr(dot + 1);
}

} // namespace

// ---- entry names ---------------------------------------------------------------------------------

std::string frame_entry(std::uint32_t f) { return "frames/" + dec(f) + "/frame.json"; }
std::string layout_entry(std::uint32_t f, std::uint32_t l) {
    return "frames/" + dec(f) + "/layouts/" + dec(l) + "/layout.json";
}
std::string nails_entry(std::uint32_t f, std::uint32_t l) {
    return "frames/" + dec(f) + "/layouts/" + dec(l) + "/nails.json";
}
std::string mesh_entry_name(std::uint32_t f) { return "frames/" + dec(f) + "/mesh.bin"; }
std::string image_entry(std::uint32_t i) { return "images/" + dec(i) + "/image.json"; }
std::string image_source_entry(std::uint32_t i, const std::string &ext) {
    return "images/" + dec(i) + "/source." + ext;
}
std::string image_target_entry(std::uint32_t i) { return "images/" + dec(i) + "/target.png"; }
std::string image_thumb_entry(std::uint32_t i) { return "images/" + dec(i) + "/thumb.png"; }
std::string result_entry(std::uint32_t r) { return "results/" + dec(r) + "/result.json"; }
std::string result_sequence_entry(std::uint32_t r) { return "results/" + dec(r) + "/sequence.bin"; }
std::string result_thumb_entry(std::uint32_t r) { return "results/" + dec(r) + "/thumb.png"; }
std::string result_settings_entry(std::uint32_t r) { return "results/" + dec(r) + "/settings.json"; }

// ---- the editor ----------------------------------------------------------------------------------

ProjectEditor::ProjectEditor(const std::string &project_id, const std::string &title,
                             const std::string &created_at) {
    if (!model::is_uuid4(project_id))
        throw FormatError(LOOMORA_ERR_INVALID, "project_id must be a lowercase UUID v4");
    if (!model::is_iso8601_utc(created_at))
        throw FormatError(LOOMORA_ERR_INVALID, "created_at must be an ISO 8601 UTC time (YYYY-MM-DDTHH:MM:SSZ)");
    if (title.size() > model::kMaxTitleBytes)
        throw FormatError(LOOMORA_ERR_INVALID, "title is longer than 1,024 bytes");
    require_utf8(title, "title");
    manifest_.project_id = project_id;
    manifest_.title = title;
    manifest_.created_at = created_at;
    manifest_.modified_at = created_at;
    manifest_.profile = model::kProfileFull;
    manifest_.editable = true;
    manifest_.generator.application = "Loomora";
    manifest_.generator.version = "0.1.0";
    manifest_.generator.platform = "unspecified";
    manifest_.generator.architecture = "unspecified";
}

void ProjectEditor::set_title(const std::string &title) {
    if (title.size() > model::kMaxTitleBytes)
        throw FormatError(LOOMORA_ERR_INVALID, "title is longer than 1,024 bytes");
    require_utf8(title, "title");
    manifest_.title = title;
}

void ProjectEditor::set_generator(const model::Generator &generator) {
    require_utf8(generator.application + generator.version + generator.platform + generator.architecture,
                 "generator");
    manifest_.generator = generator;
}

std::uint32_t ProjectEditor::add_spool(const model::Spool &spool) {
    model::Spool s = spool;
    s.id = peek_id(manifest_.next_id.spool, "spool");
    Report report;
    model::check_spool(s, "spools.json", report);
    throw_if_errors(report);
    spools_[s.id] = s;
    manifest_.order.spools.push_back(s.id);
    ++manifest_.next_id.spool;
    spools_kept_ = false;
    return s.id;
}

std::uint32_t ProjectEditor::add_frame(const model::Frame &frame) {
    model::Frame f = frame;
    f.id = peek_id(manifest_.next_id.frame, "frame");
    // A "mesh" object in the JSON only carries the optional description (watertight, manifold, source,
    // construction); the counts and flags come with frame_set_mesh, so the frame has no mesh yet.
    f.has_mesh = false;
    f.mesh.vertices = f.mesh.triangles = 0;
    f.mesh.has_normals = f.mesh.has_uvs = false;
    f.mesh.attached_source.clear();
    check_staged_frame(f, frame_entry(f.id));
    StagedFrame staged;
    staged.frame = f;
    frames_[f.id] = staged;
    manifest_.order.frames.push_back(f.id);
    ++manifest_.next_id.frame;
    return f.id;
}

std::uint32_t ProjectEditor::add_layout(std::uint32_t frame, const model::Layout &layout,
                                        const model::NailTable &nails) {
    std::map<std::uint32_t, StagedFrame>::const_iterator fit = frames_.find(frame);
    if (fit == frames_.end())
        not_found("frame " + dec(frame) + " does not exist");
    StagedLayout staged;
    staged.layout = layout;
    staged.layout.id = peek_id(manifest_.next_id.layout, "layout");
    staged.layout.frame = frame;
    staged.layout.nail_count = nails.size();
    const std::string entry = layout_entry(frame, staged.layout.id);
    if (nails.is_3d() != (fit->second.frame.kind == model::kFrame3D))
        invalid_item("schema.value", entry, kItemLayout, staged.layout.id,
                     "the nails and the frame must have the same dimension (2D or 3D)");
    Report report;
    model::check_layout(staged.layout, entry, report);
    model::check_nails(nails, nails_entry(frame, staged.layout.id), staged.layout.id, report,
                       triangle_bound(frame));
    throw_if_errors(report);
    staged.nails = nails;
    const std::uint32_t id = staged.layout.id;
    layouts_[id] = staged;
    manifest_.order.layouts.push_back(id);
    ++manifest_.next_id.layout;
    return id;
}

void ProjectEditor::layout_set_label(std::uint32_t layout, std::uint32_t nail_id, const std::string *label) {
    std::map<std::uint32_t, StagedLayout>::iterator it = layouts_.find(layout);
    if (it == layouts_.end())
        not_found("layout " + dec(layout) + " does not exist");
    layout_nails(layout); // loads the nails of a layout that is still in the old archive
    model::NailTable &t = it->second.nails;
    std::size_t pos = t.id.size();
    for (std::size_t i = 0; i < t.id.size(); ++i) {
        if (t.id[i] == nail_id) {
            pos = i;
            break;
        }
    }
    if (pos == t.id.size())
        not_found("nail " + dec(nail_id) + " does not exist in layout " + dec(layout));
    if (label != nullptr) {
        if (label->size() > model::kMaxLabelBytes)
            invalid_item("schema.value", nails_entry(it->second.layout.frame, layout), kItemLayout, layout,
                         "a nail label is at most 64 bytes");
        require_utf8(*label, "label");
    }
    it->second.nails_kept = false;
    const std::uint32_t key = static_cast<std::uint32_t>(pos);
    const std::size_t at = static_cast<std::size_t>(
        std::lower_bound(t.label_index.begin(), t.label_index.end(), key) - t.label_index.begin());
    const bool present = at < t.label_index.size() && t.label_index[at] == key;
    if (label == nullptr) {
        if (present) {
            t.label_index.erase(t.label_index.begin() + static_cast<std::ptrdiff_t>(at));
            t.label_text.erase(t.label_text.begin() + static_cast<std::ptrdiff_t>(at));
        }
    } else if (present) {
        t.label_text[at] = *label;
    } else {
        t.label_text.insert(t.label_text.begin() + static_cast<std::ptrdiff_t>(at), *label);
        t.label_index.insert(t.label_index.begin() + static_cast<std::ptrdiff_t>(at), key);
    }
}

std::uint32_t ProjectEditor::add_image(const model::Image &image, const ByteSpan &source, const ByteSpan &target,
                                       const ByteSpan &thumbnail) {
    StagedImage staged;
    model::Image &img = staged.image;
    img = image;
    img.id = peek_id(manifest_.next_id.image, "image");
    const std::string entry = image_entry(img.id);

    if (!source.present())
        invalid_item("schema.missing", entry, kItemImage, img.id, "a full project keeps the source picture");
    const ImageFormat fmt = detect_image_format(source.data, source.len);
    if (fmt == kImageUnknown)
        invalid_item("schema.value", entry, kItemImage, img.id, "the source is not a PNG, JPEG or WebP picture");
    staged.source.bytes = strip_image_metadata(source.data, source.len, fmt);
    staged.source_ext = fmt == kImageJpeg ? "jpg" : image_format_name(fmt);
    if (target.present())
        staged.target.bytes = strip_image_metadata(target.data, target.len, kImagePng);
    staged.thumbnail.bytes = clean_thumbnail(thumbnail, kItemImage, img.id, entry);

    img.has_source = true;
    img.source.entry = image_source_entry(img.id, staged.source_ext);
    img.source.format = image_format_name(fmt);
    img.source.size = static_cast<std::int64_t>(staged.source.bytes.size());
    img.has_target = !staged.target.bytes.empty();
    if (img.has_target) {
        img.target.entry = image_target_entry(img.id);
        img.target.format = "png";
        img.target.size = static_cast<std::int64_t>(staged.target.bytes.size());
    }
    img.thumbnail.entry = image_thumb_entry(img.id);

    Report report;
    model::check_image(img, model::kProfileFull, entry, report);
    throw_if_errors(report);
    const std::uint32_t id = img.id;
    images_[id] = std::move(staged);
    manifest_.order.images.push_back(id);
    ++manifest_.next_id.image;
    return id;
}

std::uint32_t ProjectEditor::add_result(const model::Result &result, StepSource &steps, std::uint64_t step_count,
                                        const ByteSpan &thumbnail, const std::string &settings_json) {
    StagedResult staged;
    model::Result &r = staged.result;
    r = result;
    r.id = peek_id(manifest_.next_id.result, "result");
    const std::string entry = result_entry(r.id);
    if (step_count > 0xFFFFFFFFull)
        invalid_item("seq.count", entry, kItemResult, r.id, "a sequence holds at most 4,294,967,295 steps");
    r.steps = step_count;

    // References.
    std::map<std::uint32_t, StagedFrame>::const_iterator fit = frames_.find(r.frame);
    if (fit == frames_.end())
        invalid_item("ref.missing", entry, kItemResult, r.id, "frame " + dec(r.frame) + " does not exist");
    std::map<std::uint32_t, StagedLayout>::const_iterator lit = layouts_.find(r.layout);
    if (lit == layouts_.end())
        invalid_item("ref.missing", entry, kItemResult, r.id, "layout " + dec(r.layout) + " does not exist");
    if (lit->second.layout.frame != r.frame)
        invalid_item("ref.layout_of_other_frame", entry, kItemResult, r.id,
                     "layout " + dec(r.layout) + " does not belong to frame " + dec(r.frame));
    for (std::size_t i = 0; i < r.images.size(); ++i)
        if (images_.find(r.images[i].image) == images_.end())
            invalid_item("ref.missing", entry, kItemResult, r.id, "image " + dec(r.images[i].image) + " does not exist");
    for (std::size_t i = 0; i < r.spools.size(); ++i) {
        std::map<std::uint32_t, model::Spool>::const_iterator sit = spools_.find(r.spools[i].spool);
        if (sit == spools_.end())
            invalid_item("ref.missing", entry, kItemResult, r.id, "spool " + dec(r.spools[i].spool) + " does not exist");
        if (sit->second.has_available_length && r.spools[i].length_mm > sit->second.available_length_mm)
            invalid_item("spool.length_over_available", entry, kItemResult, r.id,
                         "the length chosen for spool " + dec(r.spools[i].spool) + " is above its available length");
    }

    r.thumbnail.entry = result_thumb_entry(r.id);
    r.thumbnail.format.clear();
    r.thumbnail.size = -1;
    if (!settings_json.empty()) {
        const std::string compact = compact_json_object(settings_json, "settings");
        staged.settings.bytes.assign(compact.begin(), compact.end());
        r.has_settings = true;
        r.settings.entry = result_settings_entry(r.id);
    } else {
        r.has_settings = false;
    }
    Report report;
    model::check_result(r, fit->second.frame.kind, model::kProfileFull, entry, report);
    throw_if_errors(report);
    staged.thumbnail.bytes = clean_thumbnail(thumbnail, kItemResult, r.id, entry);

    // The sequence: every record against the layout and the selection, then encoded.
    std::vector<std::uint32_t> nail_ids(layout_nails(r.layout).id);
    std::sort(nail_ids.begin(), nail_ids.end());
    std::vector<unsigned char> attached(r.spools.size(), 0);
    MemorySink sink;
    SequenceWriter writer(sink, r.levels, static_cast<std::uint32_t>(step_count));
    const std::string seq_entry = result_sequence_entry(r.id);
    SequenceStep s;
    for (std::uint64_t n = 0; n < step_count; ++n) {
        if (!steps.next(&s))
            invalid_item("seq.count", seq_entry, kItemResult, r.id, "fewer steps than the count given");
        if (!std::binary_search(nail_ids.begin(), nail_ids.end(), s.nail_id))
            invalid_item("ref.missing", seq_entry, kItemResult, r.id,
                         "step " + dec(n) + " uses nail " + dec(s.nail_id) + ", which is not in the layout");
        if (s.spool_index >= r.spools.size())
            invalid_item("ref.spool_not_selected", seq_entry, kItemResult, r.id,
                         "step " + dec(n) + " uses spool index " + dec(s.spool_index) + " outside the selection");
        unsigned char &on = attached[s.spool_index];
        if (s.type == kStepStart) {
            on = 1;
        } else if (!on) {
            invalid_item("seq.order", seq_entry, kItemResult, r.id,
                         "step " + dec(n) + " continues a spool whose string is not attached");
        } else if (s.type == kStepEnd) {
            on = 0;
        }
        writer.add(s);
    }
    writer.finish();
    sink.commit();
    staged.sequence.bytes = sink.take();

    const std::uint32_t id = r.id;
    results_[id] = std::move(staged);
    manifest_.order.results.push_back(id);
    ++manifest_.next_id.result;
    return id;
}

// ---- opening an existing project -------------------------------------------------------------------

ProjectEditor::ProjectEditor(std::shared_ptr<const ProjectReader> base) : base_(base) {
    if (!base_)
        throw FormatError(LOOMORA_ERR_INVALID, "no project to edit");
    const ProjectReader &r = *base_;
    if (r.is_light())
        throw FormatError(LOOMORA_ERR_READ_ONLY_PROJECT, "This project is a light copy and cannot be edited");
    manifest_ = r.manifest();
    manifest_.format = model::FormatId();
    manifest_.min_reader = model::Version();
    manifest_.profile = model::kProfileFull;
    manifest_.editable = true;
    manifest_.counts = model::Counts();
    manifest_.entries.clear();
    manifest_.required_features.clear();

    for (std::map<std::uint32_t, SpoolRecord>::const_iterator it = r.spools().begin(); it != r.spools().end(); ++it)
        spools_[it->first] = it->second.spool;
    spools_kept_ = true;
    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = r.frames().begin(); it != r.frames().end(); ++it) {
        StagedFrame sf;
        sf.frame = it->second.frame;
        sf.json_kept = true;
        if (sf.frame.has_mesh)
            sf.mesh.from_archive = it->second.mesh_entry;
        if (sf.frame.has_mesh && !sf.frame.mesh.attached_source.empty()) {
            if (r.find_entry(sf.frame.mesh.attached_source) != nullptr)
                sf.attached.from_archive = sf.frame.mesh.attached_source;
            else
                sf.frame.mesh.attached_source.clear();
        }
        frames_[it->first] = sf;
    }
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = r.layouts().begin(); it != r.layouts().end();
         ++it) {
        StagedLayout sl;
        sl.layout = it->second.layout;
        sl.json_kept = true;
        sl.nails_kept = true;
        sl.nails_loaded = false;
        layouts_[it->first] = sl;
    }
    for (std::map<std::uint32_t, ImageRecord>::const_iterator it = r.images().begin(); it != r.images().end(); ++it) {
        StagedImage si;
        si.image = it->second.image;
        si.json_kept = true;
        if (si.image.has_source) {
            si.source.from_archive = si.image.source.entry;
            si.source_ext = entry_extension(si.image.source.entry);
        }
        if (si.image.has_target)
            si.target.from_archive = si.image.target.entry;
        si.thumbnail.from_archive = si.image.thumbnail.entry;
        images_[it->first] = si;
    }
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = r.results().begin(); it != r.results().end();
         ++it) {
        StagedResult sr;
        sr.result = it->second.result;
        sr.json_kept = true;
        sr.sequence.from_archive = it->second.sequence_entry;
        sr.thumbnail.from_archive = sr.result.thumbnail.entry;
        if (sr.result.has_settings)
            sr.settings.from_archive = sr.result.settings.entry;
        results_[it->first] = sr;
    }
    const std::vector<ZipEntry> &entries = r.zip().entries();
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (classify_entry(entries[i].name).role == kRoleUnknown)
            unknown_.push_back(entries[i].name);
}

// ---- lookups ---------------------------------------------------------------------------------------

bool ProjectEditor::frame_is_3d(std::uint32_t frame) const {
    std::map<std::uint32_t, StagedFrame>::const_iterator it = frames_.find(frame);
    if (it == frames_.end())
        not_found("frame " + dec(frame) + " does not exist");
    return it->second.frame.kind == model::kFrame3D;
}

const model::NailTable &ProjectEditor::layout_nails(std::uint32_t layout) const {
    std::map<std::uint32_t, StagedLayout>::const_iterator it = layouts_.find(layout);
    if (it == layouts_.end())
        not_found("layout " + dec(layout) + " does not exist");
    const StagedLayout &l = it->second;
    if (!l.nails_loaded) {
        model::NailTable table;
        base_->load_nails(layout, table);
        l.nails = std::move(table);
        l.nails_loaded = true;
    }
    return l.nails;
}

std::uint64_t ProjectEditor::triangle_bound(std::uint32_t frame) const {
    std::map<std::uint32_t, StagedFrame>::const_iterator it = frames_.find(frame);
    if (it != frames_.end() && it->second.frame.has_mesh)
        return it->second.frame.mesh.triangles;
    return model::kAnyTriangleCount;
}

std::vector<std::uint32_t> ProjectEditor::layouts_of_frame(std::uint32_t frame) const {
    std::vector<std::uint32_t> out;
    for (std::map<std::uint32_t, StagedLayout>::const_iterator it = layouts_.begin(); it != layouts_.end(); ++it)
        if (it->second.layout.frame == frame)
            out.push_back(it->first);
    return out;
}

std::vector<std::uint32_t> ProjectEditor::results_of_frame(std::uint32_t frame) const {
    std::vector<std::uint32_t> out;
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it)
        if (it->second.result.frame == frame)
            out.push_back(it->first);
    return out;
}

std::vector<std::uint32_t> ProjectEditor::results_of_layout(std::uint32_t layout) const {
    std::vector<std::uint32_t> out;
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it)
        if (it->second.result.layout == layout)
            out.push_back(it->first);
    return out;
}

std::vector<std::uint32_t> ProjectEditor::results_of_image(std::uint32_t image) const {
    std::vector<std::uint32_t> out;
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it) {
        const std::vector<model::ResultImage> &v = it->second.result.images;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].image == image) {
                out.push_back(it->first);
                break;
            }
        }
    }
    return out;
}

std::vector<std::uint32_t> ProjectEditor::results_of_spool(std::uint32_t spool) const {
    std::vector<std::uint32_t> out;
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it) {
        const std::vector<model::ResultSpool> &v = it->second.result.spools;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].spool == spool) {
                out.push_back(it->first);
                break;
            }
        }
    }
    return out;
}

std::vector<ItemRef> ProjectEditor::dependents(int kind, std::uint32_t id, int operation) const {
    if (operation != kOpRemove && operation != kOpChange)
        throw FormatError(LOOMORA_ERR_INVALID, "operation must be LOOMORA_OP_REMOVE or LOOMORA_OP_CHANGE");
    item_kind_of(kind);
    std::vector<ItemRef> out;
    std::vector<std::uint32_t> layouts, results;
    switch (kind) {
    case LOOMORA_KIND_FRAME:
        if (frames_.find(id) == frames_.end())
            not_found("frame " + dec(id) + " does not exist");
        // Removing a frame removes its layouts; changing its geometry keeps them.
        if (operation == kOpRemove)
            layouts = layouts_of_frame(id);
        results = results_of_frame(id);
        break;
    case LOOMORA_KIND_LAYOUT:
        if (layouts_.find(id) == layouts_.end())
            not_found("layout " + dec(id) + " does not exist");
        results = results_of_layout(id);
        break;
    case LOOMORA_KIND_IMAGE:
        if (images_.find(id) == images_.end())
            not_found("image " + dec(id) + " does not exist");
        results = results_of_image(id);
        break;
    case LOOMORA_KIND_SPOOL:
        if (spools_.find(id) == spools_.end())
            not_found("spool " + dec(id) + " does not exist");
        results = results_of_spool(id);
        break;
    default: // a result has no dependents
        if (results_.find(id) == results_.end())
            not_found("result " + dec(id) + " does not exist");
        break;
    }
    for (std::size_t i = 0; i < layouts.size(); ++i)
        out.push_back(ItemRef(LOOMORA_KIND_LAYOUT, layouts[i]));
    for (std::size_t i = 0; i < results.size(); ++i)
        out.push_back(ItemRef(LOOMORA_KIND_RESULT, results[i]));
    return out;
}

void ProjectEditor::require_confirmed(const std::vector<ItemRef> &deps, bool confirm, int kind, std::uint32_t id) const {
    if (deps.empty() || confirm)
        return;
    static const char *const names[] = {"", "frame", "layout", "image", "spool", "result"};
    std::size_t layouts = 0, results = 0;
    for (std::size_t i = 0; i < deps.size(); ++i)
        (deps[i].kind == LOOMORA_KIND_LAYOUT ? layouts : results)++;
    std::string msg = std::string(names[kind]) + " " + dec(id) + " has dependents (";
    if (layouts != 0)
        msg += dec(layouts) + " layout(s)" + (results != 0 ? ", " : "");
    if (results != 0)
        msg += dec(results) + " result(s)";
    msg += "): confirm to remove them";
    throw FormatError(LOOMORA_ERR_HAS_DEPENDENTS, msg);
}

void ProjectEditor::erase_order(std::vector<std::uint32_t> &list, std::uint32_t id) {
    list.erase(std::remove(list.begin(), list.end(), id), list.end());
}

void ProjectEditor::remove_results(const std::vector<std::uint32_t> &ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        results_.erase(ids[i]);
        erase_order(manifest_.order.results, ids[i]);
    }
}

void ProjectEditor::remove_layout(std::uint32_t layout) {
    layouts_.erase(layout);
    erase_order(manifest_.order.layouts, layout);
}

bool ProjectEditor::blob_equals(const Blob &blob, const std::vector<std::uint8_t> &bytes) const {
    if (!blob.in_archive())
        return blob.bytes == bytes;
    const ZipEntry *e = base_->find_entry(blob.from_archive);
    if (e == nullptr || e->size != bytes.size())
        return false;
    std::unique_ptr<Source> src = base_->open_entry_source(blob.from_archive);
    std::vector<std::uint8_t> chunk(64 * 1024);
    std::uint64_t at = 0;
    while (at < bytes.size()) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes.size() - at));
        if (src->read_at(at, &chunk[0], want) != want ||
            !std::equal(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(want),
                        bytes.begin() + static_cast<std::ptrdiff_t>(at)))
            return false;
        at += want;
    }
    return true;
}

// ---- 3D --------------------------------------------------------------------------------------------

void ProjectEditor::frame_set_mesh(std::uint32_t frame, const MeshInput &mesh) {
    std::map<std::uint32_t, StagedFrame>::iterator it = frames_.find(frame);
    if (it == frames_.end())
        not_found("frame " + dec(frame) + " does not exist");
    StagedFrame &sf = it->second;
    const std::string entry = frame_entry(frame);
    if (sf.frame.kind != model::kFrame3D)
        invalid_item("schema.value", entry, kItemFrame, frame, "a 2D frame has no mesh");
    if (sf.frame.has_mesh)
        invalid_item("schema.value", entry, kItemFrame, frame, "the frame has a mesh: use replace_frame_mesh");
    std::vector<std::uint8_t> encoded;
    encode_mesh(mesh, encoded, mesh_entry_name(frame), frame);
    sf.mesh.bytes = std::move(encoded);
    sf.mesh.from_archive.clear();
    sf.frame.has_mesh = true;
    sf.frame.mesh.vertices = mesh.vertices;
    sf.frame.mesh.triangles = mesh.triangles;
    sf.frame.mesh.has_normals = mesh.normals_flag;
    sf.frame.mesh.has_uvs = mesh.uvs_flag;
    sf.json_kept = false;
}

void ProjectEditor::replace_frame_mesh(std::uint32_t frame, const MeshInput &mesh, bool confirm) {
    std::map<std::uint32_t, StagedFrame>::iterator it = frames_.find(frame);
    if (it == frames_.end())
        not_found("frame " + dec(frame) + " does not exist");
    StagedFrame &sf = it->second;
    const std::string entry = frame_entry(frame);
    if (!sf.frame.has_mesh)
        invalid_item("schema.value", entry, kItemFrame, frame, "the frame has no mesh yet: use frame_set_mesh");
    std::vector<std::uint8_t> encoded;
    encode_mesh(mesh, encoded, mesh_entry_name(frame), frame);
    if (blob_equals(sf.mesh, encoded))
        return; // the same mesh: nothing changes
    require_confirmed(dependents(LOOMORA_KIND_FRAME, frame, kOpChange), confirm, LOOMORA_KIND_FRAME, frame);
    remove_results(results_of_frame(frame));
    sf.mesh.bytes = std::move(encoded);
    sf.mesh.from_archive.clear();
    sf.frame.mesh.vertices = mesh.vertices;
    sf.frame.mesh.triangles = mesh.triangles;
    sf.frame.mesh.has_normals = mesh.normals_flag;
    sf.frame.mesh.has_uvs = mesh.uvs_flag;
    sf.json_kept = false;
}

void ProjectEditor::frame_set_attached_source(std::uint32_t frame, const std::string &name, const ByteSpan &data) {
    std::map<std::uint32_t, StagedFrame>::iterator it = frames_.find(frame);
    if (it == frames_.end())
        not_found("frame " + dec(frame) + " does not exist");
    StagedFrame &sf = it->second;
    const std::string entry = frame_entry(frame);
    if (!sf.frame.has_mesh)
        invalid_item("schema.value", entry, kItemFrame, frame, "only a frame with a mesh keeps a copy of its source");
    if (!data.present()) {
        if (sf.frame.mesh.attached_source.empty())
            return;
        sf.frame.mesh.attached_source.clear();
        sf.attached = Blob();
        sf.json_kept = false;
        return;
    }
    require_utf8(name, "name");
    std::string ext = entry_extension(name);
    if (ext.empty() || ext.size() > 8 ||
        ext.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        ext = "bin";
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] = static_cast<char>(ext[i] - 'A' + 'a');
    sf.attached.bytes.assign(data.data, data.data + data.len);
    sf.attached.from_archive.clear();
    sf.frame.mesh.attached_source = "frames/" + dec(frame) + "/mesh-source." + ext;
    sf.json_kept = false;
}

// ---- replace ---------------------------------------------------------------------------------------

void ProjectEditor::replace_item(int kind, std::uint32_t id, const std::string &json, bool confirm) {
    item_kind_of(kind);
    switch (kind) {
    case LOOMORA_KIND_FRAME:
        replace_frame_json(id, json, confirm);
        break;
    case LOOMORA_KIND_LAYOUT:
        replace_layout_json(id, json);
        break;
    case LOOMORA_KIND_IMAGE:
        replace_image(id, json, ByteSpan(), ByteSpan(), ByteSpan(), confirm);
        break;
    case LOOMORA_KIND_SPOOL:
        replace_spool_json(id, json, confirm);
        break;
    default:
        replace_result_json(id, json);
        break;
    }
}

void ProjectEditor::replace_frame_json(std::uint32_t id, const std::string &json, bool confirm) {
    std::map<std::uint32_t, StagedFrame>::iterator it = frames_.find(id);
    if (it == frames_.end())
        not_found("frame " + dec(id) + " does not exist");
    StagedFrame &sf = it->second;
    const model::Frame &old = sf.frame;
    const std::string entry = frame_entry(id);
    model::Frame nf;
    parse_frame_json(json, nf);
    nf.id = id;
    if (nf.kind != old.kind)
        invalid_item("edit.immutable", entry, kItemFrame, id,
                     "the kind of a frame cannot change: remove the frame and add another");
    // What the editor owns of the mesh description stays; the optional description (watertight,
    // manifold, source, construction) is the caller's when it is given.
    const model::MeshInfo extras = nf.mesh;
    const bool given = nf.has_mesh;
    nf.has_mesh = old.has_mesh;
    nf.mesh = old.mesh;
    if (given && old.has_mesh) {
        nf.mesh.watertight = extras.watertight;
        nf.mesh.manifold = extras.manifold;
        nf.mesh.has_source = extras.has_source;
        nf.mesh.source_name = extras.source_name;
        nf.mesh.source_sha256 = extras.source_sha256;
        nf.mesh.construction_json = extras.construction_json;
    }
    check_staged_frame(nf, entry);
    const bool counts = frame_geometry_differs(old, nf);
    if (frame_json(old) == frame_json(nf))
        return;
    if (counts) {
        require_confirmed(dependents(LOOMORA_KIND_FRAME, id, kOpChange), confirm, LOOMORA_KIND_FRAME, id);
        remove_results(results_of_frame(id));
    }
    sf.frame = nf;
    sf.json_kept = false;
}

void ProjectEditor::replace_layout_json(std::uint32_t id, const std::string &json) {
    std::map<std::uint32_t, StagedLayout>::iterator it = layouts_.find(id);
    if (it == layouts_.end())
        not_found("layout " + dec(id) + " does not exist");
    StagedLayout &sl = it->second;
    const std::string entry = layout_entry(sl.layout.frame, id);
    model::Layout nl;
    parse_layout_json(json, nl);
    if (nl.frame != 0 && nl.frame != sl.layout.frame)
        invalid_item("edit.immutable", entry, kItemLayout, id,
                     "a layout cannot move to another frame: remove it and add another");
    nl.id = id;
    nl.frame = sl.layout.frame;
    nl.nail_count = sl.layout.nail_count;
    Report report;
    model::check_layout(nl, entry, report);
    throw_if_errors(report);
    if (layout_json(nl) == layout_json(sl.layout))
        return;
    sl.layout = nl;
    sl.json_kept = false;
}

void ProjectEditor::replace_spool_json(std::uint32_t id, const std::string &json, bool confirm) {
    std::map<std::uint32_t, model::Spool>::iterator it = spools_.find(id);
    if (it == spools_.end())
        not_found("spool " + dec(id) + " does not exist");
    model::Spool ns;
    parse_spool_json(json, ns);
    ns.id = id;
    Report report;
    model::check_spool(ns, "spools.json", report);
    throw_if_errors(report);
    const model::Spool &old = it->second;
    const bool counts = old.color != ns.color || old.opacity != ns.opacity || old.diameter_mm != ns.diameter_mm;
    const std::vector<model::Spool> a(1, old), b(1, ns);
    if (spools_json(a) == spools_json(b))
        return;
    if (counts) {
        require_confirmed(dependents(LOOMORA_KIND_SPOOL, id, kOpChange), confirm, LOOMORA_KIND_SPOOL, id);
        remove_results(results_of_spool(id));
    }
    it->second = ns;
    spools_kept_ = false;
}

void ProjectEditor::replace_result_json(std::uint32_t id, const std::string &json) {
    std::map<std::uint32_t, StagedResult>::iterator it = results_.find(id);
    if (it == results_.end())
        not_found("result " + dec(id) + " does not exist");
    StagedResult &sr = it->second;
    const std::string entry = result_entry(id);
    model::Result given;
    parse_result_json(json, given);
    const model::Result &old = sr.result;
    if (given.frame != old.frame || given.layout != old.layout || !same_images(given.images, old.images) ||
        !same_spools(given.spools, old.spools))
        invalid_item("edit.immutable", entry, kItemResult, id,
                     "the frame, layout, images and spools of a result cannot change: remove it and add another");
    // Only the name may change; created_at, steps, levels and the entries stay the editor's.
    model::Result nr = old;
    nr.name = given.name;
    const std::map<std::uint32_t, StagedFrame>::const_iterator fit = frames_.find(nr.frame);
    Report report;
    model::check_result(nr, fit == frames_.end() ? model::kFrame2D : fit->second.frame.kind, model::kProfileFull, entry,
                        report);
    throw_if_errors(report);
    if (result_json(nr) == result_json(old))
        return;
    sr.result = nr;
    sr.json_kept = false;
}

void ProjectEditor::replace_layout_nails(std::uint32_t layout, const model::NailTable &nails, bool confirm) {
    std::map<std::uint32_t, StagedLayout>::iterator it = layouts_.find(layout);
    if (it == layouts_.end())
        not_found("layout " + dec(layout) + " does not exist");
    StagedLayout &sl = it->second;
    const std::string entry = nails_entry(sl.layout.frame, layout);
    if (nails.is_3d() != frame_is_3d(sl.layout.frame))
        invalid_item("schema.value", entry, kItemLayout, layout,
                     "the nails and the frame must have the same dimension (2D or 3D)");
    Report report;
    model::check_nails(nails, entry, layout, report, triangle_bound(sl.layout.frame));
    throw_if_errors(report);
    const model::NailTable &old = layout_nails(layout);
    const bool counts = nails_differ(old, nails);

    // The labels of the old nails whose identifier remains; a label the new nail carries wins.
    std::map<std::uint32_t, const std::string *> labels;
    for (std::size_t i = 0; i < old.size(); ++i)
        if (const std::string *l = old.label_at(i))
            labels[old.id[i]] = l;
    model::NailTable fresh(nails.is_3d());
    fresh.reserve(nails.size());
    for (std::size_t i = 0; i < nails.size(); ++i) {
        model::Nail n = nails.get(i);
        if (!n.has_label) {
            std::map<std::uint32_t, const std::string *>::const_iterator lit = labels.find(n.id);
            if (lit != labels.end()) {
                n.has_label = true;
                n.label = *lit->second;
            }
        }
        fresh.add(n);
    }
    // Identical content (nails, order, labels): nothing changes, the entry stays in the archive.
    if (!counts && fresh.id == old.id && fresh.label_index == old.label_index && fresh.label_text == old.label_text)
        return;
    if (counts) {
        require_confirmed(dependents(LOOMORA_KIND_LAYOUT, layout, kOpChange), confirm, LOOMORA_KIND_LAYOUT, layout);
        remove_results(results_of_layout(layout));
    }
    sl.nails = std::move(fresh);
    sl.nails_loaded = true;
    sl.nails_kept = false;
    if (sl.layout.nail_count != sl.nails.size()) {
        sl.layout.nail_count = sl.nails.size();
        sl.json_kept = false;
    }
}

void ProjectEditor::replace_image(std::uint32_t id, const std::string &json, const ByteSpan &source,
                                  const ByteSpan &target, const ByteSpan &thumbnail, bool confirm) {
    std::map<std::uint32_t, StagedImage>::iterator it = images_.find(id);
    if (it == images_.end())
        not_found("image " + dec(id) + " does not exist");
    StagedImage &si = it->second;
    const std::string entry = image_entry(id);
    model::Image ni;
    parse_image_json(json, ni);
    ni.id = id;
    ni.has_source = si.image.has_source;
    ni.source = si.image.source;
    ni.has_target = si.image.has_target;
    ni.target = si.image.target;
    ni.thumbnail = si.image.thumbnail;

    // New pictures, cleaned; `pixels` says whether source or target differ from the current ones.
    bool pixels = false;
    std::vector<std::uint8_t> nsource, ntarget, nthumb;
    std::string next = si.source_ext;
    if (source.present()) {
        const ImageFormat fmt = detect_image_format(source.data, source.len);
        if (fmt == kImageUnknown)
            invalid_item("schema.value", entry, kItemImage, id, "the source is not a PNG, JPEG or WebP picture");
        nsource = strip_image_metadata(source.data, source.len, fmt);
        next = fmt == kImageJpeg ? "jpg" : image_format_name(fmt);
        if (!blob_equals(si.source, nsource) || next != si.source_ext) {
            pixels = true;
            ni.has_source = true;
            ni.source.entry = image_source_entry(id, next);
            ni.source.format = image_format_name(fmt);
            ni.source.size = static_cast<std::int64_t>(nsource.size());
        } else {
            nsource.clear();
        }
    }
    if (target.present()) {
        ntarget = strip_image_metadata(target.data, target.len, kImagePng);
        if (!ni.has_target || !blob_equals(si.target, ntarget)) {
            pixels = true;
            ni.has_target = true;
            ni.target.entry = image_target_entry(id);
            ni.target.format = "png";
            ni.target.size = static_cast<std::int64_t>(ntarget.size());
        } else {
            ntarget.clear();
        }
    }
    if (thumbnail.present())
        nthumb = clean_thumbnail(thumbnail, kItemImage, id, entry);

    Report report;
    model::check_image(ni, model::kProfileFull, entry, report);
    throw_if_errors(report);
    const model::Image &old = si.image;
    const bool counts = pixels || old.color_mode != ni.color_mode || old.shape != ni.shape ||
                        old.width_px != ni.width_px || old.height_px != ni.height_px;
    if (counts) {
        require_confirmed(dependents(LOOMORA_KIND_IMAGE, id, kOpChange), confirm, LOOMORA_KIND_IMAGE, id);
        remove_results(results_of_image(id));
    }
    if (!nsource.empty()) {
        si.source.bytes = std::move(nsource);
        si.source.from_archive.clear();
        si.source_ext = next;
    }
    if (!ntarget.empty()) {
        si.target.bytes = std::move(ntarget);
        si.target.from_archive.clear();
    }
    if (!nthumb.empty() && !blob_equals(si.thumbnail, nthumb)) {
        si.thumbnail.bytes = std::move(nthumb);
        si.thumbnail.from_archive.clear();
    }
    if (image_json(old) != image_json(ni)) {
        si.image = ni;
        si.json_kept = false;
    }
}

void ProjectEditor::replace_thumbnail(int kind, std::uint32_t id, const ByteSpan &png) {
    if (kind != LOOMORA_KIND_IMAGE && kind != LOOMORA_KIND_RESULT)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "only an image or a result has a thumbnail");
    Blob *blob = nullptr;
    std::string entry;
    if (kind == LOOMORA_KIND_IMAGE) {
        std::map<std::uint32_t, StagedImage>::iterator it = images_.find(id);
        if (it == images_.end())
            not_found("image " + dec(id) + " does not exist");
        blob = &it->second.thumbnail;
        entry = image_entry(id);
    } else {
        std::map<std::uint32_t, StagedResult>::iterator it = results_.find(id);
        if (it == results_.end())
            not_found("result " + dec(id) + " does not exist");
        blob = &it->second.thumbnail;
        entry = result_entry(id);
    }
    std::vector<std::uint8_t> fresh = clean_thumbnail(png, static_cast<ItemKind>(kind), id, entry);
    if (blob_equals(*blob, fresh))
        return;
    blob->bytes = std::move(fresh);
    blob->from_archive.clear();
}

// ---- remove, order ---------------------------------------------------------------------------------

void ProjectEditor::remove_item(int kind, std::uint32_t id, bool confirm) {
    item_kind_of(kind);
    const std::vector<ItemRef> deps = dependents(kind, id, kOpRemove); // also checks that the item exists
    require_confirmed(deps, confirm, kind, id);
    for (std::size_t i = 0; i < deps.size(); ++i) {
        if (deps[i].kind == LOOMORA_KIND_LAYOUT)
            remove_layout(deps[i].id);
    }
    switch (kind) {
    case LOOMORA_KIND_FRAME:
        remove_results(results_of_frame(id));
        frames_.erase(id);
        erase_order(manifest_.order.frames, id);
        break;
    case LOOMORA_KIND_LAYOUT:
        remove_results(results_of_layout(id));
        remove_layout(id);
        break;
    case LOOMORA_KIND_IMAGE:
        remove_results(results_of_image(id));
        images_.erase(id);
        erase_order(manifest_.order.images, id);
        break;
    case LOOMORA_KIND_SPOOL:
        remove_results(results_of_spool(id));
        spools_.erase(id);
        erase_order(manifest_.order.spools, id);
        spools_kept_ = false;
        break;
    default:
        remove_results(std::vector<std::uint32_t>(1, id));
        break;
    }
}

void ProjectEditor::set_order(int kind, const std::vector<std::uint32_t> &ids) {
    item_kind_of(kind);
    std::vector<std::uint32_t> *list = nullptr;
    switch (kind) {
    case LOOMORA_KIND_FRAME: list = &manifest_.order.frames; break;
    case LOOMORA_KIND_LAYOUT: list = &manifest_.order.layouts; break;
    case LOOMORA_KIND_IMAGE: list = &manifest_.order.images; break;
    case LOOMORA_KIND_SPOOL: list = &manifest_.order.spools; break;
    default: list = &manifest_.order.results; break;
    }
    std::vector<std::uint32_t> a(*list), b(ids);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "the order must list every item of the kind exactly once");
    *list = ids;
    if (kind == LOOMORA_KIND_SPOOL)
        spools_kept_ = false;
}

// ---- save-time rules -------------------------------------------------------------------------------

void ProjectEditor::check_ready() const {
    for (std::map<std::uint32_t, StagedFrame>::const_iterator it = frames_.begin(); it != frames_.end(); ++it) {
        const model::Frame &f = it->second.frame;
        if (f.kind == model::kFrame3D && f.surface == model::kSurfaceMesh && !f.has_mesh)
            invalid_item("schema.missing", frame_entry(it->first), kItemFrame, it->first,
                         "surface mesh requires a mesh (frame_set_mesh)");
    }
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it) {
        const model::Result &r = it->second.result;
        for (std::size_t i = 0; i < r.spools.size(); ++i) {
            std::map<std::uint32_t, model::Spool>::const_iterator sit = spools_.find(r.spools[i].spool);
            if (sit != spools_.end() && sit->second.has_available_length &&
                r.spools[i].length_mm > sit->second.available_length_mm)
                invalid_item("spool.length_over_available", result_entry(it->first), kItemResult, it->first,
                             "the length chosen for spool " + dec(r.spools[i].spool) +
                                 " is above its available length");
        }
    }
}

std::vector<model::Spool> ProjectEditor::spools_in_order() const {
    std::vector<model::Spool> out;
    out.reserve(manifest_.order.spools.size());
    for (std::size_t i = 0; i < manifest_.order.spools.size(); ++i)
        out.push_back(spools_.find(manifest_.order.spools[i])->second);
    return out;
}

std::vector<std::string> ProjectEditor::required_features() const {
    std::vector<std::string> out;
    bool mesh = false, levels = false, multi = false;
    for (std::map<std::uint32_t, StagedFrame>::const_iterator it = frames_.begin(); it != frames_.end(); ++it)
        mesh = mesh || it->second.frame.has_mesh;
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = results_.begin(); it != results_.end(); ++it) {
        levels = levels || it->second.result.levels;
        multi = multi || it->second.result.images.size() > 1;
    }
    if (mesh)
        out.push_back(model::kFeatureSurfaceMesh);
    if (levels)
        out.push_back(model::kFeatureStringLevels);
    if (multi)
        out.push_back(model::kFeatureMultiViewTarget);
    return out;
}

} // namespace format
} // namespace loomora

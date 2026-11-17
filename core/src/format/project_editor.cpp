#include "format/project_editor.hpp"

#include <algorithm>

#include "loomora.h"
#include "errors.hpp"
#include "format/codec/image_meta.hpp"
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

} // namespace

// ---- entry names ---------------------------------------------------------------------------------

std::string frame_entry(std::uint32_t f) { return "frames/" + dec(f) + "/frame.json"; }
std::string layout_entry(std::uint32_t f, std::uint32_t l) {
    return "frames/" + dec(f) + "/layouts/" + dec(l) + "/layout.json";
}
std::string nails_entry(std::uint32_t f, std::uint32_t l) {
    return "frames/" + dec(f) + "/layouts/" + dec(l) + "/nails.json";
}
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
    return s.id;
}

std::uint32_t ProjectEditor::add_frame(const model::Frame &frame) {
    model::Frame f = frame;
    f.id = peek_id(manifest_.next_id.frame, "frame");
    if (f.kind != model::kFrame2D)
        invalid_item("schema.value", frame_entry(f.id), kItemFrame, f.id,
                     "only 2D frames can be added by this version of the editor");
    Report report;
    model::check_frame(f, frame_entry(f.id), report);
    throw_if_errors(report);
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
    model::check_nails(nails, nails_entry(frame, staged.layout.id), staged.layout.id, report);
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
    staged.source = strip_image_metadata(source.data, source.len, fmt);
    staged.source_ext = fmt == kImageJpeg ? "jpg" : image_format_name(fmt);
    if (target.present())
        staged.target = strip_image_metadata(target.data, target.len, kImagePng);
    staged.thumbnail = clean_thumbnail(thumbnail, kItemImage, img.id, entry);

    img.has_source = true;
    img.source.entry = image_source_entry(img.id, staged.source_ext);
    img.source.format = image_format_name(fmt);
    img.source.size = static_cast<std::int64_t>(staged.source.size());
    img.has_target = !staged.target.empty();
    if (img.has_target) {
        img.target.entry = image_target_entry(img.id);
        img.target.format = "png";
        img.target.size = static_cast<std::int64_t>(staged.target.size());
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
        staged.settings = compact_json_object(settings_json, "settings");
        r.has_settings = true;
        r.settings.entry = result_settings_entry(r.id);
    } else {
        r.has_settings = false;
    }
    Report report;
    model::check_result(r, fit->second.frame.kind, model::kProfileFull, entry, report);
    throw_if_errors(report);
    staged.thumbnail = clean_thumbnail(thumbnail, kItemResult, r.id, entry);

    // The sequence: every record against the layout and the selection, then encoded.
    std::vector<std::uint32_t> nail_ids(lit->second.nails.id);
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
    staged.sequence = sink.take();

    const std::uint32_t id = r.id;
    results_[id] = std::move(staged);
    manifest_.order.results.push_back(id);
    ++manifest_.next_id.result;
    return id;
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

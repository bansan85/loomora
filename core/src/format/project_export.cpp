#include "format/project_export.hpp"

#include <set>
#include <utility>
#include <vector>

#include "format/codec/image_meta.hpp"
#include "format/codec/mesh_codec.hpp"
#include "format/io/memory_io.hpp"
#include "format/limits.hpp"
#include "format/project_writer.hpp"

namespace loomora {
namespace format {

namespace {

typedef std::set<std::uint32_t> IdSet;

struct NoDelete {
    void operator()(const ProjectReader *) const {}
};

std::string dec(std::uint32_t v) {
    std::string s;
    do {
        s.insert(s.begin(), static_cast<char>('0' + v % 10));
        v /= 10;
    } while (v != 0);
    return s;
}

std::vector<std::uint32_t> filtered(const std::vector<std::uint32_t> &order, const IdSet &keep) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (keep.count(order[i]) != 0)
            out.push_back(order[i]);
    return out;
}

template <typename Map> IdSet keys_of(const Map &m) {
    IdSet s;
    for (typename Map::const_iterator it = m.begin(); it != m.end(); ++it)
        s.insert(it->first);
    return s;
}

// The thumbnail of the light profile: a PNG within the limits, without metadata. The kept entry is
// copied as it is when it is already clean, else the cleaned bytes are staged.
void stage_light_thumbnail(const ProjectReader &r, const std::string &entry, Blob &blob) {
    blob.from_archive = entry;
    blob.bytes.clear();
    std::unique_ptr<Source> src = r.open_entry_source(entry);
    if (src->size() > limits().thumb_max_bytes)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "the thumbnail " + entry + " is larger than the thumbnail limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(src->size()));
    if (!bytes.empty() && src->read_at(0, &bytes[0], bytes.size()) != bytes.size())
        throw FormatError(LOOMORA_ERR_IO, "the old archive ended early while a thumbnail was read");
    if (!thumbnail_within_limits(bytes.empty() ? nullptr : &bytes[0], bytes.size()))
        throw FormatError(LOOMORA_ERR_INVALID_ITEM,
                          "the thumbnail " + entry + " is not a PNG within the thumbnail limits");
    const std::vector<std::uint8_t> clean = strip_image_metadata(&bytes[0], bytes.size(), kImagePng);
    if (clean != bytes) {
        blob.bytes = clean;
        blob.from_archive.clear();
    }
}

} // namespace

ProjectEditor::ProjectEditor(std::shared_ptr<const ProjectReader> base, const ExportSelection &selection)
    : base_(base) {
    if (!base_)
        throw FormatError(LOOMORA_ERR_INVALID, "no project to export");
    const ProjectReader &r = *base_;
    const bool light = selection.light;
    if (r.is_light() && !light)
        throw FormatError(LOOMORA_ERR_READ_ONLY_PROJECT, "A light copy cannot be exported as a full project");

    // The items that stay.
    IdSet frames, layouts, images, spools, results;
    if (selection.all_items) {
        frames = keys_of(r.frames());
        layouts = keys_of(r.layouts());
        images = keys_of(r.images());
        spools = keys_of(r.spools());
        results = keys_of(r.results());
    } else {
        for (std::size_t i = 0; i < selection.results.size(); ++i) {
            const ResultRecord *rec = r.result(selection.results[i]);
            if (rec == nullptr)
                throw FormatError(LOOMORA_ERR_NOT_FOUND, "result " + dec(selection.results[i]) + " does not exist");
            const model::Result &res = rec->result;
            results.insert(res.id);
            frames.insert(res.frame);
            layouts.insert(res.layout);
            for (std::size_t k = 0; k < res.images.size(); ++k)
                images.insert(res.images[k].image);
            for (std::size_t k = 0; k < res.spools.size(); ++k)
                spools.insert(res.spools[k].spool);
        }
    }

    manifest_ = r.manifest();
    manifest_.format = model::FormatId();
    manifest_.min_reader = model::Version();
    manifest_.profile = light ? model::kProfileLight : model::kProfileFull;
    manifest_.editable = !light;
    manifest_.counts = model::Counts();
    manifest_.entries.clear();
    manifest_.required_features.clear();
    manifest_.order.frames = filtered(manifest_.order.frames, frames);
    manifest_.order.layouts = filtered(manifest_.order.layouts, layouts);
    manifest_.order.images = filtered(manifest_.order.images, images);
    manifest_.order.spools = filtered(manifest_.order.spools, spools);
    manifest_.order.results = filtered(manifest_.order.results, results);

    for (std::map<std::uint32_t, SpoolRecord>::const_iterator it = r.spools().begin(); it != r.spools().end(); ++it)
        if (spools.count(it->first) != 0)
            spools_[it->first] = it->second.spool;
    spools_kept_ = spools_.size() == r.spools().size(); // spools.json is copied as it is when nothing is pruned

    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = r.frames().begin(); it != r.frames().end(); ++it) {
        if (frames.count(it->first) == 0)
            continue;
        StagedFrame sf;
        sf.frame = it->second.frame;
        sf.json_kept = true;
        model::MeshInfo &mesh = sf.frame.mesh;
        if (sf.frame.has_mesh) {
            sf.mesh.from_archive = it->second.mesh_entry;
            if (!mesh.attached_source.empty()) {
                if (!light && r.find_entry(mesh.attached_source) != nullptr) {
                    sf.attached.from_archive = mesh.attached_source;
                } else {
                    mesh.attached_source.clear();
                    sf.json_kept = false;
                }
            }
            if (light) {
                if (!mesh.construction_json.empty()) {
                    mesh.construction_json.clear();
                    sf.json_kept = false;
                }
                if (mesh.has_uvs) {
                    mesh.has_uvs = false;
                    sf.json_kept = false;
                    std::unique_ptr<Source> in = r.open_entry_source(it->second.mesh_entry);
                    MemorySink out;
                    rewrite_mesh_without_uvs(*in, out);
                    out.commit();
                    sf.mesh.from_archive.clear();
                    sf.mesh.bytes = out.take();
                }
            }
        }
        frames_[it->first] = sf;
    }
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = r.layouts().begin(); it != r.layouts().end();
         ++it) {
        if (layouts.count(it->first) == 0)
            continue;
        StagedLayout sl;
        sl.layout = it->second.layout;
        sl.json_kept = true;
        sl.nails_kept = true;
        sl.nails_loaded = false;
        layouts_[it->first] = sl;
    }
    for (std::map<std::uint32_t, ImageRecord>::const_iterator it = r.images().begin(); it != r.images().end(); ++it) {
        if (images.count(it->first) == 0)
            continue;
        StagedImage si;
        si.image = it->second.image;
        si.json_kept = true;
        if (light) {
            if (si.image.has_source || si.image.has_target) {
                si.image.has_source = false;
                si.image.has_target = false;
                si.image.source = model::EntryRef();
                si.image.target = model::EntryRef();
                si.json_kept = false;
            }
            stage_light_thumbnail(r, si.image.thumbnail.entry, si.thumbnail);
        } else {
            if (si.image.has_source) {
                si.source.from_archive = si.image.source.entry;
                const std::string::size_type dot = si.image.source.entry.rfind('.');
                si.source_ext = dot == std::string::npos ? std::string() : si.image.source.entry.substr(dot + 1);
            }
            if (si.image.has_target)
                si.target.from_archive = si.image.target.entry;
            si.thumbnail.from_archive = si.image.thumbnail.entry;
        }
        images_[it->first] = si;
    }
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = r.results().begin(); it != r.results().end();
         ++it) {
        if (results.count(it->first) == 0)
            continue;
        StagedResult sr;
        sr.result = it->second.result;
        sr.json_kept = true;
        sr.sequence.from_archive = it->second.sequence_entry;
        if (light) {
            if (sr.result.has_settings) {
                sr.result.has_settings = false;
                sr.result.settings = model::EntryRef();
                sr.json_kept = false;
            }
            stage_light_thumbnail(r, sr.result.thumbnail.entry, sr.thumbnail);
        } else {
            sr.thumbnail.from_archive = sr.result.thumbnail.entry;
            if (sr.result.has_settings)
                sr.settings.from_archive = sr.result.settings.entry;
        }
        results_[it->first] = sr;
    }
    if (!light) {
        const std::vector<ZipEntry> &entries = r.zip().entries();
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (classify_entry(entries[i].name).role == kRoleUnknown)
                unknown_.push_back(entries[i].name);
    }
}

std::unique_ptr<ProjectEditor> make_export_editor(const ProjectReader &base, const ExportSelection &selection) {
    return std::unique_ptr<ProjectEditor>(
        new ProjectEditor(std::shared_ptr<const ProjectReader>(&base, NoDelete()), selection));
}

void export_project_to_path(const ProjectReader &base, const ExportSelection &selection, const std::string &utf8_path,
                            const std::string &modified_at, std::uint32_t options, loomora_progress_fn progress,
                            void *progress_user, Report *report) {
    const std::unique_ptr<ProjectEditor> editor = make_export_editor(base, selection);
    save_project_to_path(*editor, utf8_path, modified_at, options, progress, progress_user, report);
}

void export_project_to_callback(const ProjectReader &base, const ExportSelection &selection,
                                const std::string &modified_at, std::uint32_t options, loomora_sink_fn sink,
                                void *sink_user, loomora_progress_fn progress, void *progress_user, Report *report) {
    const std::unique_ptr<ProjectEditor> editor = make_export_editor(base, selection);
    save_project_to_callback(*editor, modified_at, options, sink, sink_user, progress, progress_user, report);
}

} // namespace format
} // namespace loomora

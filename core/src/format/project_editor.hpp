// The project editor (spec 002, edit mode): the staged content of a project in memory, built item by
// item (or loaded from an existing file) and written by the project writer (project_writer.hpp). It
// covers creation (a new project, spools, 2D and 3D frames with their mesh, layouts with their
// nails, images, results with their weaving sequence and, on a 3D frame, their views), editing
// (replace, remove, dependents, order) and opening an existing project for edit. Every kind of
// item is held in its own `std::map` keyed by identifier (so increasing identifier order, the
// canonical entry order of project-format.md section 2, is the iteration order), with the bulk data
// next to the item.
//
// Copy-through (FR-013, SC-013): an editor opened from a file keeps the old archive open. Every part
// of an item (its JSON, a picture, the sequence, the mesh, the nails) is either staged (new bytes in
// memory) or still "in the archive" (a Blob naming the old entry, a `json_kept` flag): the writer
// copies those entries as compressed bytes, so their SHA-256 in the new manifest is the old one.
// Anything an operation touches becomes staged. Entries the format does not define are kept as they
// are (the same bytes, after the known entries, in their original relative order).
//
// Dependency rules (data-model.md): `dependents` tells what an operation removes; `replace_*` and
// `remove_item` change the item and remove its dependents in the same call, only when `confirm` is
// true (otherwise FormatError(LOOMORA_ERR_HAS_DEPENDENTS) and nothing changes). A change of a
// field that results do not use removes nothing. Refused changes (a layout's frame, a result's
// frame, layout, images or spools, a frame's kind) are FormatError(LOOMORA_ERR_INVALID_ITEM) with
// the code `edit.immutable` (a code beyond the list of c-api-project.md; stable).
//
// Rules the editor applies:
//   - Identifiers come from the `next_id` counters of the manifest, which only increase; an `add_*`
//     returns the identifier it allocated and ignores the `id` of the item it was given.
//   - `order` lists (display order) get the new identifier at the end; `counts` are derived.
//   - An add is all or nothing: the item is checked (model::check_*, references to other items, the
//     sequence against the layout and the spool selection) before anything is changed. A violation
//     throws FormatError(LOOMORA_ERR_INVALID_ITEM) carrying the first issue, an unknown identifier
//     passed as an argument FormatError(LOOMORA_ERR_NOT_FOUND).
//   - Memory: nails are kept in a NailTable (about 30 bytes per 2D nail), a sequence as its encoded
//     sequence.bin bytes (about 3 bytes per step; the steps are pulled one at a time from a
//     StepSource, never copied twice).
//   - Nothing reads a clock or a random source: the project identifier, the times and the generator
//     are given by the caller (the default generator is a constant, so that the same project gives
//     the same bytes on every platform).
#ifndef LOOMORA_FORMAT_PROJECT_EDITOR_HPP
#define LOOMORA_FORMAT_PROJECT_EDITOR_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "format/codec/sequence_codec.hpp"
#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"
#include "format/project_reader.hpp"

namespace loomora {
namespace format {

// A pull source of weaving steps (so a caller's array is encoded without a second copy).
class StepSource {
  public:
    virtual ~StepSource() {}
    // Delivers the next step; false once all were delivered.
    virtual bool next(SequenceStep *step) = 0;
};

// A StepSource over a vector.
class VectorStepSource : public StepSource {
  public:
    explicit VectorStepSource(const std::vector<SequenceStep> &steps) : steps_(steps), pos_(0) {}
    bool next(SequenceStep *step) override {
        if (pos_ >= steps_.size())
            return false;
        *step = steps_[pos_++];
        return true;
    }

  private:
    const std::vector<SequenceStep> &steps_;
    std::size_t pos_;
};

// A byte buffer given by the caller (null and 0 = absent).
struct ByteSpan {
    const std::uint8_t *data;
    std::size_t len;
    ByteSpan() : data(nullptr), len(0) {}
    ByteSpan(const std::uint8_t *d, std::size_t n) : data(d), len(n) {}
    bool present() const { return data != nullptr && len != 0; }
};

// Archive entry names of the items (project-format.md section 2); `id` is the decimal identifier.
std::string frame_entry(std::uint32_t frame);                       // frames/<fid>/frame.json
std::string mesh_entry_name(std::uint32_t frame);                    // frames/<fid>/mesh.bin
std::string layout_entry(std::uint32_t frame, std::uint32_t layout); // .../layouts/<lid>/layout.json
std::string nails_entry(std::uint32_t frame, std::uint32_t layout);  // .../layouts/<lid>/nails.json
std::string image_entry(std::uint32_t image);                       // images/<iid>/image.json
std::string image_source_entry(std::uint32_t image, const std::string &ext); // source.<ext>
std::string image_target_entry(std::uint32_t image);                // target.png
std::string image_thumb_entry(std::uint32_t image);                 // thumb.png
std::string result_entry(std::uint32_t result);                     // results/<rid>/result.json
std::string result_sequence_entry(std::uint32_t result);            // sequence.bin
std::string result_thumb_entry(std::uint32_t result);               // thumb.png
std::string result_settings_entry(std::uint32_t result);            // settings.json

// A piece of bulk data of an item: new bytes in memory, or the untouched entry of the old archive
// (`from_archive` is its name there; the bytes are then not held).
struct Blob {
    std::vector<std::uint8_t> bytes;
    std::string from_archive;
    bool in_archive() const { return !from_archive.empty(); }
};

// What is staged for each kind of item. `json_kept`: the item's JSON entry is the old one, untouched.
struct StagedFrame {
    model::Frame frame;
    bool json_kept = false;
    Blob mesh;      // mesh.bin, when frame.has_mesh
    Blob attached;  // the mesh-source copy, when frame.mesh.attached_source is set
};
struct StagedLayout {
    model::Layout layout;
    bool json_kept = false;
    bool nails_kept = false;            // nails.json is the old entry, untouched
    // The nails: always loaded for a staged layout; loaded on demand (from the old archive) for a kept one.
    mutable model::NailTable nails;
    mutable bool nails_loaded = true;
};
struct StagedImage {
    model::Image image;
    bool json_kept = false;
    std::string source_ext;                // png, jpg or webp
    Blob source;                           // metadata stripped
    Blob target;                           // empty when the image has none
    Blob thumbnail;                        // metadata stripped, within the thumbnail limits
};
struct StagedResult {
    model::Result result;
    bool json_kept = false;
    Blob sequence;                         // the sequence.bin bytes
    Blob thumbnail;
    Blob settings;                         // compact JSON object; empty when absent
};

// An item named by kind (LOOMORA_KIND_*) and identifier.
struct ItemRef {
    int kind;
    std::uint32_t id;
    ItemRef() : kind(0), id(0) {}
    ItemRef(int k, std::uint32_t i) : kind(k), id(i) {}
};

const int kOpRemove = 1; // LOOMORA_OP_REMOVE
const int kOpChange = 2; // LOOMORA_OP_CHANGE

// A mesh given by the caller (loomora_mesh_info and the arrays of the C API).
struct MeshInput {
    std::uint64_t vertices;
    std::uint64_t triangles;
    bool normals_flag;
    bool uvs_flag;
    const float *xyz;
    const std::uint32_t *abc;
    const float *normals;   // required when normals_flag
    const float *uvs;       // required when uvs_flag
    MeshInput() : vertices(0), triangles(0), normals_flag(false), uvs_flag(false), xyz(nullptr), abc(nullptr),
                  normals(nullptr), uvs(nullptr) {}
};

// What an export keeps of an open project (project_export.hpp, FR-017, FR-018).
struct ExportSelection {
    bool light;                          // the light profile: nothing that is not needed to use the project
    bool all_items;                      // true: every item (`results` is ignored); false: the closure of `results`
    std::vector<std::uint32_t> results;  // identifiers of the chosen results (each must exist)
    ExportSelection() : light(false), all_items(true) {}
};

class ProjectEditor {
  public:
    // The staged content of an export of `base` (a full or a light project): the items of `selection`, all
    // still "in the archive" (copy-through) except what the light profile rewrites (frame descriptions
    // without construction and UVs, mesh.bin without its UV section, images without source and target,
    // results without settings). Defined in project_export.cpp, which explains the rules. A light
    // `base` with a full `selection` is FormatError(LOOMORA_ERR_READ_ONLY_PROJECT); an unknown result
    // FormatError(LOOMORA_ERR_NOT_FOUND). The writer then writes it like any other editor.
    ProjectEditor(std::shared_ptr<const ProjectReader> base, const ExportSelection &selection);
    // A new full, editable, empty project. `project_id` is a lowercase UUID v4 and `created_at` an
    // ISO 8601 UTC time (FormatError(LOOMORA_ERR_INVALID) otherwise); `title` up to 1,024 bytes.
    ProjectEditor(const std::string &project_id, const std::string &title, const std::string &created_at);
    // Edits an existing project (already opened and validated by ProjectReader::open): its items are
    // staged as "in the archive", the archive stays open until the editor is destroyed. A light
    // project is refused: FormatError(LOOMORA_ERR_READ_ONLY_PROJECT, "This project is a light copy and
    // cannot be edited").
    explicit ProjectEditor(std::shared_ptr<const ProjectReader> base);

    void set_title(const std::string &title);
    // Informational fields of the manifest. The default is {"Loomora", "0.1.0", "unspecified",
    // "unspecified"}: constant on purpose (see above).
    void set_generator(const model::Generator &generator);

    // Each returns the allocated identifier. `item.id` is ignored.
    std::uint32_t add_spool(const model::Spool &spool);
    std::uint32_t add_frame(const model::Frame &frame);
    // `nails` are the nails of the layout; `layout.frame` and `layout.nail_count` are set from
    // `frame` and the nails. NOT_FOUND when the frame does not exist.
    std::uint32_t add_layout(std::uint32_t frame, const model::Layout &layout, const model::NailTable &nails);
    // Sets (`label` not null) or removes (null) the label of nail `nail_id` of a layout.
    void layout_set_label(std::uint32_t layout, std::uint32_t nail_id, const std::string *label);
    // `source` is required (a full project keeps the source picture), `target` is optional,
    // `thumbnail` is required. Metadata is stripped from every picture; the thumbnail must be a PNG
    // within 256 x 256 px and 64 KiB.
    std::uint32_t add_image(const model::Image &image, const ByteSpan &source, const ByteSpan &target,
                            const ByteSpan &thumbnail);
    // `steps` delivers exactly `step_count` steps. `settings_json` (a JSON object) is empty when
    // there are no settings. `result.steps` is set from `step_count`.
    std::uint32_t add_result(const model::Result &result, StepSource &steps, std::uint64_t step_count,
                             const ByteSpan &thumbnail, const std::string &settings_json);

    // ---- 3D ------------------------------------------------------------------------------------
    // The first mesh of a 3D frame that has none (INVALID_ITEM for a 2D frame or a frame that has
    // one; NOT_FOUND for an unknown frame). The mesh is checked while it is encoded (finite values,
    // indices below the vertex count, unit normals), the description of the frame gets its counts and
    // flags (watertight and manifold come from the frame's JSON "mesh" object, false by default).
    void frame_set_mesh(std::uint32_t frame, const MeshInput &mesh);
    // Sets (`data` present) or removes (absent) the copy of the imported file; `name` gives the
    // extension of `mesh-source.<ext>`. The frame must have a mesh. Never has dependents.
    void frame_set_attached_source(std::uint32_t frame, const std::string &name, const ByteSpan &data);

    // ---- editing (dependency rules in the file comment) ---------------------------------------
    // What `operation` (kOpRemove, kOpChange) on an item would remove: layouts first, then results,
    // each in increasing identifier. NOT_FOUND for an unknown item, INVALID for an unknown operation.
    std::vector<ItemRef> dependents(int kind, std::uint32_t id, int operation) const;
    // The JSON part of an item (the fields the editor does not own are kept).
    void replace_item(int kind, std::uint32_t id, const std::string &json, bool confirm);
    // The nails of a layout; the labels of the nails whose identifier remains are kept.
    void replace_layout_nails(std::uint32_t layout, const model::NailTable &nails, bool confirm);
    void replace_frame_mesh(std::uint32_t frame, const MeshInput &mesh, bool confirm);
    // `json`, and each picture only when present (an absent one is kept as it is).
    void replace_image(std::uint32_t image, const std::string &json, const ByteSpan &source, const ByteSpan &target,
                       const ByteSpan &thumbnail, bool confirm);
    // The thumbnail of an image or a result.
    void replace_thumbnail(int kind, std::uint32_t id, const ByteSpan &png);
    void remove_item(int kind, std::uint32_t id, bool confirm);
    // `ids` must be a permutation of the identifiers of the kind (INVALID_ITEM otherwise).
    void set_order(int kind, const std::vector<std::uint32_t> &ids);

    // True when the frame is 3D (NOT_FOUND for an unknown frame).
    bool frame_is_3d(std::uint32_t frame) const;
    // The nails of a layout (loaded from the old archive on the first call for a kept layout).
    const model::NailTable &layout_nails(std::uint32_t layout) const;
    // The triangle count nails of `frame` may refer to (kAnyTriangleCount without a mesh).
    std::uint64_t triangle_bound(std::uint32_t frame) const;

    // ---- read access (the writer, tests) -----------------------------------------------------
    // The manifest as far as the editor owns it: format, project_id, title, created_at, generator,
    // profile, editable, next_id, order. Its `modified_at`, `counts`, `entries`, `required_features`
    // are filled by the writer.
    const model::Manifest &manifest() const { return manifest_; }
    const std::map<std::uint32_t, StagedFrame> &frames() const { return frames_; }
    const std::map<std::uint32_t, StagedLayout> &layouts() const { return layouts_; }
    const std::map<std::uint32_t, StagedImage> &images() const { return images_; }
    const std::map<std::uint32_t, model::Spool> &spools() const { return spools_; }
    const std::map<std::uint32_t, StagedResult> &results() const { return results_; }

    // The old archive of an opened project (null for a new one) and what the writer needs of it.
    const ProjectReader *base() const { return base_.get(); }
    bool spools_kept() const { return spools_kept_; }
    // The entries of the old archive this version does not define, in their original order.
    const std::vector<std::string> &unknown_entries() const { return unknown_; }
    // The save-time rules the editor owns: a 3D frame whose surface is `mesh` has its mesh, a length
    // chosen for a spool is not above its available length (FormatError(LOOMORA_ERR_INVALID_ITEM)).
    void check_ready() const;

    // The spools in display order (the content of spools.json).
    std::vector<model::Spool> spools_in_order() const;
    // `required_features` derived from the content (project-format.md section 3).
    std::vector<std::string> required_features() const;

  private:
    std::vector<std::uint32_t> results_of_frame(std::uint32_t frame) const;
    std::vector<std::uint32_t> results_of_layout(std::uint32_t layout) const;
    std::vector<std::uint32_t> results_of_image(std::uint32_t image) const;
    std::vector<std::uint32_t> results_of_spool(std::uint32_t spool) const;
    std::vector<std::uint32_t> layouts_of_frame(std::uint32_t frame) const;
    // Fails with HAS_DEPENDENTS unless `confirm` or there is nothing to remove.
    void require_confirmed(const std::vector<ItemRef> &deps, bool confirm, int kind, std::uint32_t id) const;
    void remove_results(const std::vector<std::uint32_t> &ids);
    void remove_layout(std::uint32_t layout);
    void erase_order(std::vector<std::uint32_t> &list, std::uint32_t id);
    bool blob_equals(const Blob &blob, const std::vector<std::uint8_t> &bytes) const;
    void replace_frame_json(std::uint32_t id, const std::string &json, bool confirm);
    void replace_spool_json(std::uint32_t id, const std::string &json, bool confirm);
    void replace_layout_json(std::uint32_t id, const std::string &json);
    void replace_result_json(std::uint32_t id, const std::string &json);

    std::shared_ptr<const ProjectReader> base_;
    bool spools_kept_ = false;
    std::vector<std::string> unknown_;
    model::Manifest manifest_;
    std::map<std::uint32_t, StagedFrame> frames_;
    std::map<std::uint32_t, StagedLayout> layouts_;
    std::map<std::uint32_t, StagedImage> images_;
    std::map<std::uint32_t, model::Spool> spools_;
    std::map<std::uint32_t, StagedResult> results_;
};

} // namespace format
} // namespace loomora

#endif

// The project editor (spec 002, edit mode): the staged content of a project in memory, built item by
// item and written by the project writer (project_writer.hpp). This part covers creation: a new
// project, spools, 2D frames, layouts with their nails, images with their picture entries, results
// with their weaving sequence. Replace, remove, copy-through of a project opened for edit and the 3D
// parts (mesh, 3D nails, views) are added later on the same class: every kind of item is held in
// its own `std::map` keyed by identifier (so increasing identifier order, the canonical entry order
// of project-format.md section 2, is the iteration order), with the bulk data next to the item.
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
#include <string>
#include <vector>

#include "format/codec/sequence_codec.hpp"
#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"

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

// What is staged for each kind of item.
struct StagedFrame {
    model::Frame frame;
};
struct StagedLayout {
    model::Layout layout;
    model::NailTable nails;
};
struct StagedImage {
    model::Image image;
    std::string source_ext;                // png, jpg or webp
    std::vector<std::uint8_t> source;      // metadata stripped
    std::vector<std::uint8_t> target;      // empty when absent
    std::vector<std::uint8_t> thumbnail;   // metadata stripped, within the thumbnail limits
};
struct StagedResult {
    model::Result result;
    std::vector<std::uint8_t> sequence;    // the sequence.bin bytes
    std::vector<std::uint8_t> thumbnail;
    std::string settings;                  // compact JSON object; empty when absent
};

class ProjectEditor {
  public:
    // A new full, editable, empty project. `project_id` is a lowercase UUID v4 and `created_at` an
    // ISO 8601 UTC time (FormatError(LOOMORA_ERR_INVALID) otherwise); `title` up to 1,024 bytes.
    ProjectEditor(const std::string &project_id, const std::string &title, const std::string &created_at);

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

    // The spools in display order (the content of spools.json).
    std::vector<model::Spool> spools_in_order() const;
    // `required_features` derived from the content (project-format.md section 3).
    std::vector<std::string> required_features() const;

  private:
    std::uint32_t allocate(std::uint32_t &counter, const char *what);

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

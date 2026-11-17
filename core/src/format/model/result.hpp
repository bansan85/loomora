// Result (result.json, data-model.md "Result"): one computed weaving of an image on a layout, with
// the spools used. The step sequence is in sequence.bin (codec/sequence_codec.hpp).
#ifndef LOOMORA_FORMAT_MODEL_RESULT_HPP
#define LOOMORA_FORMAT_MODEL_RESULT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "format/model/common.hpp"
#include "format/model/frame.hpp"
#include "format/model/project.hpp"

namespace loomora {
namespace format {
namespace model {

enum Projection { kProjectionOrthographic = 0, kProjectionPerspective = 1 };

const char *to_string(Projection p);
bool parse_projection(const std::string &text, Projection &out);

// The view of a 3D result image. JSON key order: position, target, up, projection, size or
// fov_deg, weight.
struct View {
    double position[3] = {0, 0, 0};
    double target[3] = {0, 0, 0};
    double up[3] = {0, 1, 0};
    Projection projection = kProjectionOrthographic;
    double size = 0;                 // orthographic: > 0 (written as "size")
    double fov_deg = 0;              // perspective: in (0, 180) (written as "fov_deg")
    double weight = 1;               // >= 0
};

// One element of `images`: the image, and for a 3D frame its view.
struct ResultImage {
    std::uint32_t image = 0;
    bool has_view = false;
    View view;
};

// One element of `spools`: the spool selection with the length chosen.
struct ResultSpool {
    std::uint32_t spool = 0;
    double length_mm = 0;            // > 0 (and not above available_length_mm: checked by the validator)
};

// JSON key order: id, name, created_at, frame, layout, images, spools, steps, levels, thumbnail, settings.
struct Result {
    std::uint32_t id = 0;
    std::string name;                // up to 256 bytes
    std::string created_at;          // ISO 8601 UTC, supplied by the caller
    std::uint32_t frame = 0;         // exists
    std::uint32_t layout = 0;        // exists and belongs to `frame` (validator)
    std::vector<ResultImage> images; // 2D frame: exactly one, no view; 3D: one or more, each with a view
    std::vector<ResultSpool> spools; // the spool selection: at least one, no repeat
    std::uint64_t steps = 0;         // equals the record count of sequence.bin (validator)
    bool levels = false;             // sequence.bin header bit 0; true requires `string_levels`
    EntryRef thumbnail;              // both profiles
    bool has_settings = false;       // full profile only
    EntryRef settings;               // entry settings.json
};

// `frame_kind` is the kind of the result's frame (it decides how many images and whether views
// are expected); `profile` light has no settings.
void check_result(const Result &result, FrameKind frame_kind, Profile profile, const std::string &entry,
                  Report &report);

} // namespace model
} // namespace format
} // namespace loomora

#endif

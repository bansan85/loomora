// The semantic validator (research R11, project-format.md section 8): the rules that need several
// items at once. The reader calls it at the end of ProjectReader::open, once the container pass and
// the item schema checks are done. When every item could be read (ProjectReader::complete()) all
// the rules run; otherwise only the ones that rest on the parsed records alone (a missing
// reference, a nail or a step cannot be told from a consequence of the error already reported).
//
// Rules and codes (every one is an error unless noted):
//   ref.missing                  a layout's frame, a result's frame, layout, image or spool, an entry
//                                named by an item (thumbnail, source, target, settings), or a step's
//                                nail that does not exist
//   ref.layout_of_other_frame    a result whose layout belongs to another frame; a layout stored under
//                                a frame folder but declaring another frame
//   ref.spool_not_selected       a step that uses a spool index outside the result's spool selection
//   nail.outside_frame           a 2D nail outside the outline or inside an exclusion (0.01 mm), a 3D
//                                nail on a triangle the mesh does not have
//   spool.length_over_available  a selected length above the spool's available length
//   schema.value                 an exclusion outside the outline (0.01 mm)
//   warn.count                   a threshold of limits() exceeded (warning), named element and count
//   warn.nail_distance           two nails closer than the sum of their radii (warning, one per layout)
//   warn.origin_offset           bounding-box centre of a frame more than 0.01 mm from the origin (warning)
//   warn.thumbnail_size          a thumbnail above 256 px or 64 KiB (warning)
#ifndef LOOMORA_FORMAT_VALIDATE_VALIDATOR_HPP
#define LOOMORA_FORMAT_VALIDATE_VALIDATOR_HPP

#include <cstdint>
#include <vector>

#include "format/report.hpp"

namespace loomora {
namespace format {

class ProjectReader;

// Adds the issues of the cross-item rules to `report`.
void validate_semantic(const ProjectReader &reader, Report &report);

// The counts the threshold warnings (FR-025) are made of. The reader fills it from an opened
// project, the writer from the editor, so a file gives the same warnings when it is opened and when
// it is saved.
struct ThresholdFacts {
    struct Layout {
        std::uint32_t id, frame;
        std::uint64_t nails;
        Layout() : id(0), frame(0), nails(0) {}
    };
    struct Result {
        std::uint32_t id;
        std::uint64_t steps;
        Result() : id(0), steps(0) {}
    };
    struct Mesh {
        std::uint32_t frame;
        std::uint64_t triangles;
        Mesh() : frame(0), triangles(0) {}
    };
    std::uint64_t frames, layout_items, images, spools, results; // items per kind
    std::uint64_t total_bytes;                                   // decompressed size of the project
    std::vector<Layout> layouts;
    std::vector<Result> result_list;
    std::vector<Mesh> meshes;
    ThresholdFacts() : frames(0), layout_items(0), images(0), spools(0), results(0), total_bytes(0) {}
};

// Adds one warn.count warning per count above limits(): items per kind, the decompressed total,
// nails per layout, steps per result, triangles per mesh.
void check_thresholds(const ThresholdFacts &facts, Report &report);

} // namespace format
} // namespace loomora

#endif

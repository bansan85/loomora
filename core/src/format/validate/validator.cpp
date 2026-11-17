#include "format/validate/validator.hpp"

#include "format/project_reader.hpp"

namespace loomora {
namespace format {

// TODO T068: a later task replaces this body with the semantic pass (ref.*, nail.outside_frame,
// spool.length_over_available, num.not_finite across items, warn.count, warn.nail_distance,
// warn.origin_offset, warn.thumbnail_size). The reader already reports the container rules, the
// item schemas, seq.* and mesh.* (header, counts, CRC, index range), manifest.* and the
// warnings warn.light_project and warn.unknown_entry. Use ProjectReader::frames(), layouts(),
// load_nails(), open_entry_source() and read_steps() to reach the data.
void validate_semantic(const ProjectReader &reader, Report &report) {
    (void)reader;
    (void)report;
}

} // namespace format
} // namespace loomora

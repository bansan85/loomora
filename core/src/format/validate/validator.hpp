// The semantic validator (research R11, project-format.md section 8): the rules that need several
// items at once. The reader calls it at the end of ProjectReader::open, once the container pass and
// the item schema checks are done and every item could be read (ProjectReader::complete()).
#ifndef LOOMORA_FORMAT_VALIDATE_VALIDATOR_HPP
#define LOOMORA_FORMAT_VALIDATE_VALIDATOR_HPP

#include "format/report.hpp"

namespace loomora {
namespace format {

class ProjectReader;

// Adds the issues of the cross-item rules to `report` (references, nails inside their frame, step
// and selection indices, spool lengths, thresholds, distances, origin offset, thumbnails).
void validate_semantic(const ProjectReader &reader, Report &report);

} // namespace format
} // namespace loomora

#endif

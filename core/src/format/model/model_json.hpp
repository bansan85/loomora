// Reading the JSON entries of a project into the model structs (project-format.md section 4):
// manifest.json, spools.json, frame.json, layout.json, image.json and result.json. The input is a
// parsed DOM (json::parse_document); unknown members are ignored (FR-020), a missing required
// member, a value of the wrong type or a value outside its set is a FormatError(LOOMORA_ERR_CORRUPT)
// with the schema.* / num.not_finite codes of json_in.hpp. These functions only read: the rules of
// the data model (ranges, lengths, patterns) are the check_* functions of the model headers.
#ifndef LOOMORA_FORMAT_MODEL_MODEL_JSON_HPP
#define LOOMORA_FORMAT_MODEL_MODEL_JSON_HPP

#include <string>
#include <vector>

#include "format/json/json_in.hpp"
#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"

namespace loomora {
namespace format {
namespace model {

// Compact JSON text of a DOM value (keys in their parsed order).
std::string dump_json(const json::Value &value);

void parse_manifest(const json::Value &root, Manifest &out, const std::string &entry);
// `texts`, when not null, receives the compact JSON of each spool object (same order as `out`).
void parse_spools(const json::Value &root, std::vector<Spool> &out, std::vector<std::string> *texts,
                  const std::string &entry);
void parse_frame(const json::Value &root, Frame &out, const std::string &entry);
void parse_layout(const json::Value &root, Layout &out, const std::string &entry);
void parse_image(const json::Value &root, Image &out, const std::string &entry);
void parse_result(const json::Value &root, Result &out, const std::string &entry);

} // namespace model
} // namespace format
} // namespace loomora

#endif

// The JSON text of the items of a project (project-format.md section 4), in both directions for the
// fields the editor needs:
//
//   *_json(item)       writes the compact JSON entry of an item: keys in the order of the schema
//                      `properties` (the model headers give each order), numbers as in
//                      json/json_out.hpp. Used by the project writer.
//   parse_*_json(text) reads the JSON an application hands to the editor (loomora_editor_add_*):
//                      any key order and whitespace, unknown keys ignored, and the fields the editor
//                      owns (`id`, and for a layout `frame` and `nail_count`, for an image the
//                      `source`, `target` and `thumbnail` entries, for a result `steps`,
//                      `thumbnail` and `settings`) are ignored: the editor derives them. A text that
//                      is not valid JSON or lacks a field throws FormatError(LOOMORA_ERR_INVALID_ITEM)
//                      carrying the schema.* issue.
//                      Only 2D frames are read for now ("kind":"3d" is refused).
//
// The rules of the data model are NOT checked here (model::check_* do it, with the item identifier
// the editor allocates).
#ifndef LOOMORA_FORMAT_ITEM_JSON_HPP
#define LOOMORA_FORMAT_ITEM_JSON_HPP

#include <string>
#include <vector>

#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"

namespace loomora {
namespace format {

// ---- writing ----------------------------------------------------------------------------------
std::string spools_json(const std::vector<model::Spool> &spools); // {"spools":[...]}
std::string frame_json(const model::Frame &frame);
std::string layout_json(const model::Layout &layout);
std::string image_json(const model::Image &image);
std::string result_json(const model::Result &result);
std::string manifest_json(const model::Manifest &manifest);

// ---- reading (editor input) -------------------------------------------------------------------
void parse_spool_json(const std::string &text, model::Spool &out);
void parse_frame_json(const std::string &text, model::Frame &out);
void parse_layout_json(const std::string &text, model::Layout &out);
void parse_image_json(const std::string &text, model::Image &out);
void parse_result_json(const std::string &text, model::Result &out);

// Parses a free JSON object (settings.json, shape params) and returns it compact; throws
// FormatError(LOOMORA_ERR_INVALID_ITEM) when `text` is not a JSON object.
std::string compact_json_object(const std::string &text, const std::string &what);

} // namespace format
} // namespace loomora

#endif

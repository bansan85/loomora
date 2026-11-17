// The codec of nails.json (project-format.md 4.3): `{ "nails": [ {...}, ... ] }`, one nail per
// array element. It reads with the SAX parser (no DOM, memory proportional to the compact
// NailTable arrays, not to the text) and writes one nail after the other.
//
// Reading
//   read_nails       the whole entry into a NailTable; fills table.offsets, the range-read index
//                    (one entry per kNailsPerIndexEntry = 1,024 nails: the byte offset of the `{`
//                    of nail k * 1024), and checks the number of nails against `nail_count`.
//   read_nail_range  `count` nails starting at the byte offset of a nail (an entry of
//                    table.offsets): the reader is positioned at that `{`, so a range of the
//                    layout is read without parsing what comes before it. Stops after `count`
//                    nails or at the end of the array (fewer nails are then returned).
// Both throw FormatError(LOOMORA_ERR_CORRUPT) with the codes of json_in.hpp (schema.syntax,
// schema.depth, schema.duplicate_key, schema.type, schema.value, schema.missing, num.not_finite),
// plus schema.nail_count when the count differs from the expected one. The issue carries the entry
// name given by the caller.
//
// Which members are read: a nail needs id, x, y, diameter and kind; a nail of a 3D table (`is_3d`)
// also z, axis (3 numbers), length, side and face {triangle, u, v}. In a 2D table the 3D members
// are ignored, like any unknown member. Duplicate keys inside a nail are rejected; unknown
// members are skipped (their own content is not checked for duplicate keys). The values the
// schema constrains by range (diameter > 0, label length, axis unit length, face limits) are not
// checked here: model::check_nails does it on the filled table, so a value out of range is
// reported with the model's code and the item, not as a parse failure. `id` must be an integer
// from 0 to 4294967295; `kind` and `side` must be one of the known names.
//
// Writing
//   write_nails      appends `{"nails":[...]}` to a string: compact, keys in the order of the
//                    schema (id, x, y, z, diameter, kind, label, axis, length, side, face{triangle,
//                    u, v}; 2D nails have id, x, y, diameter, kind, label), numbers as in
//                    json_out.hpp. Throws FormatError(LOOMORA_ERR_INVALID_ITEM) for a number that
//                    is not finite, a kind or side out of range, or a label that is not UTF-8.
#ifndef LOOMORA_FORMAT_CODEC_NAILS_CODEC_HPP
#define LOOMORA_FORMAT_CODEC_NAILS_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "format/json/json_in.hpp"
#include "format/model/layout.hpp"

namespace loomora {
namespace format {
namespace codec {

// "Do not check the count" for read_nails.
const std::uint64_t kAnyNailCount = ~static_cast<std::uint64_t>(0);

// Reads the whole nails.json from `in` into `out` (left unchanged on error). `is_3d` is the kind
// of the layout's frame. `expected_count`: the `nail_count` of layout.json, or kAnyNailCount.
void read_nails(json::ByteReader &in, bool is_3d, model::NailTable &out, const std::string &entry,
                std::uint64_t expected_count = kAnyNailCount);

// Reads up to `count` nails from `in`, positioned at the `{` of a nail (see above), into `out`
// (replaced; its `offsets` stay empty).
void read_nail_range(json::ByteReader &in, bool is_3d, std::size_t count, model::NailTable &out,
                     const std::string &entry);

// Appends the text of nails.json for `nails` to `out`.
void write_nails(const model::NailTable &nails, std::string &out);

} // namespace codec
} // namespace format
} // namespace loomora

#endif

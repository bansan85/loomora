// What the model headers share: size limits of the data model, text patterns, enum helpers and
// the way a rule violation is recorded.
//
// The model structs are plain data (no behaviour beyond small helpers): the project editor, the
// reader, the writer and the validator all use them directly. Every struct has a check function
// `check_<struct>(item, entry, report)` that adds one error Issue per violated rule of
// data-model.md (nothing when the item is valid) and never throws (apart from std::bad_alloc).
// An editor that must refuse an invalid item turns the first issue into
// FormatError(LOOMORA_ERR_INVALID_ITEM, issue). Cross-item rules (references, a nail inside the
// outline, a length against the spool's available length, counts against the files) belong to
// the validator, not to these checks.
//
// Codes of the issues added here: schema.value (a value out of its range, pattern or set, or a
// combination the data model forbids), num.not_finite (NaN or Infinity), id.duplicate,
// nail.duplicate_id.
#ifndef LOOMORA_FORMAT_MODEL_COMMON_HPP
#define LOOMORA_FORMAT_MODEL_COMMON_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "format/report.hpp"

namespace loomora {
namespace format {
namespace model {

// Byte limits of strings (UTF-8 length).
const std::size_t kMaxTitleBytes = 1024;
const std::size_t kMaxNameBytes = 256;
const std::size_t kMaxLabelBytes = 64;   // nail label
const std::size_t kMaxEntryBytes = 512;  // an archive entry name written in a JSON file

// Tolerance of the unit vector of a nail axis, and of the face coordinates u, v.
const double kAxisTolerance = 1e-6;
const double kFaceTolerance = 1e-6;

// The major and minor of the format this library writes (the writer's constants).
const std::uint32_t kFormatMajor = 1;
const std::uint32_t kFormatMinor = 0;

// "#rrggbb", digits in either case.
bool is_hex_color(const std::string &s);
// "YYYY-MM-DDTHH:MM:SS[.f...]Z" (shape only: digits and separators, month 01-12, day 01-31,
// hour 00-23, minute 00-59, second 00-60).
bool is_iso8601_utc(const std::string &s);
// A UUID v4 in canonical lowercase text (8-4-4-4-12, version 4, variant 8, 9, a or b).
bool is_uuid4(const std::string &s);
// 64 lowercase hexadecimal characters.
bool is_sha256_hex(const std::string &s);
inline bool is_finite(double d) { return d - d == 0.0; }

// Adds one error issue about an item (kind kItemNone and id 0 for the project).
void add_error(Report &report, const char *code, const std::string &entry, ItemKind kind,
               std::uint32_t id, const std::string &message);
// add_error with schema.value, or num.not_finite when `value` is not finite.
void add_value_error(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                     const std::string &message);

// Checks of a double field: finite (else num.not_finite, returns false), then true when it is
// also in the asked range (else schema.value, returns false). `what` names the field.
bool check_finite(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                  const char *what, double value);
bool check_positive(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                    const char *what, double value);
// A string of at most `max_bytes` bytes.
void check_length(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                  const char *what, const std::string &value, std::size_t max_bytes);

// Looks `text` up in a table of names; false when absent. Used by the parse_* functions.
bool lookup_name(const char *const *names, std::size_t count, const std::string &text, int &index);

// A reference to an archive entry in a JSON file (image source, target, thumbnail; result
// thumbnail, settings): `entry` is the path, `format` the media format (png, jpeg, webp) or empty
// when absent, `size` the byte size or -1 when absent.
struct EntryRef {
    std::string entry;
    std::string format;
    std::int64_t size = -1;
};

void check_entry_ref(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                     const char *what, const EntryRef &ref);

} // namespace model
} // namespace format
} // namespace loomora

#endif

// Project (manifest.json, data-model.md "Project (manifest)"): the table of contents of a
// .loomora file. Items themselves (Frame, Layout, Image, Spool, Result) are in their own headers.
//
// How the pieces fit (used by the editor, the reader, the writer and the validator):
//   - an item kind has an `order` list (the display order, a permutation of the existing
//     identifiers) and a `next_id` counter that only increases: a new item takes `next_id` and
//     the counter moves up by one; a removed identifier is never reused;
//   - `counts` equals the length of the `order` lists;
//   - `entries` lists every archive entry except `mimetype` and `manifest.json`, in the canonical
//     entry order of project-format.md section 2 (a vector, not a map: the order is the one
//     written), with the uncompressed size and the SHA-256.
#ifndef LOOMORA_FORMAT_MODEL_PROJECT_HPP
#define LOOMORA_FORMAT_MODEL_PROJECT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "format/model/common.hpp"

namespace loomora {
namespace format {
namespace model {

enum Profile { kProfileFull = 0, kProfileLight = 1 };

const char *to_string(Profile p);
bool parse_profile(const std::string &text, Profile &out);

struct Version {
    std::uint32_t major = kFormatMajor;
    std::uint32_t minor = kFormatMinor;
};
// "M.m".
std::string version_text(const Version &v);
// Parses "M.m" (two decimal numbers, no sign or space); false otherwise.
bool parse_version(const std::string &text, Version &out);
// True when `a` is above `b` (major, then minor).
bool version_above(const Version &a, const Version &b);

struct FormatId {
    std::string name = "loomora"; // fixed
    std::uint32_t major = kFormatMajor;
    std::uint32_t minor = kFormatMinor;
};

struct Generator { // informational
    std::string application;
    std::string version;
    std::string platform;
    std::string architecture;
};

// The five item kinds, for the per-kind counters and lists (JSON keys differ: `next_id` uses the
// singular, `counts` and `order` the plural).
struct Counts {
    std::uint64_t frames = 0, layouts = 0, images = 0, spools = 0, results = 0;
};
struct NextIds {
    std::uint32_t frame = 1, layout = 1, image = 1, spool = 1, result = 1;
};
struct IdLists {
    std::vector<std::uint32_t> frames, layouts, images, spools, results;
};

struct ManifestEntry {
    std::string path;         // archive entry name
    std::uint64_t size = 0;   // uncompressed size
    std::string sha256;       // 64 lowercase hex characters
};

// Feature names of `required_features`.
extern const char *const kFeatureSurfaceMesh;     // "surface_mesh"
extern const char *const kFeatureStringLevels;    // "string_levels"
extern const char *const kFeatureMultiViewTarget; // "multi_view_target"

// JSON key order: format, min_reader, project_id, title, created_at, modified_at, generator,
// profile, editable, counts, next_id, order, required_features, entries, extensions.
struct Manifest {
    FormatId format;
    Version min_reader;
    std::string project_id;           // UUID v4, lowercase canonical
    std::string title;                // up to 1,024 bytes, may be empty
    std::string created_at;           // YYYY-MM-DDTHH:MM:SS[.fff]Z, supplied by the caller
    std::string modified_at;          // same; modified_at >= created_at is not enforced
    Generator generator;
    Profile profile = kProfileFull;
    bool editable = true;             // false for light
    Counts counts;
    NextIds next_id;
    IdLists order;
    std::vector<std::string> required_features; // subset of the three feature names, no repeat
    std::vector<ManifestEntry> entries;
    std::string extensions_json = "{}"; // compact JSON object, keys namespace.key; never interpreted
};

// The rules of the manifest on its own: format name, min_reader not above format, project_id,
// title, timestamps, light implies not editable, counts equal to the order lists, order lists
// without repeat, next_id above every identifier, required_features known and without repeat,
// entries without repeat, with a SHA-256 pattern and never `mimetype` or `manifest.json`.
// (That `required_features` is derived from the content and that `entries` matches the archive is
// checked by the validator.)
void check_manifest(const Manifest &manifest, Report &report);

} // namespace model
} // namespace format
} // namespace loomora

#endif

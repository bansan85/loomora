// The project writer (spec 002, T034): turns the staged content of a ProjectEditor into the bytes of
// a .loomora archive (project-format.md sections 1 to 3).
//
// Determinism (FR-004): the same editor content, `modified_at` and options give the same bytes. The
// entries go in the canonical order of section 2 (mimetype, manifest.json, spools.json, then every
// frame with its layouts, every image, every result, in increasing identifier); `.json` entries are
// deflated and every other one (`*.bin`, `*.png`, `*.jpg`, `*.webp`) stored, unless the option
// `kSaveStoreAll` (bit 0 of the C API `options`) stores everything.
//
// The manifest holds the size and SHA-256 of every other entry yet comes second in the archive, so
// the writer makes two passes over the staged content: the first only hashes and counts, the second
// writes. Entries are produced in pieces (nails.json chunk by chunk), so memory stays bounded by the
// staged content, whatever the size of the archive.
//
// The save validates the staged project (the manifest rules of model::check_manifest, and the
// FormatError(LOOMORA_ERR_INVALID_ITEM) of a modified_at that is not an ISO 8601 UTC time) and puts
// the warnings of FR-025 (`warn.count`) in `report`.
//
// Failures leave the previous file intact: a file target is written as a temporary file and
// replaced by an atomic rename only after everything was written and flushed. A cancelled progress
// callback (about every 4 MiB, plus once at the end) is FormatError(LOOMORA_ERR_CANCELLED).
#ifndef LOOMORA_FORMAT_PROJECT_WRITER_HPP
#define LOOMORA_FORMAT_PROJECT_WRITER_HPP

#include <cstdint>
#include <string>

#include "loomora.h"
#include "format/io/sink.hpp"
#include "format/project_editor.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {

const std::uint32_t kSaveStoreAll = 1;         // options bit 0: store every entry uncompressed
const std::uint32_t kSaveKnownOptions = 1;     // the bits this version knows

// Writes the whole archive to `sink` (neither committed nor aborted here). `report` may be null;
// the warnings are added to it. `progress` may be null.
void write_project(const ProjectEditor &editor, const std::string &modified_at, std::uint32_t options,
                   Sink &sink, loomora_progress_fn progress, void *progress_user, Report *report);

// After a commit: when `sink` left a file next to the target that it could not remove (Windows
// file sink, FR-028), adds the warning `warn.leftover_file` naming it to `report` (may be null).
// The save itself succeeded.
void report_leftover(const Sink &sink, Report *report);

// Writes to a file with an atomic replace (create_file_sink): the target is created or replaced only
// when everything succeeded.
void save_project_to_path(const ProjectEditor &editor, const std::string &utf8_path,
                          const std::string &modified_at, std::uint32_t options,
                          loomora_progress_fn progress, void *progress_user, Report *report);

// Writes to a caller's sink function: the archive is built in memory and handed over, in order,
// once complete (the ZIP writer patches earlier bytes, which a forward-only callback cannot do).
void save_project_to_callback(const ProjectEditor &editor, const std::string &modified_at,
                              std::uint32_t options, loomora_sink_fn sink, void *sink_user,
                              loomora_progress_fn progress, void *progress_user, Report *report);

} // namespace format
} // namespace loomora

#endif

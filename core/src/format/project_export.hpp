// The export of a project (spec 002, User Story 7; project-format.md section 7, FR-017, FR-017a,
// FR-017b, FR-018): a copy of an open project in the `full` or the `light` profile, whole or
// restricted to a subset of its results.
//
// How: the export is a ProjectEditor built over the open reader (the constructor
// ProjectEditor(base, ExportSelection), defined in project_export.cpp) and written by the ordinary
// project writer, so the determinism, the manifest regeneration, the validation of the manifest rules,
// the warnings and the atomic file replace are those of a save, and every entry the profile leaves
// unchanged is copied as its compressed bytes (ZipWriter::add_raw: same SHA-256 in the new manifest).
//
// The selection (ExportSelection):
//   - all_items: every frame, layout, image, spool and result (no pruning);
//   - otherwise the results listed and exactly what they use: their frame, their layout, their images
//     and their spools. The project identifier, `next_id` (so no identifier is ever reused) and the
//     display order of the source are kept; `counts` and `order` describe the file.
// The profile:
//   - full: everything of the selection, and the entries this version does not define;
//   - light: no images/*/source.*, target.png, results/*/settings.json, mesh-source.*; no mesh
//     `construction`; no texture coordinates (mesh.bin is rewritten without its UV section by
//     rewrite_mesh_without_uvs, positions, indices and normals keep their values byte for byte);
//     thumbnails stay and are checked (PNG, within 256 px and 64 KiB) and stripped of metadata again;
//     unknown entries are dropped; `profile` is "light" and `editable` false.
// A light project can only be exported as light (LOOMORA_ERR_READ_ONLY_PROJECT otherwise), and the
// editor refuses to open it (FR-017b).
//
// Memory: the rewritten mesh.bin of a frame with UVs is held in memory until written (the writer
// passes over each entry twice); every other piece is read from the archive in 64 KiB steps.
#ifndef LOOMORA_FORMAT_PROJECT_EXPORT_HPP
#define LOOMORA_FORMAT_PROJECT_EXPORT_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "loomora.h"
#include "format/project_editor.hpp"
#include "format/project_reader.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {

// The staged export of `base` (which must outlive the returned editor).
std::unique_ptr<ProjectEditor> make_export_editor(const ProjectReader &base, const ExportSelection &selection);

// Atomic replace of the file (the target is created or replaced only when everything succeeded).
void export_project_to_path(const ProjectReader &base, const ExportSelection &selection, const std::string &utf8_path,
                            const std::string &modified_at, std::uint32_t options, loomora_progress_fn progress,
                            void *progress_user, Report *report);

// Into a caller's sink function, in order, once the archive is complete.
void export_project_to_callback(const ProjectReader &base, const ExportSelection &selection,
                                const std::string &modified_at, std::uint32_t options, loomora_sink_fn sink,
                                void *sink_user, loomora_progress_fn progress, void *progress_user, Report *report);

} // namespace format
} // namespace loomora

#endif

/* Loomora backend: public C API of the .loomora project format (valid as C89 and C++98).
   Included at the end of loomora.h; do not include it first. LOOMORA_ABI_VERSION stays 1: the
   functions below are additive, and a caller that needs them checks loomora_format_version.
   Contract: specs/002-loomora-file-format/contracts/c-api-project.md.

   Conventions (as the rest of the C API): UTF-8 strings, caller-supplied buffers, negative
   LOOMORA_ERR_* codes, no C++ type and no exception across the boundary. Handles are opaque and
   expose no memory. */
#ifndef LOOMORA_PROJECT_H
#define LOOMORA_PROJECT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. Created and destroyed by the library; a handle exposes no memory. */
typedef struct loomora_project loomora_project;
typedef struct loomora_editor loomora_editor;
typedef struct loomora_report loomora_report;

/* Item kinds; identifiers are uint32_t, unique within their kind. */
#define LOOMORA_KIND_FRAME 1
#define LOOMORA_KIND_LAYOUT 2
#define LOOMORA_KIND_IMAGE 3
#define LOOMORA_KIND_SPOOL 4
#define LOOMORA_KIND_RESULT 5

/* Negative error codes of the project API (the codes -1 to -4 of loomora.h also apply). */
#define LOOMORA_ERR_IO -10                     /* read, write, flush, rename, disk full */
#define LOOMORA_ERR_NOT_A_PROJECT -11          /* not a ZIP archive, or mimetype missing or wrong */
#define LOOMORA_ERR_CORRUPT -12                /* container, checksum, schema or reference error */
#define LOOMORA_ERR_UNSUPPORTED_VERSION -13    /* major version above the library's */
#define LOOMORA_ERR_UNSUPPORTED_FEATURE -14    /* a required feature the library does not handle */
#define LOOMORA_ERR_BOMB -15                   /* decompression ratio above the guard */
#define LOOMORA_ERR_NO_MEMORY -16              /* allocation failed */
#define LOOMORA_ERR_READ_ONLY_PROJECT -17      /* edit of a light project */
#define LOOMORA_ERR_NOT_FOUND -18              /* unknown kind, identifier or entry */
#define LOOMORA_ERR_CANCELLED -19              /* the progress callback asked to stop */
#define LOOMORA_ERR_HAS_DEPENDENTS -20         /* the edit needs confirmation: dependents listed */
#define LOOMORA_ERR_INVALID_ITEM -21           /* the item does not satisfy the data model */
#define LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM -22 /* an offset the platform cannot address */

/* Progress callback: called about every 4 MiB processed with the bytes done and the total
   (0 when unknown). A non-zero return cancels the operation with LOOMORA_ERR_CANCELLED and, for
   a save, leaves the previous file intact. */
typedef int32_t (*loomora_progress_fn)(void *user, uint64_t done, uint64_t total);

/* Output sink of a streaming save: receives the bytes in order. A non-zero return stops the save
   with LOOMORA_ERR_IO. */
typedef int32_t (*loomora_sink_fn)(void *user, const uint8_t *data, size_t len);

/* Selectors of loomora_format_limit (the warning thresholds and guards of the data model). */
#define LOOMORA_LIMIT_NAILS_PER_LAYOUT 1   /* warn above this many nails in a layout */
#define LOOMORA_LIMIT_STEPS_PER_RESULT 2   /* warn above this many steps in a result */
#define LOOMORA_LIMIT_TRIANGLES_PER_MESH 3 /* warn above this many triangles in a mesh */
#define LOOMORA_LIMIT_DECOMPRESSED_TOTAL 4 /* warn above this many decompressed bytes in all */
#define LOOMORA_LIMIT_ITEMS_PER_KIND 5     /* warn above this many items of one kind */
#define LOOMORA_LIMIT_BOMB_RATIO 6         /* rejected above this ratio (n : 1) after 1 MiB */
#define LOOMORA_LIMIT_THUMB_MAX_SIDE 7     /* thumbnail side in pixels */
#define LOOMORA_LIMIT_THUMB_MAX_BYTES 8    /* thumbnail size in bytes */
#define LOOMORA_LIMIT_JSON_DEPTH 9         /* nesting depth of a JSON entry */
#define LOOMORA_LIMIT_ENTRY_NAME_LENGTH 10 /* entry name length in bytes */

/* Format version this library writes and reads: *major = 1, *minor = 0 (either pointer may be
   NULL). Returns 1. Never fails: a caller checks it to learn that the project API exists. */
LOOMORA_API uint32_t loomora_format_version(int32_t *major, int32_t *minor);

/* Current value of the limit `which` (a LOOMORA_LIMIT_* selector), or 0 for an unknown selector. */
LOOMORA_API uint64_t loomora_format_limit(int32_t which);

/* Builds a lowercase canonical UUID version 4 ("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", y in
   8, 9, a, b) from 16 random bytes supplied by the caller (the library reads no random source):
   the version and variant bits are set in a copy. Writes 36 characters and a NUL into out[0..37).
   Returns 0, or LOOMORA_ERR_INVALID for a NULL pointer. */
LOOMORA_API int32_t loomora_uuid4_from_random(const uint8_t random16[16], char out[37]);

/* ---- reader: open, read, report, validate ---- */

/* Bulk data layouts: fixed sizes, no hidden padding. */
typedef struct {            /* 88 bytes, 8-byte aligned */
    uint32_t id;
    uint32_t kind;          /* 1 boundary, 2 exclusion_boundary, 3 interior */
    uint32_t flags;         /* bit 0: 3D fields valid, bit 1: has a label */
    uint32_t side;          /* 3D: 0 exterior, 1 interior */
    double x, y, z;
    double diameter, length;
    float axis[3];
    float face_u;
    float face_v;
    uint32_t face_triangle;
    uint32_t reserved0, reserved1;
} loomora_nail;

typedef struct {            /* 12 bytes */
    uint32_t nail;
    uint32_t spool_index;
    uint8_t rotation;       /* 0 CW, 1 CCW */
    uint8_t type;           /* 0 segment, 1 start, 2 end */
    uint8_t level;
    uint8_t reserved;
} loomora_step;

#define LOOMORA_MESH_NORMALS 1 /* flags of loomora_mesh_info */
#define LOOMORA_MESH_UVS 2

typedef struct {            /* 24 bytes */
    uint64_t vertices;
    uint64_t triangles;
    uint32_t flags;
    uint32_t reserved;
} loomora_mesh_info;

/* Opens, validates (streaming) and returns the handle. Never writes, never creates a file. On
   success *report (if not NULL) holds the warnings; on failure *project is NULL and *report holds
   the errors (when the container could be read). The memory variant does not copy: the buffer
   must stay valid until loomora_project_close. The report is freed with loomora_report_free. */
LOOMORA_API int32_t loomora_project_open_path(const char *path, loomora_progress_fn cb, void *user,
                                              loomora_project **project, loomora_report **report);
LOOMORA_API int32_t loomora_project_open_memory(const uint8_t *data, size_t len,
                                                loomora_progress_fn cb, void *user,
                                                loomora_project **project, loomora_report **report);
LOOMORA_API void loomora_project_close(loomora_project *project);

/* Validation only (no handle kept); the same report. Returns 0 or the LOOMORA_ERR_* code. */
LOOMORA_API int32_t loomora_validate_path(const char *path, loomora_progress_fn cb, void *user,
                                          loomora_report **report);
LOOMORA_API int32_t loomora_validate_memory(const uint8_t *data, size_t len, loomora_progress_fn cb,
                                            void *user, loomora_report **report);

/* Variable-size results: the total length needed (>= 0) or a negative error; bytes are written
   only when cap is at least that length. No NUL is written. */
LOOMORA_API int64_t loomora_project_manifest(const loomora_project *p, uint8_t *buf, size_t cap);
LOOMORA_API int32_t loomora_project_is_light(const loomora_project *p); /* 1 light, 0 full */
LOOMORA_API int64_t loomora_project_count(const loomora_project *p, int32_t kind);
LOOMORA_API int64_t loomora_project_ids(const loomora_project *p, int32_t kind, uint32_t *ids,
                                        size_t cap);
LOOMORA_API int64_t loomora_project_item_json(const loomora_project *p, int32_t kind, uint32_t id,
                                              uint8_t *buf, size_t cap);
LOOMORA_API int64_t loomora_project_entry_size(const loomora_project *p, const char *entry);
/* Range of any entry: returns the bytes written (fewer than cap at the end). */
LOOMORA_API int64_t loomora_project_read_entry(const loomora_project *p, const char *entry,
                                               uint64_t offset, uint8_t *buf, size_t cap);

LOOMORA_API int64_t loomora_layout_nail_count(const loomora_project *p, uint32_t layout);
LOOMORA_API int64_t loomora_layout_read_nails(const loomora_project *p, uint32_t layout,
                                              uint64_t first, size_t count, loomora_nail *out);
LOOMORA_API int64_t loomora_layout_nail_label(const loomora_project *p, uint32_t layout,
                                              uint32_t nail_id, uint8_t *buf, size_t cap);
LOOMORA_API int64_t loomora_result_step_count(const loomora_project *p, uint32_t result);
LOOMORA_API int64_t loomora_result_read_steps(const loomora_project *p, uint32_t result,
                                              uint64_t first, size_t count, loomora_step *out);
LOOMORA_API int32_t loomora_frame_mesh_info(const loomora_project *p, uint32_t frame,
                                            loomora_mesh_info *info);
LOOMORA_API int64_t loomora_frame_read_vertices(const loomora_project *p, uint32_t frame,
                                                uint64_t first, size_t count, float *xyz);
LOOMORA_API int64_t loomora_frame_read_triangles(const loomora_project *p, uint32_t frame,
                                                 uint64_t first, size_t count, uint32_t *abc);
LOOMORA_API int64_t loomora_frame_read_normals(const loomora_project *p, uint32_t frame,
                                               uint64_t first, size_t count, float *xyz);
LOOMORA_API int64_t loomora_frame_read_uvs(const loomora_project *p, uint32_t frame, uint64_t first,
                                           size_t count, float *uv); /* 0 written when none */

/* Report: severity 1 warning, 2 error; item_kind 0 none/project or LOOMORA_KIND_*; item_id 0 when
   none; text field 1 code, 2 entry path, 3 message (UTF-8, English). */
LOOMORA_API int64_t loomora_report_count(const loomora_report *r);
LOOMORA_API int64_t loomora_report_error_count(const loomora_report *r);
LOOMORA_API int32_t loomora_report_issue(const loomora_report *r, size_t index, int32_t *severity,
                                         int32_t *item_kind, uint32_t *item_id);
LOOMORA_API int64_t loomora_report_text(const loomora_report *r, size_t index, int32_t field,
                                        uint8_t *buf, size_t cap);
LOOMORA_API void loomora_report_free(loomora_report *r);

/* ---- end of reader block ---- */

/* ---- editor: create and save ---- */

/* Save options (bit mask). */
#define LOOMORA_SAVE_STORE_ALL 1 /* store every entry uncompressed */

/* Creates an empty full project. `project_id_random16` are 16 random bytes supplied by the caller
   (turned into a UUID v4 as loomora_uuid4_from_random does); `title` is UTF-8 (up to 1,024 bytes);
   `created_at` is "YYYY-MM-DDTHH:MM:SSZ". The library reads no clock and no random source.
   On success *editor is a handle to free with loomora_editor_close. */
LOOMORA_API int32_t loomora_editor_create(const uint8_t project_id_random16[16], const char *title,
                                          const char *created_at, loomora_editor **editor);
LOOMORA_API void loomora_editor_close(loomora_editor *e);

/* Items are passed as UTF-8 JSON following the schemas; the identifier is allocated by the editor and
   returned in *id (it may be NULL); the "id" field of the JSON, if present, is ignored. A failed call
   changes nothing. Errors: LOOMORA_ERR_INVALID (null pointer), LOOMORA_ERR_INVALID_ITEM (the item does
   not satisfy the data model), LOOMORA_ERR_NOT_FOUND (unknown frame, layout or nail). */
LOOMORA_API int32_t loomora_editor_add_spool(loomora_editor *e, const char *json, uint32_t *id);
/* 2D frames only for now ("kind":"2d"): a 3D frame is refused with LOOMORA_ERR_INVALID_ITEM. */
LOOMORA_API int32_t loomora_editor_add_frame(loomora_editor *e, const char *json, uint32_t *id);
/* `json` holds the "name"; "frame" and "nail_count" come from the arguments. The "label" bit of
   loomora_nail.flags is ignored: labels are set with loomora_editor_layout_set_label. */
LOOMORA_API int32_t loomora_editor_add_layout(loomora_editor *e, uint32_t frame, const char *json,
                                              const loomora_nail *nails, size_t count, uint32_t *id);
LOOMORA_API int32_t loomora_editor_layout_set_label(loomora_editor *e, uint32_t layout, uint32_t nail_id,
                                                    const char *label_or_null); /* NULL removes it */
/* `json` holds name, color_mode, shape, width_px, height_px; the pictures are PNG, JPEG or WebP
   bytes whose metadata is stripped: `source` is required, `target_png` optional (NULL, 0), `thumb_png`
   required (PNG within 256 x 256 px and 64 KiB). */
LOOMORA_API int32_t loomora_editor_add_image(loomora_editor *e, const char *json,
                                             const uint8_t *source, size_t source_len,
                                             const uint8_t *target_png, size_t target_len,
                                             const uint8_t *thumb_png, size_t thumb_len, uint32_t *id);
/* `json` holds name, created_at, frame, layout, images, spools (the selection) and optionally
   "levels"; "steps" comes from step_count. `settings_json_or_null` is a JSON object or NULL. */
LOOMORA_API int32_t loomora_editor_add_result(loomora_editor *e, const char *json,
                                              const loomora_step *steps, size_t step_count,
                                              const uint8_t *thumb_png, size_t thumb_len,
                                              const char *settings_json_or_null, uint32_t *id);
LOOMORA_API int32_t loomora_editor_set_title(loomora_editor *e, const char *title);

/* Validates the staged project, writes a new archive and replaces `path` atomically: after any
   failure, including LOOMORA_ERR_CANCELLED from the progress callback, the previous file is intact
   and no temporary file is left. `modified_at` is the caller's time. `options`: LOOMORA_SAVE_*.
   `report` (may be NULL) receives a report to free with loomora_report_free: the warnings after a
   success, the cause after a failure. */
LOOMORA_API int32_t loomora_editor_save_path(loomora_editor *e, const char *path, const char *modified_at,
                                             uint32_t options, loomora_progress_fn cb, void *user,
                                             loomora_report **report);
/* The archive is handed to `sink` in order, once complete (a non-zero return stops with LOOMORA_ERR_IO). */
LOOMORA_API int32_t loomora_editor_save_stream(loomora_editor *e, const char *modified_at, uint32_t options,
                                               loomora_sink_fn sink, void *user, loomora_report **report);

/* ---- end of editor: create and save ---- */

/* ---- editor: 3D, edit, dependents ---- */

/* Operations of loomora_editor_dependents. */
#define LOOMORA_OP_REMOVE 1 /* the removal of the item */
#define LOOMORA_OP_CHANGE 2 /* a change of a field results depend on (data-model.md, "Which changes count") */

/* Opens a project for edit: the file is opened and validated as loomora_project_open_path does (*report
   gets the warnings, or the errors on failure), and stays open: items that are not touched are copied
   into the next save as their compressed bytes, so their SHA-256 does not change. A light project is
   refused with LOOMORA_ERR_READ_ONLY_PROJECT ("This project is a light copy and cannot be edited"). Entries
   this version does not define are kept. The handle is freed with loomora_editor_close. */
LOOMORA_API int32_t loomora_editor_open_path(const char *path, loomora_progress_fn cb, void *user,
                                             loomora_editor **editor, loomora_report **report);

/* 3D frames are added with loomora_editor_add_frame ("kind":"3d", "dimensions" with "depth_mm", a "body" of
   solid, shell or guide, a "surface" of mesh, primitive, extrusion, revolution or csg, optionally a "mesh"
   object with watertight, manifold, source and construction). On a 3D frame the nails of add_layout and
   replace_layout_nails must have bit 0 of loomora_nail.flags set and carry z, axis, length, side and the face.
   The first mesh of a frame that has none: info gives the counts and the flags (LOOMORA_MESH_NORMALS /
   LOOMORA_MESH_UVS), xyz 3 floats per vertex, abc 3 indices per triangle, normals 3 floats per vertex
   (required with the flag), uvs 2 floats per vertex (required with the flag). Afterwards use
   loomora_editor_replace_frame_mesh. A frame whose surface is "mesh" must have its mesh before a save. */
LOOMORA_API int32_t loomora_editor_frame_set_mesh(loomora_editor *e, uint32_t frame, const loomora_mesh_info *info,
                                                  const float *xyz, const uint32_t *abc,
                                                  const float *normals_or_null, const float *uvs_or_null);
/* The copy of the imported mesh file (full only): `name` gives the extension of mesh-source.<ext>; NULL data
   removes it. The frame must have a mesh. Never has dependents. */
LOOMORA_API int32_t loomora_editor_frame_set_attached_source(loomora_editor *e, uint32_t frame, const char *name,
                                                             const uint8_t *data, size_t len);

/* Replace = same identifier, new content. A change of a field results depend on (the "Which changes count"
   table of the data model) removes the dependent results, in the same call, only when confirm_dependents is
   non-zero; otherwise LOOMORA_ERR_HAS_DEPENDENTS and nothing changes (loomora_editor_dependents lists them).
   A change of any other field removes nothing. replace_item refuses with LOOMORA_ERR_INVALID_ITEM a change of
   the frame of a layout, of the frame, layout, images or spools of a result, or of the kind of a frame
   (issue code "edit.immutable"); the only change of a result is its name. replace_item of an image changes
   its JSON part only. */
LOOMORA_API int32_t loomora_editor_replace_item(loomora_editor *e, int32_t kind, uint32_t id, const char *json,
                                                int32_t confirm_dependents);
/* Keeps the labels of the nails whose identifier remains. */
LOOMORA_API int32_t loomora_editor_replace_layout_nails(loomora_editor *e, uint32_t layout,
                                                        const loomora_nail *nails, size_t count,
                                                        int32_t confirm_dependents);
LOOMORA_API int32_t loomora_editor_replace_frame_mesh(loomora_editor *e, uint32_t frame,
                                                      const loomora_mesh_info *info, const float *xyz,
                                                      const uint32_t *abc, const float *normals_or_null,
                                                      const float *uvs_or_null, int32_t confirm_dependents);
/* `json` as for add_image; a picture that is NULL (0 bytes) is kept as it is. */
LOOMORA_API int32_t loomora_editor_replace_image(loomora_editor *e, uint32_t image, const char *json,
                                                 const uint8_t *source, size_t source_len,
                                                 const uint8_t *target_png, size_t target_len,
                                                 const uint8_t *thumb_png, size_t thumb_len,
                                                 int32_t confirm_dependents);
/* The thumbnail of an image or of a result (kind LOOMORA_KIND_IMAGE or LOOMORA_KIND_RESULT): never has dependents. */
LOOMORA_API int32_t loomora_editor_replace_thumbnail(loomora_editor *e, int32_t kind, uint32_t id,
                                                     const uint8_t *png, size_t len);
/* Removes an item and what depends on it (a frame: its layouts and their results; a layout, an image or a
   spool: the results that use it; a result: nothing else). An identifier removed is never reused. */
LOOMORA_API int32_t loomora_editor_remove_item(loomora_editor *e, int32_t kind, uint32_t id,
                                               int32_t confirm_dependents);
/* What `operation` on an item would remove: pairs (kind, id) of layouts, then results, written to
   pairs[0 .. 2n) only when cap_pairs >= n. Returns n, or a negative error (LOOMORA_ERR_NOT_FOUND for an
   unknown item, LOOMORA_ERR_INVALID for an unknown operation). */
LOOMORA_API int64_t loomora_editor_dependents(const loomora_editor *e, int32_t kind, uint32_t id,
                                              int32_t operation, uint32_t *pairs, size_t cap_pairs);
/* The display order of a kind: `ids` is a permutation of the identifiers of the kind. */
LOOMORA_API int32_t loomora_editor_set_order(loomora_editor *e, int32_t kind, const uint32_t *ids, size_t n);

/* ---- end of editor: 3D, edit, dependents ---- */

#ifdef __cplusplus
}
#endif

#endif

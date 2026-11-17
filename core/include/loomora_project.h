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

#ifdef __cplusplus
}
#endif

#endif

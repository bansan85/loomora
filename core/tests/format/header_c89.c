/* Compiled as C89 (-std=c90 -pedantic-errors, see core/tests/CMakeLists.txt) to prove that the
   public header is valid C89: only comments of this style, declarations before statements. */
#include "loomora.h"

int loomora_header_c89_check(void);

int loomora_header_c89_check(void) {
    loomora_project *project = 0;
    loomora_editor *editor = 0;
    loomora_report *report = 0;
    loomora_progress_fn progress = 0;
    loomora_sink_fn sink = 0;
    int32_t major = 0;
    int32_t minor = 0;
    uint8_t random16[16];
    char uuid[37];
    int i;
    int32_t kinds[5];
    int32_t errors[13];
    loomora_nail nail;
    loomora_step step;
    loomora_mesh_info mesh;
    int64_t n;
    int32_t severity = 0;
    int32_t item_kind = 0;
    uint32_t item_id = 0;

    for (i = 0; i < 16; i++)
        random16[i] = (uint8_t)i;
    kinds[0] = LOOMORA_KIND_FRAME;
    kinds[1] = LOOMORA_KIND_LAYOUT;
    kinds[2] = LOOMORA_KIND_IMAGE;
    kinds[3] = LOOMORA_KIND_SPOOL;
    kinds[4] = LOOMORA_KIND_RESULT;
    errors[0] = LOOMORA_ERR_IO;
    errors[1] = LOOMORA_ERR_NOT_A_PROJECT;
    errors[2] = LOOMORA_ERR_CORRUPT;
    errors[3] = LOOMORA_ERR_UNSUPPORTED_VERSION;
    errors[4] = LOOMORA_ERR_UNSUPPORTED_FEATURE;
    errors[5] = LOOMORA_ERR_BOMB;
    errors[6] = LOOMORA_ERR_NO_MEMORY;
    errors[7] = LOOMORA_ERR_READ_ONLY_PROJECT;
    errors[8] = LOOMORA_ERR_NOT_FOUND;
    errors[9] = LOOMORA_ERR_CANCELLED;
    errors[10] = LOOMORA_ERR_HAS_DEPENDENTS;
    errors[11] = LOOMORA_ERR_INVALID_ITEM;
    errors[12] = LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM;
    (void)project;
    (void)editor;
    (void)report;
    (void)progress;
    (void)sink;
    (void)kinds;
    (void)errors;
    if (loomora_format_version(&major, &minor) != 1u)
        return 1;
    if (major != 1 || minor != 0)
        return 2;
    if (loomora_format_limit(LOOMORA_LIMIT_NAILS_PER_LAYOUT) == 0u)
        return 3;
    if (loomora_uuid4_from_random(random16, uuid) != 0)
        return 4;
    /* The reader block: every function must be declared and callable as C89. */
    nail.id = 0;
    step.nail = 0;
    mesh.vertices = 0;
    (void)nail;
    (void)step;
    (void)mesh;
    if (loomora_project_open_path(0, 0, 0, &project, &report) != LOOMORA_ERR_INVALID)
        return 6;
    if (loomora_project_open_memory(0, 1, 0, 0, &project, &report) != LOOMORA_ERR_INVALID)
        return 7;
    if (loomora_validate_path(0, 0, 0, &report) != LOOMORA_ERR_INVALID)
        return 8;
    if (loomora_validate_memory(0, 1, 0, 0, &report) != LOOMORA_ERR_INVALID)
        return 9;
    n = loomora_project_count(0, LOOMORA_KIND_FRAME) + loomora_project_ids(0, 1, 0, 0) +
        loomora_project_manifest(0, 0, 0) + loomora_project_item_json(0, 1, 1, 0, 0) +
        loomora_project_entry_size(0, "x") + loomora_project_read_entry(0, "x", 0, 0, 0) +
        loomora_layout_nail_count(0, 1) + loomora_layout_read_nails(0, 1, 0, 0, 0) +
        loomora_layout_nail_label(0, 1, 0, 0, 0) + loomora_result_step_count(0, 1) +
        loomora_result_read_steps(0, 1, 0, 0, 0) + loomora_frame_read_vertices(0, 1, 0, 0, 0) +
        loomora_frame_read_triangles(0, 1, 0, 0, 0) + loomora_frame_read_normals(0, 1, 0, 0, 0) +
        loomora_frame_read_uvs(0, 1, 0, 0, 0) + loomora_report_count(0) +
        loomora_report_error_count(0) + loomora_report_text(0, 0, 1, 0, 0);
    (void)n; /* the calls must link and return; the null-handle answers are tested in C++ */
    if (loomora_project_is_light(0) != LOOMORA_ERR_INVALID)
        return 10;
    if (loomora_frame_mesh_info(0, 1, &mesh) != LOOMORA_ERR_INVALID)
        return 11;
    if (loomora_report_issue(0, 0, &severity, &item_kind, &item_id) != LOOMORA_ERR_INVALID)
        return 12;
    loomora_project_close(0);
    loomora_report_free(0);
    return loomora_abi_version() == 1u ? 0 : 5;
}

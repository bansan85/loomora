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
    return loomora_abi_version() == 1u ? 0 : 5;
}

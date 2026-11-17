// C binding of the .loomora project format (loomora_project.h), the library functions only:
// loomora_format_version, loomora_format_limit, loomora_uuid4_from_random.
//
// This file holds ONLY these three functions. Each later story puts its functions of the C API in
// its own file, core/src/c_api_project_<area>.cpp (for example c_api_project_read.cpp,
// c_api_project_edit.cpp): core/CMakeLists.txt picks up every src/c_api_project*.cpp by glob, so
// no shared file has to be edited. The same rules apply everywhere: a thin forwarding layer, a
// try / catch (...) around every call (no exception ever crosses a function declared here), no C++
// type in a signature, buffers owned by the caller.
#include "../include/loomora.h"

#include "format/limits.hpp"
#include "format/report.hpp"
#include "internal/config.hpp"

extern "C" {

uint32_t loomora_format_version(int32_t *major, int32_t *minor) {
    if (major != nullptr)
        *major = 1;
    if (minor != nullptr)
        *minor = 0;
    return 1;
}

uint64_t loomora_format_limit(int32_t which) {
    try {
        const loomora::format::Limits &l = loomora::format::limits();
        switch (which) {
        case LOOMORA_LIMIT_NAILS_PER_LAYOUT:
            return l.warn_nails_per_layout;
        case LOOMORA_LIMIT_STEPS_PER_RESULT:
            return l.warn_steps_per_result;
        case LOOMORA_LIMIT_TRIANGLES_PER_MESH:
            return l.warn_triangles_per_mesh;
        case LOOMORA_LIMIT_DECOMPRESSED_TOTAL:
            return l.warn_decompressed_total;
        case LOOMORA_LIMIT_ITEMS_PER_KIND:
            return l.warn_items_per_kind;
        case LOOMORA_LIMIT_BOMB_RATIO:
            return l.bomb_ratio;
        case LOOMORA_LIMIT_THUMB_MAX_SIDE:
            return l.thumb_max_side;
        case LOOMORA_LIMIT_THUMB_MAX_BYTES:
            return l.thumb_max_bytes;
        case LOOMORA_LIMIT_JSON_DEPTH:
            return l.json_depth;
        case LOOMORA_LIMIT_ENTRY_NAME_LENGTH:
            return l.entry_name_max_bytes;
        default:
            return 0;
        }
    } catch (...) { // cannot happen today; a safe value
        return 0;
    }
}

int32_t loomora_uuid4_from_random(const uint8_t random16[16], char out[37]) {
    if (LOOMORA_UNLIKELY(random16 == nullptr || out == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        static const char hex[] = "0123456789abcdef";
        uint8_t b[16];
        for (int i = 0; i < 16; ++i)
            b[i] = random16[i];
        b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40); // version 4
        b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80); // variant 10xx
        int pos = 0;
        for (int i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                out[pos++] = '-';
            out[pos++] = hex[b[i] >> 4];
            out[pos++] = hex[b[i] & 0x0F];
        }
        out[pos] = '\0';
        return 0;
    } catch (...) {
        return LOOMORA_ERR_INTERNAL;
    }
}

} // extern "C"

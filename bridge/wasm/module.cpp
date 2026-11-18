// Translation unit of the Web module (bridge/wasm/CMakeLists.txt). The C API is implemented in core/
// (libloomora) and exported by name from bridge/wasm/exports.txt; this file only holds the three
// functions the page cannot do with the C API alone, for the project probe modes of spec 002
// (contracts/ui-validate-project.md). No logic: memory and a sink.
//
//   loomora_web_alloc(n)      a buffer of n bytes in the module's memory, for the page to fill with
//                             the file it read (loomora_project_open_memory does not copy it: the
//                             buffer stays until loomora_project_close). 0 when memory is short.
//   loomora_web_free(p)       releases such a buffer.
//   loomora_web_export(...)   loomora_project_export_stream of every result into a growing memory
//                             buffer (a page cannot give the C API a callback in both Emscripten
//                             builds): *data and *size describe it, released with loomora_web_free.
//
// C++11. The page never needs the allocator itself: it asks for these buffers by name.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "loomora.h"

namespace {

// The sink of loomora_project_export_stream: appends each chunk to a buffer grown with realloc.
struct Sink {
    std::uint8_t *data;
    std::size_t size;
    std::size_t capacity;
    bool failed;
};

std::int32_t AppendChunk(void *user, const std::uint8_t *chunk, std::size_t length) {
    Sink *sink = static_cast<Sink *>(user);
    if (length > static_cast<std::size_t>(-1) - sink->size) {
        sink->failed = true;
        return 1; // stops the export
    }
    const std::size_t needed = sink->size + length;
    if (needed > sink->capacity) {
        std::size_t capacity = sink->capacity != 0 ? sink->capacity : 4096;
        while (capacity < needed) {
            if (capacity > static_cast<std::size_t>(-1) / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        std::uint8_t *grown = static_cast<std::uint8_t *>(std::realloc(sink->data, capacity));
        if (grown == NULL) {
            sink->failed = true;
            return 1;
        }
        sink->data = grown;
        sink->capacity = capacity;
    }
    if (length != 0)
        std::memcpy(sink->data + sink->size, chunk, length);
    sink->size = needed;
    return 0;
}

} // namespace

extern "C" {

std::uint8_t *loomora_web_alloc(std::size_t n) {
    return static_cast<std::uint8_t *>(std::malloc(n != 0 ? n : 1));
}

void loomora_web_free(std::uint8_t *p) {
    std::free(p);
}

// Writes a copy of every result of `project` (profile: LOOMORA_PROFILE_FULL or _LIGHT; modified_at:
// the caller's time) into memory. Returns 0 with *data and *size set (free *data with
// loomora_web_free), or the LOOMORA_ERR_* code with *data = NULL and *size = 0.
std::int32_t loomora_web_export(const loomora_project *project, std::int32_t profile,
                                const char *modified_at, std::uint8_t **data, std::size_t *size) {
    if (data == NULL || size == NULL)
        return LOOMORA_ERR_INVALID;
    *data = NULL;
    *size = 0;
    Sink sink;
    sink.data = NULL;
    sink.size = 0;
    sink.capacity = 0;
    sink.failed = false;
    loomora_report *report = NULL;
    std::int32_t rc = loomora_project_export_stream(project, profile, NULL, 0, modified_at, 0,
                                                    AppendChunk, &sink, &report);
    if (report != NULL)
        loomora_report_free(report);
    if (rc == 0 && sink.failed)
        rc = LOOMORA_ERR_NO_MEMORY;
    if (rc != 0) {
        std::free(sink.data);
        return rc;
    }
    *data = sink.data;
    *size = sink.size;
    return 0;
}

} // extern "C"

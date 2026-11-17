// C binding of the light and subset export (loomora_project.h, "export: light and subset"): a thin
// forwarding layer over format/project_export.hpp. A try / catch (...) wraps every call, so no exception
// ever crosses a function declared here; no C++ type appears in a signature.
#include "../include/loomora.h"

#include <new>
#include <string>
#include <vector>

#include "c_api_project_handles.hpp"
#include "c_api_project_reader_handle.hpp"
#include "format/project_export.hpp"
#include "format/project_writer.hpp"
#include "format/report.hpp"
#include "internal/config.hpp"

namespace {

using loomora::format::ExportSelection;
using loomora::format::FormatError;
using loomora::format::Report;

// Hands `report` to the caller (nothing when `out` is null or the allocation fails).
void give_report(loomora_report **out, const Report &report) {
    if (out == nullptr)
        return;
    loomora_report *r = new (std::nothrow) loomora_report;
    if (r == nullptr)
        return;
    try {
        r->report = report;
    } catch (...) {
        delete r;
        return;
    }
    *out = r;
}

// The arguments of both export functions, checked: the selection of the profile and of the results.
ExportSelection selection_of(int32_t profile, const uint32_t *ids, size_t n) {
    if (profile != LOOMORA_PROFILE_FULL && profile != LOOMORA_PROFILE_LIGHT)
        throw FormatError(LOOMORA_ERR_INVALID, "profile must be LOOMORA_PROFILE_FULL or LOOMORA_PROFILE_LIGHT");
    if (n != 0 && ids == nullptr)
        throw FormatError(LOOMORA_ERR_INVALID, "result_ids is NULL while n_results is not 0");
    ExportSelection sel;
    sel.light = profile == LOOMORA_PROFILE_LIGHT;
    sel.all_items = n == 0;
    if (n != 0)
        sel.results.assign(ids, ids + n);
    return sel;
}

// Runs an export: the report holds the warnings on success and the cause on failure.
template <typename Fn> int32_t export_with_report(loomora_report **report, Fn fn) {
    Report rep;
    int32_t code = 0;
    try {
        fn(report != nullptr ? &rep : nullptr);
    } catch (const FormatError &e) {
        code = e.code();
        try {
            rep.add(e, "export.failed", "");
        } catch (...) {
        }
    } catch (...) {
        code = loomora::format::code_of_current_exception();
    }
    give_report(report, rep);
    return code;
}

} // namespace

extern "C" {

int32_t loomora_project_export_path(const loomora_project *p, const char *path, int32_t profile,
                                    const uint32_t *result_ids, size_t n_results, const char *modified_at,
                                    uint32_t options, loomora_progress_fn cb, void *user, loomora_report **report) {
    if (report != nullptr)
        *report = nullptr;
    if (LOOMORA_UNLIKELY(p == nullptr || path == nullptr || modified_at == nullptr))
        return LOOMORA_ERR_INVALID;
    return export_with_report(report, [&](Report *rep) {
        const ExportSelection sel = selection_of(profile, result_ids, n_results);
        loomora::format::export_project_to_path(*p->reader, sel, path, modified_at, options, cb, user, rep);
    });
}

int32_t loomora_project_export_stream(const loomora_project *p, int32_t profile, const uint32_t *result_ids,
                                      size_t n_results, const char *modified_at, uint32_t options,
                                      loomora_sink_fn sink, void *user, loomora_report **report) {
    if (report != nullptr)
        *report = nullptr;
    if (LOOMORA_UNLIKELY(p == nullptr || modified_at == nullptr || sink == nullptr))
        return LOOMORA_ERR_INVALID;
    return export_with_report(report, [&](Report *rep) {
        const ExportSelection sel = selection_of(profile, result_ids, n_results);
        loomora::format::export_project_to_callback(*p->reader, sel, modified_at, options, sink, user, nullptr,
                                                    nullptr, rep);
    });
}

} // extern "C"

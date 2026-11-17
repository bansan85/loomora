// C binding of the reading side of the .loomora project format (loomora_project.h, block "reader:
// open, read, report, validate"): a thin layer over format/project_reader.hpp. Every function
// catches every exception and returns the LOOMORA_ERR_* code (code_of_current_exception); no C++
// type crosses the boundary. Use mode never writes, never changes a time, never creates a file.
#include "../include/loomora.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "c_api_project_handles.hpp"
#include "c_api_project_reader_handle.hpp"
#include "format/codec/mesh_codec.hpp"
#include "format/codec/sequence_codec.hpp"
#include "format/io/file.hpp"
#include "format/io/memory_io.hpp"
#include "format/project_reader.hpp"
#include "format/report.hpp"
#include "internal/config.hpp"

using namespace loomora::format;

namespace {

int32_t fail_code() { return static_cast<int32_t>(code_of_current_exception()); }

int64_t copy_out(const void *data, std::size_t len, uint8_t *buf, std::size_t cap) {
    if (len > 0 && cap >= len) {
        if (buf == nullptr)
            return LOOMORA_ERR_INVALID;
        std::memcpy(buf, data, len);
    }
    return static_cast<int64_t>(len);
}

int32_t open_source(std::unique_ptr<Source> source, loomora_progress_fn cb, void *user,
                    loomora_project **project, loomora_report **report, bool keep) {
    std::unique_ptr<loomora_report> rep(new loomora_report());
    std::unique_ptr<ProjectReader> reader;
    const int code = ProjectReader::open(std::move(source), cb, user, &reader, rep->report);
    if (report != nullptr)
        *report = rep.release();
    if (code != 0)
        return code;
    if (keep) {
        std::unique_ptr<loomora_project> p(new loomora_project());
        p->reader = std::move(reader);
        *project = p.release();
    }
    return 0;
}

int32_t open_path_impl(const char *path, loomora_progress_fn cb, void *user, loomora_project **project,
                       loomora_report **report, bool keep) {
    if (report != nullptr)
        *report = nullptr;
    if (keep && project != nullptr)
        *project = nullptr;
    if (path == nullptr || (keep && project == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        return open_source(open_file_source(std::string(path)), cb, user, project, report, keep);
    } catch (...) {
        return fail_code();
    }
}

int32_t open_memory_impl(const uint8_t *data, size_t len, loomora_progress_fn cb, void *user,
                         loomora_project **project, loomora_report **report, bool keep) {
    if (report != nullptr)
        *report = nullptr;
    if (keep && project != nullptr)
        *project = nullptr;
    if ((data == nullptr && len != 0) || (keep && project == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        return open_source(std::unique_ptr<Source>(new MemorySource(data, len)), cb, user, project, report,
                           keep);
    } catch (...) {
        return fail_code();
    }
}

bool known_kind(int32_t kind) { return kind >= LOOMORA_KIND_FRAME && kind <= LOOMORA_KIND_RESULT; }

const FrameRecord *mesh_frame(const loomora_project *p, uint32_t frame) {
    const FrameRecord *f = p->reader->frame(frame);
    return (f != nullptr && f->frame.has_mesh && !f->mesh_entry.empty()) ? f : nullptr;
}

// Common shell of the four vertex-data readers: `kind` 0 positions, 1 indices, 2 normals, 3 uvs.
int64_t read_mesh_range(const loomora_project *p, uint32_t frame, uint64_t first, size_t count, void *out,
                        int kind) {
    if (p == nullptr || (out == nullptr && count != 0))
        return LOOMORA_ERR_INVALID;
    try {
        const FrameRecord *f = mesh_frame(p, frame);
        if (f == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        if ((kind == 2 && !f->frame.mesh.has_normals) || (kind == 3 && !f->frame.mesh.has_uvs))
            return 0;
        const std::uint64_t total = kind == 1 ? f->frame.mesh.triangles : f->frame.mesh.vertices;
        if (first >= total || count == 0)
            return 0;
        if (static_cast<std::uint64_t>(count) > total - first)
            count = static_cast<std::size_t>(total - first);
        std::unique_ptr<Source> src = p->reader->open_entry_source(f->mesh_entry);
        MeshReader mr(*src);
        switch (kind) {
        case 0: mr.read_positions(first, count, static_cast<float *>(out)); break;
        case 1: mr.read_indices(first, count, static_cast<uint32_t *>(out)); break;
        case 2: mr.read_normals(first, count, static_cast<float *>(out)); break;
        default: mr.read_uvs(first, count, static_cast<float *>(out)); break;
        }
        return static_cast<int64_t>(count);
    } catch (...) {
        return fail_code();
    }
}

} // namespace

extern "C" {

int32_t loomora_project_open_path(const char *path, loomora_progress_fn cb, void *user,
                                  loomora_project **project, loomora_report **report) {
    return open_path_impl(path, cb, user, project, report, true);
}

int32_t loomora_project_open_memory(const uint8_t *data, size_t len, loomora_progress_fn cb, void *user,
                                    loomora_project **project, loomora_report **report) {
    return open_memory_impl(data, len, cb, user, project, report, true);
}

void loomora_project_close(loomora_project *project) {
    try {
        delete project;
    } catch (...) {
    }
}

int32_t loomora_validate_path(const char *path, loomora_progress_fn cb, void *user, loomora_report **report) {
    return open_path_impl(path, cb, user, nullptr, report, false);
}

int32_t loomora_validate_memory(const uint8_t *data, size_t len, loomora_progress_fn cb, void *user,
                                loomora_report **report) {
    return open_memory_impl(data, len, cb, user, nullptr, report, false);
}

int64_t loomora_project_manifest(const loomora_project *p, uint8_t *buf, size_t cap) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        const std::string &m = p->reader->manifest_json();
        return copy_out(m.data(), m.size(), buf, cap);
    } catch (...) {
        return fail_code();
    }
}

int32_t loomora_project_is_light(const loomora_project *p) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        return p->reader->is_light() ? 1 : 0;
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_project_count(const loomora_project *p, int32_t kind) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    if (!known_kind(kind))
        return LOOMORA_ERR_NOT_FOUND;
    try {
        return static_cast<int64_t>(p->reader->count(kind));
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_project_ids(const loomora_project *p, int32_t kind, uint32_t *ids, size_t cap) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    if (!known_kind(kind))
        return LOOMORA_ERR_NOT_FOUND;
    try {
        const std::vector<uint32_t> &v = p->reader->ids(kind);
        if (cap >= v.size() && !v.empty()) {
            if (ids == nullptr)
                return LOOMORA_ERR_INVALID;
            for (std::size_t i = 0; i < v.size(); ++i)
                ids[i] = v[i];
        }
        return static_cast<int64_t>(v.size());
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_project_item_json(const loomora_project *p, int32_t kind, uint32_t id, uint8_t *buf,
                                  size_t cap) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    if (!known_kind(kind))
        return LOOMORA_ERR_NOT_FOUND;
    try {
        const std::vector<uint8_t> bytes = p->reader->read_item_json(kind, id);
        return copy_out(bytes.data(), bytes.size(), buf, cap);
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_project_entry_size(const loomora_project *p, const char *entry) {
    if (p == nullptr || entry == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        const ZipEntry *e = p->reader->find_entry(entry);
        if (e == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        return static_cast<int64_t>(e->size);
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_project_read_entry(const loomora_project *p, const char *entry, uint64_t offset, uint8_t *buf,
                                   size_t cap) {
    if (p == nullptr || entry == nullptr || (buf == nullptr && cap != 0))
        return LOOMORA_ERR_INVALID;
    try {
        return static_cast<int64_t>(p->reader->read_entry(entry, offset, buf, cap));
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_layout_nail_count(const loomora_project *p, uint32_t layout) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        const LayoutRecord *l = p->reader->layout(layout);
        return l == nullptr ? LOOMORA_ERR_NOT_FOUND : static_cast<int64_t>(l->nails_parsed);
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_layout_read_nails(const loomora_project *p, uint32_t layout, uint64_t first, size_t count,
                                  loomora_nail *out) {
    if (p == nullptr || (out == nullptr && count != 0))
        return LOOMORA_ERR_INVALID;
    try {
        if (p->reader->layout(layout) == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        loomora::format::model::NailTable t;
        p->reader->read_nails(layout, first, count, t);
        for (std::size_t i = 0; i < t.size(); ++i) {
            const loomora::format::model::Nail n = t.get(i);
            loomora_nail o;
            std::memset(&o, 0, sizeof o);
            o.id = n.id;
            o.kind = static_cast<uint32_t>(n.kind) + 1;
            o.flags = (t.is_3d() ? 1u : 0u) | (n.has_label ? 2u : 0u);
            o.side = static_cast<uint32_t>(n.side);
            o.x = n.x;
            o.y = n.y;
            o.z = n.z;
            o.diameter = n.diameter;
            o.length = n.length;
            for (int k = 0; k < 3; ++k)
                o.axis[k] = static_cast<float>(n.axis[k]);
            o.face_u = static_cast<float>(n.u);
            o.face_v = static_cast<float>(n.v);
            o.face_triangle = n.triangle;
            out[i] = o;
        }
        return static_cast<int64_t>(t.size());
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_layout_nail_label(const loomora_project *p, uint32_t layout, uint32_t nail_id, uint8_t *buf,
                                  size_t cap) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        if (p->reader->layout(layout) == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        loomora::format::model::NailTable t;
        p->reader->load_nails(layout, t);
        for (std::size_t i = 0; i < t.id.size(); ++i) {
            if (t.id[i] != nail_id)
                continue;
            const std::string *label = t.label_at(i);
            return label == nullptr ? 0 : copy_out(label->data(), label->size(), buf, cap);
        }
        return LOOMORA_ERR_NOT_FOUND;
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_result_step_count(const loomora_project *p, uint32_t result) {
    if (p == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        const ResultRecord *r = p->reader->result(result);
        return r == nullptr ? LOOMORA_ERR_NOT_FOUND : static_cast<int64_t>(r->result.steps);
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_result_read_steps(const loomora_project *p, uint32_t result, uint64_t first, size_t count,
                                  loomora_step *out) {
    if (p == nullptr || (out == nullptr && count != 0))
        return LOOMORA_ERR_INVALID;
    try {
        if (p->reader->result(result) == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        std::vector<SequenceStep> steps;
        p->reader->read_steps(result, first, count, steps);
        for (std::size_t i = 0; i < steps.size(); ++i) {
            loomora_step s;
            std::memset(&s, 0, sizeof s);
            s.nail = steps[i].nail_id;
            s.spool_index = steps[i].spool_index;
            s.rotation = steps[i].ccw ? 1 : 0;
            s.type = static_cast<uint8_t>(steps[i].type);
            s.level = steps[i].level;
            out[i] = s;
        }
        return static_cast<int64_t>(steps.size());
    } catch (...) {
        return fail_code();
    }
}

int32_t loomora_frame_mesh_info(const loomora_project *p, uint32_t frame, loomora_mesh_info *info) {
    if (p == nullptr || info == nullptr)
        return LOOMORA_ERR_INVALID;
    try {
        const FrameRecord *f = mesh_frame(p, frame);
        if (f == nullptr)
            return LOOMORA_ERR_NOT_FOUND;
        info->vertices = f->frame.mesh.vertices;
        info->triangles = f->frame.mesh.triangles;
        info->flags = (f->frame.mesh.has_normals ? LOOMORA_MESH_NORMALS : 0) |
                      (f->frame.mesh.has_uvs ? LOOMORA_MESH_UVS : 0);
        info->reserved = 0;
        return 0;
    } catch (...) {
        return fail_code();
    }
}

int64_t loomora_frame_read_vertices(const loomora_project *p, uint32_t frame, uint64_t first, size_t count,
                                    float *xyz) {
    return read_mesh_range(p, frame, first, count, xyz, 0);
}

int64_t loomora_frame_read_triangles(const loomora_project *p, uint32_t frame, uint64_t first, size_t count,
                                     uint32_t *abc) {
    return read_mesh_range(p, frame, first, count, abc, 1);
}

int64_t loomora_frame_read_normals(const loomora_project *p, uint32_t frame, uint64_t first, size_t count,
                                   float *xyz) {
    return read_mesh_range(p, frame, first, count, xyz, 2);
}

int64_t loomora_frame_read_uvs(const loomora_project *p, uint32_t frame, uint64_t first, size_t count,
                               float *uv) {
    return read_mesh_range(p, frame, first, count, uv, 3);
}

// ---- report -----------------------------------------------------------------------------------

int64_t loomora_report_count(const loomora_report *r) {
    return r == nullptr ? LOOMORA_ERR_INVALID : static_cast<int64_t>(r->report.count());
}

int64_t loomora_report_error_count(const loomora_report *r) {
    return r == nullptr ? LOOMORA_ERR_INVALID : static_cast<int64_t>(r->report.error_count());
}

int32_t loomora_report_issue(const loomora_report *r, size_t index, int32_t *severity, int32_t *item_kind,
                             uint32_t *item_id) {
    if (r == nullptr)
        return LOOMORA_ERR_INVALID;
    if (index >= r->report.count())
        return LOOMORA_ERR_NOT_FOUND;
    const Issue &is = r->report.issues()[index];
    if (severity != nullptr)
        *severity = is.severity == kError ? 2 : 1;
    if (item_kind != nullptr)
        *item_kind = static_cast<int32_t>(is.item_kind);
    if (item_id != nullptr)
        *item_id = is.item_id;
    return 0;
}

int64_t loomora_report_text(const loomora_report *r, size_t index, int32_t field, uint8_t *buf, size_t cap) {
    if (r == nullptr)
        return LOOMORA_ERR_INVALID;
    if (index >= r->report.count())
        return LOOMORA_ERR_NOT_FOUND;
    try {
        const Issue &is = r->report.issues()[index];
        const std::string *s = nullptr;
        if (field == 1)
            s = &is.code;
        else if (field == 2)
            s = &is.entry;
        else if (field == 3)
            s = &is.message;
        else
            return LOOMORA_ERR_INVALID;
        return copy_out(s->data(), s->size(), buf, cap);
    } catch (...) {
        return fail_code();
    }
}

void loomora_report_free(loomora_report *r) {
    try {
        delete r;
    } catch (...) {
    }
}

} // extern "C"

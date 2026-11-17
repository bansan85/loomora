// C binding of the project editor (loomora_project.h, "editor: create and save"): a thin forwarding
// layer over format/project_editor.hpp and format/project_writer.hpp. A try / catch (...) wraps every
// call, so no exception ever crosses a function declared here; no C++ type appears in a signature.
// The later editing functions (3D frames and meshes, replace_*, remove_item, dependents, order,
// opening a project for edit) go in this file too, on the same loomora_editor handle.
#include "../include/loomora.h"

#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "c_api_project_handles.hpp"
#include "format/io/file.hpp"
#include "format/item_json.hpp"
#include "format/project_editor.hpp"
#include "format/project_reader.hpp"
#include "format/project_writer.hpp"
#include "format/report.hpp"
#include "internal/config.hpp"

// The editor handle: the staged project.
struct loomora_editor {
    loomora::format::ProjectEditor editor;

    loomora_editor(const std::string &project_id, const std::string &title, const std::string &created_at)
        : editor(project_id, title, created_at) {}
    explicit loomora_editor(std::shared_ptr<const loomora::format::ProjectReader> base) : editor(base) {}
};

namespace {

using loomora::format::ByteSpan;
using loomora::format::FormatError;
using loomora::format::Report;

[[noreturn]] void invalid_item(const std::string &message) {
    throw FormatError(LOOMORA_ERR_INVALID_ITEM, message);
}

// The nails of the caller's array as a NailTable of the frame's dimension. On a 3D frame every nail
// must carry the "3D fields valid" bit (flags bit 0); on a 2D frame the 3D fields are ignored.
loomora::format::model::NailTable nails_of(const loomora_nail *nails, size_t count, bool is_3d) {
    using namespace loomora::format::model;
    NailTable table(is_3d);
    table.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const loomora_nail &c = nails[i];
        if (c.kind < 1 || c.kind > 3)
            invalid_item("nail kind must be 1 (boundary), 2 (exclusion_boundary) or 3 (interior)");
        Nail n;
        n.id = c.id;
        n.x = c.x;
        n.y = c.y;
        n.diameter = c.diameter;
        n.kind = static_cast<NailKind>(c.kind - 1);
        if (is_3d) {
            if ((c.flags & 1u) == 0)
                invalid_item("a nail of a 3D frame needs the 3D fields (flags bit 0)");
            if (c.side > 1)
                invalid_item("nail side must be 0 (exterior) or 1 (interior)");
            n.z = c.z;
            n.length = c.length;
            n.side = static_cast<NailSide>(c.side);
            for (int k = 0; k < 3; ++k)
                n.axis[k] = c.axis[k];
            n.triangle = c.face_triangle;
            n.u = c.face_u;
            n.v = c.face_v;
        }
        table.add(n);
    }
    return table;
}

// The mesh of the caller's arrays.
loomora::format::MeshInput mesh_of(const loomora_mesh_info *info, const float *xyz, const uint32_t *abc,
                                   const float *normals, const float *uvs) {
    if (info == nullptr)
        throw FormatError(LOOMORA_ERR_INVALID, "no mesh description");
    if ((info->flags & ~static_cast<uint32_t>(LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS)) != 0)
        invalid_item("unknown bits in the mesh flags");
    loomora::format::MeshInput m;
    m.vertices = info->vertices;
    m.triangles = info->triangles;
    m.normals_flag = (info->flags & LOOMORA_MESH_NORMALS) != 0;
    m.uvs_flag = (info->flags & LOOMORA_MESH_UVS) != 0;
    m.xyz = xyz;
    m.abc = abc;
    m.normals = normals;
    m.uvs = uvs;
    return m;
}

// Whether the layout's frame is 3D (NOT_FOUND for an unknown layout).
bool layout_is_3d(const loomora_editor *e, uint32_t layout) {
    std::map<uint32_t, loomora::format::StagedLayout>::const_iterator it = e->editor.layouts().find(layout);
    if (it == e->editor.layouts().end())
        throw FormatError(LOOMORA_ERR_NOT_FOUND, "the layout does not exist");
    return e->editor.frame_is_3d(it->second.layout.frame);
}

// The steps of the caller's array, checked field by field, handed to the editor one at a time.
class ArrayStepSource : public loomora::format::StepSource {
  public:
    ArrayStepSource(const loomora_step *steps, size_t count) : steps_(steps), count_(count), pos_(0) {}
    bool next(loomora::format::SequenceStep *out) override {
        if (pos_ >= count_)
            return false;
        const loomora_step &c = steps_[pos_++];
        if (c.rotation > 1)
            invalid_item("step rotation must be 0 (CW) or 1 (CCW)");
        if (c.type > 2)
            invalid_item("step type must be 0 (segment), 1 (start) or 2 (end)");
        out->nail_id = c.nail;
        out->ccw = c.rotation != 0;
        out->type = static_cast<loomora::format::StepType>(c.type);
        out->spool_index = c.spool_index;
        out->level = c.level;
        return true;
    }

  private:
    const loomora_step *steps_;
    size_t count_;
    size_t pos_;
};

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

// Runs a save: the report holds the warnings on success and the cause on failure.
template <typename Fn>
int32_t save_with_report(loomora_report **report, Fn fn) {
    if (report != nullptr)
        *report = nullptr;
    Report rep;
    int32_t code = 0;
    try {
        fn(report != nullptr ? &rep : nullptr);
    } catch (const FormatError &e) {
        code = e.code();
        try {
            rep.add(e, "save.failed", "");
        } catch (...) {
        }
    } catch (...) {
        code = loomora::format::code_of_current_exception();
    }
    give_report(report, rep);
    return code;
}

std::string text_of(const char *s) { return std::string(s); }

} // namespace

extern "C" {

int32_t loomora_editor_create(const uint8_t project_id_random16[16], const char *title, const char *created_at,
                              loomora_editor **editor) {
    if (LOOMORA_UNLIKELY(project_id_random16 == nullptr || title == nullptr || created_at == nullptr ||
                         editor == nullptr))
        return LOOMORA_ERR_INVALID;
    *editor = nullptr;
    try {
        char uuid[37];
        const int32_t rc = loomora_uuid4_from_random(project_id_random16, uuid);
        if (rc != 0)
            return rc;
        *editor = new loomora_editor(uuid, title, created_at);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

void loomora_editor_close(loomora_editor *e) {
    try {
        delete e;
    } catch (...) {
    }
}

int32_t loomora_editor_add_spool(loomora_editor *e, const char *json, uint32_t *id) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        loomora::format::model::Spool spool;
        loomora::format::parse_spool_json(text_of(json), spool);
        const uint32_t added = e->editor.add_spool(spool);
        if (id != nullptr)
            *id = added;
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_add_frame(loomora_editor *e, const char *json, uint32_t *id) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        loomora::format::model::Frame frame;
        loomora::format::parse_frame_json(text_of(json), frame);
        const uint32_t added = e->editor.add_frame(frame);
        if (id != nullptr)
            *id = added;
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_add_layout(loomora_editor *e, uint32_t frame, const char *json, const loomora_nail *nails,
                                  size_t count, uint32_t *id) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr || (nails == nullptr && count != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        loomora::format::model::Layout layout;
        loomora::format::parse_layout_json(text_of(json), layout);
        const uint32_t added =
            e->editor.add_layout(frame, layout, nails_of(nails, count, e->editor.frame_is_3d(frame)));
        if (id != nullptr)
            *id = added;
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_layout_set_label(loomora_editor *e, uint32_t layout, uint32_t nail_id,
                                        const char *label_or_null) {
    if (LOOMORA_UNLIKELY(e == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        if (label_or_null == nullptr) {
            e->editor.layout_set_label(layout, nail_id, nullptr);
        } else {
            const std::string label(label_or_null);
            e->editor.layout_set_label(layout, nail_id, &label);
        }
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_add_image(loomora_editor *e, const char *json, const uint8_t *source, size_t source_len,
                                 const uint8_t *target_png, size_t target_len, const uint8_t *thumb_png,
                                 size_t thumb_len, uint32_t *id) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr || (source == nullptr && source_len != 0) ||
                         (target_png == nullptr && target_len != 0) || (thumb_png == nullptr && thumb_len != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        loomora::format::model::Image image;
        loomora::format::parse_image_json(text_of(json), image);
        const uint32_t added = e->editor.add_image(image, ByteSpan(source, source_len),
                                                   ByteSpan(target_png, target_len), ByteSpan(thumb_png, thumb_len));
        if (id != nullptr)
            *id = added;
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_add_result(loomora_editor *e, const char *json, const loomora_step *steps,
                                  size_t step_count, const uint8_t *thumb_png, size_t thumb_len,
                                  const char *settings_json_or_null, uint32_t *id) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr || (steps == nullptr && step_count != 0) ||
                         (thumb_png == nullptr && thumb_len != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        loomora::format::model::Result result;
        loomora::format::parse_result_json(text_of(json), result);
        ArrayStepSource source(steps, step_count);
        const uint32_t added =
            e->editor.add_result(result, source, step_count, ByteSpan(thumb_png, thumb_len),
                                 settings_json_or_null == nullptr ? std::string() : std::string(settings_json_or_null));
        if (id != nullptr)
            *id = added;
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_set_title(loomora_editor *e, const char *title) {
    if (LOOMORA_UNLIKELY(e == nullptr || title == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.set_title(title);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_save_path(loomora_editor *e, const char *path, const char *modified_at, uint32_t options,
                                 loomora_progress_fn cb, void *user, loomora_report **report) {
    if (report != nullptr)
        *report = nullptr;
    if (LOOMORA_UNLIKELY(e == nullptr || path == nullptr || modified_at == nullptr))
        return LOOMORA_ERR_INVALID;
    return save_with_report(report, [&](Report *rep) {
        loomora::format::save_project_to_path(e->editor, path, modified_at, options, cb, user, rep);
    });
}

int32_t loomora_editor_save_stream(loomora_editor *e, const char *modified_at, uint32_t options,
                                   loomora_sink_fn sink, void *user, loomora_report **report) {
    if (report != nullptr)
        *report = nullptr;
    if (LOOMORA_UNLIKELY(e == nullptr || modified_at == nullptr || sink == nullptr))
        return LOOMORA_ERR_INVALID;
    return save_with_report(report, [&](Report *rep) {
        loomora::format::save_project_to_callback(e->editor, modified_at, options, sink, user, nullptr, nullptr,
                                                  rep);
    });
}

/* ---- editor: 3D, edit, dependents ---- */

int32_t loomora_editor_open_path(const char *path, loomora_progress_fn cb, void *user, loomora_editor **editor,
                                 loomora_report **report) {
    if (report != nullptr)
        *report = nullptr;
    if (editor != nullptr)
        *editor = nullptr;
    if (LOOMORA_UNLIKELY(path == nullptr || editor == nullptr))
        return LOOMORA_ERR_INVALID;
    std::unique_ptr<loomora_report> rep;
    int32_t code = 0;
    try {
        rep.reset(new loomora_report());
        std::unique_ptr<loomora::format::ProjectReader> reader;
        code = loomora::format::ProjectReader::open(loomora::format::open_file_source(std::string(path)), cb, user,
                                                    &reader, rep->report);
        if (code == 0) {
            std::shared_ptr<const loomora::format::ProjectReader> base(std::move(reader));
            *editor = new loomora_editor(base); // refuses a light project
        }
    } catch (const FormatError &ex) {
        code = ex.code();
        try {
            if (rep)
                rep->report.add(ex, "edit.read_only", "");
        } catch (...) {
        }
    } catch (...) {
        code = loomora::format::code_of_current_exception();
    }
    if (report != nullptr)
        *report = rep.release();
    return code;
}

int32_t loomora_editor_frame_set_mesh(loomora_editor *e, uint32_t frame, const loomora_mesh_info *info,
                                      const float *xyz, const uint32_t *abc, const float *normals_or_null,
                                      const float *uvs_or_null) {
    if (LOOMORA_UNLIKELY(e == nullptr || info == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.frame_set_mesh(frame, mesh_of(info, xyz, abc, normals_or_null, uvs_or_null));
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_frame_set_attached_source(loomora_editor *e, uint32_t frame, const char *name,
                                                 const uint8_t *data, size_t len) {
    if (LOOMORA_UNLIKELY(e == nullptr || (data == nullptr && len != 0) || (data != nullptr && name == nullptr)))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.frame_set_attached_source(frame, name == nullptr ? std::string() : std::string(name),
                                            ByteSpan(data, len));
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_replace_item(loomora_editor *e, int32_t kind, uint32_t id, const char *json,
                                    int32_t confirm_dependents) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.replace_item(kind, id, text_of(json), confirm_dependents != 0);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_replace_layout_nails(loomora_editor *e, uint32_t layout, const loomora_nail *nails,
                                            size_t count, int32_t confirm_dependents) {
    if (LOOMORA_UNLIKELY(e == nullptr || (nails == nullptr && count != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.replace_layout_nails(layout, nails_of(nails, count, layout_is_3d(e, layout)),
                                       confirm_dependents != 0);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_replace_frame_mesh(loomora_editor *e, uint32_t frame, const loomora_mesh_info *info,
                                          const float *xyz, const uint32_t *abc, const float *normals_or_null,
                                          const float *uvs_or_null, int32_t confirm_dependents) {
    if (LOOMORA_UNLIKELY(e == nullptr || info == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.replace_frame_mesh(frame, mesh_of(info, xyz, abc, normals_or_null, uvs_or_null),
                                     confirm_dependents != 0);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_replace_image(loomora_editor *e, uint32_t image, const char *json, const uint8_t *source,
                                     size_t source_len, const uint8_t *target_png, size_t target_len,
                                     const uint8_t *thumb_png, size_t thumb_len, int32_t confirm_dependents) {
    if (LOOMORA_UNLIKELY(e == nullptr || json == nullptr || (source == nullptr && source_len != 0) ||
                         (target_png == nullptr && target_len != 0) || (thumb_png == nullptr && thumb_len != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.replace_image(image, text_of(json), ByteSpan(source, source_len), ByteSpan(target_png, target_len),
                                ByteSpan(thumb_png, thumb_len), confirm_dependents != 0);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_replace_thumbnail(loomora_editor *e, int32_t kind, uint32_t id, const uint8_t *png,
                                         size_t len) {
    if (LOOMORA_UNLIKELY(e == nullptr || png == nullptr || len == 0))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.replace_thumbnail(kind, id, ByteSpan(png, len));
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_remove_item(loomora_editor *e, int32_t kind, uint32_t id, int32_t confirm_dependents) {
    if (LOOMORA_UNLIKELY(e == nullptr))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.remove_item(kind, id, confirm_dependents != 0);
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int64_t loomora_editor_dependents(const loomora_editor *e, int32_t kind, uint32_t id, int32_t operation,
                                  uint32_t *pairs, size_t cap_pairs) {
    if (LOOMORA_UNLIKELY(e == nullptr || (pairs == nullptr && cap_pairs != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        const std::vector<loomora::format::ItemRef> deps = e->editor.dependents(kind, id, operation);
        if (cap_pairs >= deps.size()) {
            for (size_t i = 0; i < deps.size(); ++i) {
                pairs[2 * i] = static_cast<uint32_t>(deps[i].kind);
                pairs[2 * i + 1] = deps[i].id;
            }
        }
        return static_cast<int64_t>(deps.size());
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

int32_t loomora_editor_set_order(loomora_editor *e, int32_t kind, const uint32_t *ids, size_t n) {
    if (LOOMORA_UNLIKELY(e == nullptr || (ids == nullptr && n != 0)))
        return LOOMORA_ERR_INVALID;
    try {
        e->editor.set_order(kind, std::vector<uint32_t>(ids, ids + n));
        return 0;
    } catch (...) {
        return loomora::format::code_of_current_exception();
    }
}

} // extern "C"

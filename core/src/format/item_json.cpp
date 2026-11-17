#include "format/item_json.hpp"

#include "loomora.h"
#include "format/json/json_in.hpp"
#include "format/json/json_out.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {

using json::JsonWriter;

namespace {

// ---- writing -----------------------------------------------------------------------------------

void write_ref(JsonWriter &w, const char *name, const model::EntryRef &ref) {
    w.key(name);
    w.begin_object();
    w.field_string("entry", ref.entry);
    if (!ref.format.empty())
        w.field_string("format", ref.format);
    if (ref.size >= 0)
        w.field_uint("size", static_cast<std::uint64_t>(ref.size));
    w.end_object();
}

void write_vec3(JsonWriter &w, const char *name, const double v[3]) {
    w.key(name);
    w.begin_array();
    for (int i = 0; i < 3; ++i)
        w.number(v[i]);
    w.end_array();
}

void write_shape(JsonWriter &w, const model::Shape &s) {
    w.begin_object();
    w.field_string("type", s.type);
    w.field_raw("params", s.params_json.empty() ? std::string("{}") : s.params_json);
    w.key("polyline");
    w.begin_array();
    for (std::size_t i = 0; i < s.polyline.size(); ++i) {
        w.begin_array();
        w.number(s.polyline[i].x);
        w.number(s.polyline[i].y);
        w.end_array();
    }
    w.end_array();
    w.end_object();
}

void write_spool(JsonWriter &w, const model::Spool &s) {
    w.begin_object();
    w.field_uint("id", s.id);
    w.field_string("name", s.name);
    w.field_string("color", s.color);
    w.field_number("opacity", s.opacity);
    w.field_number("diameter_mm", s.diameter_mm);
    if (s.has_available_length)
        w.field_number("available_length_mm", s.available_length_mm);
    if (s.has_material)
        w.field_string("material", s.material);
    w.field_string("domain", model::to_string(s.domain));
    w.end_object();
}

void write_view(JsonWriter &w, const model::View &v) {
    w.key("view");
    w.begin_object();
    write_vec3(w, "position", v.position);
    write_vec3(w, "target", v.target);
    write_vec3(w, "up", v.up);
    w.field_string("projection", model::to_string(v.projection));
    if (v.projection == model::kProjectionPerspective)
        w.field_number("fov_deg", v.fov_deg);
    else
        w.field_number("size", v.size);
    w.field_number("weight", v.weight);
    w.end_object();
}

// ---- reading -----------------------------------------------------------------------------------

// Parses `text` as the JSON of an item. A parse failure is an invalid item, not a corrupt file.
void parse_text(const std::string &text, json::Document &doc, const std::string &what) {
    try {
        json::parse_document(text.data(), text.size(), doc, what);
    } catch (const FormatError &e) {
        if (e.has_issue())
            throw FormatError(LOOMORA_ERR_INVALID_ITEM, e.issue());
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, e.what());
    }
}

// Runs `fn` and turns the FormatError of the typed access helpers (schema.*) into INVALID_ITEM.
template <typename Fn>
void as_item(const std::string &text, const char *what, Fn fn) {
    json::Document doc;
    parse_text(text, doc, what);
    try {
        fn(doc);
    } catch (const FormatError &e) {
        if (e.code() == LOOMORA_ERR_CORRUPT) {
            if (e.has_issue())
                throw FormatError(LOOMORA_ERR_INVALID_ITEM, e.issue());
            throw FormatError(LOOMORA_ERR_INVALID_ITEM, e.what());
        }
        throw;
    }
}

std::string compact_value(const json::Value &v) {
    std::string out;
    JsonWriter w(out);
    w.value(v);
    return out;
}

void read_shape(const json::Fields &o, model::Shape &s) {
    s.type = o.string("type", 64);
    if (o.has("params")) {
        if (!o.get("params").IsObject())
            json::fail(o.entry(), "schema.type", o.path_of("params") + " must be an object");
        s.params_json = compact_value(o.get("params"));
    } else {
        s.params_json = "{}";
    }
    s.polyline.clear();
    const json::Value &pl = o.array("polyline");
    for (rapidjson::SizeType i = 0; i < pl.Size(); ++i) {
        const std::string path = o.path_of("polyline") + "[" + json::dec(i) + "]";
        if (!pl[i].IsArray() || pl[i].Size() != 2)
            json::fail(o.entry(), "schema.type", path + " must be an array of two numbers");
        s.polyline.push_back(model::Point(json::as_number(pl[i][0], o.entry(), path + "[0]"),
                                          json::as_number(pl[i][1], o.entry(), path + "[1]")));
    }
}

void read_vec3(const json::Fields &o, const char *name, double out[3]) {
    const json::Value &a = o.array(name);
    if (a.Size() != 3)
        json::fail(o.entry(), "schema.type", o.path_of(name) + " must hold three numbers");
    for (rapidjson::SizeType i = 0; i < 3; ++i)
        out[i] = json::as_number(a[i], o.entry(), o.path_of(name) + "[" + json::dec(i) + "]");
}

void read_view(const json::Fields &o, model::View &v) {
    read_vec3(o, "position", v.position);
    read_vec3(o, "target", v.target);
    read_vec3(o, "up", v.up);
    if (!model::parse_projection(o.string("projection", 64), v.projection))
        json::fail(o.entry(), "schema.value", o.path_of("projection") + " must be orthographic or perspective");
    if (v.projection == model::kProjectionPerspective)
        v.fov_deg = o.number("fov_deg");
    else
        v.size = o.number("size");
    v.weight = o.has("weight") ? o.number("weight") : 1.0;
}

} // namespace

// ---- writers -----------------------------------------------------------------------------------

std::string spools_json(const std::vector<model::Spool> &spools) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.key("spools");
    w.begin_array();
    for (std::size_t i = 0; i < spools.size(); ++i)
        write_spool(w, spools[i]);
    w.end_array();
    w.end_object();
    return out;
}

std::string frame_json(const model::Frame &f) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field_uint("id", f.id);
    w.field_string("name", f.name);
    w.field_string("kind", model::to_string(f.kind));
    w.key("dimensions");
    w.begin_object();
    w.field_number("width_mm", f.width_mm);
    w.field_number("height_mm", f.height_mm);
    if (f.kind == model::kFrame3D)
        w.field_number("depth_mm", f.depth_mm);
    w.end_object();
    w.field_string("background", f.background);
    w.key("outline");
    write_shape(w, f.outline);
    w.key("exclusions");
    w.begin_array();
    for (std::size_t i = 0; i < f.exclusions.size(); ++i)
        write_shape(w, f.exclusions[i]);
    w.end_array();
    w.field_string("surface", model::to_string(f.surface));
    if (f.kind == model::kFrame3D && f.body != model::kBodyNone)
        w.field_string("body", model::to_string(f.body));
    if (f.has_mesh) {
        const model::MeshInfo &m = f.mesh;
        w.key("mesh");
        w.begin_object();
        w.field_uint("vertices", m.vertices);
        w.field_uint("triangles", m.triangles);
        w.field_bool("watertight", m.watertight);
        w.field_bool("manifold", m.manifold);
        w.field_bool("has_normals", m.has_normals);
        w.field_bool("has_uvs", m.has_uvs);
        if (m.has_source) {
            w.key("source");
            w.begin_object();
            w.field_string("name", m.source_name);
            if (!m.source_sha256.empty())
                w.field_string("sha256", m.source_sha256);
            w.end_object();
        }
        if (!m.construction_json.empty())
            w.field_raw("construction", m.construction_json);
        if (!m.attached_source.empty())
            w.field_string("attached_source", m.attached_source);
        w.end_object();
    }
    w.end_object();
    return out;
}

std::string layout_json(const model::Layout &l) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field_uint("id", l.id);
    w.field_string("name", l.name);
    w.field_uint("frame", l.frame);
    w.field_uint("nail_count", l.nail_count);
    w.end_object();
    return out;
}

std::string image_json(const model::Image &img) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field_uint("id", img.id);
    w.field_string("name", img.name);
    w.field_string("color_mode", model::to_string(img.color_mode));
    w.field_string("shape", model::to_string(img.shape));
    w.field_uint("width_px", img.width_px);
    w.field_uint("height_px", img.height_px);
    if (img.has_source)
        write_ref(w, "source", img.source);
    if (img.has_target)
        write_ref(w, "target", img.target);
    w.key("thumbnail");
    w.begin_object();
    w.field_string("entry", img.thumbnail.entry);
    w.end_object();
    w.end_object();
    return out;
}

std::string result_json(const model::Result &r) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field_uint("id", r.id);
    w.field_string("name", r.name);
    w.field_string("created_at", r.created_at);
    w.field_uint("frame", r.frame);
    w.field_uint("layout", r.layout);
    w.key("images");
    w.begin_array();
    for (std::size_t i = 0; i < r.images.size(); ++i) {
        w.begin_object();
        w.field_uint("image", r.images[i].image);
        if (r.images[i].has_view)
            write_view(w, r.images[i].view);
        w.end_object();
    }
    w.end_array();
    w.key("spools");
    w.begin_array();
    for (std::size_t i = 0; i < r.spools.size(); ++i) {
        w.begin_object();
        w.field_uint("spool", r.spools[i].spool);
        w.field_number("length_mm", r.spools[i].length_mm);
        w.end_object();
    }
    w.end_array();
    w.field_uint("steps", r.steps);
    w.field_bool("levels", r.levels);
    w.key("thumbnail");
    w.begin_object();
    w.field_string("entry", r.thumbnail.entry);
    w.end_object();
    if (r.has_settings) {
        w.key("settings");
        w.begin_object();
        w.field_string("entry", r.settings.entry);
        w.end_object();
    }
    w.end_object();
    return out;
}

std::string manifest_json(const model::Manifest &m) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.key("format");
    w.begin_object();
    w.field_string("name", m.format.name);
    w.field_uint("major", m.format.major);
    w.field_uint("minor", m.format.minor);
    w.end_object();
    w.field_string("min_reader", model::version_text(m.min_reader));
    w.field_string("project_id", m.project_id);
    w.field_string("title", m.title);
    w.field_string("created_at", m.created_at);
    w.field_string("modified_at", m.modified_at);
    w.key("generator");
    w.begin_object();
    w.field_string("application", m.generator.application);
    w.field_string("version", m.generator.version);
    w.field_string("platform", m.generator.platform);
    w.field_string("architecture", m.generator.architecture);
    w.end_object();
    w.field_string("profile", model::to_string(m.profile));
    w.field_bool("editable", m.editable);
    w.key("counts");
    w.begin_object();
    w.field_uint("frames", m.counts.frames);
    w.field_uint("layouts", m.counts.layouts);
    w.field_uint("images", m.counts.images);
    w.field_uint("spools", m.counts.spools);
    w.field_uint("results", m.counts.results);
    w.end_object();
    w.key("next_id");
    w.begin_object();
    w.field_uint("frame", m.next_id.frame);
    w.field_uint("layout", m.next_id.layout);
    w.field_uint("image", m.next_id.image);
    w.field_uint("spool", m.next_id.spool);
    w.field_uint("result", m.next_id.result);
    w.end_object();
    w.key("order");
    w.begin_object();
    const std::vector<std::uint32_t> *lists[5] = {&m.order.frames, &m.order.layouts, &m.order.images,
                                                  &m.order.spools, &m.order.results};
    const char *const names[5] = {"frames", "layouts", "images", "spools", "results"};
    for (int k = 0; k < 5; ++k) {
        w.key(names[k]);
        w.begin_array();
        for (std::size_t i = 0; i < lists[k]->size(); ++i)
            w.uint((*lists[k])[i]);
        w.end_array();
    }
    w.end_object();
    w.key("required_features");
    w.begin_array();
    for (std::size_t i = 0; i < m.required_features.size(); ++i)
        w.string(m.required_features[i]);
    w.end_array();
    w.key("entries");
    w.begin_object();
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
        w.key(m.entries[i].path);
        w.begin_object();
        w.field_uint("size", m.entries[i].size);
        w.field_string("sha256", m.entries[i].sha256);
        w.end_object();
    }
    w.end_object();
    w.field_raw("extensions", m.extensions_json.empty() ? std::string("{}") : m.extensions_json);
    w.end_object();
    return out;
}

// ---- readers -----------------------------------------------------------------------------------

void parse_spool_json(const std::string &text, model::Spool &out) {
    as_item(text, "spool", [&](const json::Document &doc) {
        json::Fields f(doc, "spool", "");
        model::Spool s;
        s.name = f.string("name", model::kMaxNameBytes);
        if (f.has("color"))
            s.color = f.string("color", 16);
        if (f.has("opacity"))
            s.opacity = f.number("opacity");
        s.diameter_mm = f.number("diameter_mm");
        if (f.has("available_length_mm")) {
            s.has_available_length = true;
            s.available_length_mm = f.number("available_length_mm");
        }
        if (f.has("material")) {
            s.has_material = true;
            s.material = f.string("material", 4096);
        }
        if (f.has("domain") && !model::parse_domain(f.string("domain", 64), s.domain))
            json::fail("spool", "schema.value", "domain must be exterior, interior or both");
        out = s;
    });
}

void parse_frame_json(const std::string &text, model::Frame &out) {
    as_item(text, "frame", [&](const json::Document &doc) {
        json::Fields f(doc, "frame", "");
        model::Frame fr;
        fr.name = f.string("name", model::kMaxNameBytes);
        if (!model::parse_frame_kind(f.string("kind", 64), fr.kind))
            json::fail("frame", "schema.value", "kind must be 2d or 3d");
        const json::Fields dims = f.object("dimensions");
        fr.width_mm = dims.number("width_mm");
        fr.height_mm = dims.number("height_mm");
        if (fr.kind == model::kFrame3D)
            fr.depth_mm = dims.number("depth_mm");
        if (f.has("background"))
            fr.background = f.string("background", 16);
        read_shape(f.object("outline"), fr.outline);
        if (f.has("exclusions")) {
            const json::Value &ex = f.array("exclusions");
            for (rapidjson::SizeType i = 0; i < ex.Size(); ++i) {
                model::Shape s;
                read_shape(json::Fields(ex[i], "frame", "exclusions[" + json::dec(i) + "]"), s);
                fr.exclusions.push_back(s);
            }
        }
        if (f.has("surface") && !model::parse_surface(f.string("surface", 64), fr.surface))
            json::fail("frame", "schema.value", "surface is not a known surface");
        if (f.has("body") && !model::parse_body(f.string("body", 64), fr.body))
            json::fail("frame", "schema.value", "body must be solid, shell or guide");
        // The optional description of the mesh: only what the caller knows (the counts and flags are the
        // editor's, derived from the mesh it is given).
        if (f.has("mesh")) {
            const json::Fields m = f.object("mesh");
            fr.has_mesh = true;
            if (m.has("watertight"))
                fr.mesh.watertight = m.boolean("watertight");
            if (m.has("manifold"))
                fr.mesh.manifold = m.boolean("manifold");
            if (m.has("source")) {
                const json::Fields s = m.object("source");
                fr.mesh.has_source = true;
                fr.mesh.source_name = s.string("name", model::kMaxNameBytes);
                if (s.has("sha256"))
                    fr.mesh.source_sha256 = s.string("sha256", 64);
            }
            if (m.has("construction")) {
                if (!m.get("construction").IsObject())
                    json::fail("frame", "schema.type", "mesh.construction must be an object");
                fr.mesh.construction_json = compact_value(m.get("construction"));
            }
        }
        out = fr;
    });
}

void parse_layout_json(const std::string &text, model::Layout &out) {
    as_item(text, "layout", [&](const json::Document &doc) {
        json::Fields f(doc, "layout", "");
        model::Layout l;
        l.name = f.string("name", model::kMaxNameBytes);
        if (f.has("frame")) // 0 when absent: the editor compares it with the layout's frame on a replace
            l.frame = f.u32("frame");
        out = l;
    });
}

void parse_image_json(const std::string &text, model::Image &out) {
    as_item(text, "image", [&](const json::Document &doc) {
        json::Fields f(doc, "image", "");
        model::Image img;
        img.name = f.string("name", model::kMaxNameBytes);
        if (!model::parse_color_mode(f.string("color_mode", 64), img.color_mode))
            json::fail("image", "schema.value", "color_mode must be bw, gray or color");
        if (!model::parse_image_shape(f.string("shape", 64), img.shape))
            json::fail("image", "schema.value", "shape is not one of the known shapes");
        img.width_px = f.u64("width_px");
        img.height_px = f.u64("height_px");
        out = img;
    });
}

void parse_result_json(const std::string &text, model::Result &out) {
    as_item(text, "result", [&](const json::Document &doc) {
        json::Fields f(doc, "result", "");
        model::Result r;
        r.name = f.string("name", model::kMaxNameBytes);
        r.created_at = f.string("created_at", 64);
        r.frame = f.u32("frame");
        r.layout = f.u32("layout");
        const json::Value &imgs = f.array("images");
        for (rapidjson::SizeType i = 0; i < imgs.Size(); ++i) {
            const json::Fields o(imgs[i], "result", "images[" + json::dec(i) + "]");
            model::ResultImage ri;
            ri.image = o.u32("image");
            if (o.has("view")) {
                ri.has_view = true;
                read_view(o.object("view"), ri.view);
            }
            r.images.push_back(ri);
        }
        const json::Value &sp = f.array("spools");
        for (rapidjson::SizeType i = 0; i < sp.Size(); ++i) {
            const json::Fields o(sp[i], "result", "spools[" + json::dec(i) + "]");
            model::ResultSpool rs;
            rs.spool = o.u32("spool");
            rs.length_mm = o.number("length_mm");
            r.spools.push_back(rs);
        }
        if (f.has("levels"))
            r.levels = f.boolean("levels");
        out = r;
    });
}

std::string compact_json_object(const std::string &text, const std::string &what) {
    json::Document doc;
    parse_text(text, doc, what);
    if (!doc.IsObject()) {
        Issue issue;
        issue.severity = kError;
        issue.code = "schema.type";
        issue.entry = what;
        issue.message = what + " must be a JSON object";
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, issue);
    }
    return compact_value(doc);
}

} // namespace format
} // namespace loomora

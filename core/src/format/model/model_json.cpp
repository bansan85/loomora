#include "format/model/model_json.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace loomora {
namespace format {
namespace model {

using json::Fields;
using json::Value;

namespace {

const std::size_t kBig = 1u << 20; // the length rules are the model's check_* functions

std::string index_path(const std::string &base, rapidjson::SizeType i) {
    return base + "[" + json::dec(i) + "]";
}

const Value &array_of(const Value &v, const std::string &entry, const std::string &path) {
    if (!v.IsArray())
        json::fail(entry, "schema.type", path + " must be an array");
    return v;
}

void fail_value(const std::string &entry, const std::string &message) {
    json::fail(entry, "schema.value", message);
}

std::string str(const Fields &f, const char *name) { return f.string(name, kBig); }

std::string opt_str(const Fields &f, const char *name) { return f.has(name) ? f.string(name, kBig) : std::string(); }

void number3(const Value &v, double out[3], const std::string &entry, const std::string &path) {
    array_of(v, entry, path);
    if (v.Size() != 3)
        fail_value(entry, path + " must have 3 numbers");
    for (rapidjson::SizeType i = 0; i < 3; ++i)
        out[i] = json::as_number(v[i], entry, index_path(path, i));
}

void parse_entry_ref(const Fields &parent, const char *name, EntryRef &ref) {
    const Fields f = parent.object(name);
    ref.entry = str(f, "entry");
    ref.format = opt_str(f, "format");
    ref.size = f.has("size") ? static_cast<std::int64_t>(f.u64("size")) : -1;
}

void parse_shape(const Value &v, Shape &s, const std::string &entry, const std::string &path) {
    const Fields f(v, entry, path);
    s.type = str(f, "type");
    s.params_json = "{}";
    if (const Value *p = f.find("params")) {
        if (!p->IsObject())
            json::fail(entry, "schema.type", f.path_of("params") + " must be an object");
        s.params_json = dump_json(*p);
    }
    const std::string ppath = f.path_of("polyline");
    const Value &poly = f.array("polyline");
    s.polyline.clear();
    s.polyline.reserve(poly.Size());
    for (rapidjson::SizeType i = 0; i < poly.Size(); ++i) {
        const Value &pt = poly[i];
        const std::string at = index_path(ppath, i);
        array_of(pt, entry, at);
        if (pt.Size() != 2)
            fail_value(entry, at + " must be [x, y]");
        s.polyline.push_back(Point(json::as_number(pt[0], entry, index_path(at, 0)),
                                   json::as_number(pt[1], entry, index_path(at, 1))));
    }
}

} // namespace

std::string dump_json(const Value &value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return std::string(buffer.GetString(), buffer.GetSize());
}

void parse_manifest(const Value &root, Manifest &m, const std::string &entry) {
    const Fields f(root, entry, "");
    {
        const Fields fmt = f.object("format");
        m.format.name = str(fmt, "name");
        m.format.major = fmt.u32("major");
        m.format.minor = fmt.u32("minor");
    }
    if (!parse_version(str(f, "min_reader"), m.min_reader))
        fail_value(entry, "min_reader must be \"M.m\"");
    m.project_id = str(f, "project_id");
    m.title = str(f, "title");
    m.created_at = str(f, "created_at");
    m.modified_at = str(f, "modified_at");
    {
        const Fields g = f.object("generator");
        m.generator.application = str(g, "application");
        m.generator.version = str(g, "version");
        m.generator.platform = str(g, "platform");
        m.generator.architecture = str(g, "architecture");
    }
    if (!parse_profile(str(f, "profile"), m.profile))
        fail_value(entry, "profile must be full or light");
    m.editable = f.boolean("editable");
    {
        const Fields c = f.object("counts");
        m.counts.frames = c.u64("frames");
        m.counts.layouts = c.u64("layouts");
        m.counts.images = c.u64("images");
        m.counts.spools = c.u64("spools");
        m.counts.results = c.u64("results");
    }
    {
        const Fields n = f.object("next_id");
        m.next_id.frame = n.u32("frame");
        m.next_id.layout = n.u32("layout");
        m.next_id.image = n.u32("image");
        m.next_id.spool = n.u32("spool");
        m.next_id.result = n.u32("result");
    }
    {
        const Fields o = f.object("order");
        struct Slot {
            const char *name;
            std::vector<std::uint32_t> *list;
        };
        const Slot slots[] = {{"frames", &m.order.frames}, {"layouts", &m.order.layouts},
                              {"images", &m.order.images}, {"spools", &m.order.spools},
                              {"results", &m.order.results}};
        for (std::size_t s = 0; s < 5; ++s) {
            const Value &a = o.array(slots[s].name);
            const std::string path = o.path_of(slots[s].name);
            slots[s].list->clear();
            slots[s].list->reserve(a.Size());
            for (rapidjson::SizeType i = 0; i < a.Size(); ++i)
                slots[s].list->push_back(json::as_u32(a[i], entry, index_path(path, i)));
        }
    }
    {
        const Value &a = f.array("required_features");
        m.required_features.clear();
        for (rapidjson::SizeType i = 0; i < a.Size(); ++i)
            m.required_features.push_back(
                json::as_string(a[i], entry, index_path("required_features", i), kBig));
    }
    {
        const Value &e = f.get("entries");
        if (!e.IsObject())
            json::fail(entry, "schema.type", "entries must be an object");
        m.entries.clear();
        m.entries.reserve(e.MemberCount());
        for (Value::ConstMemberIterator it = e.MemberBegin(); it != e.MemberEnd(); ++it) {
            ManifestEntry me;
            me.path.assign(it->name.GetString(), it->name.GetStringLength());
            const Fields g(it->value, entry, "entries[" + me.path + "]");
            me.size = g.u64("size");
            me.sha256 = str(g, "sha256");
            m.entries.push_back(me);
        }
    }
    m.extensions_json = "{}";
    if (const Value *x = f.find("extensions")) {
        if (!x->IsObject())
            json::fail(entry, "schema.type", "extensions must be an object");
        m.extensions_json = dump_json(*x);
    }
}

void parse_spools(const Value &root, std::vector<Spool> &out, std::vector<std::string> *texts,
                  const std::string &entry) {
    const Fields f(root, entry, "");
    const Value &a = f.array("spools");
    out.clear();
    if (texts != nullptr)
        texts->clear();
    for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
        const Fields s(a[i], entry, index_path("spools", i));
        Spool sp;
        sp.id = s.u32("id");
        sp.name = str(s, "name");
        sp.color = str(s, "color");
        sp.opacity = s.number("opacity");
        sp.diameter_mm = s.number("diameter_mm");
        if (s.has("available_length_mm")) {
            sp.has_available_length = true;
            sp.available_length_mm = s.number("available_length_mm");
        }
        if (s.has("material")) {
            sp.has_material = true;
            sp.material = str(s, "material");
        }
        if (s.has("domain") && !parse_domain(str(s, "domain"), sp.domain))
            fail_value(entry, s.path_of("domain") + " must be exterior, interior or both");
        out.push_back(sp);
        if (texts != nullptr)
            texts->push_back(dump_json(a[i]));
    }
}

void parse_frame(const Value &root, Frame &fr, const std::string &entry) {
    const Fields f(root, entry, "");
    fr.id = f.u32("id");
    fr.name = str(f, "name");
    if (!parse_frame_kind(str(f, "kind"), fr.kind))
        fail_value(entry, "kind must be 2d or 3d");
    {
        const Fields d = f.object("dimensions");
        fr.width_mm = d.number("width_mm");
        fr.height_mm = d.number("height_mm");
        fr.depth_mm = d.has("depth_mm") ? d.number("depth_mm") : 0;
    }
    fr.background = str(f, "background");
    parse_shape(f.get("outline"), fr.outline, entry, "outline");
    fr.exclusions.clear();
    {
        const Value &a = f.array("exclusions");
        for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
            Shape s;
            parse_shape(a[i], s, entry, index_path("exclusions", i));
            fr.exclusions.push_back(s);
        }
    }
    if (!parse_surface(str(f, "surface"), fr.surface))
        fail_value(entry, "surface is not one of plane, mesh, primitive, extrusion, revolution, csg");
    fr.body = kBodyNone;
    if (f.has("body") && !parse_body(str(f, "body"), fr.body))
        fail_value(entry, "body must be solid, shell or guide");
    fr.has_mesh = false;
    if (f.has("mesh")) {
        const Fields m = f.object("mesh");
        fr.has_mesh = true;
        MeshInfo &mi = fr.mesh;
        mi = MeshInfo();
        mi.vertices = m.u64("vertices");
        mi.triangles = m.u64("triangles");
        mi.watertight = m.boolean("watertight");
        mi.manifold = m.boolean("manifold");
        mi.has_normals = m.boolean("has_normals");
        mi.has_uvs = m.has("has_uvs") ? m.boolean("has_uvs") : false;
        if (m.has("source")) {
            const Fields s = m.object("source");
            mi.has_source = true;
            mi.source_name = str(s, "name");
            mi.source_sha256 = opt_str(s, "sha256");
        }
        if (const Value *c = m.find("construction"))
            mi.construction_json = dump_json(*c);
        mi.attached_source = opt_str(m, "attached_source");
    }
}

void parse_layout(const Value &root, Layout &out, const std::string &entry) {
    const Fields f(root, entry, "");
    out.id = f.u32("id");
    out.name = str(f, "name");
    out.frame = f.u32("frame");
    out.nail_count = f.u64("nail_count");
}

void parse_image(const Value &root, Image &im, const std::string &entry) {
    const Fields f(root, entry, "");
    im.id = f.u32("id");
    im.name = str(f, "name");
    if (!parse_color_mode(str(f, "color_mode"), im.color_mode))
        fail_value(entry, "color_mode must be bw, gray or color");
    if (!parse_image_shape(str(f, "shape"), im.shape))
        fail_value(entry, "shape is not one of circle, landscape, portrait, square, free");
    im.width_px = f.u64("width_px");
    im.height_px = f.u64("height_px");
    im.has_source = f.has("source");
    if (im.has_source)
        parse_entry_ref(f, "source", im.source);
    im.has_target = f.has("target");
    if (im.has_target)
        parse_entry_ref(f, "target", im.target);
    parse_entry_ref(f, "thumbnail", im.thumbnail);
}

void parse_result(const Value &root, Result &r, const std::string &entry) {
    const Fields f(root, entry, "");
    r.id = f.u32("id");
    r.name = str(f, "name");
    r.created_at = str(f, "created_at");
    r.frame = f.u32("frame");
    r.layout = f.u32("layout");
    r.images.clear();
    {
        const Value &a = f.array("images");
        for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
            const Fields g(a[i], entry, index_path("images", i));
            ResultImage ri;
            ri.image = g.u32("image");
            if (g.has("view")) {
                const Fields v = g.object("view");
                ri.has_view = true;
                number3(v.get("position"), ri.view.position, entry, v.path_of("position"));
                number3(v.get("target"), ri.view.target, entry, v.path_of("target"));
                number3(v.get("up"), ri.view.up, entry, v.path_of("up"));
                if (!parse_projection(str(v, "projection"), ri.view.projection))
                    fail_value(entry, v.path_of("projection") + " must be orthographic or perspective");
                ri.view.size = v.has("size") ? v.number("size") : 0;
                ri.view.fov_deg = v.has("fov_deg") ? v.number("fov_deg") : 0;
                ri.view.weight = v.number("weight");
            }
            r.images.push_back(ri);
        }
    }
    r.spools.clear();
    {
        const Value &a = f.array("spools");
        for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
            const Fields g(a[i], entry, index_path("spools", i));
            ResultSpool rs;
            rs.spool = g.u32("spool");
            rs.length_mm = g.number("length_mm");
            r.spools.push_back(rs);
        }
    }
    r.steps = f.u64("steps");
    r.levels = f.boolean("levels");
    parse_entry_ref(f, "thumbnail", r.thumbnail);
    r.has_settings = f.has("settings");
    if (r.has_settings)
        parse_entry_ref(f, "settings", r.settings);
}

} // namespace model
} // namespace format
} // namespace loomora

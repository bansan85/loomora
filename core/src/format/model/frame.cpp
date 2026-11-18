#include "format/model/frame.hpp"

namespace loomora {
namespace format {
namespace model {

namespace {

const char *const kKindNames[] = {"2d", "3d"};
const char *const kSurfaceNames[] = {"plane", "mesh", "primitive", "extrusion", "revolution", "csg"};
const char *const kBodyNames[] = {"solid", "shell", "guide"};
const char *const kOutlineTypes[] = {"circle", "ellipse", "rectangle", "heart", "star", "svg", "polygon"};

void check_shape(const Shape &shape, const char *what, bool allow_empty, const Frame &f, const std::string &entry,
                 Report &report) {
    const std::string w(what);
    int unused;
    if (!lookup_name(kOutlineTypes, 7, shape.type, unused))
        add_value_error(report, entry, kItemFrame, f.id, w + ".type is not a known outline type");
    const bool empty_ok = allow_empty && shape.polyline.empty();
    if (shape.polyline.size() < 3 && !empty_ok)
        add_value_error(report, entry, kItemFrame, f.id, w + ".polyline needs at least 3 points");
    for (std::size_t i = 0; i < shape.polyline.size(); ++i) {
        if (!is_finite(shape.polyline[i].x) || !is_finite(shape.polyline[i].y)) {
            add_error(report, "num.not_finite", entry, kItemFrame, f.id, w + ".polyline has a point that is not finite");
            break; // one issue per shape
        }
    }
    if (shape.params_json.empty())
        add_value_error(report, entry, kItemFrame, f.id, w + ".params must be a JSON object text");
}

} // namespace

const char *to_string(FrameKind k) {
    const int i = static_cast<int>(k);
    return (i >= 0 && i < 2) ? kKindNames[i] : "";
}

const char *to_string(Surface s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < 6) ? kSurfaceNames[i] : "";
}

const char *to_string(Body b) {
    const int i = static_cast<int>(b);
    return (i >= 1 && i <= 3) ? kBodyNames[i - 1] : "";
}

bool parse_frame_kind(const std::string &text, FrameKind &out) {
    int i;
    if (!lookup_name(kKindNames, 2, text, i))
        return false;
    out = static_cast<FrameKind>(i);
    return true;
}

bool parse_surface(const std::string &text, Surface &out) {
    int i;
    if (!lookup_name(kSurfaceNames, 6, text, i))
        return false;
    out = static_cast<Surface>(i);
    return true;
}

bool parse_body(const std::string &text, Body &out) {
    int i;
    if (!lookup_name(kBodyNames, 3, text, i))
        return false;
    out = static_cast<Body>(i + 1);
    return true;
}

bool is_outline_type(const std::string &type) {
    int i;
    return lookup_name(kOutlineTypes, 7, type, i);
}

void check_frame(const Frame &f, const std::string &entry, Report &report) {
    const bool is3d = (f.kind == kFrame3D);
    check_length(report, entry, kItemFrame, f.id, "name", f.name, kMaxNameBytes);
    if (f.kind != kFrame2D && f.kind != kFrame3D)
        add_value_error(report, entry, kItemFrame, f.id, "kind must be 2d or 3d");
    check_positive(report, entry, kItemFrame, f.id, "dimensions.width_mm", f.width_mm);
    check_positive(report, entry, kItemFrame, f.id, "dimensions.height_mm", f.height_mm);
    if (is3d)
        check_positive(report, entry, kItemFrame, f.id, "dimensions.depth_mm", f.depth_mm);
    if (!is_hex_color(f.background))
        add_value_error(report, entry, kItemFrame, f.id, "background must be #rrggbb");

    check_shape(f.outline, "outline", is3d, f, entry, report);
    for (std::size_t i = 0; i < f.exclusions.size(); ++i)
        check_shape(f.exclusions[i], "exclusions[]", false, f, entry, report);

    if (!is3d) {
        if (f.surface != kSurfacePlane)
            add_value_error(report, entry, kItemFrame, f.id, "a 2D frame requires surface plane");
        if (f.has_mesh)
            add_value_error(report, entry, kItemFrame, f.id, "a 2D frame has no mesh");
        if (f.body != kBodyNone)
            add_value_error(report, entry, kItemFrame, f.id, "body is for 3D frames only");
    } else {
        if (f.surface == kSurfacePlane)
            add_value_error(report, entry, kItemFrame, f.id,
                            "surface of a 3D frame must be mesh, primitive, extrusion, revolution or csg");
        if (f.body == kBodyNone)
            add_value_error(report, entry, kItemFrame, f.id, "a 3D frame requires body solid, shell or guide");
        if (f.surface == kSurfaceMesh && !f.has_mesh)
            add_value_error(report, entry, kItemFrame, f.id, "surface mesh requires a mesh description");
    }
    if (f.has_mesh) {
        if (!f.mesh.source_sha256.empty() && !is_sha256_hex(f.mesh.source_sha256))
            add_value_error(report, entry, kItemFrame, f.id, "mesh.source.sha256 must be 64 lowercase hex characters");
        if (f.mesh.attached_source.size() > kMaxEntryBytes)
            add_value_error(report, entry, kItemFrame, f.id, "mesh.attached_source is longer than 512 bytes");
    }
}

} // namespace model
} // namespace format
} // namespace loomora

// Frame (frame.json, data-model.md "Frame"): the shape the thread is wound on. 2D (flat, with an
// outline) or 3D (a surface, usually a mesh whose data is in mesh.bin).
#ifndef LOOMORA_FORMAT_MODEL_FRAME_HPP
#define LOOMORA_FORMAT_MODEL_FRAME_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "format/model/common.hpp"

namespace loomora {
namespace format {
namespace model {

enum FrameKind { kFrame2D = 0, kFrame3D = 1 };
enum Surface {
    kSurfacePlane = 0,      // 2D only
    kSurfaceMesh = 1,       // 3D from here on
    kSurfacePrimitive = 2,
    kSurfaceExtrusion = 3,
    kSurfaceRevolution = 4,
    kSurfaceCsg = 5
};
enum Body { kBodyNone = 0, kBodySolid = 1, kBodyShell = 2, kBodyGuide = 3 }; // 3D only; kBodyNone = absent

const char *to_string(FrameKind k);
const char *to_string(Surface s);
const char *to_string(Body b); // "" for kBodyNone
bool parse_frame_kind(const std::string &text, FrameKind &out);
bool parse_surface(const std::string &text, Surface &out);
bool parse_body(const std::string &text, Body &out);

// The outline types: circle ellipse rectangle heart star svg polygon.
bool is_outline_type(const std::string &type);

struct Point {
    double x, y;
    Point() : x(0), y(0) {}
    Point(double x_, double y_) : x(x_), y(y_) {}
};

// An outline or an exclusion. JSON key order: type, params, polyline.
struct Shape {
    std::string type;                 // one of the outline types
    std::string params_json = "{}";   // free object per type, kept as compact JSON text
    std::vector<Point> polyline;      // resolved closed polygon, at least 3 points
                                      // (a 3D frame's outline footprint may be empty)
};

// The mesh description of a 3D frame (the data is in mesh.bin). JSON key order: vertices,
// triangles, watertight, manifold, has_normals, has_uvs, source, construction, attached_source.
struct MeshInfo {
    std::uint64_t vertices = 0;
    std::uint64_t triangles = 0;
    bool watertight = false;
    bool manifold = false;
    bool has_normals = false;
    bool has_uvs = false;             // default false; full profile only
    bool has_source = false;          // the imported file the mesh came from (optional)
    std::string source_name;
    std::string source_sha256;        // empty when absent, else 64 lowercase hex
    std::string construction_json;    // parametric description as compact JSON; empty when absent (full only)
    std::string attached_source;      // path of mesh-source.<ext>; empty when absent (full only)
};

// JSON key order: id, name, kind, dimensions, background, outline, exclusions, surface, body, mesh.
struct Frame {
    std::uint32_t id = 0;
    std::string name;                 // up to 256 bytes
    FrameKind kind = kFrame2D;
    double width_mm = 0;              // dimensions: finite and > 0
    double height_mm = 0;
    double depth_mm = 0;              // 3D only (bounding box); 0 and not written for 2D
    std::string background = "#ffffff"; // #rrggbb
    Shape outline;
    std::vector<Shape> exclusions;    // each inside the outline (checked by the validator)
    Surface surface = kSurfacePlane;  // 2D requires plane
    Body body = kBodyNone;            // 3D: required
    bool has_mesh = false;            // 3D only; required when surface is mesh
    MeshInfo mesh;
};

void check_frame(const Frame &frame, const std::string &entry, Report &report);

} // namespace model
} // namespace format
} // namespace loomora

#endif

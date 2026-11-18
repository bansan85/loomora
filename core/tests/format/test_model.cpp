// The rules of the model structs (data-model.md): each check function adds one error Issue per
// violated rule, with the item kind and identifier, and nothing for a valid item.
#include <catch2/catch.hpp>

#include <cmath>
#include <limits>
#include <string>

#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"

using namespace loomora::format;
using namespace loomora::format::model;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

// The codes of the error issues of a report, joined by spaces.
std::string codes(const Report &r) {
    std::string s;
    for (std::size_t i = 0; i < r.count(); ++i) {
        CHECK(r.at(i).severity == kError);
        s += (i ? " " : "") + r.at(i).code;
    }
    return s;
}

Frame frame2d() {
    Frame f;
    f.id = 3;
    f.name = "Square";
    f.kind = kFrame2D;
    f.width_mm = 500;
    f.height_mm = 400;
    f.background = "#ffffff";
    f.outline.type = "rectangle";
    f.outline.params_json = "{\"width_mm\":500,\"height_mm\":400}";
    const Point pts[] = {{-250, -200}, {250, -200}, {250, 200}, {-250, 200}};
    f.outline.polyline.assign(pts, pts + 4);
    f.surface = kSurfacePlane;
    return f;
}

Frame frame3d() {
    Frame f = frame2d();
    f.kind = kFrame3D;
    f.depth_mm = 100;
    f.surface = kSurfaceMesh;
    f.body = kBodyShell;
    f.has_mesh = true;
    f.mesh.vertices = 8;
    f.mesh.triangles = 12;
    f.mesh.watertight = true;
    f.mesh.manifold = true;
    return f;
}

std::string check(const Frame &f) {
    Report r;
    check_frame(f, "frames/3/frame.json", r);
    return codes(r);
}

Spool spool() {
    Spool s;
    s.id = 1;
    s.name = "Black";
    s.color = "#000000";
    s.opacity = 1;
    s.diameter_mm = 0.3;
    return s;
}

std::string check(const Spool &s) {
    Report r;
    check_spool(s, "spools.json", r);
    return codes(r);
}

Nail nail2d(std::uint32_t id) {
    Nail n;
    n.id = id;
    n.x = 1;
    n.y = 2;
    n.diameter = 2;
    n.kind = kNailBoundary;
    return n;
}

Nail nail3d(std::uint32_t id) {
    Nail n = nail2d(id);
    n.z = 3;
    n.axis[0] = 0;
    n.axis[1] = 0;
    n.axis[2] = 1;
    n.length = 5;
    n.side = kSideExterior;
    n.triangle = 4;
    n.u = 0.25;
    n.v = 0.25;
    return n;
}

std::string check(const NailTable &t, std::uint64_t triangles = kAnyTriangleCount) {
    Report r;
    check_nails(t, "nails.json", 2, r, triangles);
    return codes(r);
}

Image image() {
    Image i;
    i.id = 1;
    i.name = "Portrait";
    i.color_mode = kColorBw;
    i.shape = kShapeCircle;
    i.width_px = 1200;
    i.height_px = 1200;
    i.has_source = true;
    i.source.entry = "images/1/source.jpg";
    i.source.format = "jpeg";
    i.source.size = 345678;
    i.thumbnail.entry = "images/1/thumb.png";
    return i;
}

std::string check(const Image &i, Profile p = kProfileFull) {
    Report r;
    check_image(i, p, "images/1/image.json", r);
    return codes(r);
}

Result result2d() {
    Result r;
    r.id = 7;
    r.name = "R";
    r.created_at = "2026-11-17T09:30:00Z";
    r.frame = 1;
    r.layout = 1;
    ResultImage im;
    im.image = 1;
    r.images.push_back(im);
    ResultSpool s;
    s.spool = 1;
    s.length_mm = 1000;
    r.spools.push_back(s);
    r.steps = 10;
    r.thumbnail.entry = "results/7/thumb.png";
    return r;
}

ResultImage view_image(std::uint32_t id) {
    ResultImage im;
    im.image = id;
    im.has_view = true;
    im.view.position[0] = 0; im.view.position[1] = 0; im.view.position[2] = 500;
    im.view.target[0] = 0; im.view.target[1] = 0; im.view.target[2] = 0;
    im.view.up[0] = 0; im.view.up[1] = 1; im.view.up[2] = 0;
    im.view.projection = kProjectionOrthographic;
    im.view.size = 400;
    im.view.weight = 1;
    return im;
}

std::string check(const Result &r, FrameKind k = kFrame2D, Profile p = kProfileFull) {
    Report rep;
    check_result(r, k, p, "results/7/result.json", rep);
    return codes(rep);
}

Manifest manifest() {
    Manifest m;
    m.project_id = "9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d";
    m.title = "Living room";
    m.created_at = "2026-11-17T09:00:00Z";
    m.modified_at = "2026-11-17T09:00:00.250Z";
    m.generator.application = "Loomora";
    m.counts.frames = 1;
    m.counts.spools = 2;
    m.next_id.frame = 2;
    m.next_id.spool = 3;
    m.order.frames.push_back(1);
    m.order.spools.push_back(1);
    m.order.spools.push_back(2);
    ManifestEntry e;
    e.path = "spools.json";
    e.size = 10;
    e.sha256 = std::string(64, 'a');
    m.entries.push_back(e);
    return m;
}

std::string check(const Manifest &m) {
    Report r;
    check_manifest(m, r);
    return codes(r);
}

} // namespace

TEST_CASE("model: a valid 2D frame has no issue", "[model]") {
    CHECK(check(frame2d()) == "");
    Frame f = frame2d();
    f.name = std::string(256, 'n');
    CHECK(check(f) == "");
    f.name = std::string(257, 'n');
    CHECK(check(f) == "schema.value");
}

TEST_CASE("model: an issue names the item and the entry", "[model]") {
    Frame f = frame2d();
    f.name = std::string(300, 'x');
    Report r;
    check_frame(f, "frames/3/frame.json", r);
    REQUIRE(r.count() == 1);
    CHECK(r.at(0).entry == "frames/3/frame.json");
    CHECK(r.at(0).item_kind == kItemFrame);
    CHECK(r.at(0).item_id == 3);
    CHECK(r.at(0).message.find("name") != std::string::npos);
}

TEST_CASE("model: frame dimensions must be finite and positive", "[model]") {
    Frame f = frame2d();
    f.width_mm = 0;
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.height_mm = -1;
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.width_mm = kNaN;
    CHECK(check(f) == "num.not_finite");
    f = frame2d();
    f.height_mm = kInf;
    CHECK(check(f) == "num.not_finite");
    f = frame3d();
    f.depth_mm = 0;
    CHECK(check(f) == "schema.value");
    f.depth_mm = 1e-9;
    CHECK(check(f) == "");
}

TEST_CASE("model: an outline polyline has at least 3 finite points", "[model]") {
    Frame f = frame2d();
    f.outline.polyline.resize(2);
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.outline.polyline[1].y = kNaN;
    CHECK(check(f) == "num.not_finite");
    f = frame2d();
    f.outline.type = "blob";
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.background = "ffffff";
    CHECK(check(f) == "schema.value");
    f.background = "#FfA00b";
    CHECK(check(f) == "");

    Shape hole;
    hole.type = "circle";
    hole.polyline = f.outline.polyline;
    f.exclusions.push_back(hole);
    CHECK(check(f) == "");
    f.exclusions[0].polyline.resize(2);
    CHECK(check(f) == "schema.value");

    // The footprint of a 3D frame may be empty.
    Frame g = frame3d();
    g.outline.polyline.clear();
    CHECK(check(g) == "");
    g.outline.polyline.resize(2);
    CHECK(check(g) == "schema.value");
}

TEST_CASE("model: surface, body and mesh follow the frame kind", "[model]") {
    CHECK(check(frame3d()) == "");
    Frame f = frame2d();
    f.surface = kSurfaceMesh;
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.has_mesh = true;
    CHECK(check(f) == "schema.value");
    f = frame2d();
    f.body = kBodySolid;
    CHECK(check(f) == "schema.value");
    f = frame3d();
    f.surface = kSurfacePlane;
    CHECK(check(f) == "schema.value");
    f = frame3d();
    f.body = kBodyNone;
    CHECK(check(f) == "schema.value");
    f = frame3d();
    f.has_mesh = false; // surface mesh needs a mesh
    CHECK(check(f) == "schema.value");
    f.surface = kSurfacePrimitive;
    CHECK(check(f) == "");
    f = frame3d();
    f.mesh.source_sha256 = "xyz";
    CHECK(check(f) == "schema.value");
    f.mesh.source_sha256 = std::string(64, '0');
    CHECK(check(f) == "");
    f.mesh.attached_source = std::string(513, 'a');
    CHECK(check(f) == "schema.value");
}

TEST_CASE("model: spool rules", "[model]") {
    CHECK(check(spool()) == "");
    Spool s = spool();
    s.opacity = 0;
    CHECK(check(s) == "schema.value");
    s.opacity = 1e-12;
    CHECK(check(s) == "");
    s.opacity = 1.0000001;
    CHECK(check(s) == "schema.value");
    s.opacity = kNaN;
    CHECK(check(s) == "num.not_finite");
    s = spool();
    s.diameter_mm = 0;
    CHECK(check(s) == "schema.value");
    s = spool();
    s.color = "#12345";
    CHECK(check(s) == "schema.value");
    s = spool();
    s.has_available_length = true;
    s.available_length_mm = 0;
    CHECK(check(s) == "schema.value");
    s.available_length_mm = 200000;
    CHECK(check(s) == "");
    CHECK(s.domain == kDomainExterior);
}

TEST_CASE("model: spool identifiers are unique", "[model]") {
    std::vector<Spool> list;
    list.push_back(spool());
    list.push_back(spool());
    Report r;
    check_spools(list, "spools.json", r);
    CHECK(codes(r) == "id.duplicate");
    list[1].id = 2;
    Report r2;
    check_spools(list, "spools.json", r2);
    CHECK(r2.count() == 0);
}

TEST_CASE("model: layout and image rules", "[model]") {
    Layout l;
    l.id = 2;
    l.name = "Circle";
    l.frame = 1;
    l.nail_count = 200;
    Report r;
    check_layout(l, "layout.json", r);
    CHECK(r.count() == 0);
    l.name = std::string(257, 'a');
    check_layout(l, "layout.json", r);
    CHECK(codes(r) == "schema.value");

    CHECK(check(image()) == "");
    Image i = image();
    i.width_px = 0;
    CHECK(check(i) == "schema.value");
    i = image();
    i.height_px = 0;
    CHECK(check(i) == "schema.value");
    i = image();
    i.source.format = "gif";
    CHECK(check(i) == "schema.value");
    i = image();
    i.thumbnail.entry = std::string(513, 'a');
    CHECK(check(i) == "schema.value");
    // A light project keeps no source and no target.
    CHECK(check(image(), kProfileLight) == "schema.value");
    i = image();
    i.has_source = false;
    CHECK(check(i, kProfileLight) == "");
    i.has_target = true;
    i.target.entry = "images/1/target.png";
    CHECK(check(i, kProfileLight) == "schema.value");
}

TEST_CASE("model: nail rules", "[model]") {
    NailTable t2(false);
    t2.add(nail2d(1));
    t2.add(nail2d(2));
    CHECK(check(t2) == "");
    CHECK(check(NailTable(true)) == "");

    NailTable t(false);
    Nail n = nail2d(1);
    n.diameter = 0;
    t.add(n);
    CHECK(check(t) == "schema.value");

    NailTable d(false);
    d.add(nail2d(5));
    d.add(nail2d(6));
    d.add(nail2d(5));
    CHECK(check(d) == "nail.duplicate_id");

    NailTable f(false);
    n = nail2d(1);
    n.x = kNaN;
    f.add(n);
    CHECK(check(f) == "num.not_finite");

    NailTable big(false);
    n = nail2d(1);
    n.label = "0123456789012345678901234567890123456789012345678901234567890123"; // 64 bytes
    n.has_label = true;
    big.add(n);
    CHECK(check(big) == "");
    NailTable big2(false);
    n.label += "x";
    big2.add(n);
    CHECK(check(big2) == "schema.value");
}

TEST_CASE("model: 3D nail rules", "[model]") {
    NailTable ok(true);
    ok.add(nail3d(1));
    CHECK(check(ok) == "");

    Nail n = nail3d(1);
    n.axis[2] = 1.00001; // |axis| = 1.00001: off by more than 1e-6
    NailTable a(true);
    a.add(n);
    CHECK(check(a) == "schema.value");
    n.axis[2] = 1.0000001; // off by 1e-7
    NailTable a2(true);
    a2.add(n);
    CHECK(check(a2) == "");
    n = nail3d(1);
    n.axis[0] = 1; n.axis[1] = 1; n.axis[2] = 1;
    NailTable a3(true);
    a3.add(n);
    CHECK(check(a3) == "schema.value");

    n = nail3d(1);
    n.u = -2e-6;
    NailTable u(true);
    u.add(n);
    CHECK(check(u) == "schema.value");
    n.u = -5e-7;
    NailTable u2(true);
    u2.add(n);
    CHECK(check(u2) == "");
    n = nail3d(1);
    n.v = -0.5;
    NailTable v(true);
    v.add(n);
    CHECK(check(v) == "schema.value");

    n = nail3d(1);
    n.u = 0.5;
    n.v = 0.5 + 2e-6;
    NailTable s(true);
    s.add(n);
    CHECK(check(s) == "schema.value");
    n.v = 0.5 + 5e-7;
    NailTable s2(true);
    s2.add(n);
    CHECK(check(s2) == "");

    n = nail3d(1);
    n.length = -1;
    NailTable len(true);
    len.add(n);
    CHECK(check(len) == "schema.value");

    // face.triangle below the triangle count of the frame's mesh
    NailTable tri(true);
    tri.add(nail3d(1)); // triangle 4
    CHECK(check(tri, 5) == "");
    CHECK(check(tri, 4) == "schema.value");
}

TEST_CASE("model: nail table keeps every field and the labels", "[model]") {
    NailTable t(true);
    Nail a = nail3d(10);
    a.has_label = true;
    a.label = "A1";
    Nail b = nail3d(7);
    b.kind = kNailInterior;
    b.side = kSideInterior;
    Nail c = nail3d(99);
    c.has_label = true;
    c.label = "c";
    t.add(a);
    t.add(b);
    t.add(c);
    REQUIRE(t.size() == 3);
    const Nail g = t.get(1);
    CHECK(g.id == 7);
    CHECK(g.kind == kNailInterior);
    CHECK(g.side == kSideInterior);
    CHECK_FALSE(g.has_label);
    CHECK(g.axis[2] == 1.0);
    CHECK(g.triangle == 4);
    CHECK(t.get(0).label == "A1");
    CHECK(t.get(0).has_label);
    CHECK(t.get(2).label == "c");
    CHECK(t.label_count() == 2);
    t.clear();
    CHECK(t.size() == 0);
    CHECK(t.label_count() == 0);
}

TEST_CASE("model: result spool selection", "[model]") {
    CHECK(check(result2d()) == "");
    Result r = result2d();
    r.spools.clear();
    CHECK(check(r) == "schema.value");
    r = result2d();
    r.spools.push_back(r.spools[0]); // the same spool twice
    CHECK(check(r) == "schema.value");
    r = result2d();
    r.spools[0].length_mm = 0;
    CHECK(check(r) == "schema.value");
    r.spools[0].length_mm = kInf;
    CHECK(check(r) == "num.not_finite");
    r = result2d();
    r.created_at = "2026-11-17 09:30:00";
    CHECK(check(r) == "schema.value");
    r = result2d();
    r.images.clear();
    CHECK(check(r) == "schema.value");
}

TEST_CASE("model: result images follow the frame kind", "[model]") {
    Result r = result2d();
    r.images.push_back(r.images[0]);
    r.images[1].image = 2;
    CHECK(check(r, kFrame2D) == "schema.value"); // 2D: exactly one image
    r = result2d();
    r.images[0] = view_image(1);
    CHECK(check(r, kFrame2D) == "schema.value"); // 2D: no view

    r = result2d();
    CHECK(check(r, kFrame3D) == "schema.value"); // 3D: each image needs a view
    r.images.clear();
    r.images.push_back(view_image(1));
    r.images.push_back(view_image(2));
    CHECK(check(r, kFrame3D) == "");

    r.images[0].view.projection = kProjectionPerspective;
    r.images[0].view.fov_deg = 45;
    CHECK(check(r, kFrame3D) == "");
    r.images[0].view.fov_deg = 180;
    CHECK(check(r, kFrame3D) == "schema.value");
    r.images[0].view.fov_deg = 0;
    CHECK(check(r, kFrame3D) == "schema.value");
    r.images[0] = view_image(1);
    r.images[0].view.size = 0;
    CHECK(check(r, kFrame3D) == "schema.value");
    r.images[0] = view_image(1);
    r.images[0].view.weight = -0.1;
    CHECK(check(r, kFrame3D) == "schema.value");
    r.images[0] = view_image(1);
    r.images[0].view.position[1] = kNaN;
    CHECK(check(r, kFrame3D) == "num.not_finite");
}

TEST_CASE("model: result settings only in a full project", "[model]") {
    Result r = result2d();
    r.has_settings = true;
    r.settings.entry = "results/7/settings.json";
    CHECK(check(r) == "");
    CHECK(check(r, kFrame2D, kProfileLight) == "schema.value");
}

TEST_CASE("model: a valid manifest has no issue", "[model]") {
    CHECK(check(manifest()) == "");
    CHECK(version_text(manifest().min_reader) == "1.0");
}

TEST_CASE("model: manifest rules", "[model]") {
    Manifest m = manifest();
    m.title = std::string(1024, 't');
    CHECK(check(m) == "");
    m.title += "t";
    CHECK(check(m) == "schema.value");

    m = manifest();
    m.project_id = "9B1DEB4D-3B7D-4BAD-9BDD-2B0D7B3DCB6D"; // uppercase
    CHECK(check(m) == "schema.value");
    m.project_id = "9b1deb4d-3b7d-1bad-9bdd-2b0d7b3dcb6d"; // version 1
    CHECK(check(m) == "schema.value");
    m.project_id = "9b1deb4d-3b7d-4bad-cbdd-2b0d7b3dcb6d"; // variant bits
    CHECK(check(m) == "schema.value");

    m = manifest();
    m.min_reader.major = 1;
    m.min_reader.minor = 1; // above format 1.0
    CHECK(check(m) == "schema.value");
    m = manifest();
    m.format.name = "other";
    CHECK(check(m) == "schema.value");

    m = manifest();
    m.created_at = "2026-11-17T09:00:00";
    CHECK(check(m) == "schema.value");
    m = manifest();
    m.profile = kProfileLight; // light implies editable false
    CHECK(check(m) == "schema.value");
    m.editable = false;
    CHECK(check(m) == "");

    m = manifest();
    m.counts.spools = 3;
    CHECK(check(m) == "schema.value");
    m = manifest();
    m.next_id.spool = 2; // not above the identifier 2
    CHECK(check(m) == "schema.value");
    m = manifest();
    m.order.spools[1] = 1; // a repeat
    CHECK(check(m) == "id.duplicate");
}

TEST_CASE("model: manifest features and entries", "[model]") {
    Manifest m = manifest();
    m.required_features.push_back("surface_mesh");
    m.required_features.push_back("string_levels");
    m.required_features.push_back("multi_view_target");
    CHECK(check(m) == "");
    m.required_features.push_back("teleport");
    CHECK(check(m) == "schema.value");

    m = manifest();
    m.entries[0].sha256 = "ABC";
    CHECK(check(m) == "schema.value");
    m = manifest();
    m.entries.push_back(m.entries[0]); // the same path twice
    CHECK(check(m) == "id.duplicate");
    m = manifest();
    m.entries[0].path = "manifest.json";
    CHECK(check(m) == "schema.value");
    m.entries[0].path = "mimetype";
    CHECK(check(m) == "schema.value");
}

TEST_CASE("model: enum names round trip", "[model]") {
    FrameKind k;
    CHECK(parse_frame_kind("3d", k));
    CHECK(k == kFrame3D);
    CHECK(std::string(to_string(kFrame2D)) == "2d");
    CHECK_FALSE(parse_frame_kind("4d", k));
    Surface s;
    CHECK(parse_surface("csg", s));
    CHECK(s == kSurfaceCsg);
    Body b;
    CHECK(parse_body("guide", b));
    CHECK(b == kBodyGuide);
    NailKind nk;
    CHECK(parse_nail_kind("exclusion_boundary", nk));
    CHECK(nk == kNailExclusionBoundary);
    CHECK(std::string(to_string(kNailInterior)) == "interior");
    NailSide ns;
    CHECK(parse_nail_side("interior", ns));
    CHECK(std::string(to_string(kSideExterior)) == "exterior");
    ColorMode cm;
    CHECK(parse_color_mode("gray", cm));
    CHECK(cm == kColorGray);
    ImageShape sh;
    CHECK(parse_image_shape("landscape", sh));
    CHECK(std::string(to_string(kShapeFree)) == "free");
    Domain d;
    CHECK(parse_domain("both", d));
    CHECK(d == kDomainBoth);
    Projection p;
    CHECK(parse_projection("perspective", p));
    CHECK(std::string(to_string(kProjectionOrthographic)) == "orthographic");
    Profile pr;
    CHECK(parse_profile("light", pr));
    CHECK(std::string(to_string(kProfileFull)) == "full");
    CHECK_FALSE(parse_profile("heavy", pr));
}

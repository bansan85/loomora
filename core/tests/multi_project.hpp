// Builders of projects with several items and a 3D frame, through the PUBLIC C API only (loomora.h),
// for the tests of editing, dependencies and copy-through (spec 002, User Story 3). Header only, plain
// C++11, no framework, built on basic_project.hpp (its PNG maker, constants and helpers).
//
// `build_small` makes a small multi-item project: 6 spools, three frames (two 2D, one 3D cube), five
// layouts (two on frame 1, one on frame 2 that no result uses, one on the 3D frame), four images, five
// results (two share a layout, one result has two views on the 3D frame). Everything is deterministic
// and small (24 nails per 2D layout, 60 steps per result) so a test can build one per case.
//
// `build_multi` makes the reference `multi` project of T045 / T054 (see its comment), with a `scale`
// that multiplies the nails of each layout and the steps of each result (loomora_refgen --scale N).
#ifndef LOOMORA_TESTS_MULTI_PROJECT_HPP
#define LOOMORA_TESTS_MULTI_PROJECT_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "basic_project.hpp"

namespace multi_project {

using basic_project::Bytes;
using basic_project::num;

static const std::uint32_t kNails2D = 24;
static const std::uint32_t kNails3D = 12;
static const std::uint32_t kSteps = 60;

// A cube of 100 mm: 8 vertices, 12 triangles, unit normals, UVs in [0, 1].
struct Cube {
    std::vector<float> xyz;
    std::vector<std::uint32_t> abc;
    std::vector<float> normals;
    std::vector<float> uvs;
    Cube(float scale = 50.0f) {
        const float s = scale;
        const float v[8][3] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                               {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
        for (int i = 0; i < 8; ++i) {
            const float len = std::sqrt(v[i][0] * v[i][0] + v[i][1] * v[i][1] + v[i][2] * v[i][2]);
            for (int k = 0; k < 3; ++k) {
                xyz.push_back(v[i][k]);
                normals.push_back(v[i][k] / len);
            }
            uvs.push_back(v[i][0] > 0 ? 1.0f : 0.0f);
            uvs.push_back(v[i][1] > 0 ? 1.0f : 0.0f);
        }
        const std::uint32_t t[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                                        {2, 3, 7}, {2, 7, 6}, {1, 2, 6}, {1, 6, 5}, {3, 0, 4}, {3, 4, 7}};
        for (int i = 0; i < 12; ++i)
            for (int k = 0; k < 3; ++k)
                abc.push_back(t[i][k]);
    }
    loomora_mesh_info info(bool with_normals, bool with_uvs) const {
        loomora_mesh_info m = loomora_mesh_info();
        m.vertices = xyz.size() / 3;
        m.triangles = abc.size() / 3;
        m.flags = (with_normals ? LOOMORA_MESH_NORMALS : 0u) | (with_uvs ? LOOMORA_MESH_UVS : 0u);
        return m;
    }
};

// The identifiers of what build_small made.
struct Ids {
    std::uint32_t spool[6];
    std::uint32_t frame_a, frame_b, frame_3d;
    std::uint32_t layout_a1, layout_a2, layout_b, layout_b_unused, layout_3d;
    std::uint32_t image[4];
    std::uint32_t result_a1, result_a2, result_b, result_3d, result_shared; // result_shared: on layout_a1 too
    Ids() {
        for (int i = 0; i < 6; ++i)
            spool[i] = 0;
        for (int i = 0; i < 4; ++i)
            image[i] = 0;
        frame_a = frame_b = frame_3d = 0;
        layout_a1 = layout_a2 = layout_b = layout_b_unused = layout_3d = 0;
        result_a1 = result_a2 = result_b = result_3d = result_shared = 0;
    }
};

inline std::string rect_json(const char *name, double w, double h, const char *background = "#ffffff") {
    const std::string ws = num(static_cast<unsigned long>(w)), hs = num(static_cast<unsigned long>(h));
    const std::string x = num(static_cast<unsigned long>(w / 2)), y = num(static_cast<unsigned long>(h / 2));
    return std::string("{\"name\":\"") + name + "\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":" + ws +
           ",\"height_mm\":" + hs + "},\"background\":\"" + background + "\",\"outline\":{\"type\":\"rectangle\"," +
           "\"params\":{\"width_mm\":" + ws + ",\"height_mm\":" + hs + "},\"polyline\":[[-" + x + ",-" + y + "],[" +
           x + ",-" + y + "],[" + x + "," + y + "],[-" + x + "," + y + "]]},\"exclusions\":[],\"surface\":\"plane\"}";
}

// A 3D frame: a 100 mm box with the given surface and body ("mesh" needs frame_set_mesh before a save).
inline std::string frame3d_json(const char *name, const char *surface = "mesh", const char *body = "solid",
                                const char *background = "#ffffff") {
    return std::string("{\"name\":\"") + name + "\",\"kind\":\"3d\",\"dimensions\":{\"width_mm\":100," +
           "\"height_mm\":100,\"depth_mm\":100},\"background\":\"" + background + "\",\"outline\":{\"type\":" +
           "\"rectangle\",\"params\":{\"width_mm\":100,\"height_mm\":100},\"polyline\":[]},\"exclusions\":[]," +
           "\"surface\":\"" + surface + "\",\"body\":\"" + body + "\"," +
           "\"mesh\":{\"watertight\":true,\"manifold\":true}}";
}

inline std::vector<loomora_nail> ring_2d(unsigned count, double radius, std::uint32_t first_id = 0) {
    std::vector<loomora_nail> nails(count);
    for (unsigned i = 0; i < count; ++i) {
        const double a = 6.283185307179586 * i / count;
        loomora_nail n = loomora_nail();
        n.id = first_id + i;
        n.kind = 1;
        n.x = basic_project::round3(radius * std::cos(a));
        n.y = basic_project::round3(radius * std::sin(a));
        n.diameter = 2;
        nails[i] = n;
    }
    return nails;
}

// Nails on the top face of the cube: a ring of radius 30 at z = 50, axis +Z, on the triangles 0..11.
inline std::vector<loomora_nail> ring_3d(unsigned count, std::uint32_t first_id = 0) {
    std::vector<loomora_nail> nails(count);
    for (unsigned i = 0; i < count; ++i) {
        const double a = 6.283185307179586 * i / count;
        loomora_nail n = loomora_nail();
        n.id = first_id + i;
        n.kind = 1;
        n.flags = 1;
        n.x = basic_project::round3(30.0 * std::cos(a));
        n.y = basic_project::round3(30.0 * std::sin(a));
        n.z = 50;
        n.diameter = 2;
        n.length = 5;
        n.axis[2] = 1.0f;
        n.side = 0;
        n.face_triangle = i % 12;
        n.face_u = 0.25f;
        n.face_v = 0.25f;
        nails[i] = n;
    }
    return nails;
}

inline int32_t add_frame_json(loomora_editor *e, const std::string &json, std::uint32_t *id) {
    return loomora_editor_add_frame(e, json.c_str(), id);
}

// The 3D frame (surface mesh) with the cube mesh set.
inline int32_t add_frame_3d(loomora_editor *e, std::uint32_t *id, bool uvs = false, const char *surface = "mesh",
                            const char *body = "solid") {
    int32_t rc = add_frame_json(e, frame3d_json("Cube 100", surface, body), id);
    if (rc != 0)
        return rc;
    const Cube cube;
    const loomora_mesh_info info = cube.info(true, uvs);
    return loomora_editor_frame_set_mesh(e, *id, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0],
                                         uvs ? &cube.uvs[0] : nullptr);
}

inline int32_t add_layout(loomora_editor *e, std::uint32_t frame, const char *name, const std::vector<loomora_nail> &n,
                          std::uint32_t *id) {
    const std::string json = std::string("{\"name\":\"") + name + "\"}";
    return loomora_editor_add_layout(e, frame, json.c_str(), &n[0], n.size(), id);
}

inline int32_t add_image_mode(loomora_editor *e, const char *name, const char *color_mode, const char *shape,
                              unsigned gray, std::uint32_t *id) {
    const Bytes source = basic_project::make_png(8, 8, static_cast<std::uint8_t>(gray));
    const Bytes target = basic_project::make_png(8, 8, static_cast<std::uint8_t>(gray + 50));
    const Bytes thumb = basic_project::make_png(4, 4, static_cast<std::uint8_t>(gray + 7));
    const std::string json = std::string("{\"name\":\"") + name + "\",\"color_mode\":\"" + color_mode +
                             "\",\"shape\":\"" + shape + "\",\"width_px\":8,\"height_px\":8}";
    return loomora_editor_add_image(e, json.c_str(), &source[0], source.size(), &target[0], target.size(), &thumb[0],
                                    thumb.size(), id);
}

// A result of `kSteps` steps over `nail_count` nails (ids 0 .. nail_count-1) with the given spool
// selection; `images_json` is the content of the "images" array (with views on a 3D frame).
inline int32_t add_result_json(loomora_editor *e, const char *name, std::uint32_t frame, std::uint32_t layout,
                               const std::string &images_json, const std::vector<std::uint32_t> &spools,
                               unsigned nail_count, std::uint32_t *id, std::uint32_t steps = kSteps,
                               unsigned length_base = 1000) {
    std::string sp;
    for (std::size_t i = 0; i < spools.size(); ++i)
        sp += std::string(i ? "," : "") + "{\"spool\":" + num(spools[i]) + ",\"length_mm\":" +
              num(length_base + 10 * i) + "}";
    const std::string json = std::string("{\"name\":\"") + name + "\",\"created_at\":\"" +
                             basic_project::kResultCreatedAt + "\",\"frame\":" + num(frame) +
                             ",\"layout\":" + num(layout) + ",\"images\":[" + images_json + "],\"spools\":[" + sp + "]}";
    const unsigned n = static_cast<unsigned>(spools.size());
    std::vector<loomora_step> list(steps);
    for (std::uint32_t i = 0; i < steps; ++i) {
        loomora_step s = loomora_step();
        const bool first = i < n;
        const bool last = i >= steps - n;
        s.spool_index = first ? i : last ? i - (steps - n) : (i * 5u + 1u) % n;
        s.nail = (i * 7u + s.spool_index * 3u) % nail_count;
        s.rotation = static_cast<std::uint8_t>(i & 1u);
        s.type = first ? 1 : last ? 2 : 0;
        list[i] = s;
    }
    const Bytes thumb = basic_project::make_png(4, 4, 200);
    return loomora_editor_add_result(e, json.c_str(), &list[0], list.size(), &thumb[0], thumb.size(),
                                     "{\"algorithm\":\"greedy\",\"seed\":1}", id);
}

inline std::string one_image(std::uint32_t image) { return "{\"image\":" + num(image) + "}"; }

// Two views of the 3D frame: an orthographic one and a perspective one.
inline std::string two_views(std::uint32_t first, std::uint32_t second) {
    return "{\"image\":" + num(first) + ",\"view\":{\"position\":[0,0,300],\"target\":[0,0,0],\"up\":[0,1,0]," +
           "\"projection\":\"orthographic\",\"size\":200,\"weight\":1}},{\"image\":" + num(second) +
           ",\"view\":{\"position\":[200,0,200],\"target\":[0,0,0],\"up\":[0,1,0],\"projection\":\"perspective\"," +
           "\"fov_deg\":40,\"weight\":0.5}}";
}

// The small multi-item project (see the file comment).
inline int32_t build_small(loomora_editor **out, Ids *ids, bool uvs = true) {
    Ids local;
    Ids &d = ids != nullptr ? *ids : local;
    d = Ids();
    loomora_editor *e = nullptr;
    int32_t rc = basic_project::create(&e, "Several items");
    if (rc != 0)
        return rc;
    *out = e;
    do {
        // Six spools: 0 .. 2 as basic_project, 3 .. 5 with other colors and a diameter.
        for (int i = 0; i < 6; ++i) {
            const char *colors[6] = {"#000000", "#ff0000", "#0000ff", "#00aa00", "#ffaa00", "#808080"};
            const std::string json = "{\"name\":\"Spool " + num(i + 1) + "\",\"color\":\"" + colors[i] +
                                     "\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000," +
                                     "\"material\":\"cotton\",\"domain\":\"exterior\"}";
            if ((rc = loomora_editor_add_spool(e, json.c_str(), &d.spool[i])) != 0)
                break;
        }
        if (rc != 0)
            break;
        if ((rc = add_frame_json(e, rect_json("Square 50", 500, 500), &d.frame_a)) != 0 ||
            (rc = add_frame_json(e, rect_json("Wide 60", 600, 400), &d.frame_b)) != 0 ||
            (rc = add_frame_3d(e, &d.frame_3d, uvs)) != 0)
            break;
        if ((rc = add_layout(e, d.frame_a, "Circle 200", ring_2d(kNails2D, 200), &d.layout_a1)) != 0 ||
            (rc = add_layout(e, d.frame_a, "Circle 150", ring_2d(kNails2D, 150), &d.layout_a2)) != 0 ||
            (rc = add_layout(e, d.frame_b, "Circle 100", ring_2d(kNails2D, 100), &d.layout_b)) != 0 ||
            (rc = add_layout(e, d.frame_b, "Circle 50", ring_2d(kNails2D, 50), &d.layout_b_unused)) != 0 ||
            (rc = add_layout(e, d.frame_3d, "Top ring", ring_3d(kNails3D), &d.layout_3d)) != 0)
            break;
        if ((rc = loomora_editor_layout_set_label(e, d.layout_a1, 0, "first")) != 0)
            break;
        if ((rc = add_image_mode(e, "Portrait", "bw", "circle", 40, &d.image[0])) != 0 ||
            (rc = add_image_mode(e, "Landscape", "gray", "landscape", 80, &d.image[1])) != 0 ||
            (rc = add_image_mode(e, "Color", "color", "portrait", 120, &d.image[2])) != 0 ||
            (rc = add_image_mode(e, "Square", "color", "square", 160, &d.image[3])) != 0)
            break;
        std::vector<std::uint32_t> s12, s23, s14, s56, s123;
        s12.push_back(d.spool[0]);
        s12.push_back(d.spool[1]);
        s23.push_back(d.spool[1]);
        s23.push_back(d.spool[2]);
        s14.push_back(d.spool[0]);
        s14.push_back(d.spool[3]);
        s56.push_back(d.spool[4]);
        s56.push_back(d.spool[5]);
        s123 = s12;
        s123.push_back(d.spool[2]);
        if ((rc = add_result_json(e, "A1", d.frame_a, d.layout_a1, one_image(d.image[0]), s12, kNails2D,
                                  &d.result_a1)) != 0 ||
            (rc = add_result_json(e, "A2", d.frame_a, d.layout_a2, one_image(d.image[1]), s23, kNails2D,
                                  &d.result_a2)) != 0 ||
            (rc = add_result_json(e, "B", d.frame_b, d.layout_b, one_image(d.image[0]), s14, kNails2D,
                                  &d.result_b)) != 0 ||
            (rc = add_result_json(e, "Cube", d.frame_3d, d.layout_3d, two_views(d.image[2], d.image[3]), s56,
                                  kNails3D, &d.result_3d)) != 0 ||
            (rc = add_result_json(e, "Shared", d.frame_a, d.layout_a1, one_image(d.image[2]), s123, kNails2D,
                                  &d.result_shared)) != 0)
            break;
    } while (false);
    if (rc != 0) {
        loomora_editor_close(e);
        *out = nullptr;
    }
    return rc;
}

// ---- the reference `multi` project ---------------------------------------------------------------

// Base sizes of `multi` (multiplied by the scale): nails per layout and steps per result.
static const unsigned kMultiNailsCircle = 120, kMultiNailsHeart = 96, kMultiNailsRing = 80, kMultiNailsSmall = 40,
                      kMultiNails3D = 24;
static const unsigned kMultiStepsHeart = 400, kMultiStepsCircle = 600, kMultiStepsRing = 300, kMultiSteps3D = 240,
                      kMultiStepsShared = 500;

// A heart `size` mm wide (about): x = 16 sin^3 t, y = 13 cos t - 5 cos 2t - 2 cos 3t - cos 4t, times
// size / 32, t = 2 pi i / count. The two tips of this curve are cusps where the nails get close, hence
// the small nail diameter (0.05 mm: no nail-distance warning).
inline std::vector<loomora_nail> heart_2d(unsigned count, double size, std::uint32_t first_id = 0) {
    std::vector<loomora_nail> nails(count);
    for (unsigned i = 0; i < count; ++i) {
        const double t = 6.283185307179586 * i / count;
        const double s = std::sin(t);
        const double k = size / 32.0;
        loomora_nail n = loomora_nail();
        n.id = first_id + i;
        n.kind = 1;
        n.x = basic_project::round3(k * 16.0 * s * s * s);
        n.y = basic_project::round3(k * (13.0 * std::cos(t) - 5.0 * std::cos(2 * t) - 2.0 * std::cos(3 * t) -
                                         std::cos(4 * t)));
        n.diameter = 0.05;
        nails[i] = n;
    }
    return nails;
}

// The reference `multi` project: 3 frames (a 500 x 500 mm square and a 600 x 400 mm rectangle, both 2D,
// and a 3D cube with its mesh, normals and UVs), 5 layouts (a circle and a heart on the square; two rings
// on the rectangle, the second used by no result; a ring on the top face of the cube), 4 images of mixed
// color mode and shape, 6 spools shared by 5 results with different selections and lengths (heart;
// circle; ring of the rectangle; the cube with two views, each tied to its own image; a second result
// on the circle). `scale` multiplies the nails of each layout and the steps of each result.
// Ids: layout_a1 the circle, layout_a2 the heart; result_a2 the heart result, result_a1 the circle
// result, result_shared the second result on the circle.
inline int32_t build_multi(loomora_editor **out, Ids *ids, unsigned scale = 1) {
    Ids local;
    Ids &d = ids != nullptr ? *ids : local;
    d = Ids();
    loomora_editor *e = nullptr;
    int32_t rc = basic_project::create(&e, "Several items");
    if (rc != 0)
        return rc;
    *out = e;
    const unsigned k = scale == 0 ? 1 : scale;
    do {
        const char *colors[6] = {"#000000", "#ff0000", "#0000ff", "#00aa00", "#ffaa00", "#808080"};
        for (int i = 0; i < 6; ++i) {
            const std::string json = "{\"name\":\"Spool " + num(i + 1) + "\",\"color\":\"" + colors[i] +
                                     "\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000," +
                                     "\"material\":\"cotton\",\"domain\":\"exterior\"}";
            if ((rc = loomora_editor_add_spool(e, json.c_str(), &d.spool[i])) != 0)
                break;
        }
        if (rc != 0)
            break;
        if ((rc = add_frame_json(e, rect_json("Square 50", 500, 500), &d.frame_a)) != 0 ||
            (rc = add_frame_json(e, rect_json("Wide 60", 600, 400), &d.frame_b)) != 0 ||
            (rc = add_frame_3d(e, &d.frame_3d, true)) != 0)
            break;
        if ((rc = add_layout(e, d.frame_a, "Circle 200", ring_2d(kMultiNailsCircle * k, 200), &d.layout_a1)) != 0 ||
            (rc = add_layout(e, d.frame_a, "Heart 190", heart_2d(kMultiNailsHeart * k, 380), &d.layout_a2)) != 0 ||
            (rc = add_layout(e, d.frame_b, "Ring 100", ring_2d(kMultiNailsRing * k, 100), &d.layout_b)) != 0 ||
            (rc = add_layout(e, d.frame_b, "Ring 50", ring_2d(kMultiNailsSmall * k, 50), &d.layout_b_unused)) != 0 ||
            (rc = add_layout(e, d.frame_3d, "Top ring", ring_3d(kMultiNails3D * k), &d.layout_3d)) != 0)
            break;
        if ((rc = loomora_editor_layout_set_label(e, d.layout_a1, 0, "first")) != 0 ||
            (rc = loomora_editor_layout_set_label(e, d.layout_a2, 0, "cleft")) != 0)
            break;
        if ((rc = add_image_mode(e, "Portrait", "bw", "circle", 40, &d.image[0])) != 0 ||
            (rc = add_image_mode(e, "Landscape", "gray", "landscape", 80, &d.image[1])) != 0 ||
            (rc = add_image_mode(e, "Color", "color", "portrait", 120, &d.image[2])) != 0 ||
            (rc = add_image_mode(e, "Square", "color", "square", 160, &d.image[3])) != 0)
            break;
        std::vector<std::uint32_t> heart, circle, ring, cube, shared;
        heart.push_back(d.spool[0]);
        heart.push_back(d.spool[1]);
        circle.push_back(d.spool[1]);
        circle.push_back(d.spool[2]);
        circle.push_back(d.spool[3]);
        ring.push_back(d.spool[0]);
        ring.push_back(d.spool[3]);
        cube.push_back(d.spool[4]);
        cube.push_back(d.spool[5]);
        shared.push_back(d.spool[0]);
        shared.push_back(d.spool[2]);
        shared.push_back(d.spool[4]);
        shared.push_back(d.spool[5]);
        if ((rc = add_result_json(e, "Heart", d.frame_a, d.layout_a2, one_image(d.image[0]), heart,
                                  kMultiNailsHeart * k, &d.result_a2, kMultiStepsHeart * k, 1000)) != 0 ||
            (rc = add_result_json(e, "Circle", d.frame_a, d.layout_a1, one_image(d.image[1]), circle,
                                  kMultiNailsCircle * k, &d.result_a1, kMultiStepsCircle * k, 2000)) != 0 ||
            (rc = add_result_json(e, "Ring", d.frame_b, d.layout_b, one_image(d.image[0]), ring, kMultiNailsRing * k,
                                  &d.result_b, kMultiStepsRing * k, 1500)) != 0 ||
            (rc = add_result_json(e, "Cube", d.frame_3d, d.layout_3d, two_views(d.image[2], d.image[3]), cube,
                                  kMultiNails3D * k, &d.result_3d, kMultiSteps3D * k, 800)) != 0 ||
            (rc = add_result_json(e, "Circle again", d.frame_a, d.layout_a1, one_image(d.image[2]), shared,
                                  kMultiNailsCircle * k, &d.result_shared, kMultiStepsShared * k, 3000)) != 0)
            break;
    } while (false);
    if (rc != 0) {
        loomora_editor_close(e);
        *out = nullptr;
    }
    return rc;
}

} // namespace multi_project

#endif

// Builders for the tests of the light and subset export (spec 002, User Story 7), through the PUBLIC C API
// only. Header only, plain C++11. It extends multi_project.hpp without changing it:
//
//   `build_pixels` is `build_small` with images that carry pixels (source and processed PNGs of `side` x
//   `side` pixels, stored deflate so the size is that of the pixels), thumbnails and result thumbnails
//   that carry GPS and camera metadata (the editor strips them: SC-009), and a 3D frame whose mesh has
//   texture coordinates, a parametric construction and an attached copy of the imported file.
#ifndef LOOMORA_TESTS_EXPORT_PROJECT_HPP
#define LOOMORA_TESTS_EXPORT_PROJECT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "basic_project.hpp"
#include "multi_project.hpp"

namespace export_project {

using basic_project::Bytes;
using basic_project::num;
using multi_project::Ids;

// `png` with an `eXIf` chunk (GPS and camera text) and a `tEXt` chunk inserted before IEND.
inline Bytes with_metadata(const Bytes &png) {
    Bytes out(png.begin(), png.end() - 12); // everything before IEND (12 bytes)
    const char exif[] = "MM\0*GPSLatitude 48.8566 Camera Make ACME Model X1";
    basic_project::png_chunk(out, "eXIf", Bytes(exif, exif + sizeof exif - 1));
    const char text[] = "Software\0Camera Firmware 1.2 GPS";
    basic_project::png_chunk(out, "tEXt", Bytes(text, text + sizeof text - 1));
    out.insert(out.end(), png.end() - 12, png.end());
    return out;
}

inline int32_t add_image_pixels(loomora_editor *e, const char *name, const char *color_mode, const char *shape,
                                unsigned gray, unsigned side, std::uint32_t *id) {
    const Bytes source = with_metadata(basic_project::make_png(side, side, static_cast<std::uint8_t>(gray)));
    const Bytes target = with_metadata(basic_project::make_png(side, side, static_cast<std::uint8_t>(gray + 50)));
    const Bytes thumb = with_metadata(basic_project::make_png(4, 4, static_cast<std::uint8_t>(gray + 7)));
    const std::string json = std::string("{\"name\":\"") + name + "\",\"color_mode\":\"" + color_mode +
                             "\",\"shape\":\"" + shape + "\",\"width_px\":" + num(side) + ",\"height_px\":" +
                             num(side) + "}";
    return loomora_editor_add_image(e, json.c_str(), &source[0], source.size(), &target[0], target.size(), &thumb[0],
                                    thumb.size(), id);
}

// The 3D frame of the cube with UVs, a construction and an attached mesh file.
inline int32_t add_frame_3d_full(loomora_editor *e, std::uint32_t *id) {
    const std::string json =
        "{\"name\":\"Cube 100\",\"kind\":\"3d\",\"dimensions\":{\"width_mm\":100,\"height_mm\":100,\"depth_mm\":100}," +
        std::string("\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":100,") +
        "\"height_mm\":100},\"polyline\":[]},\"exclusions\":[],\"surface\":\"mesh\",\"body\":\"solid\"," +
        "\"mesh\":{\"watertight\":true,\"manifold\":true,\"source\":{\"name\":\"cube.stl\",\"sha256\":\"" +
        std::string(64, 'a') + "\"},\"construction\":{\"primitive\":\"box\",\"size\":[100,100,100]}}}";
    int32_t rc = loomora_editor_add_frame(e, json.c_str(), id);
    if (rc != 0)
        return rc;
    const multi_project::Cube cube;
    const loomora_mesh_info info = cube.info(true, true);
    rc = loomora_editor_frame_set_mesh(e, *id, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0], &cube.uvs[0]);
    if (rc != 0)
        return rc;
    const char stl[] = "solid cube\nendsolid cube\n";
    return loomora_editor_frame_set_attached_source(e, *id, "cube.stl",
                                                    reinterpret_cast<const std::uint8_t *>(stl), sizeof stl - 1);
}

// build_small with pixels (see the file comment): 3 frames, 5 layouts, 4 images, 6 spools, 5 results.
inline int32_t build_pixels(loomora_editor **out, Ids *ids, unsigned side = 96) {
    Ids local;
    Ids &d = ids != nullptr ? *ids : local;
    d = Ids();
    loomora_editor *e = nullptr;
    int32_t rc = basic_project::create(&e, "With pixels");
    if (rc != 0)
        return rc;
    *out = e;
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
        if ((rc = multi_project::add_frame_json(e, multi_project::rect_json("Square 50", 500, 500), &d.frame_a)) != 0 ||
            (rc = multi_project::add_frame_json(e, multi_project::rect_json("Wide 60", 600, 400), &d.frame_b)) != 0 ||
            (rc = add_frame_3d_full(e, &d.frame_3d)) != 0)
            break;
        if ((rc = multi_project::add_layout(e, d.frame_a, "Circle 200", multi_project::ring_2d(24, 200), &d.layout_a1)) != 0 ||
            (rc = multi_project::add_layout(e, d.frame_a, "Circle 150", multi_project::ring_2d(24, 150), &d.layout_a2)) != 0 ||
            (rc = multi_project::add_layout(e, d.frame_b, "Circle 100", multi_project::ring_2d(24, 100), &d.layout_b)) != 0 ||
            (rc = multi_project::add_layout(e, d.frame_b, "Circle 50", multi_project::ring_2d(24, 50), &d.layout_b_unused)) != 0 ||
            (rc = multi_project::add_layout(e, d.frame_3d, "Top ring", multi_project::ring_3d(12), &d.layout_3d)) != 0)
            break;
        if ((rc = add_image_pixels(e, "Portrait", "bw", "circle", 40, side, &d.image[0])) != 0 ||
            (rc = add_image_pixels(e, "Landscape", "gray", "landscape", 80, side, &d.image[1])) != 0 ||
            (rc = add_image_pixels(e, "Color", "color", "portrait", 120, side, &d.image[2])) != 0 ||
            (rc = add_image_pixels(e, "Square", "color", "square", 160, side, &d.image[3])) != 0)
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
        using multi_project::add_result_json;
        using multi_project::one_image;
        if ((rc = add_result_json(e, "A1", d.frame_a, d.layout_a1, one_image(d.image[0]), s12, 24, &d.result_a1)) != 0 ||
            (rc = add_result_json(e, "A2", d.frame_a, d.layout_a2, one_image(d.image[1]), s23, 24, &d.result_a2)) != 0 ||
            (rc = add_result_json(e, "B", d.frame_b, d.layout_b, one_image(d.image[0]), s14, 24, &d.result_b)) != 0 ||
            (rc = add_result_json(e, "Cube", d.frame_3d, d.layout_3d,
                                  multi_project::two_views(d.image[2], d.image[3]), s56, 12, &d.result_3d)) != 0 ||
            (rc = add_result_json(e, "Shared", d.frame_a, d.layout_a1, one_image(d.image[2]), s123, 24,
                                  &d.result_shared)) != 0)
            break;
        // The thumbnail of every result carries metadata too (the editor strips it).
        const std::uint32_t rs[5] = {d.result_a1, d.result_a2, d.result_b, d.result_3d, d.result_shared};
        const Bytes thumb = with_metadata(basic_project::make_png(4, 4, 200));
        for (int i = 0; i < 5 && rc == 0; ++i)
            rc = loomora_editor_replace_thumbnail(e, LOOMORA_KIND_RESULT, rs[i], &thumb[0], thumb.size());
    } while (false);
    if (rc != 0) {
        loomora_editor_close(e);
        *out = nullptr;
    }
    return rc;
}

} // namespace export_project

#endif

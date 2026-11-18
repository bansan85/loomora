// T045 / T054, User Story 3: the reference project `multi` (3 frames incl. a 3D cube, 5 layouts incl. a
// heart and a circle on one frame and a frame-and-layout combination no result uses, 4 images of mixed
// color mode and shape, 6 spools shared by 5 results with different selections and lengths, a result on
// the 3D frame with two views each tied to its own image), through the PUBLIC C API only: what the library
// reads back is compared with core/tests/format/multi.expected.json, which tools/gen_reference.py writes
// without any Loomora code (FMT_MULTI_EXPECTED, set by core/tests/CMakeLists.txt); the display order; the
// SC-011 sequence (save, open, remove one result, save again, every reference intact); a scaled copy.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "../multi_project.hpp"
#include "fs_test_util.hpp"

#ifndef FMT_MULTI_EXPECTED
#define FMT_MULTI_EXPECTED ""
#endif

using basic_project::Bytes;
using multi_project::Ids;

namespace {

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *out = static_cast<Bytes *>(user);
    out->insert(out->end(), data, data + len);
    return 0;
}

Bytes save_bytes(loomora_editor *e, const char *modified = basic_project::kModifiedAt, bool quiet = true) {
    Bytes out;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_stream(e, modified, 0, collect, &out, &report) == 0);
    if (report != nullptr) {
        CHECK((loomora_report_count(report) == 0 || !quiet)); // the writer has nothing to say about `multi`
        loomora_report_free(report);
    }
    return out;
}

// The memory is not copied by the library: the object keeps its own copy of the bytes.
struct Opened {
    Bytes data;
    loomora_project *p;
    explicit Opened(const Bytes &bytes, bool quiet = true) : data(bytes), p(nullptr) {
        loomora_report *report = nullptr;
        REQUIRE(loomora_project_open_memory(data.data(), data.size(), nullptr, nullptr, &p, &report) == 0);
        REQUIRE(p != nullptr);
        if (report != nullptr) {
            CHECK((loomora_report_count(report) == 0 || !quiet));
            CHECK(loomora_report_error_count(report) == 0);
            loomora_report_free(report);
        }
    }
    ~Opened() { loomora_project_close(p); }

  private:
    Opened(const Opened &);
    Opened &operator=(const Opened &);
};

struct Editor {
    loomora_editor *e;
    Ids ids;
    explicit Editor(unsigned scale = 1) : e(nullptr) {
        REQUIRE(multi_project::build_multi(&e, &ids, scale) == 0);
        REQUIRE(e != nullptr);
    }
    ~Editor() { loomora_editor_close(e); }

  private:
    Editor(const Editor &);
    Editor &operator=(const Editor &);
};

Bytes read_entry(const loomora_project *p, const std::string &entry) {
    const std::int64_t size = loomora_project_entry_size(p, entry.c_str());
    REQUIRE(size >= 0);
    Bytes out(static_cast<std::size_t>(size));
    if (size > 0)
        REQUIRE(loomora_project_read_entry(p, entry.c_str(), 0, &out[0], out.size()) == size);
    return out;
}

void parse(const Bytes &b, rapidjson::Document &d) {
    d.Parse(reinterpret_cast<const char *>(b.data()), b.size());
    REQUIRE_FALSE(d.HasParseError());
}

Bytes item_json(const loomora_project *p, int32_t kind, std::uint32_t id) {
    const std::int64_t n = loomora_project_item_json(p, kind, id, nullptr, 0);
    REQUIRE(n > 0);
    Bytes out(static_cast<std::size_t>(n));
    REQUIRE(loomora_project_item_json(p, kind, id, &out[0], out.size()) == n);
    return out;
}

std::vector<std::uint32_t> ids_of(const loomora_project *p, int32_t kind) {
    const std::int64_t n = loomora_project_count(p, kind);
    REQUIRE(n >= 0);
    std::vector<std::uint32_t> v(static_cast<std::size_t>(n) + 1);
    REQUIRE(loomora_project_ids(p, kind, &v[0], static_cast<std::size_t>(n)) == n);
    v.resize(static_cast<std::size_t>(n));
    return v;
}

std::vector<std::uint32_t> seq(std::uint32_t first, std::uint32_t last) {
    std::vector<std::uint32_t> v;
    for (std::uint32_t i = first; i <= last; ++i)
        v.push_back(i);
    return v;
}

std::vector<loomora_nail> read_nails(const loomora_project *p, std::uint32_t layout) {
    const std::int64_t n = loomora_layout_nail_count(p, layout);
    REQUIRE(n > 0);
    std::vector<loomora_nail> nails(static_cast<std::size_t>(n));
    REQUIRE(loomora_layout_read_nails(p, layout, 0, nails.size(), &nails[0]) == n);
    return nails;
}

std::vector<loomora_step> read_steps(const loomora_project *p, std::uint32_t result) {
    const std::int64_t n = loomora_result_step_count(p, result);
    REQUIRE(n > 0);
    std::vector<loomora_step> steps(static_cast<std::size_t>(n));
    REQUIRE(loomora_result_read_steps(p, result, 0, steps.size(), &steps[0]) == n);
    return steps;
}

// CRC-32 of the steps as 12-byte little-endian records, byte by byte (independent of the host order).
std::string steps_crc(const std::vector<loomora_step> &steps) {
    Bytes raw;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        for (int s = 0; s < 32; s += 8)
            raw.push_back(static_cast<std::uint8_t>(steps[i].nail >> s));
        for (int s = 0; s < 32; s += 8)
            raw.push_back(static_cast<std::uint8_t>(steps[i].spool_index >> s));
        raw.push_back(steps[i].rotation);
        raw.push_back(steps[i].type);
        raw.push_back(steps[i].level);
        raw.push_back(0);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(basic_project::crc32_of(&raw[0], raw.size())));
    return buf;
}

std::string label_of(const loomora_project *p, std::uint32_t layout, std::uint32_t nail) {
    const std::int64_t n = loomora_layout_nail_label(p, layout, nail, nullptr, 0);
    REQUIRE(n >= 0);
    std::string s(static_cast<std::size_t>(n), '\0');
    if (n > 0)
        REQUIRE(loomora_layout_nail_label(p, layout, nail, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
    return s;
}

void load_expected(rapidjson::Document &want) {
    REQUIRE(std::string(FMT_MULTI_EXPECTED).size() > 0);
    fstest::Bytes b;
    REQUIRE(fstest::read_file(FMT_MULTI_EXPECTED, b));
    parse(b, want);
    REQUIRE(want.IsObject());
}

std::string str(const rapidjson::Value &v, const char *key) {
    REQUIRE(v.HasMember(key));
    REQUIRE(v[key].IsString());
    return v[key].GetString();
}

std::uint32_t u32(const rapidjson::Value &v, const char *key) {
    REQUIRE(v.HasMember(key));
    REQUIRE(v[key].IsUint());
    return v[key].GetUint();
}

double dbl(const rapidjson::Value &v) {
    REQUIRE(v.IsNumber());
    return v.GetDouble();
}

} // namespace

TEST_CASE("multi: the library reads what the independent description says", "[multi][format]") {
    Editor ed;
    const Bytes bytes = save_bytes(ed.e);
    Opened o(bytes);
    rapidjson::Document want;
    load_expected(want);

    // Ids: the builder allocates 1..n in creation order.
    CHECK(ed.ids.frame_a == 1u);
    CHECK(ed.ids.layout_a2 == 2u);
    CHECK(ed.ids.result_shared == 5u);

    const rapidjson::Value &counts = want["counts"];
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_FRAME) == u32(counts, "frames"));
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_LAYOUT) == u32(counts, "layouts"));
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_IMAGE) == u32(counts, "images"));
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_SPOOL) == u32(counts, "spools"));
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_RESULT) == u32(counts, "results"));

    rapidjson::Document manifest;
    parse(read_entry(o.p, "manifest.json"), manifest);
    CHECK(str(manifest, "title") == str(want, "title"));
    // The required features follow the content: the 3D cube mesh and the result with two views.
    REQUIRE(manifest["required_features"].IsArray());
    REQUIRE(manifest["required_features"].Size() == want["required_features"].Size());
    for (rapidjson::SizeType i = 0; i < want["required_features"].Size(); ++i)
        CHECK(std::string(manifest["required_features"][i].GetString()) == want["required_features"][i].GetString());

    for (rapidjson::SizeType i = 0; i < want["spools"].Size(); ++i) {
        const rapidjson::Value &w = want["spools"][i];
        rapidjson::Document got;
        parse(item_json(o.p, LOOMORA_KIND_SPOOL, u32(w, "id")), got);
        INFO("spool " << u32(w, "id"));
        CHECK(got == w);
    }

    for (rapidjson::SizeType i = 0; i < want["frames"].Size(); ++i) {
        const rapidjson::Value &w = want["frames"][i];
        rapidjson::Document got;
        parse(item_json(o.p, LOOMORA_KIND_FRAME, u32(w, "id")), got);
        INFO("frame " << u32(w, "id"));
        CHECK(std::string(got["name"].GetString()) == str(w, "name"));
        CHECK(std::string(got["kind"].GetString()) == str(w, "kind"));
        CHECK(dbl(got["dimensions"]["width_mm"]) == dbl(w["width_mm"]));
        CHECK(dbl(got["dimensions"]["height_mm"]) == dbl(w["height_mm"]));
        if (w.HasMember("mesh")) {
            CHECK(dbl(got["dimensions"]["depth_mm"]) == dbl(w["depth_mm"]));
            loomora_mesh_info info = loomora_mesh_info();
            REQUIRE(loomora_frame_mesh_info(o.p, u32(w, "id"), &info) == 0);
            CHECK(info.vertices == u32(w["mesh"], "vertices"));
            CHECK(info.triangles == u32(w["mesh"], "triangles"));
            CHECK(((info.flags & LOOMORA_MESH_NORMALS) != 0) == w["mesh"]["normals"].GetBool());
            CHECK(((info.flags & LOOMORA_MESH_UVS) != 0) == w["mesh"]["uvs"].GetBool());
        } else {
            CHECK_FALSE(got.HasMember("mesh"));
        }
    }

    for (rapidjson::SizeType i = 0; i < want["layouts"].Size(); ++i) {
        const rapidjson::Value &w = want["layouts"][i];
        const std::uint32_t id = u32(w, "id");
        INFO("layout " << id);
        rapidjson::Document got;
        parse(item_json(o.p, LOOMORA_KIND_LAYOUT, id), got);
        CHECK(std::string(got["name"].GetString()) == str(w, "name"));
        CHECK(u32(got, "frame") == u32(w, "frame"));
        CHECK(u32(got, "nail_count") == u32(w, "nail_count"));
        const std::vector<loomora_nail> nails = read_nails(o.p, id);
        REQUIRE(nails.size() == w["nails"].Size());
        bool same = true;
        for (std::size_t k = 0; k < nails.size(); ++k) {
            const rapidjson::Value &xy = w["nails"][static_cast<rapidjson::SizeType>(k)];
            const bool is3d = xy.Size() == 3;
            same = same && nails[k].id == k && nails[k].kind == 1u && nails[k].x == dbl(xy[0u]) &&
                   nails[k].y == dbl(xy[1u]) && (!is3d || nails[k].z == dbl(xy[2u])) && ((nails[k].flags & 1u) != 0) == is3d;
        }
        CHECK(same);
        const bool labelled = w.HasMember("labels");
        CHECK(((nails[0].flags & 2u) != 0) == labelled);
        if (labelled)
            CHECK(label_of(o.p, id, 0) == str(w["labels"], "0"));
    }

    for (rapidjson::SizeType i = 0; i < want["images"].Size(); ++i) {
        const rapidjson::Value &w = want["images"][i];
        rapidjson::Document got;
        parse(item_json(o.p, LOOMORA_KIND_IMAGE, u32(w, "id")), got);
        INFO("image " << u32(w, "id"));
        CHECK(std::string(got["name"].GetString()) == str(w, "name"));
        CHECK(std::string(got["color_mode"].GetString()) == str(w, "color_mode"));
        CHECK(std::string(got["shape"].GetString()) == str(w, "shape"));
        CHECK(u32(got, "width_px") == u32(w, "width_px"));
    }

    for (rapidjson::SizeType i = 0; i < want["results"].Size(); ++i) {
        const rapidjson::Value &w = want["results"][i];
        const std::uint32_t id = u32(w, "id");
        INFO("result " << id);
        rapidjson::Document got;
        parse(item_json(o.p, LOOMORA_KIND_RESULT, id), got);
        CHECK(std::string(got["name"].GetString()) == str(w, "name"));
        CHECK(u32(got, "frame") == u32(w, "frame"));
        CHECK(u32(got, "layout") == u32(w, "layout"));
        CHECK(u32(got, "steps") == u32(w, "steps"));
        REQUIRE(got["images"].Size() == w["images"].Size());
        for (rapidjson::SizeType k = 0; k < w["images"].Size(); ++k) {
            CHECK(u32(got["images"][k], "image") == w["images"][k].GetUint());
            CHECK(got["images"][k].HasMember("view") == w["views"].GetBool());
        }
        REQUIRE(got["spools"].Size() == w["spools"].Size());
        for (rapidjson::SizeType k = 0; k < w["spools"].Size(); ++k) {
            CHECK(u32(got["spools"][k], "spool") == w["spools"][k][0u].GetUint());
            CHECK(dbl(got["spools"][k]["length_mm"]) == dbl(w["spools"][k][1u]));
        }
        const std::vector<loomora_step> steps = read_steps(o.p, id);
        REQUIRE(steps.size() == u32(w, "steps"));
        CHECK(steps_crc(steps) == str(w, "steps_crc32"));
        for (int part = 0; part < 2; ++part) {
            const rapidjson::Value &list = w[part == 0 ? "first_steps" : "last_steps"];
            const std::size_t base = part == 0 ? 0 : steps.size() - list.Size();
            for (rapidjson::SizeType k = 0; k < list.Size(); ++k) {
                const loomora_step &s = steps[base + k];
                CHECK(s.nail == list[k][0u].GetUint());
                CHECK(s.spool_index == list[k][1u].GetUint());
                CHECK(s.rotation == list[k][2u].GetUint());
                CHECK(s.type == list[k][3u].GetUint());
                CHECK(s.level == 0u);
            }
        }
    }

    // The combination of frame 2 and layout 4 has no result; every other layout has one.
    std::set<std::pair<std::uint32_t, std::uint32_t> > used;
    for (rapidjson::SizeType i = 0; i < want["results"].Size(); ++i)
        used.insert(std::make_pair(u32(want["results"][i], "frame"), u32(want["results"][i], "layout")));
    REQUIRE(want["combinations_without_result"].Size() == 1);
    const std::pair<std::uint32_t, std::uint32_t> free_pair(want["combinations_without_result"][0u][0u].GetUint(),
                                                          want["combinations_without_result"][0u][1u].GetUint());
    CHECK(used.count(free_pair) == 0);
    CHECK(free_pair.first == ed.ids.frame_b);
    CHECK(free_pair.second == ed.ids.layout_b_unused);
    CHECK(used.size() == 4);
}

TEST_CASE("multi: two layouts on one frame, a heart and a circle", "[multi][format]") {
    Editor ed;
    Opened o(save_bytes(ed.e));
    rapidjson::Document circle, heart;
    parse(item_json(o.p, LOOMORA_KIND_LAYOUT, ed.ids.layout_a1), circle);
    parse(item_json(o.p, LOOMORA_KIND_LAYOUT, ed.ids.layout_a2), heart);
    CHECK(u32(circle, "frame") == ed.ids.frame_a);
    CHECK(u32(heart, "frame") == ed.ids.frame_a);
    const std::vector<loomora_nail> c = read_nails(o.p, ed.ids.layout_a1);
    const std::vector<loomora_nail> h = read_nails(o.p, ed.ids.layout_a2);
    REQUIRE(c.size() == multi_project::kMultiNailsCircle);
    REQUIRE(h.size() == multi_project::kMultiNailsHeart);
    // The circle: every nail 200 mm from the centre; the heart: tall in y, asymmetric (tip down), symmetric in x.
    for (std::size_t i = 0; i < c.size(); ++i)
        CHECK(std::fabs(std::sqrt(c[i].x * c[i].x + c[i].y * c[i].y) - 200.0) < 0.001);
    double min_y = 1e9, max_y = -1e9;
    for (std::size_t i = 0; i < h.size(); ++i) {
        min_y = (std::min)(min_y, h[i].y);
        max_y = (std::max)(max_y, h[i].y);
    }
    CHECK(min_y < -190.0); // the tip
    CHECK((max_y > 100.0 && max_y < 150.0)); // the lobes
    CHECK(h[0].x == 0.0);  // the notch between the lobes
    CHECK(h[h.size() / 2].x == 0.0); // the tip
}

TEST_CASE("multi: a result on the 3D frame has several views, each tied to its own image", "[multi][format]") {
    Editor ed;
    Opened o(save_bytes(ed.e));
    rapidjson::Document r;
    parse(item_json(o.p, LOOMORA_KIND_RESULT, ed.ids.result_3d), r);
    CHECK(u32(r, "frame") == ed.ids.frame_3d);
    CHECK(u32(r, "layout") == ed.ids.layout_3d);
    REQUIRE(r["images"].Size() == 2);
    const std::uint32_t a = u32(r["images"][0u], "image"), b = u32(r["images"][1u], "image");
    CHECK(a == ed.ids.image[2]);
    CHECK(b == ed.ids.image[3]);
    CHECK(a != b);
    CHECK(std::string(r["images"][0u]["view"]["projection"].GetString()) == "orthographic");
    CHECK(std::string(r["images"][1u]["view"]["projection"].GetString()) == "perspective");
    CHECK(dbl(r["images"][0u]["view"]["size"]) == 200.0);
    CHECK(dbl(r["images"][1u]["view"]["fov_deg"]) == 40.0);
    CHECK(dbl(r["images"][1u]["view"]["weight"]) == 0.5);
    // The 2D results have no view.
    rapidjson::Document r2;
    parse(item_json(o.p, LOOMORA_KIND_RESULT, ed.ids.result_a1), r2);
    CHECK_FALSE(r2["images"][0u].HasMember("view"));
    // The cube mesh: 8 vertices, 12 triangles, normals and UVs, readable by range.
    loomora_mesh_info info = loomora_mesh_info();
    REQUIRE(loomora_frame_mesh_info(o.p, ed.ids.frame_3d, &info) == 0);
    CHECK(info.vertices == 8u);
    CHECK(info.triangles == 12u);
    CHECK(info.flags == (LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS));
    const multi_project::Cube cube;
    std::vector<float> xyz(24);
    REQUIRE(loomora_frame_read_vertices(o.p, ed.ids.frame_3d, 0, 8, &xyz[0]) == 8);
    CHECK(xyz == cube.xyz);
    std::vector<std::uint32_t> abc(36);
    REQUIRE(loomora_frame_read_triangles(o.p, ed.ids.frame_3d, 0, 12, &abc[0]) == 12);
    CHECK(abc == cube.abc);
}

TEST_CASE("multi: six spools shared by five results with their own selections and lengths", "[multi][format]") {
    Editor ed;
    Opened o(save_bytes(ed.e));
    std::map<std::uint32_t, unsigned> uses; // spool id -> number of results selecting it
    std::set<std::string> selections;
    const std::vector<std::uint32_t> results = ids_of(o.p, LOOMORA_KIND_RESULT);
    REQUIRE(results.size() == 5);
    for (std::size_t i = 0; i < results.size(); ++i) {
        rapidjson::Document r;
        parse(item_json(o.p, LOOMORA_KIND_RESULT, results[i]), r);
        std::string sel;
        for (rapidjson::SizeType k = 0; k < r["spools"].Size(); ++k) {
            ++uses[u32(r["spools"][k], "spool")];
            sel += std::to_string(u32(r["spools"][k], "spool")) + "@" +
                   std::to_string(static_cast<unsigned>(dbl(r["spools"][k]["length_mm"]))) + " ";
        }
        selections.insert(sel);
    }
    CHECK(uses.size() == 6);
    for (std::map<std::uint32_t, unsigned>::const_iterator it = uses.begin(); it != uses.end(); ++it)
        CHECK(it->second >= 2u); // every spool is shared
    CHECK(selections.size() == 5); // five different selections / lengths
}

TEST_CASE("multi: the display order is written and read back, per kind", "[multi][format]") {
    Editor ed;
    const Bytes first = save_bytes(ed.e);
    {
        Opened o(first);
        CHECK(ids_of(o.p, LOOMORA_KIND_FRAME) == seq(1, 3));
        CHECK(ids_of(o.p, LOOMORA_KIND_LAYOUT) == seq(1, 5));
        CHECK(ids_of(o.p, LOOMORA_KIND_IMAGE) == seq(1, 4));
        CHECK(ids_of(o.p, LOOMORA_KIND_SPOOL) == seq(1, 6));
        CHECK(ids_of(o.p, LOOMORA_KIND_RESULT) == seq(1, 5));
    }
    const std::uint32_t frames[3] = {3, 1, 2};
    const std::uint32_t layouts[5] = {5, 3, 1, 4, 2};
    const std::uint32_t images[4] = {4, 3, 2, 1};
    const std::uint32_t spools[6] = {6, 5, 4, 3, 2, 1};
    const std::uint32_t results[5] = {2, 4, 1, 5, 3};
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_FRAME, frames, 3) == 0);
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_LAYOUT, layouts, 5) == 0);
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_IMAGE, images, 4) == 0);
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_SPOOL, spools, 6) == 0);
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_RESULT, results, 5) == 0);
    // Not a permutation: refused, nothing changes.
    const std::uint32_t bad[5] = {1, 1, 2, 3, 4};
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_RESULT, bad, 5) != 0);
    CHECK(loomora_editor_set_order(ed.e, LOOMORA_KIND_RESULT, results, 4) != 0);
    const Bytes second = save_bytes(ed.e);
    Opened o(second);
    CHECK(ids_of(o.p, LOOMORA_KIND_FRAME) == std::vector<std::uint32_t>(frames, frames + 3));
    CHECK(ids_of(o.p, LOOMORA_KIND_LAYOUT) == std::vector<std::uint32_t>(layouts, layouts + 5));
    CHECK(ids_of(o.p, LOOMORA_KIND_IMAGE) == std::vector<std::uint32_t>(images, images + 4));
    CHECK(ids_of(o.p, LOOMORA_KIND_SPOOL) == std::vector<std::uint32_t>(spools, spools + 6));
    CHECK(ids_of(o.p, LOOMORA_KIND_RESULT) == std::vector<std::uint32_t>(results, results + 5));
    // The order is the only thing that changed: every item entry is the same as in the first file.
    Opened before(first);
    rapidjson::Document m;
    parse(read_entry(o.p, "manifest.json"), m);
    for (rapidjson::Value::ConstMemberIterator it = m["entries"].MemberBegin(); it != m["entries"].MemberEnd(); ++it) {
        const std::string name = it->name.GetString();
        if (name == "spools.json")
            continue; // the spools are listed in the order of the file
        INFO(name);
        CHECK(read_entry(o.p, name) == read_entry(before.p, name));
    }
}

TEST_CASE("multi: save, open, remove one result, save again, every reference stays intact (SC-011)", "[multi][format]") {
    fstest::ScratchDir dir("multi");
    const std::string path = dir.file("multi.loomora");
    Ids ids;
    {
        Editor ed;
        ids = ed.ids;
        REQUIRE(loomora_editor_save_path(ed.e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr) == 0);
    }
    fstest::Bytes original;
    REQUIRE(fstest::read_file(path, original));

    // Opening for edit and saving with no change gives the same bytes (copy-through, same time).
    {
        loomora_editor *e = nullptr;
        loomora_report *report = nullptr;
        REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, &report) == 0);
        if (report != nullptr)
            loomora_report_free(report);
        CHECK(save_bytes(e) == original);
        loomora_editor_close(e);
    }

    // Remove one result (it has no dependents), save again.
    loomora_editor *e = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, nullptr) == 0);
    CHECK(loomora_editor_dependents(e, LOOMORA_KIND_RESULT, ids.result_shared, LOOMORA_OP_REMOVE, nullptr, 0) == 0);
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, ids.result_shared, 0) == 0);
    const std::string path2 = dir.file("multi2.loomora");
    REQUIRE(loomora_editor_save_path(e, path2.c_str(), "2026-11-17T11:00:00Z", 0, nullptr, nullptr, nullptr) == 0);
    loomora_editor_close(e);

    fstest::Bytes second;
    REQUIRE(fstest::read_file(path2, second));
    Opened a(original);
    Opened b(second);
    CHECK(loomora_project_count(b.p, LOOMORA_KIND_RESULT) == 4);
    CHECK(ids_of(b.p, LOOMORA_KIND_RESULT) == seq(1, 4));
    CHECK(loomora_project_count(b.p, LOOMORA_KIND_FRAME) == 3);
    CHECK(loomora_project_count(b.p, LOOMORA_KIND_LAYOUT) == 5);
    CHECK(loomora_project_count(b.p, LOOMORA_KIND_IMAGE) == 4);
    CHECK(loomora_project_count(b.p, LOOMORA_KIND_SPOOL) == 6);

    // Every reference of every remaining result points at something that exists.
    const std::vector<std::uint32_t> frames = ids_of(b.p, LOOMORA_KIND_FRAME), layouts = ids_of(b.p, LOOMORA_KIND_LAYOUT),
                                     images = ids_of(b.p, LOOMORA_KIND_IMAGE), spools = ids_of(b.p, LOOMORA_KIND_SPOOL);
    const std::vector<std::uint32_t> results = ids_of(b.p, LOOMORA_KIND_RESULT);
    for (std::size_t i = 0; i < results.size(); ++i) {
        rapidjson::Document r;
        parse(item_json(b.p, LOOMORA_KIND_RESULT, results[i]), r);
        INFO("result " << results[i]);
        CHECK(std::count(frames.begin(), frames.end(), u32(r, "frame")) == 1);
        CHECK(std::count(layouts.begin(), layouts.end(), u32(r, "layout")) == 1);
        for (rapidjson::SizeType k = 0; k < r["images"].Size(); ++k)
            CHECK(std::count(images.begin(), images.end(), u32(r["images"][k], "image")) == 1);
        for (rapidjson::SizeType k = 0; k < r["spools"].Size(); ++k)
            CHECK(std::count(spools.begin(), spools.end(), u32(r["spools"][k], "spool")) == 1);
        CHECK(static_cast<std::int64_t>(u32(r, "steps")) == loomora_result_step_count(b.p, results[i]));
    }

    // Every entry but the manifest and the removed result is byte for byte what it was; the removed
    // result's entries are gone; the manifest says 4 results and never reuses the identifier 5.
    rapidjson::Document m1, m2;
    parse(read_entry(a.p, "manifest.json"), m1);
    parse(read_entry(b.p, "manifest.json"), m2);
    for (rapidjson::Value::ConstMemberIterator it = m1["entries"].MemberBegin(); it != m1["entries"].MemberEnd(); ++it) {
        const std::string name = it->name.GetString();
        INFO(name);
        if (name.compare(0, 10, "results/5/") == 0) {
            CHECK(loomora_project_entry_size(b.p, name.c_str()) < 0);
            CHECK_FALSE(m2["entries"].HasMember(name.c_str()));
        } else {
            CHECK(read_entry(b.p, name) == read_entry(a.p, name));
            REQUIRE(m2["entries"].HasMember(name.c_str()));
            CHECK(str(m2["entries"][name.c_str()], "sha256") == str(it->value, "sha256"));
        }
    }
    CHECK(m2["counts"]["results"].GetUint() == 4u);
    CHECK(m2["next_id"]["result"].GetUint() == 6u);
    CHECK(str(m2, "modified_at") == "2026-11-17T11:00:00Z");

    // The independent validator of the library reports nothing on the new file.
    loomora_report *report = nullptr;
    CHECK(loomora_validate_path(path2.c_str(), nullptr, nullptr, &report) == 0);
    if (report != nullptr) {
        CHECK(loomora_report_count(report) == 0);
        loomora_report_free(report);
    }
}

TEST_CASE("multi: a scaled copy multiplies the nails and the steps", "[multi][format]") {
    Editor ed(3);
    // Three times the nails put some heart nails closer than their diameters: a warning, not an error.
    Opened o(save_bytes(ed.e, basic_project::kModifiedAt, false), false);
    CHECK(loomora_layout_nail_count(o.p, ed.ids.layout_a1) == 3 * multi_project::kMultiNailsCircle);
    CHECK(loomora_layout_nail_count(o.p, ed.ids.layout_a2) == 3 * multi_project::kMultiNailsHeart);
    CHECK(loomora_layout_nail_count(o.p, ed.ids.layout_3d) == 3 * multi_project::kMultiNails3D);
    CHECK(loomora_result_step_count(o.p, ed.ids.result_a1) == 3 * multi_project::kMultiStepsCircle);
    CHECK(loomora_result_step_count(o.p, ed.ids.result_3d) == 3 * multi_project::kMultiSteps3D);
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_RESULT) == 5);
}

TEST_CASE("multi: two builds give the same bytes", "[multi][format]") {
    Editor one, two;
    CHECK(save_bytes(one.e) == save_bytes(two.e));
}

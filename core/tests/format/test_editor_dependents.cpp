// Editing a project (spec 002, T046): the dependency table and the "Which changes count" table of
// data-model.md, through every function that makes the change; `loomora_editor_dependents` against
// what the matching call removes; LOOMORA_ERR_HAS_DEPENDENTS with nothing changed; identifiers never
// reused; 3D frames, meshes, nails and views; opening a project for edit; copy-through (SC-013) and
// the save / open / remove / save round trip (SC-011). Everything goes through the public C API; the
// state of the editor is observed by saving it to memory and opening that with the reader.
#include <catch2/catch.hpp>
#include <rapidjson/document.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../../include/loomora.h"
#include "../../src/c_api_project_handles.hpp"
#include "../multi_project.hpp"
#include "format/io/memory_io.hpp"
#include "format/sha256.hpp"
#include "format/zip/zip_reader.hpp"
#include "fs_test_util.hpp"

#ifndef FMT_CORPUS_DIR
#define FMT_CORPUS_DIR ""
#endif

using namespace loomora::format;
using multi_project::Cube;
using multi_project::Ids;
using basic_project::Bytes;

namespace {

typedef std::pair<std::uint32_t, std::uint32_t> Pair; // (kind, id)

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *b = static_cast<Bytes *>(user);
    b->insert(b->end(), data, data + len);
    return 0;
}

Bytes save_memory(loomora_editor *e) {
    Bytes out;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, &report) == 0);
    delete report;
    return out;
}

// A project opened from bytes (kept alive with the object).
struct Proj {
    Bytes bytes;
    loomora_project *p;
    loomora_report *report;
    int32_t rc;
    explicit Proj(const Bytes &b) : bytes(b), p(nullptr), report(nullptr), rc(0) {
        rc = loomora_project_open_memory(bytes.empty() ? nullptr : &bytes[0], bytes.size(), nullptr, nullptr, &p,
                                         &report);
    }
    ~Proj() {
        loomora_project_close(p);
        loomora_report_free(report);
    }
    std::vector<std::uint32_t> ids(int32_t kind) const {
        const int64_t n = loomora_project_ids(p, kind, nullptr, 0);
        REQUIRE(n >= 0);
        std::vector<std::uint32_t> v(static_cast<std::size_t>(n) + 1);
        REQUIRE(loomora_project_ids(p, kind, &v[0], static_cast<std::size_t>(n)) == n);
        v.resize(static_cast<std::size_t>(n));
        return v;
    }
    std::string json(int32_t kind, std::uint32_t id) const {
        const int64_t n = loomora_project_item_json(p, kind, id, nullptr, 0);
        REQUIRE(n >= 0);
        std::string s(static_cast<std::size_t>(n), '\0');
        REQUIRE(loomora_project_item_json(p, kind, id, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
        return s;
    }
    std::string manifest() const {
        const int64_t n = loomora_project_manifest(p, nullptr, 0);
        REQUIRE(n >= 0);
        std::string s(static_cast<std::size_t>(n), '\0');
        REQUIRE(loomora_project_manifest(p, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
        return s;
    }
};

// What an editor holds, as the saved archive shows it.
struct Snap {
    Bytes bytes;
    std::vector<std::uint32_t> frames, layouts, images, spools, results;
};

Snap snapshot(loomora_editor *e) {
    Snap s;
    s.bytes = save_memory(e);
    Proj p(s.bytes);
    REQUIRE(p.rc == 0);
    s.frames = p.ids(LOOMORA_KIND_FRAME);
    s.layouts = p.ids(LOOMORA_KIND_LAYOUT);
    s.images = p.ids(LOOMORA_KIND_IMAGE);
    s.spools = p.ids(LOOMORA_KIND_SPOOL);
    s.results = p.ids(LOOMORA_KIND_RESULT);
    return s;
}

std::vector<Pair> dependents(const loomora_editor *e, int32_t kind, std::uint32_t id, int32_t op) {
    const int64_t n = loomora_editor_dependents(e, kind, id, op, nullptr, 0);
    REQUIRE(n >= 0);
    std::vector<std::uint32_t> flat(static_cast<std::size_t>(2 * n) + 2);
    REQUIRE(loomora_editor_dependents(e, kind, id, op, &flat[0], static_cast<std::size_t>(n)) == n);
    std::vector<Pair> out;
    for (int64_t i = 0; i < n; ++i)
        out.push_back(Pair(flat[static_cast<std::size_t>(2 * i)], flat[static_cast<std::size_t>(2 * i + 1)]));
    return out;
}

std::vector<std::uint32_t> without(std::vector<std::uint32_t> v, const std::vector<Pair> &gone, std::uint32_t kind) {
    for (std::size_t i = 0; i < gone.size(); ++i)
        if (gone[i].first == kind)
            v.erase(std::remove(v.begin(), v.end(), gone[i].second), v.end());
    return v;
}

Pair layout_of(std::uint32_t id) { return Pair(LOOMORA_KIND_LAYOUT, id); }
Pair result_of(std::uint32_t id) { return Pair(LOOMORA_KIND_RESULT, id); }

std::string subst(std::string text, const std::string &from, const std::string &to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

typedef std::function<std::uint32_t(const Ids &)> Selector;
typedef std::function<int32_t(loomora_editor *, const Ids &, int32_t confirm)> Action;

// A change that REMOVES dependents. `removes_item` is true for remove_item (the item goes too). The
// dependents listed (LOOMORA_OP_REMOVE or LOOMORA_OP_CHANGE as `op`) must be `expected`; without the
// confirmation the call fails with HAS_DEPENDENTS and the archive is byte-for-byte the same; with it the
// item is changed and exactly the dependents are gone.
void check_removal(int32_t kind, const Selector &item, int32_t op,
                   const std::function<std::vector<Pair>(const Ids &)> &expected, const Action &act,
                   bool removes_item) {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    const Snap before = snapshot(e);
    const std::uint32_t id = item(ids);
    const std::vector<Pair> want = expected(ids);
    REQUIRE(!want.empty());
    CHECK(dependents(e, kind, id, op) == want);

    CHECK(act(e, ids, 0) == LOOMORA_ERR_HAS_DEPENDENTS);
    const Snap refused = snapshot(e);
    CHECK(refused.bytes == before.bytes); // nothing changed

    REQUIRE(act(e, ids, 1) == 0);
    const Snap after = snapshot(e);
    std::vector<std::uint32_t> layouts = without(before.layouts, want, LOOMORA_KIND_LAYOUT);
    std::vector<std::uint32_t> results = without(before.results, want, LOOMORA_KIND_RESULT);
    std::vector<std::uint32_t> frames = before.frames, images = before.images, spools = before.spools;
    if (removes_item) {
        std::vector<std::uint32_t> *list = nullptr;
        switch (kind) {
        case LOOMORA_KIND_FRAME: list = &frames; break;
        case LOOMORA_KIND_LAYOUT: list = &layouts; break;
        case LOOMORA_KIND_IMAGE: list = &images; break;
        case LOOMORA_KIND_SPOOL: list = &spools; break;
        default: list = &results; break;
        }
        list->erase(std::remove(list->begin(), list->end(), id), list->end());
    }
    CHECK(after.frames == frames);
    CHECK(after.layouts == layouts);
    CHECK(after.images == images);
    CHECK(after.spools == spools);
    CHECK(after.results == results);
    // What remains still opens without an error: no reference is left dangling.
    Proj p(after.bytes);
    CHECK(p.rc == 0);
    loomora_editor_close(e);
}

// A change that removes NOTHING: it succeeds without a confirmation and no layout or result goes.
// `check` looks at the saved archive for the effect of the change.
void check_harmless(const Action &act, const std::function<void(const Proj &, const Ids &)> &check) {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    const Snap before = snapshot(e);
    REQUIRE(act(e, ids, 0) == 0);
    const Snap after = snapshot(e);
    CHECK(after.frames == before.frames);
    CHECK(after.layouts == before.layouts);
    CHECK(after.images == before.images);
    CHECK(after.spools == before.spools);
    CHECK(after.results == before.results);
    Proj p(after.bytes);
    CHECK(p.rc == 0);
    if (check)
        check(p, ids);
    loomora_editor_close(e);
}

bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

Action replace_item(int32_t kind, const Selector &item, const std::function<std::string(const Ids &)> &json) {
    return [=](loomora_editor *e, const Ids &ids, int32_t confirm) {
        return loomora_editor_replace_item(e, kind, item(ids), json(ids).c_str(), confirm);
    };
}

// The expected dependents of the single items of build_small.
std::vector<Pair> results_of_frame_a(const Ids &d) {
    return std::vector<Pair>{result_of(d.result_a1), result_of(d.result_a2), result_of(d.result_shared)};
}
std::vector<Pair> results_of_layout_a1(const Ids &d) {
    return std::vector<Pair>{result_of(d.result_a1), result_of(d.result_shared)};
}
std::vector<Pair> results_of_image0(const Ids &d) { return std::vector<Pair>{result_of(d.result_a1), result_of(d.result_b)}; }
std::vector<Pair> results_of_spool0(const Ids &d) {
    return std::vector<Pair>{result_of(d.result_a1), result_of(d.result_b), result_of(d.result_shared)};
}

std::uint32_t pick_frame_a(const Ids &d) { return d.frame_a; }
std::uint32_t pick_frame_3d(const Ids &d) { return d.frame_3d; }
std::uint32_t pick_layout_a1(const Ids &d) { return d.layout_a1; }
std::uint32_t pick_layout_3d(const Ids &d) { return d.layout_3d; }
std::uint32_t pick_image0(const Ids &d) { return d.image[0]; }
std::uint32_t pick_spool0(const Ids &d) { return d.spool[0]; }

} // namespace

// ---- the dependency table, row by row ----------------------------------------------------------

TEST_CASE("frame removed: its layouts and their results go", "[format][editor][dependents]") {
    std::function<std::vector<Pair>(const Ids &)> expected = [](const Ids &d) {
        return std::vector<Pair>{layout_of(d.layout_a1), layout_of(d.layout_a2), result_of(d.result_a1),
                                 result_of(d.result_a2), result_of(d.result_shared)};
    };
    check_removal(LOOMORA_KIND_FRAME, pick_frame_a, LOOMORA_OP_REMOVE, expected,
                  [](loomora_editor *e, const Ids &d, int32_t c) {
                      return loomora_editor_remove_item(e, LOOMORA_KIND_FRAME, d.frame_a, c);
                  },
                  true);
}

TEST_CASE("frame geometry changed through replace_item removes the results of the frame, keeps the layouts",
          "[format][editor][dependents]") {
    const std::string base = multi_project::rect_json("Square 50", 500, 500);
    std::vector<std::string> changed;
    // outline
    changed.push_back(subst(base, "[[-250,-250],[250,-250],[250,250],[-250,250]]",
                            "[[-250,-250],[250,-250],[250,250],[-240,250]]"));
    // outline params
    changed.push_back(subst(base, "\"params\":{\"width_mm\":500,\"height_mm\":500}", "\"params\":{\"width_mm\":501,\"height_mm\":500}"));
    // exclusions
    changed.push_back(subst(base, "\"exclusions\":[]",
                            "\"exclusions\":[{\"type\":\"circle\",\"params\":{\"radius_mm\":20},"
                            "\"polyline\":[[-20,0],[0,20],[20,0],[0,-20]]}]"));
    // dimensions
    changed.push_back(subst(base, "\"dimensions\":{\"width_mm\":500", "\"dimensions\":{\"width_mm\":520"));
    changed.push_back(subst(base, "\"height_mm\":500},\"background\"", "\"height_mm\":510},\"background\""));
    for (std::size_t i = 0; i < changed.size(); ++i) {
        INFO("case " << i);
        const std::string json = changed[i];
        check_removal(LOOMORA_KIND_FRAME, pick_frame_a, LOOMORA_OP_CHANGE, results_of_frame_a,
                      replace_item(LOOMORA_KIND_FRAME, pick_frame_a, [json](const Ids &) { return json; }), false);
    }
}

TEST_CASE("3D frame surface, body and mesh changes remove the results of the frame", "[format][editor][dependents]") {
    check_removal(LOOMORA_KIND_FRAME, pick_frame_3d, LOOMORA_OP_CHANGE, [](const Ids &d) { return std::vector<Pair>{result_of(d.result_3d)}; },
                  replace_item(LOOMORA_KIND_FRAME, pick_frame_3d,
                               [](const Ids &) { return multi_project::frame3d_json("Cube 100", "primitive", "solid"); }),
                  false);
    check_removal(LOOMORA_KIND_FRAME, pick_frame_3d, LOOMORA_OP_CHANGE, [](const Ids &d) { return std::vector<Pair>{result_of(d.result_3d)}; },
                  replace_item(LOOMORA_KIND_FRAME, pick_frame_3d,
                               [](const Ids &) { return multi_project::frame3d_json("Cube 100", "mesh", "shell"); }),
                  false);
    // the mesh: positions, then the UVs, then the normals
    const Cube moved(60.0f);
    const Cube plain;
    Action positions = [moved](loomora_editor *e, const Ids &d, int32_t c) {
        const loomora_mesh_info info = moved.info(true, true);
        return loomora_editor_replace_frame_mesh(e, d.frame_3d, &info, &moved.xyz[0], &moved.abc[0], &moved.normals[0],
                                                 &moved.uvs[0], c);
    };
    Action no_uvs = [plain](loomora_editor *e, const Ids &d, int32_t c) {
        const loomora_mesh_info info = plain.info(true, false);
        return loomora_editor_replace_frame_mesh(e, d.frame_3d, &info, &plain.xyz[0], &plain.abc[0], &plain.normals[0],
                                                 nullptr, c);
    };
    Action no_normals = [plain](loomora_editor *e, const Ids &d, int32_t c) {
        const loomora_mesh_info info = plain.info(false, true);
        return loomora_editor_replace_frame_mesh(e, d.frame_3d, &info, &plain.xyz[0], &plain.abc[0], nullptr,
                                                 &plain.uvs[0], c);
    };
    Action indices = [plain](loomora_editor *e, const Ids &d, int32_t c) {
        Cube other;
        std::swap(other.abc[0], other.abc[1]);
        const loomora_mesh_info info = other.info(true, true);
        return loomora_editor_replace_frame_mesh(e, d.frame_3d, &info, &other.xyz[0], &other.abc[0], &other.normals[0],
                                                 &other.uvs[0], c);
    };
    const Action all[4] = {positions, no_uvs, no_normals, indices};
    for (int i = 0; i < 4; ++i) {
        INFO("mesh case " << i);
        check_removal(LOOMORA_KIND_FRAME, pick_frame_3d, LOOMORA_OP_CHANGE, [](const Ids &d) { return std::vector<Pair>{result_of(d.result_3d)}; }, all[i], false);
    }
}

TEST_CASE("the same mesh replaced again changes nothing", "[format][editor][dependents]") {
    const Cube cube;
    check_harmless(
        [&cube](loomora_editor *e, const Ids &d, int32_t c) {
            const loomora_mesh_info info = cube.info(true, true);
            return loomora_editor_replace_frame_mesh(e, d.frame_3d, &info, &cube.xyz[0], &cube.abc[0],
                                                     &cube.normals[0], &cube.uvs[0], c);
        },
        nullptr);
}

TEST_CASE("frame renamed or background changed removes nothing", "[format][editor][dependents]") {
    check_harmless(replace_item(LOOMORA_KIND_FRAME, pick_frame_a,
                                [](const Ids &) { return multi_project::rect_json("Renamed", 500, 500, "#00ff00"); }),
                   [](const Proj &p, const Ids &d) {
                       const std::string j = p.json(LOOMORA_KIND_FRAME, d.frame_a);
                       CHECK(has(j, "\"name\":\"Renamed\""));
                       CHECK(has(j, "\"background\":\"#00ff00\""));
                   });
    check_harmless(replace_item(LOOMORA_KIND_FRAME, pick_frame_3d,
                                [](const Ids &) { return multi_project::frame3d_json("Cube", "mesh", "solid", "#102030"); }),
                   [](const Proj &p, const Ids &d) {
                       CHECK(has(p.json(LOOMORA_KIND_FRAME, d.frame_3d), "\"background\":\"#102030\""));
                   });
}

TEST_CASE("layout removed: the results that use it go", "[format][editor][dependents]") {
    check_removal(LOOMORA_KIND_LAYOUT, pick_layout_a1, LOOMORA_OP_REMOVE, results_of_layout_a1,
                  [](loomora_editor *e, const Ids &d, int32_t c) {
                      return loomora_editor_remove_item(e, LOOMORA_KIND_LAYOUT, d.layout_a1, c);
                  },
                  true);
}

TEST_CASE("layout nails changed through replace_layout_nails remove the results of the layout",
          "[format][editor][dependents]") {
    typedef std::function<void(std::vector<loomora_nail> &)> Edit;
    std::vector<Edit> edits;
    edits.push_back([](std::vector<loomora_nail> &n) { n[3].x += 1; });          // a nail moved
    edits.push_back([](std::vector<loomora_nail> &n) { n[3].diameter = 3; });    // diameter
    edits.push_back([](std::vector<loomora_nail> &n) { n[3].kind = 3; });        // kind
    edits.push_back([](std::vector<loomora_nail> &n) {                           // a nail added
        loomora_nail extra = loomora_nail();
        extra.id = 1000;
        extra.kind = 3;
        extra.x = 10;
        extra.y = 10;
        extra.diameter = 2;
        n.push_back(extra);
    });
    edits.push_back([](std::vector<loomora_nail> &n) { n.pop_back(); });         // a nail removed
    for (std::size_t i = 0; i < edits.size(); ++i) {
        INFO("edit " << i);
        const Edit edit = edits[i];
        check_removal(LOOMORA_KIND_LAYOUT, pick_layout_a1, LOOMORA_OP_CHANGE, results_of_layout_a1,
                      [edit](loomora_editor *e, const Ids &d, int32_t c) {
                          std::vector<loomora_nail> n = multi_project::ring_2d(multi_project::kNails2D, 200);
                          edit(n);
                          return loomora_editor_replace_layout_nails(e, d.layout_a1, &n[0], n.size(), c);
                      },
                      false);
    }
}

TEST_CASE("3D nail fields count as a change", "[format][editor][dependents]") {
    typedef std::function<void(std::vector<loomora_nail> &)> Edit;
    std::vector<Edit> edits;
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].z = 49; });
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].length = 6; });
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].side = 1; });
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].face_triangle = 11; });
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].face_u = 0.5f; });
    edits.push_back([](std::vector<loomora_nail> &n) { n[2].axis[0] = 1.0f; n[2].axis[2] = 0.0f; });
    for (std::size_t i = 0; i < edits.size(); ++i) {
        INFO("edit " << i);
        const Edit edit = edits[i];
        check_removal(LOOMORA_KIND_LAYOUT, pick_layout_3d, LOOMORA_OP_CHANGE, [](const Ids &d) { return std::vector<Pair>{result_of(d.result_3d)}; },
                      [edit](loomora_editor *e, const Ids &d, int32_t c) {
                          std::vector<loomora_nail> n = multi_project::ring_3d(multi_project::kNails3D);
                          edit(n);
                          return loomora_editor_replace_layout_nails(e, d.layout_3d, &n[0], n.size(), c);
                      },
                      false);
    }
}

TEST_CASE("nail labels and layout names never remove anything", "[format][editor][dependents]") {
    // a label set, changed and removed
    check_harmless(
        [](loomora_editor *e, const Ids &d, int32_t) {
            int32_t rc = loomora_editor_layout_set_label(e, d.layout_a1, 5, "five");
            if (rc == 0)
                rc = loomora_editor_layout_set_label(e, d.layout_a1, 0, "changed");
            if (rc == 0)
                rc = loomora_editor_layout_set_label(e, d.layout_a1, 5, nullptr);
            return rc;
        },
        [](const Proj &p, const Ids &d) {
            std::uint8_t buf[16];
            CHECK(loomora_layout_nail_label(p.p, d.layout_a1, 0, buf, sizeof buf) == 7);
            CHECK(loomora_layout_nail_label(p.p, d.layout_a1, 5, buf, sizeof buf) == 0);
        });
    // a layout renamed
    check_harmless(replace_item(LOOMORA_KIND_LAYOUT, pick_layout_a1,
                                [](const Ids &) { return std::string("{\"name\":\"Renamed layout\"}"); }),
                   [](const Proj &p, const Ids &d) {
                       CHECK(has(p.json(LOOMORA_KIND_LAYOUT, d.layout_a1), "\"name\":\"Renamed layout\""));
                       CHECK(has(p.json(LOOMORA_KIND_LAYOUT, d.layout_a1), "\"nail_count\":24"));
                   });
    // the same nails again, in another order: nothing changes, the labels stay
    check_harmless(
        [](loomora_editor *e, const Ids &d, int32_t c) {
            std::vector<loomora_nail> n = multi_project::ring_2d(multi_project::kNails2D, 200);
            std::reverse(n.begin(), n.end());
            return loomora_editor_replace_layout_nails(e, d.layout_a1, &n[0], n.size(), c);
        },
        [](const Proj &p, const Ids &d) {
            std::uint8_t buf[16];
            CHECK(loomora_layout_nail_label(p.p, d.layout_a1, 0, buf, sizeof buf) == 5); // "first" survived
            loomora_nail first = loomora_nail();
            CHECK(loomora_layout_read_nails(p.p, d.layout_a1, 0, 1, &first) == 1);
            CHECK(first.id == multi_project::kNails2D - 1); // the new order is kept
        });
}

TEST_CASE("image removed: the results that use it go", "[format][editor][dependents]") {
    check_removal(LOOMORA_KIND_IMAGE, pick_image0, LOOMORA_OP_REMOVE, results_of_image0,
                  [](loomora_editor *e, const Ids &d, int32_t c) {
                      return loomora_editor_remove_item(e, LOOMORA_KIND_IMAGE, d.image[0], c);
                  },
                  true);
}

TEST_CASE("image pixels, color mode, shape, dimensions changed remove the results that use it",
          "[format][editor][dependents]") {
    const Bytes other_source = basic_project::make_png(8, 8, 200);
    const Bytes other_target = basic_project::make_png(8, 8, 210);
    const char *base = "{\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,\"height_px\":8}";
    std::vector<Action> acts;
    // new source pixels (replace_image)
    acts.push_back([&](loomora_editor *e, const Ids &d, int32_t c) {
        return loomora_editor_replace_image(e, d.image[0], base, &other_source[0], other_source.size(), nullptr, 0,
                                            nullptr, 0, c);
    });
    // new target pixels
    acts.push_back([&](loomora_editor *e, const Ids &d, int32_t c) {
        return loomora_editor_replace_image(e, d.image[0], base, nullptr, 0, &other_target[0], other_target.size(),
                                            nullptr, 0, c);
    });
    // color mode, shape, width, height through replace_item and through replace_image
    const char *jsons[4] = {"{\"name\":\"Portrait\",\"color_mode\":\"gray\",\"shape\":\"circle\",\"width_px\":8,\"height_px\":8}",
                            "{\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"square\",\"width_px\":8,\"height_px\":8}",
                            "{\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":9,\"height_px\":8}",
                            "{\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,\"height_px\":9}"};
    for (int i = 0; i < 4; ++i) {
        const std::string j = jsons[i];
        acts.push_back(replace_item(LOOMORA_KIND_IMAGE, pick_image0, [j](const Ids &) { return j; }));
        acts.push_back([j](loomora_editor *e, const Ids &d, int32_t c) {
            return loomora_editor_replace_image(e, d.image[0], j.c_str(), nullptr, 0, nullptr, 0, nullptr, 0, c);
        });
    }
    for (std::size_t i = 0; i < acts.size(); ++i) {
        INFO("action " << i);
        check_removal(LOOMORA_KIND_IMAGE, pick_image0, LOOMORA_OP_CHANGE, results_of_image0,
                      acts[i], false);
    }
}

TEST_CASE("image renamed or thumbnail replaced removes nothing", "[format][editor][dependents]") {
    const Bytes thumb = basic_project::make_png(5, 5, 99);
    const Bytes same_source = basic_project::make_png(8, 8, 40); // the pixels build_small gave image 0
    check_harmless(replace_item(LOOMORA_KIND_IMAGE, pick_image0,
                                [](const Ids &) {
                                    return std::string("{\"name\":\"New name\",\"color_mode\":\"bw\",\"shape\":\"circle\","
                                                       "\"width_px\":8,\"height_px\":8}");
                                }),
                   [](const Proj &p, const Ids &d) { CHECK(has(p.json(LOOMORA_KIND_IMAGE, d.image[0]), "New name")); });
    check_harmless(
        [&](loomora_editor *e, const Ids &d, int32_t) {
            return loomora_editor_replace_thumbnail(e, LOOMORA_KIND_IMAGE, d.image[0], &thumb[0], thumb.size());
        },
        [&](const Proj &p, const Ids &d) {
            std::string entry = "images/" + basic_project::num(d.image[0]) + "/thumb.png";
            Bytes got(thumb.size() + 8);
            CHECK(loomora_project_read_entry(p.p, entry.c_str(), 0, &got[0], got.size()) ==
                  static_cast<int64_t>(thumb.size()));
            got.resize(thumb.size());
            CHECK(got == thumb);
        });
    // replace_image with a new name and thumbnail, and the very same source pixels again
    check_harmless(
        [&](loomora_editor *e, const Ids &d, int32_t c) {
            return loomora_editor_replace_image(
                e, d.image[0],
                "{\"name\":\"Both\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,\"height_px\":8}",
                &same_source[0], same_source.size(), nullptr, 0, &thumb[0], thumb.size(), c);
        },
        [](const Proj &p, const Ids &d) { CHECK(has(p.json(LOOMORA_KIND_IMAGE, d.image[0]), "\"name\":\"Both\"")); });
    // the thumbnail of a result
    check_harmless(
        [&](loomora_editor *e, const Ids &d, int32_t) {
            return loomora_editor_replace_thumbnail(e, LOOMORA_KIND_RESULT, d.result_a1, &thumb[0], thumb.size());
        },
        nullptr);
}

TEST_CASE("spool removed, or color, opacity, diameter changed: the results that select it go",
          "[format][editor][dependents]") {
    check_removal(LOOMORA_KIND_SPOOL, pick_spool0, LOOMORA_OP_REMOVE, results_of_spool0,
                  [](loomora_editor *e, const Ids &d, int32_t c) {
                      return loomora_editor_remove_item(e, LOOMORA_KIND_SPOOL, d.spool[0], c);
                  },
                  true);
    const char *jsons[3] = {
        "{\"name\":\"Spool 1\",\"color\":\"#010101\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}",
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":0.5,\"diameter_mm\":0.3,\"available_length_mm\":200000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}",
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.4,\"available_length_mm\":200000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}"};
    for (int i = 0; i < 3; ++i) {
        INFO("field " << i);
        const std::string j = jsons[i];
        check_removal(LOOMORA_KIND_SPOOL, pick_spool0, LOOMORA_OP_CHANGE, results_of_spool0,
                      replace_item(LOOMORA_KIND_SPOOL, pick_spool0, [j](const Ids &) { return j; }), false);
    }
}

TEST_CASE("spool name, available length, material, domain changed remove nothing", "[format][editor][dependents]") {
    const char *jsons[4] = {
        "{\"name\":\"Renamed\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}",
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":300000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}",
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,"
        "\"material\":\"linen\",\"domain\":\"exterior\"}",
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,"
        "\"material\":\"cotton\",\"domain\":\"both\"}"};
    const char *seen[4] = {"\"name\":\"Renamed\"", "\"available_length_mm\":300000", "\"material\":\"linen\"",
                           "\"domain\":\"both\""};
    for (int i = 0; i < 4; ++i) {
        INFO("field " << i);
        const std::string j = jsons[i], s = seen[i];
        check_harmless(replace_item(LOOMORA_KIND_SPOOL, pick_spool0, [j](const Ids &) { return j; }),
                       [s](const Proj &p, const Ids &d) { CHECK(has(p.json(LOOMORA_KIND_SPOOL, d.spool[0]), s)); });
    }
}

TEST_CASE("an available length below a chosen length is an error at the save, not at the edit",
          "[format][editor][dependents]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    const char *low =
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":500,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}";
    REQUIRE(loomora_editor_replace_item(e, LOOMORA_KIND_SPOOL, ids.spool[0], low, 0) == 0); // removes nothing
    Bytes out;
    loomora_report *report = nullptr;
    CHECK(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, &report) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(out.empty());
    CHECK(loomora_report_error_count(report) >= 1);
    delete report;
    // resolved: the application raises the length again
    const char *high =
        "{\"name\":\"Spool 1\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":5000,"
        "\"material\":\"cotton\",\"domain\":\"exterior\"}";
    REQUIRE(loomora_editor_replace_item(e, LOOMORA_KIND_SPOOL, ids.spool[0], high, 0) == 0);
    const Snap s = snapshot(e);
    CHECK(s.results.size() == 5);
    loomora_editor_close(e);
}

TEST_CASE("result removed or renamed: nothing else changes", "[format][editor][dependents]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    CHECK(dependents(e, LOOMORA_KIND_RESULT, ids.result_a1, LOOMORA_OP_REMOVE).empty());
    CHECK(dependents(e, LOOMORA_KIND_RESULT, ids.result_a1, LOOMORA_OP_CHANGE).empty());
    const Snap before = snapshot(e);
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, ids.result_b, 0) == 0);
    const Snap after = snapshot(e);
    CHECK(after.frames == before.frames);
    CHECK(after.layouts == before.layouts);
    CHECK(after.images == before.images);
    CHECK(after.spools == before.spools);
    CHECK(after.results == without(before.results, std::vector<Pair>(1, result_of(ids.result_b)), LOOMORA_KIND_RESULT));
    loomora_editor_close(e);

    check_harmless(
        [](loomora_editor *e2, const Ids &d, int32_t c) {
            // only the name differs from the result as it was added
            return loomora_editor_replace_item(
                e2, LOOMORA_KIND_RESULT, d.result_a1,
                (std::string("{\"name\":\"Renamed result\",\"created_at\":\"") + basic_project::kResultCreatedAt +
                 "\",\"frame\":" + basic_project::num(d.frame_a) + ",\"layout\":" + basic_project::num(d.layout_a1) +
                 ",\"images\":[{\"image\":" + basic_project::num(d.image[0]) + "}],\"spools\":[{\"spool\":" +
                 basic_project::num(d.spool[0]) + ",\"length_mm\":1000},{\"spool\":" + basic_project::num(d.spool[1]) +
                 ",\"length_mm\":1010}]}")
                    .c_str(),
                c);
        },
        [](const Proj &p, const Ids &d) {
            CHECK(has(p.json(LOOMORA_KIND_RESULT, d.result_a1), "\"name\":\"Renamed result\""));
        });
}

TEST_CASE("changes that are refused: a layout's frame, a result's references, a frame's kind",
          "[format][editor][dependents]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    const Snap before = snapshot(e);

    // a layout moved to another frame
    const std::string move = "{\"name\":\"Circle 200\",\"frame\":" + basic_project::num(ids.frame_b) + "}";
    CHECK(loomora_editor_replace_item(e, LOOMORA_KIND_LAYOUT, ids.layout_a1, move.c_str(), 1) == LOOMORA_ERR_INVALID_ITEM);
    // the same frame is accepted
    const std::string stay = "{\"name\":\"Circle 200\",\"frame\":" + basic_project::num(ids.frame_a) + "}";
    CHECK(loomora_editor_replace_item(e, LOOMORA_KIND_LAYOUT, ids.layout_a1, stay.c_str(), 0) == 0);

    // a frame's kind
    CHECK(loomora_editor_replace_item(e, LOOMORA_KIND_FRAME, ids.frame_a, multi_project::frame3d_json("Square 50", "primitive").c_str(),
                                      1) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_replace_item(e, LOOMORA_KIND_FRAME, ids.frame_3d, multi_project::rect_json("Cube", 100, 100).c_str(), 1) ==
          LOOMORA_ERR_INVALID_ITEM);

    // a result's frame, layout, images, spools
    const std::string head = std::string("{\"name\":\"A1\",\"created_at\":\"") + basic_project::kResultCreatedAt + "\",";
    const std::string f = "\"frame\":" + basic_project::num(ids.frame_a) + ",";
    const std::string l = "\"layout\":" + basic_project::num(ids.layout_a1) + ",";
    const std::string i0 = "\"images\":[{\"image\":" + basic_project::num(ids.image[0]) + "}],";
    const std::string s0 = "\"spools\":[{\"spool\":" + basic_project::num(ids.spool[0]) + ",\"length_mm\":1000},{\"spool\":" +
                           basic_project::num(ids.spool[1]) + ",\"length_mm\":1010}]}";
    std::vector<std::string> bad;
    bad.push_back(head + "\"frame\":" + basic_project::num(ids.frame_b) + "," + l + i0 + s0);
    bad.push_back(head + f + "\"layout\":" + basic_project::num(ids.layout_a2) + "," + i0 + s0);
    bad.push_back(head + f + l + "\"images\":[{\"image\":" + basic_project::num(ids.image[1]) + "}]," + s0);
    bad.push_back(head + f + l + i0 + "\"spools\":[{\"spool\":" + basic_project::num(ids.spool[0]) + ",\"length_mm\":1000}]}");
    bad.push_back(head + f + l + i0 + "\"spools\":[{\"spool\":" + basic_project::num(ids.spool[0]) +
                  ",\"length_mm\":1000},{\"spool\":" + basic_project::num(ids.spool[1]) + ",\"length_mm\":1011}]}");
    for (std::size_t k = 0; k < bad.size(); ++k) {
        INFO("result edit " << k);
        CHECK(loomora_editor_replace_item(e, LOOMORA_KIND_RESULT, ids.result_a1, bad[k].c_str(), 1) ==
              LOOMORA_ERR_INVALID_ITEM);
    }
    const Snap after = snapshot(e);
    CHECK(after.bytes == before.bytes); // the accepted "same frame" layout edit changes nothing either
    loomora_editor_close(e);
}

// ---- dependents() on its own ---------------------------------------------------------------------

TEST_CASE("dependents: REMOVE and CHANGE, sizes, errors", "[format][editor][dependents]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    // a frame: its layouts only come with REMOVE
    const std::vector<Pair> removal = dependents(e, LOOMORA_KIND_FRAME, ids.frame_a, LOOMORA_OP_REMOVE);
    const std::vector<Pair> change = dependents(e, LOOMORA_KIND_FRAME, ids.frame_a, LOOMORA_OP_CHANGE);
    CHECK(removal.size() == 5);
    CHECK(change == results_of_frame_a(ids));
    CHECK(removal[0] == layout_of(ids.layout_a1));
    // a frame with a layout and no result: only the layout
    const std::vector<Pair> unused = dependents(e, LOOMORA_KIND_FRAME, ids.frame_b, LOOMORA_OP_REMOVE);
    CHECK(unused == std::vector<Pair>{layout_of(ids.layout_b), layout_of(ids.layout_b_unused), result_of(ids.result_b)});
    CHECK(dependents(e, LOOMORA_KIND_LAYOUT, ids.layout_b_unused, LOOMORA_OP_REMOVE).empty());
    CHECK(dependents(e, LOOMORA_KIND_IMAGE, ids.image[2], LOOMORA_OP_CHANGE) ==
          (std::vector<Pair>{result_of(ids.result_3d), result_of(ids.result_shared)}));
    // sizes: the number needed comes back, and nothing is written when the buffer is too small
    std::uint32_t buf[4] = {99, 99, 99, 99};
    CHECK(loomora_editor_dependents(e, LOOMORA_KIND_FRAME, ids.frame_a, LOOMORA_OP_REMOVE, buf, 1) == 5);
    CHECK(buf[0] == 99);
    CHECK(loomora_editor_dependents(e, LOOMORA_KIND_FRAME, ids.frame_a, LOOMORA_OP_REMOVE, nullptr, 0) == 5);
    // errors
    CHECK(loomora_editor_dependents(e, LOOMORA_KIND_FRAME, 999, LOOMORA_OP_REMOVE, nullptr, 0) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_editor_dependents(e, 99, 1, LOOMORA_OP_REMOVE, nullptr, 0) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_editor_dependents(e, LOOMORA_KIND_FRAME, ids.frame_a, 7, nullptr, 0) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_dependents(nullptr, LOOMORA_KIND_FRAME, 1, LOOMORA_OP_REMOVE, nullptr, 0) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, 999, 1) == LOOMORA_ERR_NOT_FOUND);
    loomora_editor_close(e);
}

// ---- identifiers, order, title ---------------------------------------------------------------------

TEST_CASE("an identifier removed is never reused, also after a save and a reopen", "[format][editor][dependents]") {
    fstest::ScratchDir dir("ids");
    const std::string path = dir.file("ids.loomora");
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    // remove the last result and the last image, then add new ones
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, ids.result_shared, 0) == 0);
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_IMAGE, ids.image[3], 1) == 0); // takes the 3D result with it
    std::uint32_t image = 0, result = 0;
    REQUIRE(multi_project::add_image_mode(e, "New", "bw", "free", 10, &image) == 0);
    CHECK(image > ids.image[3]);
    std::vector<std::uint32_t> sp(1, ids.spool[0]);
    REQUIRE(multi_project::add_result_json(e, "New", ids.frame_a, ids.layout_a1, multi_project::one_image(image), sp,
                                           multi_project::kNails2D, &result) == 0);
    CHECK(result > ids.result_shared);
    REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr) == 0);
    loomora_editor_close(e);

    // reopen for edit: the counters come from the manifest, not from the highest identifier left
    loomora_editor *again = nullptr;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &again, &report) == 0);
    loomora_report_free(report);
    REQUIRE(loomora_editor_remove_item(again, LOOMORA_KIND_RESULT, result, 0) == 0);
    std::uint32_t result2 = 0;
    REQUIRE(multi_project::add_result_json(again, "Newer", ids.frame_a, ids.layout_a1, multi_project::one_image(image),
                                           sp, multi_project::kNails2D, &result2) == 0);
    CHECK(result2 > result);
    // a layout, a frame and a spool as well
    REQUIRE(loomora_editor_remove_item(again, LOOMORA_KIND_LAYOUT, ids.layout_b_unused, 0) == 0);
    std::uint32_t layout = 0;
    REQUIRE(multi_project::add_layout(again, ids.frame_b, "Again", multi_project::ring_2d(8, 100), &layout) == 0);
    CHECK(layout > ids.layout_3d);
    loomora_editor_close(again);
}

TEST_CASE("set_order and set_title", "[format][editor][dependents]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    std::vector<std::uint32_t> order(ids.spool, ids.spool + 6);
    std::reverse(order.begin(), order.end());
    REQUIRE(loomora_editor_set_order(e, LOOMORA_KIND_SPOOL, &order[0], order.size()) == 0);
    std::vector<std::uint32_t> results;
    results.push_back(ids.result_shared);
    results.push_back(ids.result_3d);
    results.push_back(ids.result_b);
    results.push_back(ids.result_a2);
    results.push_back(ids.result_a1);
    REQUIRE(loomora_editor_set_order(e, LOOMORA_KIND_RESULT, &results[0], results.size()) == 0);
    REQUIRE(loomora_editor_set_title(e, "Retitled") == 0);
    // not a permutation: missing, repeated, unknown
    std::vector<std::uint32_t> bad(results.begin(), results.end() - 1);
    CHECK(loomora_editor_set_order(e, LOOMORA_KIND_RESULT, &bad[0], bad.size()) == LOOMORA_ERR_INVALID_ITEM);
    bad = results;
    bad[0] = bad[1];
    CHECK(loomora_editor_set_order(e, LOOMORA_KIND_RESULT, &bad[0], bad.size()) == LOOMORA_ERR_INVALID_ITEM);
    bad = results;
    bad[0] = 4242;
    CHECK(loomora_editor_set_order(e, LOOMORA_KIND_RESULT, &bad[0], bad.size()) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_set_order(e, 42, &results[0], results.size()) == LOOMORA_ERR_NOT_FOUND);
    const Snap s = snapshot(e);
    CHECK(s.spools == order);
    CHECK(s.results == results);
    Proj p(s.bytes);
    CHECK(has(p.manifest(), "\"title\":\"Retitled\""));
    // the order survives a removal
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, ids.result_3d, 0) == 0);
    const Snap t = snapshot(e);
    CHECK(t.results == without(results, std::vector<Pair>(1, result_of(ids.result_3d)), LOOMORA_KIND_RESULT));
    loomora_editor_close(e);
}

// ---- 3D: frames, meshes, nails, views --------------------------------------------------------------

TEST_CASE("a 3D frame with a mesh, 3D nails and a result with two views round-trips", "[format][editor][3d]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids, true) == 0);
    const Snap s = snapshot(e);
    Proj p(s.bytes);
    REQUIRE(p.rc == 0);
    const std::string manifest = p.manifest();
    CHECK(has(manifest, "\"required_features\":[\"surface_mesh\",\"multi_view_target\"]"));
    // mesh
    loomora_mesh_info info = loomora_mesh_info();
    REQUIRE(loomora_frame_mesh_info(p.p, ids.frame_3d, &info) == 0);
    CHECK(info.vertices == 8);
    CHECK(info.triangles == 12);
    CHECK(info.flags == (LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS));
    const Cube cube;
    std::vector<float> xyz(24), nrm(24), uv(16);
    std::vector<std::uint32_t> abc(36);
    CHECK(loomora_frame_read_vertices(p.p, ids.frame_3d, 0, 8, &xyz[0]) == 8);
    CHECK(loomora_frame_read_triangles(p.p, ids.frame_3d, 0, 12, &abc[0]) == 12);
    CHECK(loomora_frame_read_normals(p.p, ids.frame_3d, 0, 8, &nrm[0]) == 8);
    CHECK(loomora_frame_read_uvs(p.p, ids.frame_3d, 0, 8, &uv[0]) == 8);
    CHECK(xyz == cube.xyz);
    CHECK(abc == cube.abc);
    CHECK(nrm == cube.normals);
    CHECK(uv == cube.uvs);
    const std::string fj = p.json(LOOMORA_KIND_FRAME, ids.frame_3d);
    CHECK(has(fj, "\"kind\":\"3d\""));
    CHECK(has(fj, "\"depth_mm\":100"));
    CHECK(has(fj, "\"body\":\"solid\""));
    CHECK(has(fj, "\"mesh\":{\"vertices\":8,\"triangles\":12,\"watertight\":true,\"manifold\":true,\"has_normals\":true,\"has_uvs\":true"));
    // 3D nails
    std::vector<loomora_nail> got(multi_project::kNails3D);
    REQUIRE(loomora_layout_read_nails(p.p, ids.layout_3d, 0, got.size(), &got[0]) == static_cast<int64_t>(got.size()));
    const std::vector<loomora_nail> want = multi_project::ring_3d(multi_project::kNails3D);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK((got[i].flags & 1u) == 1u);
        CHECK(got[i].id == want[i].id);
        CHECK(got[i].z == want[i].z);
        CHECK(got[i].length == want[i].length);
        CHECK(got[i].side == want[i].side);
        CHECK(got[i].axis[2] == 1.0f);
        CHECK(got[i].face_triangle == want[i].face_triangle);
        CHECK(got[i].face_u == want[i].face_u);
    }
    // views
    const std::string rj = p.json(LOOMORA_KIND_RESULT, ids.result_3d);
    CHECK(has(rj, "\"view\":{\"position\":[0,0,300],\"target\":[0,0,0],\"up\":[0,1,0],\"projection\":\"orthographic\",\"size\":200,\"weight\":1}"));
    CHECK(has(rj, "\"projection\":\"perspective\",\"fov_deg\":40,\"weight\":0.5"));
    loomora_editor_close(e);
}

TEST_CASE("3D frames: the surfaces and bodies, a mesh without UVs, no mesh for a primitive", "[format][editor][3d]") {
    const char *surfaces[5] = {"mesh", "primitive", "extrusion", "revolution", "csg"};
    const char *bodies[3] = {"solid", "shell", "guide"};
    for (int s = 0; s < 5; ++s) {
        loomora_editor *e = nullptr;
        REQUIRE(basic_project::create(&e) == 0);
        std::uint32_t frame = 0;
        const int32_t rc = loomora_editor_add_frame(
            e, multi_project::frame3d_json("F", surfaces[s], bodies[s % 3]).c_str(), &frame);
        REQUIRE(rc == 0);
        Bytes out;
        loomora_report *rep = nullptr;
        if (s == 0) {
            // surface mesh: no save before the mesh is set
            CHECK(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, &rep) ==
                  LOOMORA_ERR_INVALID_ITEM);
            delete rep;
            out.clear();
            const Cube cube;
            const loomora_mesh_info info = cube.info(false, false);
            REQUIRE(loomora_editor_frame_set_mesh(e, frame, &info, &cube.xyz[0], &cube.abc[0], nullptr, nullptr) == 0);
        }
        REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, &rep) == 0);
        delete rep;
        Proj p(out);
        REQUIRE(p.rc == 0);
        loomora_mesh_info info = loomora_mesh_info();
        if (s == 0) {
            REQUIRE(loomora_frame_mesh_info(p.p, frame, &info) == 0);
            CHECK(info.flags == 0);
            std::vector<float> uv(16);
            CHECK(loomora_frame_read_uvs(p.p, frame, 0, 8, &uv[0]) == 0);
            CHECK(has(p.manifest(), "\"required_features\":[\"surface_mesh\"]"));
        } else {
            CHECK(loomora_frame_mesh_info(p.p, frame, &info) == LOOMORA_ERR_NOT_FOUND);
            CHECK(has(p.manifest(), "\"required_features\":[]"));
        }
        CHECK(has(p.json(LOOMORA_KIND_FRAME, frame), std::string("\"surface\":\"") + surfaces[s] + "\""));
        CHECK(has(p.json(LOOMORA_KIND_FRAME, frame), std::string("\"body\":\"") + bodies[s % 3] + "\""));
        loomora_editor_close(e);
    }
}

TEST_CASE("3D editing is refused where the data model refuses it", "[format][editor][3d]") {
    loomora_editor *e = nullptr;
    Ids ids;
    REQUIRE(multi_project::build_small(&e, &ids) == 0);
    const Cube cube;
    const loomora_mesh_info info = cube.info(true, false);
    // a second first mesh, a mesh on a 2D frame, an unknown frame
    CHECK(loomora_editor_frame_set_mesh(e, ids.frame_3d, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr) ==
          LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_frame_set_mesh(e, ids.frame_a, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr) ==
          LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_frame_set_mesh(e, 999, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr) ==
          LOOMORA_ERR_NOT_FOUND);
    // replace_frame_mesh of a frame without mesh
    std::uint32_t prim = 0;
    REQUIRE(loomora_editor_add_frame(e, multi_project::frame3d_json("P", "primitive").c_str(), &prim) == 0);
    CHECK(loomora_editor_replace_frame_mesh(e, prim, &info, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr, 1) ==
          LOOMORA_ERR_INVALID_ITEM);
    // bad data: an index out of range, a normal that is not a unit vector, a flag without its array, an unknown flag
    Cube bad;
    bad.abc[5] = 8;
    CHECK(loomora_editor_replace_frame_mesh(e, ids.frame_3d, &info, &bad.xyz[0], &bad.abc[0], &bad.normals[0], nullptr,
                                            1) == LOOMORA_ERR_INVALID_ITEM);
    Cube shortn;
    shortn.normals[0] = 3;
    CHECK(loomora_editor_replace_frame_mesh(e, ids.frame_3d, &info, &shortn.xyz[0], &shortn.abc[0], &shortn.normals[0],
                                            nullptr, 1) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_replace_frame_mesh(e, ids.frame_3d, &info, &cube.xyz[0], &cube.abc[0], nullptr, nullptr, 1) ==
          LOOMORA_ERR_INVALID_ITEM);
    loomora_mesh_info odd = info;
    odd.flags = 8;
    CHECK(loomora_editor_replace_frame_mesh(e, ids.frame_3d, &odd, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr,
                                            1) == LOOMORA_ERR_INVALID_ITEM);
    // a 3D nail without the 3D bit, a nail whose face is off the mesh
    std::vector<loomora_nail> n = multi_project::ring_3d(multi_project::kNails3D);
    n[1].flags = 0;
    CHECK(loomora_editor_replace_layout_nails(e, ids.layout_3d, &n[0], n.size(), 1) == LOOMORA_ERR_INVALID_ITEM);
    n = multi_project::ring_3d(multi_project::kNails3D);
    n[1].face_triangle = 12;
    CHECK(loomora_editor_replace_layout_nails(e, ids.layout_3d, &n[0], n.size(), 1) == LOOMORA_ERR_INVALID_ITEM);
    n = multi_project::ring_3d(multi_project::kNails3D);
    n[1].axis[2] = 2.0f;
    CHECK(loomora_editor_replace_layout_nails(e, ids.layout_3d, &n[0], n.size(), 1) == LOOMORA_ERR_INVALID_ITEM);
    // 2D nails on a 3D frame, 3D nails on a 2D frame are not the same dimension
    std::uint32_t layout = 0;
    CHECK(multi_project::add_layout(e, ids.frame_3d, "Flat", multi_project::ring_2d(6, 20), &layout) ==
          LOOMORA_ERR_INVALID_ITEM);
    // nothing of that was kept
    const Snap s = snapshot(e);
    CHECK(s.layouts.size() == 5);
    loomora_editor_close(e);
}

TEST_CASE("the first mesh, a replaced mesh with new UVs, an attached source", "[format][editor][3d]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e) == 0);
    std::uint32_t frame = 0;
    REQUIRE(loomora_editor_add_frame(e, multi_project::frame3d_json("F", "mesh").c_str(), &frame) == 0);
    const Cube cube;
    // no attached source before the mesh
    const std::uint8_t src[] = {'s', 'o', 'l', 'i', 'd'};
    CHECK(loomora_editor_frame_set_attached_source(e, frame, "cube.STL", src, sizeof src) == LOOMORA_ERR_INVALID_ITEM);
    const loomora_mesh_info plain = cube.info(true, false);
    REQUIRE(loomora_editor_frame_set_mesh(e, frame, &plain, &cube.xyz[0], &cube.abc[0], &cube.normals[0], nullptr) == 0);
    REQUIRE(loomora_editor_frame_set_attached_source(e, frame, "cube.STL", src, sizeof src) == 0);
    // add UVs: a replace; no result depends on the frame, so no confirmation is needed
    const loomora_mesh_info uvinfo = cube.info(true, true);
    REQUIRE(loomora_editor_replace_frame_mesh(e, frame, &uvinfo, &cube.xyz[0], &cube.abc[0], &cube.normals[0],
                                              &cube.uvs[0], 0) == 0);
    const Bytes saved = save_memory(e);
    Proj p(saved);
    REQUIRE(p.rc == 0);
    loomora_mesh_info info = loomora_mesh_info();
    REQUIRE(loomora_frame_mesh_info(p.p, frame, &info) == 0);
    CHECK(info.flags == (LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS));
    const std::string entry = "frames/" + basic_project::num(frame) + "/mesh-source.stl";
    CHECK(has(p.json(LOOMORA_KIND_FRAME, frame), "\"attached_source\":\"" + entry + "\""));
    std::uint8_t got[8];
    CHECK(loomora_project_read_entry(p.p, entry.c_str(), 0, got, sizeof got) == static_cast<int64_t>(sizeof src));
    CHECK(std::memcmp(got, src, sizeof src) == 0);
    // removing it again
    REQUIRE(loomora_editor_frame_set_attached_source(e, frame, nullptr, nullptr, 0) == 0);
    const Bytes without_source = save_memory(e);
    Proj q(without_source);
    CHECK(q.rc == 0);
    CHECK(loomora_project_entry_size(q.p, entry.c_str()) == LOOMORA_ERR_NOT_FOUND);
    loomora_editor_close(e);
}

// ---- opening for edit, copy-through ------------------------------------------------------------------

namespace {

struct Entries {
    Bytes bytes;
    MemorySource source;
    ZipReader zip;
    explicit Entries(const Bytes &b) : bytes(b), source(&bytes[0], bytes.size()), zip(source) { zip.open(); }
    // The compressed bytes of an entry as they are in the archive, with method and CRC.
    std::string raw(const std::string &name) const {
        const ZipEntry *e = zip.find(name);
        REQUIRE(e != nullptr);
        std::string out(reinterpret_cast<const char *>(&bytes[0] + e->data_offset),
                        static_cast<std::size_t>(e->compressed_size));
        out += "|" + basic_project::num(e->method) + "|" + basic_project::num(e->crc32) + "|" +
               basic_project::num(static_cast<unsigned long>(e->size));
        return out;
    }
    std::set<std::string> names() const {
        std::set<std::string> n;
        for (std::size_t i = 0; i < zip.entries().size(); ++i)
            n.insert(zip.entries()[i].name);
        return n;
    }
    std::string sha(const std::string &name) const {
        const ZipEntry *e = zip.find(name);
        REQUIRE(e != nullptr);
        const Bytes d = zip.read_all(*e, 1ull << 30);
        return sha256_hex(d.empty() ? nullptr : &d[0], d.size());
    }
};

} // namespace

TEST_CASE("SC-013: after adding one result every other entry is byte for byte the same", "[format][editor][copy]") {
    fstest::ScratchDir dir("copy");
    const std::string path = dir.file("multi.loomora");
    {
        loomora_editor *e = nullptr;
        Ids ids;
        REQUIRE(multi_project::build_small(&e, &ids) == 0);
        REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr) == 0);
        loomora_editor_close(e);
    }
    Bytes old_bytes;
    REQUIRE(fstest::read_file(path, old_bytes));

    loomora_editor *e = nullptr;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, &report) == 0);
    REQUIRE(report != nullptr);
    CHECK(loomora_report_error_count(report) == 0);
    loomora_report_free(report);
    Ids ids;
    // the ids of the file: frames 1..3, layouts 1..5 in creation order, images 1..4, spools 1..6, results 1..5
    std::uint32_t added = 0;
    std::vector<std::uint32_t> sp(1, 3);
    REQUIRE(multi_project::add_result_json(e, "Extra", 1, 2, multi_project::one_image(2), sp, multi_project::kNails2D,
                                           &added) == 0);
    CHECK(added == 6);
    Bytes new_bytes;
    REQUIRE(loomora_editor_save_stream(e, "2026-11-18T08:00:00Z", 0, collect, &new_bytes, nullptr) == 0);
    loomora_editor_close(e);

    const Entries before(old_bytes), after(new_bytes);
    std::set<std::string> expected = before.names();
    expected.insert("results/6/result.json");
    expected.insert("results/6/sequence.bin");
    expected.insert("results/6/thumb.png");
    expected.insert("results/6/settings.json");
    CHECK(after.names() == expected);
    std::size_t same = 0;
    const std::set<std::string> old_names = before.names();
    for (std::set<std::string>::const_iterator it = old_names.begin(); it != old_names.end(); ++it) {
        if (*it == "manifest.json")
            continue;
        INFO(*it);
        CHECK(after.raw(*it) == before.raw(*it)); // the compressed bytes, the method and the CRC
        CHECK(after.sha(*it) == before.sha(*it));
        ++same;
    }
    CHECK(same >= 40);
    // the new file is valid and the manifest lists the old sums
    Proj p(new_bytes);
    CHECK(p.rc == 0);
    CHECK(p.ids(LOOMORA_KIND_RESULT).size() == 6);
    // the file really is the same one for the entries of the untouched items, including the mimetype
    const std::string manifest_after = p.manifest();
    CHECK(has(manifest_after, "\"modified_at\":\"2026-11-18T08:00:00Z\""));
    CHECK(has(manifest_after, "\"next_id\":{\"frame\":4,\"layout\":6,\"image\":5,\"spool\":7,\"result\":7}"));
    (void)ids;
}

TEST_CASE("SC-011: save, open, remove one result, save again: the references are intact", "[format][editor][copy]") {
    fstest::ScratchDir dir("sc11");
    const std::string path = dir.file("a.loomora");
    Ids ids;
    {
        loomora_editor *e = nullptr;
        REQUIRE(multi_project::build_small(&e, &ids) == 0);
        REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr) == 0);
        loomora_editor_close(e);
    }
    loomora_editor *e = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, nullptr) == 0);
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_RESULT, ids.result_a2, 0) == 0);
    // save over the very file the editor still reads from (the copy-through source)
    REQUIRE(loomora_editor_save_path(e, path.c_str(), "2026-11-18T08:00:00Z", 0, nullptr, nullptr, nullptr) == 0);
    // and once more: the editor still holds the old archive, which the second save reads again
    REQUIRE(loomora_editor_set_title(e, "Twice") == 0);
    REQUIRE(loomora_editor_save_path(e, path.c_str(), "2026-11-18T09:00:00Z", 0, nullptr, nullptr, nullptr) == 0);
    loomora_editor_close(e);

    loomora_project *p = nullptr;
    loomora_report *report = nullptr;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &p, &report) == 0);
    CHECK(loomora_report_error_count(report) == 0);
    CHECK(loomora_project_count(p, LOOMORA_KIND_RESULT) == 4);
    CHECK(loomora_project_count(p, LOOMORA_KIND_LAYOUT) == 5);
    // the remaining results still read through their layout and their sequence
    std::vector<loomora_step> steps(multi_project::kSteps);
    CHECK(loomora_result_read_steps(p, ids.result_a1, 0, steps.size(), &steps[0]) ==
          static_cast<int64_t>(multi_project::kSteps));
    CHECK(loomora_result_step_count(p, ids.result_a2) == LOOMORA_ERR_NOT_FOUND);
    loomora_report_free(report);
    loomora_project_close(p);
    // the directory holds the file and nothing else
    CHECK(dir.names().size() == 1);
}

TEST_CASE("editing an opened project: replace and remove take effect on the next save", "[format][editor][copy]") {
    fstest::ScratchDir dir("open");
    const std::string path = dir.file("m.loomora");
    Ids ids;
    {
        loomora_editor *e = nullptr;
        REQUIRE(multi_project::build_small(&e, &ids) == 0);
        REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, nullptr) == 0);
        loomora_editor_close(e);
    }
    loomora_editor *e = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, nullptr) == 0);
    // the dependency engine works on what was loaded
    CHECK(dependents(e, LOOMORA_KIND_LAYOUT, ids.layout_a1, LOOMORA_OP_REMOVE) == results_of_layout_a1(ids));
    // labels of a layout still in the archive: the nails are loaded on demand
    REQUIRE(loomora_editor_layout_set_label(e, ids.layout_a1, 1, "second") == 0);
    // a moved nail: the results of the layout go, the layout keeps its other labels
    std::vector<loomora_nail> n = multi_project::ring_2d(multi_project::kNails2D, 200);
    n[2].x += 1;
    CHECK(loomora_editor_replace_layout_nails(e, ids.layout_a1, &n[0], n.size(), 0) == LOOMORA_ERR_HAS_DEPENDENTS);
    REQUIRE(loomora_editor_replace_layout_nails(e, ids.layout_a1, &n[0], n.size(), 1) == 0);
    // a new mesh on the 3D frame, a new picture, a thumbnail
    const Cube moved(70.0f);
    const loomora_mesh_info info = moved.info(true, true);
    REQUIRE(loomora_editor_replace_frame_mesh(e, ids.frame_3d, &info, &moved.xyz[0], &moved.abc[0], &moved.normals[0],
                                              &moved.uvs[0], 1) == 0);
    Bytes out;
    REQUIRE(loomora_editor_save_stream(e, "2026-11-18T08:00:00Z", 0, collect, &out, nullptr) == 0);
    loomora_editor_close(e);

    Proj p(out);
    REQUIRE(p.rc == 0);
    CHECK(p.ids(LOOMORA_KIND_RESULT) == (std::vector<std::uint32_t>{ids.result_a2, ids.result_b}));
    std::uint8_t buf[16];
    CHECK(loomora_layout_nail_label(p.p, ids.layout_a1, 0, buf, sizeof buf) == 5);
    CHECK(loomora_layout_nail_label(p.p, ids.layout_a1, 1, buf, sizeof buf) == 6);
    loomora_nail nail = loomora_nail();
    REQUIRE(loomora_layout_read_nails(p.p, ids.layout_a1, 2, 1, &nail) == 1);
    CHECK(nail.x == n[2].x);
    std::vector<float> xyz(24);
    REQUIRE(loomora_frame_read_vertices(p.p, ids.frame_3d, 0, 8, &xyz[0]) == 8);
    CHECK(xyz == moved.xyz);
    CHECK(has(p.manifest(), "\"required_features\":[\"surface_mesh\"]")); // the 3D result with two views is gone
}

TEST_CASE("a corpus file with unknown entries and extensions is kept on rewrite", "[format][editor][copy]") {
    const std::string corpus = FMT_CORPUS_DIR;
    std::ifstream index((corpus + "/corpus.json").c_str(), std::ios::binary);
    if (corpus.empty() || !index.good()) {
        WARN("the corpus is not present: unknown-entry and light-copy edit tests skipped");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(index)), std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    REQUIRE(!doc.HasParseError());
    // the first file of the corpus that gives a code (a warning: the file opens)
    struct Finder {
        const rapidjson::Document &d;
        std::string operator()(const char *code) const {
            const rapidjson::Value &files = d["files"];
            for (rapidjson::Value::ConstMemberIterator it = files.MemberBegin(); it != files.MemberEnd(); ++it)
                if (it->value["code"].IsString() && std::string(it->value["code"].GetString()) == code &&
                    std::string(it->value["kind"].GetString()) == "warning")
                    return it->name.GetString();
            return std::string();
        }
    };
    const Finder find = {doc};
    const std::string unknown_file = find("warn.unknown_entry");
    const std::string light_file = find("warn.light_project");
    REQUIRE(!unknown_file.empty());
    REQUIRE(!light_file.empty());

    // a light project is refused, with its message in the report
    {
        loomora_editor *e = nullptr;
        loomora_report *report = nullptr;
        CHECK(loomora_editor_open_path((corpus + "/" + light_file).c_str(), nullptr, nullptr, &e, &report) ==
              LOOMORA_ERR_READ_ONLY_PROJECT);
        CHECK(e == nullptr);
        REQUIRE(report != nullptr);
        bool said = false;
        for (int64_t i = 0; i < loomora_report_count(report); ++i) {
            const int64_t len = loomora_report_text(report, static_cast<std::size_t>(i), 3, nullptr, 0);
            std::string msg(static_cast<std::size_t>(len), '\0');
            if (len > 0)
                loomora_report_text(report, static_cast<std::size_t>(i), 3, reinterpret_cast<std::uint8_t *>(&msg[0]), msg.size());
            said = said || msg == "This project is a light copy and cannot be edited";
        }
        CHECK(said);
        loomora_report_free(report);
    }

    // a project with an entry this version does not know: kept, same bytes, listed in the new manifest
    Bytes old_bytes;
    REQUIRE(fstest::read_file(corpus + "/" + unknown_file, old_bytes));
    loomora_editor *e = nullptr;
    REQUIRE(loomora_editor_open_path((corpus + "/" + unknown_file).c_str(), nullptr, nullptr, &e, nullptr) == 0);
    REQUIRE(loomora_editor_set_title(e, "Edited") == 0);
    Bytes new_bytes;
    REQUIRE(loomora_editor_save_stream(e, "2026-11-18T08:00:00Z", 0, collect, &new_bytes, nullptr) == 0);
    loomora_editor_close(e);
    const Entries before(old_bytes), after(new_bytes);
    CHECK(after.names() == before.names());
    CHECK(after.names().count("x-acme/note.txt") == 1);
    const std::set<std::string> old_names = before.names();
    for (std::set<std::string>::const_iterator it = old_names.begin(); it != old_names.end(); ++it) {
        if (*it == "manifest.json")
            continue;
        INFO(*it);
        CHECK(after.raw(*it) == before.raw(*it));
    }
    Proj q(new_bytes);
    CHECK(q.rc == 0);
    Proj o(old_bytes);
    // the extensions of the manifest are kept as they were
    const std::string a = o.manifest(), b = q.manifest();
    const std::size_t ea = a.find("\"extensions\":"), eb = b.find("\"extensions\":");
    REQUIRE(ea != std::string::npos);
    REQUIRE(eb != std::string::npos);
    CHECK(a.substr(ea) == b.substr(eb));
}

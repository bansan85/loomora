// Self-test of the .loomora format code: plain C++11 `main`, no test framework, so it can be
// cross-compiled and run on every target (the Catch2 tests of core/tests/format/ run on the host
// only). Each later story adds its checks here. Same output rule as selftest.cpp: through
// <cstdio> where LOOMORA_AVOID_IOSTREAM is defined (the MinGW-w64 toolchains of the Windows floors
// older than Vista), and no <iostream>, <thread>, <mutex> or <locale> in this file.
//
// User Story 1 (T039): the `basic` reference project (core/tests/basic_project.hpp, built through the
// public C API) is written in memory and its SHA-256 is compared with a golden constant, so a platform
// whose writer differs by one byte fails here (FR-004, the "same bytes on every platform" rule); the
// file is read back and every field compared; every truncation of the small `empty` file, and a
// sample of the truncations of `basic`, is rejected without a crash.
//
// T084: the light references. `multi-light` (the light export of `multi`, every item) and `one` (the
// light export of ONE result of `multi`, the Cube result) are exported through the public C API
// (loomora_project_export_stream) and their SHA-256 compared with golden constants, the same on every
// platform; read back, their content is checked (items, identifiers, nails, steps, no source or processed
// image, no settings, no UV, mesh positions, indices and normals equal to the full file).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "basic_project.hpp"
#include "multi_project.hpp"
#include "format/sha256.hpp"

namespace {

// SHA-256 of the archive written for each reference project (modification time kModifiedAt). The
// same constants must hold on Windows (clang, MinGW-w64) and Linux (g++): measured on Windows
// clang and Linux g++ (Ubuntu 26.04), 2026-11-17. T060 replaces them when the format changes.
const char kGoldenBasic[] = "fcdf97f0257076ba1c3ecae1867e57ebdc397440286a278f2cbe34eff8ce5f96";
const char kGoldenEmpty[] = "8ca4dc6a2139539feb87f92b11cc0d5685fdff051fa044b692a7be5d984d7913";
const char kGoldenEscape[] = "b17920aabf9cc165a5c3b9290ba15f22e99903592426a8e55c96dc67ae570e2a";
const char kGoldenMulti[] = "8bcd3ae91876dcf6048c3a5dc689754a8eab90367faed373942b819a7c3f8607";
const char kGoldenMultiLight[] = "b32c8a390657d4f56ec9bd448e6c291e9781dfbb3b34b8164d049af67a7c4a8d";
const char kGoldenOne[] = "fc3fb5ebd929689bd7ee32da020b279ca81b3412c6c947eda8c9e7e3ddb846c5";

typedef std::vector<std::uint8_t> Bytes;

int g_failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        ++g_failures;
        std::fputs("FAIL: ", stdout);
        std::fputs(what, stdout);
        std::fputs("\n", stdout);
    }
}

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *out = static_cast<Bytes *>(user);
    out->insert(out->end(), data, data + len);
    return 0;
}

bool save(loomora_editor *e, Bytes &out) {
    out.clear();
    loomora_report *report = 0;
    const int32_t rc = loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, &report);
    const bool clean = report == 0 || loomora_report_count(report) == 0;
    if (report != 0)
        loomora_report_free(report);
    return rc == 0 && clean;
}

std::string digest(const Bytes &b) {
    return loomora::format::sha256_hex(b.empty() ? 0 : &b[0], b.size());
}

// Opens `b`; true when the project opened with no issue at all.
bool open_clean(const Bytes &b, loomora_project **p) {
    loomora_report *report = 0;
    *p = 0;
    const int32_t rc = loomora_project_open_memory(b.empty() ? 0 : &b[0], b.size(), 0, 0, p, &report);
    const bool clean = report == 0 || loomora_report_count(report) == 0;
    if (report != 0)
        loomora_report_free(report);
    return rc == 0 && *p != 0 && clean;
}

bool same_bytes(const loomora_project *p, const char *entry, const Bytes &want) {
    const std::int64_t n = loomora_project_entry_size(p, entry);
    if (n != static_cast<std::int64_t>(want.size()))
        return false;
    Bytes b(want.size() + 1);
    return loomora_project_read_entry(p, entry, 0, &b[0], want.size()) == n && std::memcmp(&b[0], &want[0], want.size()) == 0;
}

bool same_text(const loomora_project *p, const char *entry, const std::string &want) {
    return same_bytes(p, entry, Bytes(want.begin(), want.end()));
}

bool same_item(const loomora_project *p, int32_t kind, std::uint32_t id, const std::string &want) {
    Bytes b(want.size() + 64);
    const std::int64_t n = loomora_project_item_json(p, kind, id, &b[0], b.size());
    return n == static_cast<std::int64_t>(want.size()) && std::memcmp(&b[0], want.data(), want.size()) == 0;
}

std::string spool_json(unsigned id) {
    const char *color = (id - 1) % 3 == 0 ? "#000000" : (id - 1) % 3 == 1 ? "#ff0000" : "#0000ff";
    return "{\"id\":" + basic_project::num(id) + ",\"name\":\"Spool " + basic_project::num(id) + "\",\"color\":\"" +
           color + "\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,\"material\":\"cotton\"," +
           "\"domain\":\"exterior\"}";
}

// Nails and steps of the generator of basic_project.hpp, recomputed here.
bool check_nails_steps(const loomora_project *p, unsigned spool_count, std::uint32_t steps) {
    std::vector<loomora_nail> nails(basic_project::kNailCount);
    if (loomora_layout_nail_count(p, 1) != static_cast<std::int64_t>(nails.size()) ||
        loomora_layout_read_nails(p, 1, 0, nails.size(), &nails[0]) != static_cast<std::int64_t>(nails.size()))
        return false;
    for (std::uint32_t i = 0; i < basic_project::kNailCount; ++i) {
        const double a = 6.283185307179586 * i / basic_project::kNailCount;
        if (nails[i].id != i || nails[i].kind != 1u || nails[i].flags != (i == 0 ? 2u : 0u) ||
            nails[i].x != basic_project::round3(200.0 * std::cos(a)) ||
            nails[i].y != basic_project::round3(200.0 * std::sin(a)) || nails[i].diameter != 2.0)
            return false;
    }
    std::vector<loomora_step> got(steps);
    if (loomora_result_step_count(p, 1) != static_cast<std::int64_t>(steps) ||
        loomora_result_read_steps(p, 1, 0, got.size(), &got[0]) != static_cast<std::int64_t>(steps))
        return false;
    for (std::uint32_t i = 0; i < steps; ++i) {
        const bool first = i < spool_count;
        const bool last = i >= steps - spool_count;
        const std::uint32_t spool = first ? i : last ? i - (steps - spool_count) : (i * 5u + 1u) % spool_count;
        if (got[i].spool_index != spool || got[i].nail != (i * 7u + spool * 13u) % basic_project::kNailCount ||
            got[i].rotation != (i & 1u) || got[i].type != (first ? 1 : last ? 2 : 0) || got[i].level != 0)
            return false;
    }
    return true;
}

// Number of prefixes of `bytes` (every `stride`-th above `dense` bytes) the reader accepts: must be 0.
unsigned accepted_prefixes(const Bytes &bytes, std::size_t dense, std::size_t stride) {
    unsigned accepted = 0;
    for (std::size_t len = 0; len < bytes.size(); len += (len < dense ? 1 : stride)) {
        loomora_project *q = 0;
        loomora_report *report = 0;
        const int32_t rc = loomora_project_open_memory(len == 0 ? 0 : &bytes[0], len, 0, 0, &q, &report);
        if (rc == 0 || q != 0)
            ++accepted;
        if (q != 0)
            loomora_project_close(q);
        if (report != 0)
            loomora_report_free(report);
    }
    return accepted;
}

void test_basic() {
    loomora_editor *e = 0;
    check(basic_project::build_basic(&e) == 0, "basic: build through the C API");
    if (e == 0)
        return;
    Bytes bytes, again;
    check(save(e, bytes), "basic: save, clean report");
    check(save(e, again) && again == bytes, "basic: saving twice gives the same bytes");
    loomora_editor_close(e);
    const std::string hex = digest(bytes);
    check(hex == kGoldenBasic, "basic: SHA-256 of the archive equals the golden value");
    if (hex != kGoldenBasic) {
        std::fputs("  basic sha256 = ", stdout);
        std::fputs(hex.c_str(), stdout);
        std::fputs("\n", stdout);
    }

    loomora_project *p = 0;
    check(open_clean(bytes, &p), "basic: reads back, no issue");
    if (p == 0)
        return;
    check(loomora_project_count(p, LOOMORA_KIND_FRAME) == 1 && loomora_project_count(p, LOOMORA_KIND_LAYOUT) == 1 &&
              loomora_project_count(p, LOOMORA_KIND_IMAGE) == 1 && loomora_project_count(p, LOOMORA_KIND_SPOOL) == 3 &&
              loomora_project_count(p, LOOMORA_KIND_RESULT) == 1,
          "basic: item counts");
    for (unsigned i = 1; i <= 3; ++i)
        check(same_item(p, LOOMORA_KIND_SPOOL, i, spool_json(i)), "basic: spool JSON");
    check(same_item(p, LOOMORA_KIND_LAYOUT, 1, "{\"id\":1,\"name\":\"Circle 200\",\"frame\":1,\"nail_count\":200}"),
          "basic: layout JSON");
    check(same_item(p, LOOMORA_KIND_FRAME, 1,
                    "{\"id\":1,\"name\":\"Square 50\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
                    "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
                    "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},\"exclusions\":[],"
                    "\"surface\":\"plane\"}"),
          "basic: frame JSON");
    check(same_item(p, LOOMORA_KIND_IMAGE, 1,
                    "{\"id\":1,\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,"
                    "\"height_px\":8,\"source\":{\"entry\":\"images/1/source.png\",\"format\":\"png\",\"size\":140},"
                    "\"target\":{\"entry\":\"images/1/target.png\",\"format\":\"png\",\"size\":140},"
                    "\"thumbnail\":{\"entry\":\"images/1/thumb.png\"}}"),
          "basic: image JSON");
    check(same_item(p, LOOMORA_KIND_RESULT, 1,
                    "{\"id\":1,\"name\":\"Portrait on circle\",\"created_at\":\"2026-11-17T09:30:00Z\",\"frame\":1,"
                    "\"layout\":1,\"images\":[{\"image\":1}],\"spools\":[{\"spool\":1,\"length_mm\":1000},{\"spool\":2,"
                    "\"length_mm\":1010},{\"spool\":3,\"length_mm\":1020}],\"steps\":5000,\"levels\":false,"
                    "\"thumbnail\":{\"entry\":\"results/1/thumb.png\"},\"settings\":{\"entry\":\"results/1/settings.json\"}}"),
          "basic: result JSON");
    check(check_nails_steps(p, 3, basic_project::kStepCount), "basic: 200 nails and 5,000 steps, field by field");
    check(same_bytes(p, "images/1/source.png", basic_project::make_png(8, 8, 40)), "basic: source picture");
    check(same_bytes(p, "images/1/target.png", basic_project::make_png(8, 8, 90)), "basic: target picture");
    check(same_bytes(p, "images/1/thumb.png", basic_project::make_png(4, 4, 10)), "basic: image thumbnail");
    check(same_bytes(p, "results/1/thumb.png", basic_project::make_png(4, 4, 200)), "basic: result thumbnail");
    check(same_text(p, "results/1/settings.json", "{\"algorithm\":\"greedy\",\"seed\":1}"), "basic: result settings");
    loomora_project_close(p);

    check(accepted_prefixes(bytes, 600, 173) == 0, "basic: every sampled truncation is rejected");
}

void test_empty() {
    loomora_editor *e = 0;
    check(basic_project::create(&e, "") == 0, "empty: create");
    if (e == 0)
        return;
    Bytes bytes;
    check(save(e, bytes), "empty: save, clean report");
    loomora_editor_close(e);
    check(digest(bytes) == kGoldenEmpty, "empty: SHA-256 equals the golden value");
    loomora_project *p = 0;
    check(open_clean(bytes, &p), "empty: reads back, no issue");
    if (p != 0) {
        bool none = true;
        for (int32_t k = LOOMORA_KIND_FRAME; k <= LOOMORA_KIND_RESULT; ++k)
            none = none && loomora_project_count(p, k) == 0;
        check(none, "empty: no item");
        loomora_project_close(p);
    }
    // The small file: every proper prefix, byte by byte, is rejected without a crash.
    check(accepted_prefixes(bytes, bytes.size(), 1) == 0, "empty: every truncation is rejected");
}

void test_escape() {
    loomora_editor *e = 0;
    check(basic_project::build_escape(&e) == 0, "escape: build");
    if (e == 0)
        return;
    Bytes bytes;
    check(save(e, bytes), "escape: save, clean report");
    loomora_editor_close(e);
    check(digest(bytes) == kGoldenEscape, "escape: SHA-256 equals the golden value");
    loomora_project *p = 0;
    check(open_clean(bytes, &p), "escape: reads back, no issue");
    if (p != 0) {
        check(loomora_project_count(p, LOOMORA_KIND_SPOOL) == 40, "escape: 40 spools");
        check(check_nails_steps(p, 40, 400), "escape: 400 steps with spool indexes up to 39");
        loomora_project_close(p);
    }
}

// The `multi` reference project (3 frames incl. a 3D cube, 5 layouts, 4 images, 6 spools, 5 results):
// the archive hash, the read-back counts, the identifiers and the first nails and steps of each result.
void test_multi() {
    loomora_editor *e = 0;
    multi_project::Ids ids;
    check(multi_project::build_multi(&e, &ids) == 0, "multi: build through the C API");
    if (e == 0)
        return;
    Bytes bytes, again;
    check(save(e, bytes), "multi: save, clean report");
    check(save(e, again) && again == bytes, "multi: saving twice gives the same bytes");
    loomora_editor_close(e);
    const std::string hex = digest(bytes);
    check(hex == kGoldenMulti, "multi: SHA-256 of the archive equals the golden value");
    if (hex != kGoldenMulti) {
        std::fputs("  multi sha256 = ", stdout);
        std::fputs(hex.c_str(), stdout);
        std::fputs("\n", stdout);
    }
    loomora_project *p = 0;
    check(open_clean(bytes, &p), "multi: reads back, no issue");
    if (p == 0)
        return;
    check(loomora_project_count(p, LOOMORA_KIND_FRAME) == 3 && loomora_project_count(p, LOOMORA_KIND_LAYOUT) == 5 &&
              loomora_project_count(p, LOOMORA_KIND_IMAGE) == 4 && loomora_project_count(p, LOOMORA_KIND_SPOOL) == 6 &&
              loomora_project_count(p, LOOMORA_KIND_RESULT) == 5,
          "multi: item counts");
    check(loomora_layout_nail_count(p, ids.layout_a1) == multi_project::kMultiNailsCircle &&
              loomora_layout_nail_count(p, ids.layout_a2) == multi_project::kMultiNailsHeart &&
              loomora_layout_nail_count(p, ids.layout_b) == multi_project::kMultiNailsRing &&
              loomora_layout_nail_count(p, ids.layout_b_unused) == multi_project::kMultiNailsSmall &&
              loomora_layout_nail_count(p, ids.layout_3d) == multi_project::kMultiNails3D,
          "multi: nails per layout");
    check(loomora_result_step_count(p, ids.result_a2) == multi_project::kMultiStepsHeart &&
              loomora_result_step_count(p, ids.result_a1) == multi_project::kMultiStepsCircle &&
              loomora_result_step_count(p, ids.result_b) == multi_project::kMultiStepsRing &&
              loomora_result_step_count(p, ids.result_3d) == multi_project::kMultiSteps3D &&
              loomora_result_step_count(p, ids.result_shared) == multi_project::kMultiStepsShared,
          "multi: steps per result");
    // The heart: nail 0 is the notch (0, 5 * 380 / 32), the tip is half way round.
    loomora_nail heart[2];
    check(loomora_layout_read_nails(p, ids.layout_a2, 0, 1, heart) == 1 &&
              loomora_layout_read_nails(p, ids.layout_a2, multi_project::kMultiNailsHeart / 2, 1, heart + 1) == 1 &&
              heart[0].x == 0.0 && heart[0].y == basic_project::round3(5.0 * 380.0 / 32.0) && heart[1].x == 0.0 &&
              heart[1].y == basic_project::round3(-17.0 * 380.0 / 32.0),
          "multi: the heart notch and tip");
    loomora_mesh_info info = loomora_mesh_info();
    check(loomora_frame_mesh_info(p, ids.frame_3d, &info) == 0 && info.vertices == 8 && info.triangles == 12 &&
              info.flags == (LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS),
          "multi: the cube mesh");
    loomora_project_close(p);
}


// ---- the light references (T084): `multi-light` and `one` ---------------------------------------------

// Light export of `p`: every item, or the `n` results of `ids`. Clean = no error in the report.
bool export_light(const loomora_project *p, const std::uint32_t *ids, std::size_t n, Bytes &out) {
    out.clear();
    loomora_report *report = 0;
    const int32_t rc = loomora_project_export_stream(p, LOOMORA_PROFILE_LIGHT, ids, n, basic_project::kModifiedAt, 0,
                                                     collect, &out, &report);
    const bool clean = report == 0 || loomora_report_error_count(report) == 0;
    if (report != 0)
        loomora_report_free(report);
    return rc == 0 && clean;
}

// Opens a light file: no error, and the one warning that says "light project".
bool open_light(const Bytes &b, loomora_project **p) {
    loomora_report *report = 0;
    *p = 0;
    const int32_t rc = loomora_project_open_memory(b.empty() ? 0 : &b[0], b.size(), 0, 0, p, &report);
    const bool ok = report != 0 && loomora_report_error_count(report) == 0 && loomora_report_count(report) == 1;
    if (report != 0)
        loomora_report_free(report);
    return rc == 0 && *p != 0 && ok && loomora_project_is_light(*p) == 1;
}

bool same_nail_list(const loomora_project *a, const loomora_project *b, std::uint32_t layout) {
    const std::int64_t n = loomora_layout_nail_count(a, layout);
    if (n <= 0 || n != loomora_layout_nail_count(b, layout))
        return false;
    std::vector<loomora_nail> x(static_cast<std::size_t>(n)), y(static_cast<std::size_t>(n));
    if (loomora_layout_read_nails(a, layout, 0, x.size(), &x[0]) != n || loomora_layout_read_nails(b, layout, 0, y.size(), &y[0]) != n)
        return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (x[i].id != y[i].id || x[i].kind != y[i].kind || x[i].flags != y[i].flags || x[i].x != y[i].x ||
            x[i].y != y[i].y || x[i].z != y[i].z || x[i].diameter != y[i].diameter || x[i].face_triangle != y[i].face_triangle)
            return false;
    return true;
}

bool same_step_list(const loomora_project *a, const loomora_project *b, std::uint32_t result) {
    const std::int64_t n = loomora_result_step_count(a, result);
    if (n <= 0 || n != loomora_result_step_count(b, result))
        return false;
    std::vector<loomora_step> x(static_cast<std::size_t>(n)), y(static_cast<std::size_t>(n));
    if (loomora_result_read_steps(a, result, 0, x.size(), &x[0]) != n || loomora_result_read_steps(b, result, 0, y.size(), &y[0]) != n)
        return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (x[i].spool_index != y[i].spool_index || x[i].nail != y[i].nail || x[i].rotation != y[i].rotation ||
            x[i].type != y[i].type || x[i].level != y[i].level)
            return false;
    return true;
}

// Positions, indices and normals of a frame's mesh: the same in both projects (the light rule of section 7).
bool same_mesh_geometry(const loomora_project *a, const loomora_project *b, std::uint32_t frame) {
    loomora_mesh_info ia = loomora_mesh_info(), ib = loomora_mesh_info();
    if (loomora_frame_mesh_info(a, frame, &ia) != 0 || loomora_frame_mesh_info(b, frame, &ib) != 0 ||
        ia.vertices != ib.vertices || ia.triangles != ib.triangles || (ia.flags & LOOMORA_MESH_NORMALS) != (ib.flags & LOOMORA_MESH_NORMALS))
        return false;
    const std::size_t nv = static_cast<std::size_t>(ia.vertices), nt = static_cast<std::size_t>(ia.triangles);
    std::vector<float> va(nv * 3), vb(nv * 3), na(nv * 3), nb(nv * 3);
    std::vector<std::uint32_t> ta(nt * 3), tb(nt * 3);
    if (loomora_frame_read_vertices(a, frame, 0, nv, &va[0]) != static_cast<std::int64_t>(nv) ||
        loomora_frame_read_vertices(b, frame, 0, nv, &vb[0]) != static_cast<std::int64_t>(nv) ||
        loomora_frame_read_triangles(a, frame, 0, nt, &ta[0]) != static_cast<std::int64_t>(nt) ||
        loomora_frame_read_triangles(b, frame, 0, nt, &tb[0]) != static_cast<std::int64_t>(nt) ||
        loomora_frame_read_normals(a, frame, 0, nv, &na[0]) != static_cast<std::int64_t>(nv) ||
        loomora_frame_read_normals(b, frame, 0, nv, &nb[0]) != static_cast<std::int64_t>(nv))
        return false;
    return std::memcmp(&va[0], &vb[0], va.size() * sizeof(float)) == 0 && std::memcmp(&ta[0], &tb[0], ta.size() * sizeof(std::uint32_t)) == 0 &&
           std::memcmp(&na[0], &nb[0], na.size() * sizeof(float)) == 0;
}

bool same_item_json(const loomora_project *a, const loomora_project *b, int32_t kind, std::uint32_t id) {
    std::vector<std::uint8_t> x(1 << 16), y(1 << 16);
    const std::int64_t nx = loomora_project_item_json(a, kind, id, &x[0], x.size());
    const std::int64_t ny = loomora_project_item_json(b, kind, id, &y[0], y.size());
    return nx > 0 && nx == ny && std::memcmp(&x[0], &y[0], static_cast<std::size_t>(nx)) == 0;
}

// What a light project never holds: the source and processed images, the settings, the UVs.
void check_light_content(const loomora_project *l, const char *tag, const std::uint32_t *images, std::size_t n_images,
                         const std::uint32_t *results, std::size_t n_results, std::uint32_t cube_frame) {
    const std::string t = tag;
    for (std::size_t i = 0; i < n_images; ++i) {
        const std::string dir = "images/" + basic_project::num(images[i]);
        check(loomora_project_entry_size(l, (dir + "/source.png").c_str()) < 0, (t + ": no source image").c_str());
        check(loomora_project_entry_size(l, (dir + "/target.png").c_str()) < 0, (t + ": no processed image").c_str());
        check(loomora_project_entry_size(l, (dir + "/thumb.png").c_str()) > 0, (t + ": the image thumbnail stays").c_str());
    }
    for (std::size_t i = 0; i < n_results; ++i) {
        const std::string dir = "results/" + basic_project::num(results[i]);
        check(loomora_project_entry_size(l, (dir + "/settings.json").c_str()) < 0, (t + ": no result settings").c_str());
        check(loomora_project_entry_size(l, (dir + "/thumb.png").c_str()) > 0, (t + ": the result thumbnail stays").c_str());
    }
    loomora_mesh_info info = loomora_mesh_info();
    check(loomora_frame_mesh_info(l, cube_frame, &info) == 0 && info.vertices == 8 && info.triangles == 12 &&
              info.flags == LOOMORA_MESH_NORMALS,
          (t + ": the cube mesh keeps its normals and has no UVs").c_str());
    float uv[16];
    check(loomora_frame_read_uvs(l, cube_frame, 0, 8, uv) == 0, (t + ": no UV to read").c_str());
}

// The light export of the whole `multi` project, and of one of its results (the Cube result, which has a
// 3D frame and two views): archive hashes, read-back, light content, mesh geometry equal to the full file.
void test_light() {
    loomora_editor *e = 0;
    multi_project::Ids ids;
    check(multi_project::build_multi(&e, &ids) == 0, "light: build multi");
    if (e == 0)
        return;
    Bytes full;
    check(save(e, full), "light: save multi");
    loomora_editor_close(e);
    loomora_project *p = 0;
    check(open_clean(full, &p), "light: multi reads back");
    if (p == 0)
        return;
    check(loomora_project_is_light(p) == 0, "light: multi is a full project");

    // multi-light: every item
    Bytes all, all2;
    check(export_light(p, 0, 0, all) && export_light(p, 0, 0, all2) && all == all2, "multi-light: exporting twice gives the same bytes");
    const std::string hex_all = digest(all);
    check(hex_all == kGoldenMultiLight, "multi-light: SHA-256 of the archive equals the golden value");
    if (hex_all != kGoldenMultiLight) {
        std::fputs("  multi-light sha256 = ", stdout);
        std::fputs(hex_all.c_str(), stdout);
        std::fputs("\n", stdout);
    }
    check(all.size() < full.size(), "multi-light: smaller than the full file");
    loomora_project *l = 0;
    check(open_light(all, &l), "multi-light: reads back as a light project, one warning");
    if (l != 0) {
        check(loomora_project_count(l, LOOMORA_KIND_FRAME) == 3 && loomora_project_count(l, LOOMORA_KIND_LAYOUT) == 5 &&
                  loomora_project_count(l, LOOMORA_KIND_IMAGE) == 4 && loomora_project_count(l, LOOMORA_KIND_SPOOL) == 6 &&
                  loomora_project_count(l, LOOMORA_KIND_RESULT) == 5,
              "multi-light: item counts");
        const std::uint32_t layouts[5] = {ids.layout_a1, ids.layout_a2, ids.layout_b, ids.layout_b_unused, ids.layout_3d};
        const std::uint32_t results[5] = {ids.result_a1, ids.result_a2, ids.result_b, ids.result_3d, ids.result_shared};
        bool same = true;
        for (int i = 0; i < 5; ++i)
            same = same && same_nail_list(p, l, layouts[i]) && same_step_list(p, l, results[i]) &&
                   same_item_json(p, l, LOOMORA_KIND_LAYOUT, layouts[i]);
        check(same, "multi-light: nails, steps and layout JSON equal those of the full file");
        bool spools = true;
        for (int i = 0; i < 6; ++i)
            spools = spools && same_item_json(p, l, LOOMORA_KIND_SPOOL, ids.spool[i]);
        check(spools, "multi-light: spool JSON equals that of the full file");
        check(same_mesh_geometry(p, l, ids.frame_3d), "multi-light: mesh positions, indices and normals equal those of the full file");
        check_light_content(l, "multi-light", ids.image, 4, results, 5, ids.frame_3d);
        loomora_project_close(l);
    }
    check(accepted_prefixes(all, 600, 173) == 0, "multi-light: every sampled truncation is rejected");

    // one: a single result
    Bytes one, one2;
    check(export_light(p, &ids.result_3d, 1, one) && export_light(p, &ids.result_3d, 1, one2) && one == one2,
          "one: exporting twice gives the same bytes");
    const std::string hex_one = digest(one);
    check(hex_one == kGoldenOne, "one: SHA-256 of the archive equals the golden value");
    if (hex_one != kGoldenOne) {
        std::fputs("  one sha256 = ", stdout);
        std::fputs(hex_one.c_str(), stdout);
        std::fputs("\n", stdout);
    }
    check(one.size() < all.size(), "one: smaller than multi-light");
    loomora_project *o = 0;
    check(open_light(one, &o), "one: reads back as a light project, one warning");
    if (o != 0) {
        check(loomora_project_count(o, LOOMORA_KIND_FRAME) == 1 && loomora_project_count(o, LOOMORA_KIND_LAYOUT) == 1 &&
                  loomora_project_count(o, LOOMORA_KIND_IMAGE) == 2 && loomora_project_count(o, LOOMORA_KIND_SPOOL) == 2 &&
                  loomora_project_count(o, LOOMORA_KIND_RESULT) == 1,
              "one: item counts (the result and what it uses)");
        std::uint32_t got[2] = {0, 0};
        check(loomora_project_ids(o, LOOMORA_KIND_RESULT, got, 2) == 1 && got[0] == ids.result_3d, "one: the result keeps its identifier");
        check(loomora_project_ids(o, LOOMORA_KIND_FRAME, got, 2) == 1 && got[0] == ids.frame_3d, "one: the frame keeps its identifier");
        check(loomora_project_ids(o, LOOMORA_KIND_LAYOUT, got, 2) == 1 && got[0] == ids.layout_3d, "one: the layout keeps its identifier");
        check(loomora_project_ids(o, LOOMORA_KIND_IMAGE, got, 2) == 2 && got[0] == ids.image[2] && got[1] == ids.image[3],
              "one: the two images of the views");
        check(loomora_project_ids(o, LOOMORA_KIND_SPOOL, got, 2) == 2 && got[0] == ids.spool[4] && got[1] == ids.spool[5],
              "one: the two spools of the result");
        std::uint8_t probe[8];
        check(loomora_project_item_json(o, LOOMORA_KIND_SPOOL, ids.spool[0], probe, sizeof probe) < 0 &&
                  loomora_project_item_json(o, LOOMORA_KIND_RESULT, ids.result_a1, probe, sizeof probe) < 0,
              "one: the items the result does not use are not in the file");
        check(loomora_result_step_count(o, ids.result_3d) == multi_project::kMultiSteps3D && same_step_list(p, o, ids.result_3d) &&
                  same_nail_list(p, o, ids.layout_3d) && same_mesh_geometry(p, o, ids.frame_3d),
              "one: steps, nails and mesh geometry equal those of the full file");
        const std::uint32_t images[2] = {ids.image[2], ids.image[3]};
        check_light_content(o, "one", images, 2, &ids.result_3d, 1, ids.frame_3d);
        loomora_project_close(o);
    }
    // The subset of a light file is the same file as the light subset of the full file.
    loomora_project *l2 = 0;
    Bytes via_light;
    if (open_light(all, &l2)) {
        check(export_light(l2, &ids.result_3d, 1, via_light) && via_light == one,
              "one: exported from multi-light it is the same file as exported from multi");
        loomora_project_close(l2);
    } else {
        check(false, "one: multi-light opens for the second export");
    }
    check(accepted_prefixes(one, one.size(), 7) == 0, "one: every sampled truncation is rejected");
    loomora_project_close(p);
}

} // namespace

int main() {
    int32_t major = 0;
    int32_t minor = -1;
    check(loomora_abi_version() == 1u, "ABI version stays 1");
    check(loomora_format_version(&major, &minor) == 1u && major == 1 && minor == 0,
          "format version 1.0");
    const uint8_t zeros[16] = {0};
    char uuid[37];
    check(loomora_uuid4_from_random(zeros, uuid) == 0 &&
              std::strcmp(uuid, "00000000-0000-4000-8000-000000000000") == 0,
          "uuid4 from random");
    test_basic();
    test_empty();
    test_escape();
    test_multi();
    test_light();
    std::fputs(g_failures == 0 ? "format selftest: ok\n" : "format selftest: FAILED\n", stdout);
    return g_failures == 0 ? 0 : 1;
}

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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../include/loomora.h"
#include "basic_project.hpp"
#include "format/sha256.hpp"

namespace {

// SHA-256 of the archive written for each reference project (modification time kModifiedAt). The
// same constants must hold on Windows (clang, MinGW-w64) and Linux (g++): measured on Windows
// clang and Linux g++ (Ubuntu 26.04), 2026-11-17. T060 replaces them when the format changes.
const char kGoldenBasic[] = "fcdf97f0257076ba1c3ecae1867e57ebdc397440286a278f2cbe34eff8ce5f96";
const char kGoldenEmpty[] = "8ca4dc6a2139539feb87f92b11cc0d5685fdff051fa044b692a7be5d984d7913";
const char kGoldenEscape[] = "b17920aabf9cc165a5c3b9290ba15f22e99903592426a8e55c96dc67ae570e2a";

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
    std::fputs(g_failures == 0 ? "format selftest: ok\n" : "format selftest: FAILED\n", stdout);
    return g_failures == 0 ? 0 : 1;
}

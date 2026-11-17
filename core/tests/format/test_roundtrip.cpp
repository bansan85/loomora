// T027, User Story 1: what the writer writes the reader reads back, field by field, through the PUBLIC
// C API only (loomora.h): the `basic` reference project (one 2D frame, one layout of 200 nails, one
// image with a PNG, 3 spools, one result of 5,000 steps), the `empty` project, a result selecting 40
// spools (escape path of sequence.bin), determinism (FR-004) and the caller's modification time.
// The expectations are stated here, independently of the writer: item JSON as literals, nails and
// steps recomputed from the same formulas as core/tests/basic_project.hpp, pictures as the PNGs the
// test supplied.
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "fs_test_util.hpp"

using basic_project::Bytes;

namespace {

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *out = static_cast<Bytes *>(user);
    out->insert(out->end(), data, data + len);
    return 0;
}

Bytes save_bytes(loomora_editor *e, const char *modified = basic_project::kModifiedAt) {
    Bytes out;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_stream(e, modified, 0, collect, &out, &report) == 0);
    if (report != nullptr) {
        // The writer does not warn about the reference projects.
        CHECK(loomora_report_count(report) == 0);
        loomora_report_free(report);
    }
    return out;
}

// Opens `bytes` and requires an empty report: the reader finds nothing to say about what the writer wrote.
struct Opened {
    loomora_project *p;
    explicit Opened(const Bytes &bytes) : p(nullptr) {
        loomora_report *report = nullptr;
        const int32_t rc = loomora_project_open_memory(bytes.data(), bytes.size(), nullptr, nullptr, &p, &report);
        REQUIRE(rc == 0);
        REQUIRE(p != nullptr);
        if (report != nullptr) {
            CHECK(loomora_report_count(report) == 0);
            CHECK(loomora_report_error_count(report) == 0);
            loomora_report_free(report);
        }
    }
    ~Opened() { loomora_project_close(p); }

  private:
    Opened(const Opened &);
    Opened &operator=(const Opened &);
};

Bytes read_all(const loomora_project *p, const char *entry) {
    const std::int64_t size = loomora_project_entry_size(p, entry);
    REQUIRE(size >= 0);
    Bytes out(static_cast<std::size_t>(size));
    if (size > 0)
        REQUIRE(loomora_project_read_entry(p, entry, 0, &out[0], out.size()) == size);
    return out;
}

// Parses the JSON of an item or of an entry.
void parse(const Bytes &b, rapidjson::Document &d) {
    d.Parse(reinterpret_cast<const char *>(b.data()), b.size());
    REQUIRE_FALSE(d.HasParseError());
}

void parse_text(const std::string &s, rapidjson::Document &d) {
    d.Parse(s.c_str(), s.size());
    REQUIRE_FALSE(d.HasParseError());
}

Bytes item_json(const loomora_project *p, int32_t kind, std::uint32_t id) {
    const std::int64_t n = loomora_project_item_json(p, kind, id, nullptr, 0);
    REQUIRE(n > 0);
    Bytes out(static_cast<std::size_t>(n));
    REQUIRE(loomora_project_item_json(p, kind, id, &out[0], out.size()) == n);
    return out;
}

// Compares the item JSON read back with the expected literal (member by member, order ignored).
void check_item(const loomora_project *p, int32_t kind, std::uint32_t id, const std::string &expected) {
    rapidjson::Document got, want;
    parse(item_json(p, kind, id), got);
    parse_text(expected, want);
    INFO("kind " << kind << " id " << id << ": " << expected);
    CHECK(got == want);
}

std::vector<std::uint32_t> ids(const loomora_project *p, int32_t kind) {
    const std::int64_t n = loomora_project_count(p, kind);
    REQUIRE(n >= 0);
    std::vector<std::uint32_t> v(static_cast<std::size_t>(n) + 1);
    REQUIRE(loomora_project_ids(p, kind, &v[0], static_cast<std::size_t>(n)) == n);
    v.resize(static_cast<std::size_t>(n));
    return v;
}

std::vector<std::uint32_t> list(std::uint32_t a, std::uint32_t b) {
    std::vector<std::uint32_t> v;
    for (std::uint32_t i = a; i <= b; ++i)
        v.push_back(i);
    return v;
}

std::string text(const loomora_project *p, const char *entry) {
    const Bytes b = read_all(p, entry);
    return std::string(b.begin(), b.end());
}

std::string spool_json(unsigned id) {
    const char *color = (id - 1) % 3 == 0 ? "#000000" : (id - 1) % 3 == 1 ? "#ff0000" : "#0000ff";
    return "{\"id\":" + basic_project::num(id) + ",\"name\":\"Spool " + basic_project::num(id) + "\",\"color\":\"" +
           color + "\",\"opacity\":1,\"diameter_mm\":0.3,\"available_length_mm\":200000,\"material\":\"cotton\"," +
           "\"domain\":\"exterior\"}";
}

// The nails of add_frame_and_layout, recomputed.
void check_nails(const loomora_project *p, std::uint32_t layout) {
    REQUIRE(loomora_layout_nail_count(p, layout) == basic_project::kNailCount);
    std::vector<loomora_nail> nails(basic_project::kNailCount);
    REQUIRE(loomora_layout_read_nails(p, layout, 0, nails.size(), &nails[0]) == basic_project::kNailCount);
    for (std::uint32_t i = 0; i < basic_project::kNailCount; ++i) {
        const double a = 6.283185307179586 * i / basic_project::kNailCount;
        const loomora_nail &n = nails[i];
        INFO("nail " << i);
        CHECK(n.id == i);
        CHECK(n.kind == 1u);
        CHECK(n.flags == (i == 0 ? 2u : 0u)); // bit 1: has a label (nail 0 is labelled "first")
        CHECK(n.side == 0u);
        CHECK(n.x == basic_project::round3(200.0 * std::cos(a)));
        CHECK(n.y == basic_project::round3(200.0 * std::sin(a)));
        CHECK(n.z == 0.0);
        CHECK(n.diameter == 2.0);
        CHECK(n.length == 0.0);
        CHECK(n.face_triangle == 0u);
    }
    // A range in the middle gives the same nails as the whole read.
    loomora_nail part[3];
    REQUIRE(loomora_layout_read_nails(p, layout, 100, 3, part) == 3);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(part[i].id == nails[100 + i].id);
        CHECK(part[i].x == nails[100 + i].x);
        CHECK(part[i].y == nails[100 + i].y);
    }
    // The label of nail 0 only.
    Bytes label(16);
    const std::int64_t n = loomora_layout_nail_label(p, layout, 0, &label[0], label.size());
    REQUIRE(n == 5);
    CHECK(std::string(label.begin(), label.begin() + 5) == "first");
    CHECK(loomora_layout_nail_label(p, layout, 1, nullptr, 0) <= 0);
}

// The steps of add_result, recomputed.
void check_steps(const loomora_project *p, std::uint32_t result, unsigned spool_count, std::uint32_t steps) {
    REQUIRE(loomora_result_step_count(p, result) == steps);
    std::vector<loomora_step> got(steps);
    REQUIRE(loomora_result_read_steps(p, result, 0, got.size(), &got[0]) == steps);
    for (std::uint32_t i = 0; i < steps; ++i) {
        const bool first = i < spool_count;
        const bool last = i >= steps - spool_count;
        const std::uint32_t spool = first ? i : last ? i - (steps - spool_count) : (i * 5u + 1u) % spool_count;
        INFO("step " << i);
        CHECK(got[i].spool_index == spool);
        CHECK(got[i].nail == (i * 7u + spool * 13u) % basic_project::kNailCount);
        CHECK(got[i].rotation == (i & 1u));
        CHECK(got[i].type == (first ? 1 : last ? 2 : 0));
        CHECK(got[i].level == 0);
    }
    // A range read in the middle.
    loomora_step part[4];
    REQUIRE(loomora_result_read_steps(p, result, steps / 2, 4, part) == 4);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(part[i].nail == got[steps / 2 + i].nail);
        CHECK(part[i].spool_index == got[steps / 2 + i].spool_index);
    }
    // Past the end: nothing.
    CHECK(loomora_result_read_steps(p, result, steps, 4, part) == 0);
}

} // namespace

TEST_CASE("basic round trip: every field is read back", "[format][roundtrip]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Bytes bytes = save_bytes(e);
    loomora_editor_close(e);
    Opened o(bytes);
    const loomora_project *p = o.p;

    CHECK(loomora_project_is_light(p) == 0);
    CHECK(loomora_project_count(p, LOOMORA_KIND_FRAME) == 1);
    CHECK(loomora_project_count(p, LOOMORA_KIND_LAYOUT) == 1);
    CHECK(loomora_project_count(p, LOOMORA_KIND_IMAGE) == 1);
    CHECK(loomora_project_count(p, LOOMORA_KIND_SPOOL) == 3);
    CHECK(loomora_project_count(p, LOOMORA_KIND_RESULT) == 1);
    CHECK(ids(p, LOOMORA_KIND_SPOOL) == list(1, 3));
    CHECK(ids(p, LOOMORA_KIND_FRAME) == list(1, 1));
    CHECK(ids(p, LOOMORA_KIND_LAYOUT) == list(1, 1));
    CHECK(ids(p, LOOMORA_KIND_IMAGE) == list(1, 1));
    CHECK(ids(p, LOOMORA_KIND_RESULT) == list(1, 1));

    // manifest.json
    {
        const std::int64_t n = loomora_project_manifest(p, nullptr, 0);
        REQUIRE(n > 0);
        Bytes mb(static_cast<std::size_t>(n));
        REQUIRE(loomora_project_manifest(p, &mb[0], mb.size()) == n);
        rapidjson::Document m;
        parse(mb, m);
        CHECK(std::string(m["project_id"].GetString()) == "9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d");
        CHECK(std::string(m["title"].GetString()) == "Living room");
        CHECK(std::string(m["created_at"].GetString()) == basic_project::kCreatedAt);
        CHECK(std::string(m["modified_at"].GetString()) == basic_project::kModifiedAt);
        CHECK(std::string(m["profile"].GetString()) == "full");
        CHECK(m["editable"].GetBool());
        CHECK(m["format"]["major"].GetInt() == 1);
        CHECK(m["format"]["minor"].GetInt() == 0);
        CHECK(m["counts"]["spools"].GetInt() == 3);
        CHECK(m["next_id"]["spool"].GetInt() == 4);
        CHECK(m["next_id"]["result"].GetInt() == 2);
        CHECK(m["required_features"].Size() == 0);
    }

    // Item JSON.
    for (unsigned i = 1; i <= 3; ++i)
        check_item(p, LOOMORA_KIND_SPOOL, i, spool_json(i));
    check_item(p, LOOMORA_KIND_FRAME, 1,
               "{\"id\":1,\"name\":\"Square 50\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
               "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
               "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},\"exclusions\":[],"
               "\"surface\":\"plane\"}");
    check_item(p, LOOMORA_KIND_LAYOUT, 1, "{\"id\":1,\"name\":\"Circle 200\",\"frame\":1,\"nail_count\":200}");
    check_item(p, LOOMORA_KIND_IMAGE, 1,
               "{\"id\":1,\"name\":\"Portrait\",\"color_mode\":\"bw\",\"shape\":\"circle\",\"width_px\":8,"
               "\"height_px\":8,\"source\":{\"entry\":\"images/1/source.png\",\"format\":\"png\",\"size\":140},"
               "\"target\":{\"entry\":\"images/1/target.png\",\"format\":\"png\",\"size\":140},"
               "\"thumbnail\":{\"entry\":\"images/1/thumb.png\"}}");
    check_item(p, LOOMORA_KIND_RESULT, 1,
               "{\"id\":1,\"name\":\"Portrait on circle\",\"created_at\":\"2026-11-17T09:30:00Z\",\"frame\":1,"
               "\"layout\":1,\"images\":[{\"image\":1}],\"spools\":[{\"spool\":1,\"length_mm\":1000},{\"spool\":2,"
               "\"length_mm\":1010},{\"spool\":3,\"length_mm\":1020}],\"steps\":5000,\"levels\":false,"
               "\"thumbnail\":{\"entry\":\"results/1/thumb.png\"},\"settings\":{\"entry\":\"results/1/settings.json\"}}");

    // Unknown identifiers and kinds.
    CHECK(loomora_project_item_json(p, LOOMORA_KIND_SPOOL, 99, nullptr, 0) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_project_count(p, 99) < 0);

    // Bulk data.
    check_nails(p, 1);
    check_steps(p, 1, 3, basic_project::kStepCount);

    // Pictures and settings come back byte for byte (nothing to strip in these PNGs).
    CHECK(read_all(p, "images/1/source.png") == basic_project::make_png(8, 8, 40));
    CHECK(read_all(p, "images/1/target.png") == basic_project::make_png(8, 8, 90));
    CHECK(read_all(p, "images/1/thumb.png") == basic_project::make_png(4, 4, 10));
    CHECK(read_all(p, "results/1/thumb.png") == basic_project::make_png(4, 4, 200));
    CHECK(text(p, "results/1/settings.json") == "{\"algorithm\":\"greedy\",\"seed\":1}");
    CHECK(loomora_project_entry_size(p, "images/1/thumb.png") == 88);
    CHECK(loomora_project_entry_size(p, "images/9/thumb.png") == LOOMORA_ERR_NOT_FOUND);

    // A range read of an entry.
    Bytes tail(8);
    const std::int64_t whole = loomora_project_entry_size(p, "images/1/source.png");
    REQUIRE(loomora_project_read_entry(p, "images/1/source.png", static_cast<std::uint64_t>(whole) - 4, &tail[0],
                                       tail.size()) == 4);
    const Bytes png = basic_project::make_png(8, 8, 40);
    CHECK(std::vector<std::uint8_t>(tail.begin(), tail.begin() + 4) ==
          std::vector<std::uint8_t>(png.end() - 4, png.end()));
}

TEST_CASE("empty round trip: no item", "[format][roundtrip]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "") == 0);
    const Bytes bytes = save_bytes(e);
    loomora_editor_close(e);
    Opened o(bytes);
    for (int32_t k = LOOMORA_KIND_FRAME; k <= LOOMORA_KIND_RESULT; ++k) {
        CHECK(loomora_project_count(o.p, k) == 0);
        CHECK(loomora_project_ids(o.p, k, nullptr, 0) == 0);
    }
    const std::int64_t n = loomora_project_manifest(o.p, nullptr, 0);
    REQUIRE(n > 0);
    Bytes mb(static_cast<std::size_t>(n));
    loomora_project_manifest(o.p, &mb[0], mb.size());
    rapidjson::Document m;
    parse(mb, m);
    CHECK(std::string(m["title"].GetString()).empty());
    CHECK(m["counts"]["frames"].GetInt() == 0);
    CHECK(m["next_id"]["frame"].GetInt() == 1);
    CHECK(loomora_project_entry_size(o.p, "spools.json") >= 0);
}

TEST_CASE("a result selecting 40 spools round trips through the escape path", "[format][roundtrip]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_escape(&e) == 0);
    const Bytes bytes = save_bytes(e);
    loomora_editor_close(e);
    Opened o(bytes);
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_SPOOL) == 40);
    check_steps(o.p, 1, 40, 400);
    std::vector<loomora_step> steps(400);
    REQUIRE(loomora_result_read_steps(o.p, 1, 0, steps.size(), &steps[0]) == 400);
    std::uint32_t top = 0;
    for (std::size_t i = 0; i < steps.size(); ++i)
        top = steps[i].spool_index > top ? steps[i].spool_index : top;
    CHECK(top == 39); // indexes 31 to 39 do appear, hence the escape records
}

TEST_CASE("determinism: the same project gives the same bytes; the time is the caller's", "[format][roundtrip]") {
    loomora_editor *e1 = nullptr, *e2 = nullptr;
    REQUIRE(basic_project::build_basic(&e1) == 0);
    REQUIRE(basic_project::build_basic(&e2) == 0);
    const Bytes a = save_bytes(e1);
    const Bytes again = save_bytes(e1);   // the same editor twice
    const Bytes other = save_bytes(e2);   // an identical project built separately
    CHECK(a == again);
    CHECK(a == other);
    const Bytes later = save_bytes(e1, "2027-01-02T03:04:05Z");
    CHECK(later != a); // the manifest carries the time
    const Bytes back = save_bytes(e1);
    CHECK(back == a); // saving with another time changed nothing in the editor
    Opened o(later);
    rapidjson::Document m;
    const std::int64_t n = loomora_project_manifest(o.p, nullptr, 0);
    Bytes mb(static_cast<std::size_t>(n));
    loomora_project_manifest(o.p, &mb[0], mb.size());
    parse(mb, m);
    CHECK(std::string(m["modified_at"].GetString()) == "2027-01-02T03:04:05Z");
    CHECK(std::string(m["created_at"].GetString()) == basic_project::kCreatedAt);
    loomora_editor_close(e1);
    loomora_editor_close(e2);
}

TEST_CASE("save to a path, then open the path", "[format][roundtrip]") {
    fstest::ScratchDir dir("roundtrip");
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const std::string path = dir.file("basic.loomora");
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, &report) == 0);
    CHECK(loomora_report_count(report) == 0);
    loomora_report_free(report);
    const Bytes streamed = save_bytes(e);
    loomora_editor_close(e);
    Bytes onDisk;
    REQUIRE(fstest::read_file(path, onDisk));
    CHECK(onDisk == streamed);
    CHECK(dir.names().size() == 1); // no temporary file left

    loomora_project *p = nullptr;
    report = nullptr;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &p, &report) == 0);
    CHECK(loomora_report_count(report) == 0);
    loomora_report_free(report);
    CHECK(loomora_result_step_count(p, 1) == basic_project::kStepCount);
    check_nails(p, 1);
    loomora_project_close(p);
    CHECK(dir.names().size() == 1); // reading created nothing
}

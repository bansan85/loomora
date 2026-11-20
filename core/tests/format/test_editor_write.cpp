// The creation part of the project editor and the project writer (T033, T034) through the public C
// API: the `basic`, `empty` and 40-spool projects are built, saved to memory and to a file, and the
// archive is checked with the ZIP reader, a JSON parse of the manifest, the SHA-256 and size of every
// entry, the codecs, and the determinism rules (FR-004: the same bytes when saved twice on one platform).
// No reader of the project is used.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../../src/c_api_project_handles.hpp"
#include "../basic_project.hpp"
#include "format/codec/nails_codec.hpp"
#include "format/codec/sequence_codec.hpp"
#include "format/io/memory_io.hpp"
#include "format/json/json_in.hpp"
#include "format/sha256.hpp"
#include "format/zip/zip_reader.hpp"
#include "fs_test_util.hpp"

using namespace loomora::format;
using basic_project::Bytes;

static_assert(sizeof(loomora_nail) == 88, "loomora_nail is 88 bytes");
static_assert(sizeof(loomora_step) == 12, "loomora_step is 12 bytes");
static_assert(sizeof(loomora_mesh_info) == 24, "loomora_mesh_info is 24 bytes");

namespace {

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *b = static_cast<Bytes *>(user);
    b->insert(b->end(), data, data + len);
    return 0;
}

Bytes save_memory(loomora_editor *e, const char *modified = basic_project::kModifiedAt, std::uint32_t options = 0) {
    Bytes out;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_stream(e, modified, options, collect, &out, &report) == 0);
    delete report; // allocated with new by the editor; the free function belongs to the reader side
    return out;
}

// The archive opened with the ZIP reader (the source lives as long as the object).
struct Archive {
    Bytes bytes;
    MemorySource source;
    ZipReader zip;
    explicit Archive(const Bytes &b) : bytes(b), source(bytes.empty() ? nullptr : &bytes[0], bytes.size()), zip(source) {
        zip.open();
    }
    Bytes read(const std::string &name) const {
        const ZipEntry *e = zip.find(name);
        REQUIRE(e != nullptr);
        return zip.read_all(*e, 1ull << 30);
    }
    std::vector<std::string> names() const {
        std::vector<std::string> n;
        for (std::size_t i = 0; i < zip.entries().size(); ++i)
            n.push_back(zip.entries()[i].name);
        return n;
    }
};

std::vector<std::string> basic_names() {
    const char *const n[] = {"mimetype",
                             "manifest.json",
                             "spools.json",
                             "frames/1/frame.json",
                             "frames/1/layouts/1/layout.json",
                             "frames/1/layouts/1/nails.json",
                             "images/1/image.json",
                             "images/1/source.png",
                             "images/1/target.png",
                             "images/1/thumb.png",
                             "results/1/result.json",
                             "results/1/sequence.bin",
                             "results/1/thumb.png",
                             "results/1/settings.json"};
    return std::vector<std::string>(n, n + sizeof n / sizeof n[0]);
}

// manifest.json consistent with the archive: every other entry listed once with its size and SHA-256.
void check_manifest_entries(const Archive &a, json::Document &manifest, bool all_stored = false) {
    const Bytes text = a.read("manifest.json");
    json::parse_document(reinterpret_cast<const char *>(text.data()), text.size(), manifest, "manifest.json");
    json::Fields m(manifest, "manifest.json", "");
    json::Fields entries = m.object("entries");
    std::set<std::string> listed;
    const json::Value &obj = m.get("entries");
    for (rapidjson::Value::ConstMemberIterator it = obj.MemberBegin(); it != obj.MemberEnd(); ++it)
        listed.insert(it->name.GetString());
    std::set<std::string> in_zip;
    for (std::size_t i = 0; i < a.zip.entries().size(); ++i) {
        const ZipEntry &e = a.zip.entries()[i];
        if (e.name == "mimetype" || e.name == "manifest.json")
            continue;
        in_zip.insert(e.name);
        const Bytes data = a.read(e.name);
        json::Fields f = entries.object(e.name.c_str());
        CHECK(f.u64("size") == data.size());
        CHECK(f.string("sha256", 64) == sha256_hex(data.empty() ? nullptr : &data[0], data.size()));
        const bool json_entry = e.name.size() > 5 && e.name.compare(e.name.size() - 5, 5, ".json") == 0;
        CHECK(e.method == (json_entry && !all_stored ? 8 : 0));
    }
    CHECK(listed == in_zip);
}

std::vector<std::uint32_t> ids_of(const json::Value &a) {
    std::vector<std::uint32_t> v;
    for (rapidjson::SizeType i = 0; i < a.Size(); ++i)
        v.push_back(a[i].GetUint());
    return v;
}

const std::uint8_t kRandom2[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

} // namespace

TEST_CASE("basic project: entries, order and manifest", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Archive a(save_memory(e));
    loomora_editor_close(e);

    CHECK(a.names() == basic_names());
    const ZipEntry &first = a.zip.entries()[0];
    CHECK(first.name == "mimetype");
    CHECK(first.method == 0);
    CHECK(first.local_offset == 0);

    json::Document manifest;
    check_manifest_entries(a, manifest);
    json::Fields m(manifest, "manifest.json", "");
    CHECK(m.object("format").string("name", 16) == "loomora");
    CHECK(m.object("format").u32("major") == 1);
    CHECK(m.object("format").u32("minor") == 0);
    CHECK(m.string("min_reader", 8) == "1.0");
    CHECK(m.string("project_id", 40) == "9b1deb4d-3b7d-4bad-9bdd-2b0d7b3dcb6d");
    CHECK(m.string("title", 64) == "Living room");
    CHECK(m.string("created_at", 32) == basic_project::kCreatedAt);
    CHECK(m.string("modified_at", 32) == basic_project::kModifiedAt);
    CHECK(m.string("profile", 8) == "full");
    CHECK(m.boolean("editable"));
    json::Fields counts = m.object("counts");
    CHECK(counts.u32("frames") == 1);
    CHECK(counts.u32("layouts") == 1);
    CHECK(counts.u32("images") == 1);
    CHECK(counts.u32("spools") == 3);
    CHECK(counts.u32("results") == 1);
    json::Fields next = m.object("next_id");
    CHECK(next.u32("frame") == 2);
    CHECK(next.u32("layout") == 2);
    CHECK(next.u32("image") == 2);
    CHECK(next.u32("spool") == 4);
    CHECK(next.u32("result") == 2);
    json::Fields order = m.object("order");
    CHECK(ids_of(order.array("spools")) == std::vector<std::uint32_t>({1, 2, 3}));
    CHECK(ids_of(order.array("results")) == std::vector<std::uint32_t>({1}));
    CHECK(m.array("required_features").Size() == 0);
    CHECK(m.get("extensions").IsObject());

    // JSON entries are compact (no whitespace outside strings) and in schema key order.
    const Bytes frame = a.read("frames/1/frame.json");
    const std::string f(frame.begin(), frame.end());
    CHECK(f.compare(0, 60, "{\"id\":1,\"name\":\"Square 50\",\"kind\":\"2d\",\"dimensions\":{\"width_") == 0);
    CHECK(f.find(", ") == std::string::npos);
    CHECK(f.find(": ") == std::string::npos);
    const Bytes layout = a.read("frames/1/layouts/1/layout.json");
    CHECK(std::string(layout.begin(), layout.end()) == "{\"id\":1,\"name\":\"Circle 200\",\"frame\":1,\"nail_count\":200}");
    const Bytes image = a.read("images/1/image.json");
    CHECK(std::string(image.begin(), image.end()).find("\"source\":{\"entry\":\"images/1/source.png\",\"format\":\"png\",\"size\":") !=
          std::string::npos);
}

TEST_CASE("basic project: nails, sequence and pictures decode", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Archive a(save_memory(e));
    loomora_editor_close(e);

    const Bytes nails_text = a.read("frames/1/layouts/1/nails.json");
    json::MemoryByteReader reader(nails_text.data(), nails_text.size());
    model::NailTable table;
    codec::read_nails(reader, false, table, "nails.json", 200);
    REQUIRE(table.size() == 200);
    CHECK(table.id[0] == 0);
    CHECK(table.id[199] == 199);
    CHECK(table.x[0] == 200.0);
    CHECK(table.kind[0] == model::kNailBoundary);
    REQUIRE(table.label_at(0) != nullptr);
    CHECK(*table.label_at(0) == "first");
    CHECK(table.label_at(1) == nullptr);

    const Bytes seq = a.read("results/1/sequence.bin");
    MemorySource src(seq.data(), seq.size());
    std::vector<SequenceStep> steps;
    std::uint32_t total = 0;
    bool levels = true;
    decode_sequence(src, &steps, 0, ~static_cast<std::uint64_t>(0), &total, &levels);
    CHECK(total == basic_project::kStepCount);
    CHECK_FALSE(levels);
    REQUIRE(steps.size() == basic_project::kStepCount);
    CHECK(steps[0].type == kStepStart);
    CHECK(steps[2].spool_index == 2);
    CHECK(steps[100].type == kStepSegment);
    CHECK(steps.back().type == kStepEnd);

    const Bytes result = a.read("results/1/result.json");
    const std::string r(result.begin(), result.end());
    CHECK(r.find("\"steps\":5000,\"levels\":false,\"thumbnail\":{\"entry\":\"results/1/thumb.png\"},\"settings\":{\"entry\":\"results/1/settings.json\"}}") !=
          std::string::npos);
    const Bytes settings = a.read("results/1/settings.json");
    CHECK(std::string(settings.begin(), settings.end()) == "{\"algorithm\":\"greedy\",\"seed\":1}");
    CHECK(a.read("images/1/thumb.png") == basic_project::make_png(4, 4, 10));
    CHECK(a.read("images/1/source.png") == basic_project::make_png(8, 8, 40));
}

TEST_CASE("empty project", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "") == 0);
    const Archive a(save_memory(e));
    loomora_editor_close(e);
    CHECK(a.names() == std::vector<std::string>({"mimetype", "manifest.json", "spools.json"}));
    json::Document manifest;
    check_manifest_entries(a, manifest);
    json::Fields m(manifest, "manifest.json", "");
    CHECK(m.object("counts").u32("frames") == 0);
    CHECK(m.object("next_id").u32("result") == 1);
    CHECK(m.array("required_features").Size() == 0);
    const Bytes spools = a.read("spools.json");
    CHECK(std::string(spools.begin(), spools.end()) == "{\"spools\":[]}");
}

TEST_CASE("a result selecting 40 spools uses the escape path", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_escape(&e) == 0);
    const Archive a(save_memory(e));
    loomora_editor_close(e);
    const Bytes seq = a.read("results/1/sequence.bin");
    MemorySource src(seq.data(), seq.size());
    std::vector<SequenceStep> steps;
    decode_sequence(src, &steps);
    REQUIRE(steps.size() == 400);
    std::set<std::uint32_t> used;
    for (std::size_t i = 0; i < steps.size(); ++i)
        used.insert(steps[i].spool_index);
    for (std::uint32_t k = 0; k < 40; ++k)
        CHECK(used.count(k) == 1);
    CHECK(steps[35].spool_index == 35);
    json::Document manifest;
    check_manifest_entries(a, manifest);
}

TEST_CASE("determinism: same project, same bytes; the time comes from the caller", "[format][editor]") {
    loomora_editor *e1 = nullptr, *e2 = nullptr;
    REQUIRE(basic_project::build_basic(&e1) == 0);
    REQUIRE(basic_project::build_basic(&e2) == 0);
    const Bytes a1 = save_memory(e1);
    CHECK(save_memory(e1) == a1);                   // twice from one editor
    CHECK(save_memory(e2) == a1);                   // from an identical editor
    const Bytes later = save_memory(e1, "2026-11-18T10:00:00Z");
    CHECK(later != a1);                              // the manifest carries modified_at
    CHECK(save_memory(e2, "2026-11-18T10:00:00Z") == later);

    // Identity of entries across two saves: only manifest.json differs.
    const Archive x(a1), y(later);
    REQUIRE(x.names() == y.names());
    for (std::size_t i = 0; i < x.zip.entries().size(); ++i) {
        const std::string &n = x.zip.entries()[i].name;
        if (n == "manifest.json")
            CHECK(x.read(n) != y.read(n));
        else
            CHECK(x.read(n) == y.read(n));
    }
    loomora_editor_close(e1);
    loomora_editor_close(e2);
}

TEST_CASE("option store-all stores every entry", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Bytes plain = save_memory(e);
    const Archive a(save_memory(e, basic_project::kModifiedAt, LOOMORA_SAVE_STORE_ALL));
    loomora_editor_close(e);
    for (std::size_t i = 0; i < a.zip.entries().size(); ++i)
        CHECK(a.zip.entries()[i].method == 0);
    CHECK(a.bytes != plain);
    json::Document manifest;
    check_manifest_entries(a, manifest, true);
}

TEST_CASE("save to a file; the file holds the same bytes as the stream", "[format][editor]") {
    fstest::ScratchDir dir("editor");
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const std::string path = dir.file("basic.loomora");
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr, &report) == 0);
    REQUIRE(report != nullptr);
    delete report;
    Bytes file;
    REQUIRE(fstest::read_file(path, file));
    CHECK(file == save_memory(e));
    CHECK(dir.names().size() == 1); // no temporary file left
    loomora_editor_close(e);
}

TEST_CASE("identifiers only increase and orders follow the adds", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e) == 0);
    std::uint32_t id = 0;
    REQUIRE(basic_project::add_spools(e, 2) == 0);
    REQUIRE(loomora_editor_add_spool(e, "{\"id\":99,\"name\":\"x\",\"color\":\"#102030\",\"opacity\":0.5,\"diameter_mm\":1}", &id) == 0);
    CHECK(id == 3); // the "id" of the JSON is ignored
    loomora_editor_close(e);
}

TEST_CASE("the editor refuses what the data model forbids", "[format][editor]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e) == 0);
    std::uint32_t id = 0;
    CHECK(loomora_editor_add_spool(e, "{\"name\":\"x\"", &id) == LOOMORA_ERR_INVALID_ITEM);              // not JSON
    CHECK(loomora_editor_add_spool(e, "{\"name\":\"x\",\"diameter_mm\":-1}", &id) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_add_spool(e, "{\"name\":\"x\",\"diameter_mm\":1,\"color\":\"red\"}", &id) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_add_spool(nullptr, "{}", &id) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_add_spool(e, nullptr, &id) == LOOMORA_ERR_INVALID);
    // A 3D frame needs a 3D surface and a body (3D frames themselves are added: test_editor_dependents.cpp).
    CHECK(loomora_editor_add_frame(e, "{\"name\":\"3d\",\"kind\":\"3d\",\"dimensions\":{\"width_mm\":1,\"height_mm\":1,\"depth_mm\":1},"
                                      "\"outline\":{\"type\":\"circle\",\"polyline\":[]},\"surface\":\"plane\",\"body\":\"solid\"}",
                                   &id) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_add_frame(e, "{\"name\":\"3d\",\"kind\":\"3d\",\"dimensions\":{\"width_mm\":1,\"height_mm\":1},"
                                      "\"outline\":{\"type\":\"circle\",\"polyline\":[]},\"surface\":\"mesh\",\"body\":\"solid\"}",
                                   &id) == LOOMORA_ERR_INVALID_ITEM); // no depth_mm
    CHECK(loomora_editor_add_frame(e, "{\"name\":\"3d\",\"kind\":\"3d\",\"dimensions\":{\"width_mm\":1,\"height_mm\":1,\"depth_mm\":1},"
                                      "\"outline\":{\"type\":\"circle\",\"polyline\":[]},\"surface\":\"mesh\"}",
                                   &id) == LOOMORA_ERR_INVALID_ITEM); // no body
    CHECK(loomora_editor_add_layout(e, 7, "{\"name\":\"l\"}", nullptr, 0, &id) == LOOMORA_ERR_NOT_FOUND);

    // A failed add changes nothing: the next spool still gets identifier 1.
    REQUIRE(loomora_editor_add_spool(e, "{\"name\":\"ok\",\"diameter_mm\":0.3}", &id) == 0);
    CHECK(id == 1);

    uint32_t frame = 0, layout = 0, image = 0;
    REQUIRE(basic_project::add_frame_and_layout(e, &frame, &layout) == 0);
    REQUIRE(basic_project::add_image(e, &image) == 0);
    // A nail with a duplicate identifier.
    loomora_nail two[2];
    std::memset(two, 0, sizeof two);
    two[0].kind = two[1].kind = 1;
    two[0].diameter = two[1].diameter = 2;
    CHECK(loomora_editor_add_layout(e, frame, "{\"name\":\"dup\"}", two, 2, &id) == LOOMORA_ERR_INVALID_ITEM);
    two[1].kind = 9;
    two[1].id = 1;
    CHECK(loomora_editor_add_layout(e, frame, "{\"name\":\"kind\"}", two, 2, &id) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_layout_set_label(e, layout, 5000, "x") == LOOMORA_ERR_NOT_FOUND);

    // Results: bad references, a spool length above the available one, a step outside the layout.
    const Bytes thumb = basic_project::make_png(4, 4, 1);
    const std::string head = "{\"name\":\"r\",\"created_at\":\"2026-11-17T09:30:00Z\",";
    const std::string imgs = "\"images\":[{\"image\":1}],";
    loomora_step step;
    std::memset(&step, 0, sizeof step);
    step.type = 1;
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":9," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &thumb[0], thumb.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":4,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &thumb[0], thumb.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    step.nail = 100000; // not a nail of the layout
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &thumb[0], thumb.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    step.nail = 0;
    step.spool_index = 1; // outside the selection
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &thumb[0], thumb.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    step.spool_index = 0;
    step.type = 0; // a segment of a spool that was never started
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &thumb[0], thumb.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    step.type = 1;
    // A thumbnail that is not a PNG; a bad save time; unknown option bits.
    const Bytes junk(10, 0x42);
    CHECK(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                    &step, 1, &junk[0], junk.size(), nullptr, &id) == LOOMORA_ERR_INVALID_ITEM);
    Bytes out;
    CHECK(loomora_editor_save_stream(e, "yesterday", 0, collect, &out, nullptr) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_save_stream(e, basic_project::kModifiedAt, 2, collect, &out, nullptr) == LOOMORA_ERR_INVALID);
    CHECK(out.empty());
    // The valid one passes, with a report object that holds no issue.
    REQUIRE(loomora_editor_add_result(e, (head + "\"frame\":1,\"layout\":1," + imgs + "\"spools\":[{\"spool\":1,\"length_mm\":10}]}").c_str(),
                                      &step, 1, &thumb[0], thumb.size(), nullptr, &id) == 0);
    CHECK(id == 1);
    loomora_editor_close(e);
}

TEST_CASE("the editor creation arguments are checked", "[format][editor]") {
    loomora_editor *e = nullptr;
    CHECK(loomora_editor_create(nullptr, "t", basic_project::kCreatedAt, &e) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_create(kRandom2, nullptr, basic_project::kCreatedAt, &e) == LOOMORA_ERR_INVALID);
    CHECK(loomora_editor_create(kRandom2, "t", "now", &e) == LOOMORA_ERR_INVALID);
    CHECK(e == nullptr);
    CHECK(loomora_editor_create(kRandom2, std::string(1025, 'x').c_str(), basic_project::kCreatedAt, &e) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_editor_create(kRandom2, "t", basic_project::kCreatedAt, &e) == 0);
    CHECK(loomora_editor_set_title(e, "caf\xC3\xA9") == 0);
    CHECK(loomora_editor_set_title(e, "bad\xFF") == LOOMORA_ERR_INVALID);
    const Archive a(save_memory(e));
    json::Document manifest;
    check_manifest_entries(a, manifest);
    json::Fields m(manifest, "manifest.json", "");
    CHECK(m.string("title", 64) == "caf\xC3\xA9");
    CHECK(m.string("project_id", 64) == "01020304-0506-4708-890a-0b0c0d0e0f10");
    loomora_editor_close(e);
    loomora_editor_close(nullptr);
}

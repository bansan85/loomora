// T056, User Story 4 (FR-020, FR-021, FR-022, SC-007): files written by another version of the format.
// A file of format 1.7 (extra fields in the manifest and in the items, an extra entry under `x-acme/`, an
// extension) opens in this library (1.0), and an edit followed by a save keeps the unknown entry and the
// extension byte for byte; a file of format 2.0 is refused with LOOMORA_ERR_UNSUPPORTED_VERSION and the
// message `This file needs Loomora format version 2.0 or newer`; a required feature this library does not
// know is refused naming it (LOOMORA_ERR_UNSUPPORTED_FEATURE); a project of 2D frames lists no feature that
// only 3D needs. The files are assembled here from the entries of a `basic` project with the test-side
// ZIP builder, so no code of the writer is used to make them.
#include <catch2/catch.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "../multi_project.hpp"
#include "format/sha256.hpp"
#include "fs_test_util.hpp"
#include "zip_builder.hpp"

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
    if (report != nullptr)
        loomora_report_free(report);
    return out;
}

Bytes basic_bytes() {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Bytes b = save_bytes(e);
    loomora_editor_close(e);
    return b;
}

struct Opened {
    Bytes data;
    loomora_project *p;
    explicit Opened(const Bytes &bytes) : data(bytes), p(nullptr) {
        loomora_report *report = nullptr;
        REQUIRE(loomora_project_open_memory(data.data(), data.size(), nullptr, nullptr, &p, &report) == 0);
        REQUIRE(p != nullptr);
        if (report != nullptr)
            loomora_report_free(report);
    }
    ~Opened() { loomora_project_close(p); }

  private:
    Opened(const Opened &);
    Opened &operator=(const Opened &);
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

std::string dump(const rapidjson::Value &v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

std::string report_text(const loomora_report *r, std::size_t index, int32_t field) {
    const std::int64_t n = loomora_report_text(r, index, field, nullptr, 0);
    REQUIRE(n >= 0);
    std::string s(static_cast<std::size_t>(n), '\0');
    if (n > 0)
        REQUIRE(loomora_report_text(r, index, field, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
    return s;
}

// What a skewed file is made of.
struct Skew {
    int major, minor;
    std::string min_reader;
    bool set_features;
    std::vector<std::string> features;
    bool extras;   // extra fields in the manifest and in the items, an extension
    bool x_entry;  // an entry under x-acme/
    Skew() : major(1), minor(7), min_reader("1.0"), set_features(false), extras(true), x_entry(true) {}
};

const char kXEntryName[] = "x-acme/notes.bin";
const unsigned char kXEntryBytes[] = {0x00, 0xFF, 0x10, 0x80, 'a', 'c', 'm', 'e', 0x0D, 0x0A, 0x00, 0x7F};

Bytes x_entry_bytes() { return Bytes(kXEntryBytes, kXEntryBytes + sizeof kXEntryBytes); }

void add_member(rapidjson::Document &d, rapidjson::Value &obj, const char *key, rapidjson::Value &value) {
    obj.AddMember(rapidjson::Value(key, d.GetAllocator()).Move(), value, d.GetAllocator());
}

// Adds a member to the object of `bytes` (a JSON entry) and returns the compact JSON.
Bytes with_extra_member(const Bytes &bytes, const char *key, const char *json_value, const char *array_key = nullptr) {
    rapidjson::Document d;
    parse(bytes, d);
    rapidjson::Document value;
    value.Parse(json_value);
    REQUIRE_FALSE(value.HasParseError());
    if (array_key == nullptr) {
        rapidjson::Value v(value, d.GetAllocator());
        add_member(d, d, key, v);
    } else {
        for (rapidjson::SizeType i = 0; i < d[array_key].Size(); ++i) {
            rapidjson::Value v(value, d.GetAllocator());
            add_member(d, d[array_key][i], key, v);
        }
    }
    const std::string s = dump(d);
    return Bytes(s.begin(), s.end());
}

// Assembles a file from the entries of `base` with the changes of `skew`; sizes and SHA-256 of the manifest follow.
Bytes make_skewed(const Bytes &base, const Skew &skew) {
    Opened o(base);
    rapidjson::Document manifest;
    parse(read_entry(o.p, "manifest.json"), manifest);
    std::vector<std::string> names;
    std::map<std::string, Bytes> data;
    for (rapidjson::Value::ConstMemberIterator it = manifest["entries"].MemberBegin(); it != manifest["entries"].MemberEnd(); ++it) {
        names.push_back(it->name.GetString());
        data[names.back()] = read_entry(o.p, names.back());
    }
    if (skew.extras) {
        data["frames/1/frame.json"] = with_extra_member(data["frames/1/frame.json"], "future_frame", "{\"depth\":[1,2,{\"x\":null}]}");
        data["frames/1/layouts/1/layout.json"] = with_extra_member(data["frames/1/layouts/1/layout.json"], "tag", "\"acme\"");
        data["images/1/image.json"] = with_extra_member(data["images/1/image.json"], "license", "\"cc0\"");
        data["results/1/result.json"] = with_extra_member(data["results/1/result.json"], "rating", "5");
        data["spools.json"] = with_extra_member(data["spools.json"], "supplier", "\"acme\"", "spools");
    }
    if (skew.x_entry) {
        names.push_back(kXEntryName);
        data[kXEntryName] = x_entry_bytes();
    }
    // The manifest.
    rapidjson::Document::AllocatorType &al = manifest.GetAllocator();
    manifest["format"]["major"] = skew.major;
    manifest["format"]["minor"] = skew.minor;
    manifest["min_reader"].SetString(skew.min_reader.c_str(), al);
    if (skew.set_features) {
        manifest["required_features"].SetArray();
        for (std::size_t i = 0; i < skew.features.size(); ++i)
            manifest["required_features"].PushBack(rapidjson::Value(skew.features[i].c_str(), al).Move(), al);
    }
    manifest["entries"].SetObject();
    for (std::size_t i = 0; i < names.size(); ++i) {
        rapidjson::Value e(rapidjson::kObjectType);
        e.AddMember("size", static_cast<std::uint64_t>(data[names[i]].size()), al);
        const std::string hex = loomora::format::sha256_hex(data[names[i]].empty() ? nullptr : &data[names[i]][0], data[names[i]].size());
        e.AddMember("sha256", rapidjson::Value(hex.c_str(), al).Move(), al);
        manifest["entries"].AddMember(rapidjson::Value(names[i].c_str(), al).Move(), e, al);
    }
    if (skew.extras) {
        rapidjson::Value ext(rapidjson::kObjectType);
        rapidjson::Document inner;
        inner.Parse("{\"mode\":\"fancy\",\"levels\":[1,2,3]}");
        ext.AddMember("acme.render", rapidjson::Value(inner, al).Move(), al);
        ext.AddMember("acme.flag", true, al);
        manifest["extensions"] = ext;
        rapidjson::Document future;
        future.Parse("{\"since\":\"1.7\",\"items\":[1,2]}");
        manifest.AddMember("future_block", rapidjson::Value(future, al).Move(), al);
        manifest["generator"].AddMember("build_flavour", "nightly", al);
        manifest["format"].AddMember("codename", "seven", al);
    }
    const std::string mtext = dump(manifest);
    std::vector<ziptest::Entry> entries;
    entries.push_back(ziptest::file("manifest.json", mtext, 8));
    for (std::size_t i = 0; i < names.size(); ++i) {
        ziptest::Entry e;
        e.name = names[i];
        e.data = data[names[i]];
        e.method = (names[i].size() > 4 && names[i].compare(names[i].size() - 4, 4, ".png") == 0) ? 0 : 8;
        entries.push_back(e);
    }
    return ziptest::build(ziptest::with_mimetype(entries)).bytes;
}

// Opens `bytes` expecting a refusal; returns the code, fills the first error's code and message.
int32_t refused(const Bytes &bytes, std::string &issue_code, std::string &message) {
    loomora_project *p = nullptr;
    loomora_report *report = nullptr;
    const int32_t rc = loomora_project_open_memory(bytes.data(), bytes.size(), nullptr, nullptr, &p, &report);
    CHECK(p == nullptr);
    if (p != nullptr)
        loomora_project_close(p);
    REQUIRE(report != nullptr);
    REQUIRE(loomora_report_error_count(report) >= 1);
    issue_code = report_text(report, 0, 1);
    message = report_text(report, 0, 3);
    loomora_report_free(report);
    return rc;
}

} // namespace

TEST_CASE("version skew: a file of format 1.7 with extra fields and an x-acme entry opens", "[version][format]") {
    const Bytes file = make_skewed(basic_bytes(), Skew());
    loomora_project *p = nullptr;
    loomora_report *report = nullptr;
    REQUIRE(loomora_project_open_memory(file.data(), file.size(), nullptr, nullptr, &p, &report) == 0);
    REQUIRE(p != nullptr);
    REQUIRE(report != nullptr);
    CHECK(loomora_report_error_count(report) == 0);
    // The only thing to say: the entry this version does not define is kept.
    REQUIRE(loomora_report_count(report) == 1);
    CHECK(report_text(report, 0, 1) == "warn.unknown_entry");
    CHECK(report_text(report, 0, 2) == kXEntryName);
    loomora_report_free(report);
    CHECK(loomora_project_count(p, LOOMORA_KIND_FRAME) == 1);
    CHECK(loomora_project_count(p, LOOMORA_KIND_SPOOL) == 3);
    CHECK(loomora_project_count(p, LOOMORA_KIND_RESULT) == 1);
    CHECK(loomora_result_step_count(p, 1) == basic_project::kStepCount);
    CHECK(read_entry(p, kXEntryName) == x_entry_bytes());
    // The manifest says what it says (the unknown fields are in the file the reader was given).
    rapidjson::Document m;
    parse(read_entry(p, "manifest.json"), m);
    CHECK(m["format"]["minor"].GetInt() == 7);
    loomora_project_close(p);

    // The same file without the extra entry and fields opens with an empty report.
    Skew minor_only;
    minor_only.extras = false;
    minor_only.x_entry = false;
    const Bytes plain = make_skewed(basic_bytes(), minor_only);
    loomora_report *r2 = nullptr;
    loomora_project *q = nullptr;
    REQUIRE(loomora_project_open_memory(plain.data(), plain.size(), nullptr, nullptr, &q, &r2) == 0);
    if (r2 != nullptr) {
        CHECK(loomora_report_count(r2) == 0);
        loomora_report_free(r2);
    }
    loomora_project_close(q);
}

TEST_CASE("version skew: the unknown entry and the extensions survive an edit and a save", "[version][format]") {
    fstest::ScratchDir dir("skew");
    const std::string path = dir.file("skew.loomora");
    const Bytes file = make_skewed(basic_bytes(), Skew());
    REQUIRE(fstest::write_file(path, file));
    Opened before(file);

    loomora_editor *e = nullptr;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, &report) == 0);
    if (report != nullptr)
        loomora_report_free(report);
    // An edit: a fourth spool.
    std::uint32_t spool = 0;
    REQUIRE(loomora_editor_add_spool(e, "{\"name\":\"Spool 4\",\"color\":\"#00aa00\",\"opacity\":1,\"diameter_mm\":0.3,"
                                        "\"available_length_mm\":1000,\"material\":\"cotton\",\"domain\":\"exterior\"}",
                                     &spool) == 0);
    const Bytes saved = save_bytes(e, "2026-11-17T12:00:00Z");
    loomora_editor_close(e);

    Opened after(saved);
    CHECK(loomora_project_count(after.p, LOOMORA_KIND_SPOOL) == 4);
    // The unknown entry: present, byte for byte, listed in the manifest with its size and SHA-256.
    CHECK(read_entry(after.p, kXEntryName) == x_entry_bytes());
    rapidjson::Document ma, mb;
    parse(read_entry(after.p, "manifest.json"), ma);
    parse(read_entry(before.p, "manifest.json"), mb);
    REQUIRE(ma["entries"].HasMember(kXEntryName));
    CHECK(dump(ma["entries"][kXEntryName]) == dump(mb["entries"][kXEntryName]));
    // The extensions are kept as they were.
    REQUIRE(ma.HasMember("extensions"));
    CHECK(dump(ma["extensions"]) == dump(mb["extensions"]));
    CHECK(dump(ma["extensions"]) == "{\"acme.render\":{\"mode\":\"fancy\",\"levels\":[1,2,3]},\"acme.flag\":true}");
    // What was not touched is byte for byte what the other version wrote, extra fields included.
    for (rapidjson::Value::ConstMemberIterator it = mb["entries"].MemberBegin(); it != mb["entries"].MemberEnd(); ++it) {
        const std::string name = it->name.GetString();
        if (name == "spools.json")
            continue;
        INFO(name);
        CHECK(read_entry(after.p, name) == read_entry(before.p, name));
        CHECK(dump(ma["entries"][name.c_str()]) == dump(it->value));
    }
    // A second edit and save keeps them again (the file is now one of this version).
    REQUIRE(fstest::write_file(path, saved));
    REQUIRE(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, nullptr) == 0);
    REQUIRE(loomora_editor_set_title(e, "Renamed") == 0);
    const Bytes saved2 = save_bytes(e, "2026-11-17T12:30:00Z");
    loomora_editor_close(e);
    Opened again(saved2);
    CHECK(read_entry(again.p, kXEntryName) == x_entry_bytes());
    rapidjson::Document mc;
    parse(read_entry(again.p, "manifest.json"), mc);
    CHECK(dump(mc["extensions"]) == dump(mb["extensions"]));
}

TEST_CASE("version skew: a file of format 2.0 is refused with the minimum reader version", "[version][format]") {
    Skew s;
    s.major = 2;
    s.minor = 0;
    s.min_reader = "2.0";
    const Bytes file = make_skewed(basic_bytes(), s);
    std::string code, message;
    CHECK(refused(file, code, message) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(code == "version.major");
    CHECK(message == "This file needs Loomora format version 2.0 or newer");

    // A later major whose min_reader is 3.1 names that version.
    s.major = 3;
    s.minor = 4;
    s.min_reader = "3.1";
    CHECK(refused(make_skewed(basic_bytes(), s), code, message) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(message == "This file needs Loomora format version 3.1 or newer");

    // A file of this major whose min_reader is above what this library reads (1.0) is refused as well.
    Skew m;
    m.min_reader = "1.9";
    m.minor = 9;
    CHECK(refused(make_skewed(basic_bytes(), m), code, message) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(message == "This file needs Loomora format version 1.9 or newer");

    // The version rule comes before the strict parse: a 2.0 manifest shaped differently gets the same answer.
    const std::string odd = "{\"format\":{\"name\":\"loomora\",\"major\":2,\"minor\":0},\"min_reader\":\"2.0\",\"layout\":[1,2,3]}";
    std::vector<ziptest::Entry> entries;
    entries.push_back(ziptest::file("manifest.json", odd, 8));
    const Bytes shaped = ziptest::build(ziptest::with_mimetype(entries)).bytes;
    CHECK(refused(shaped, code, message) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(message == "This file needs Loomora format version 2.0 or newer");

    // The same through the path functions and the editor.
    fstest::ScratchDir dir("skew2");
    const std::string path = dir.file("v2.loomora");
    s.major = 2;
    s.minor = 0;
    s.min_reader = "2.0";
    REQUIRE(fstest::write_file(path, make_skewed(basic_bytes(), s)));
    loomora_project *p = nullptr;
    loomora_report *report = nullptr;
    CHECK(loomora_project_open_path(path.c_str(), nullptr, nullptr, &p, &report) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(p == nullptr);
    if (report != nullptr)
        loomora_report_free(report);
    loomora_editor *e = nullptr;
    report = nullptr;
    CHECK(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, &report) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    CHECK(e == nullptr);
    if (report != nullptr)
        loomora_report_free(report);
    report = nullptr;
    CHECK(loomora_validate_path(path.c_str(), nullptr, nullptr, &report) == LOOMORA_ERR_UNSUPPORTED_VERSION);
    if (report != nullptr)
        loomora_report_free(report);
}

TEST_CASE("version skew: an unknown required feature is refused naming it", "[version][format]") {
    Skew s;
    s.set_features = true;
    s.features.push_back("hologram_threads");
    std::string code, message;
    CHECK(refused(make_skewed(basic_bytes(), s), code, message) == LOOMORA_ERR_UNSUPPORTED_FEATURE);
    CHECK(code == "feature.unsupported");
    CHECK(message.find("hologram_threads") != std::string::npos);

    // Known features next to an unknown one: still refused, the unknown one is named.
    s.features.clear();
    s.features.push_back("surface_mesh");
    s.features.push_back("quantum_knots");
    CHECK(refused(make_skewed(basic_bytes(), s), code, message) == LOOMORA_ERR_UNSUPPORTED_FEATURE);
    CHECK(message.find("quantum_knots") != std::string::npos);

    // A known feature listed for a project that does not need it is not refused here (the content check
    // is the validator's business), and an empty list opens.
    Skew none;
    none.set_features = true;
    Opened o(make_skewed(basic_bytes(), none));
    CHECK(loomora_project_count(o.p, LOOMORA_KIND_FRAME) == 1);
}

TEST_CASE("version skew: a project of 2D frames lists no feature that only 3D needs", "[version][format]") {
    // basic: 2D, one image per result.
    {
        Opened o(basic_bytes());
        rapidjson::Document m;
        parse(read_entry(o.p, "manifest.json"), m);
        CHECK(m["required_features"].Size() == 0);
    }
    // multi: the cube needs surface_mesh, its result with two views needs multi_view_target.
    loomora_editor *e = nullptr;
    multi_project::Ids ids;
    REQUIRE(multi_project::build_multi(&e, &ids) == 0);
    {
        Opened o(save_bytes(e));
        rapidjson::Document m;
        parse(read_entry(o.p, "manifest.json"), m);
        REQUIRE(m["required_features"].Size() == 2);
        CHECK(std::string(m["required_features"][0u].GetString()) == "surface_mesh");
        CHECK(std::string(m["required_features"][1u].GetString()) == "multi_view_target");
    }
    // Remove the 3D frame (with its layout and its result): only 2D frames are left, no feature is listed.
    CHECK(loomora_editor_remove_item(e, LOOMORA_KIND_FRAME, ids.frame_3d, 0) == LOOMORA_ERR_HAS_DEPENDENTS);
    REQUIRE(loomora_editor_remove_item(e, LOOMORA_KIND_FRAME, ids.frame_3d, 1) == 0);
    {
        Opened o(save_bytes(e));
        CHECK(loomora_project_count(o.p, LOOMORA_KIND_FRAME) == 2);
        rapidjson::Document m;
        parse(read_entry(o.p, "manifest.json"), m);
        CHECK(m["required_features"].Size() == 0);
        // No entry of the 3D frame is left either.
        CHECK(loomora_project_entry_size(o.p, "frames/3/mesh.bin") < 0);
    }
    loomora_editor_close(e);

    // A 3D frame whose surface is not a mesh (a primitive: no mesh.bin) needs no surface_mesh.
    REQUIRE(basic_project::create(&e, "Primitive") == 0);
    std::uint32_t frame = 0;
    REQUIRE(multi_project::add_frame_json(e, multi_project::frame3d_json("Ball", "primitive", "solid"), &frame) == 0);
    {
        Opened o(save_bytes(e));
        rapidjson::Document m;
        parse(read_entry(o.p, "manifest.json"), m);
        CHECK(m["required_features"].Size() == 0);
    }
    loomora_editor_close(e);
}

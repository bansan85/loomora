// T080, User Story 7 (FR-017, FR-017a, FR-017b, FR-018, SC-006, SC-009): the light and subset export
// through the public C API. A full project with pixels, a 3D frame with UVs, a construction and an
// attached mesh file is built and saved; it is exported light, as a subset and as full again, and the
// archives are checked entry by entry (ZIP reader, manifest parse, the reader's own validation on every
// opening, the codecs through the C reader functions).
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "../export_project.hpp"
#include "format/io/memory_io.hpp"
#include "format/sha256.hpp"
#include "format/testing.hpp"
#include "format/zip/zip_reader.hpp"
#include "fs_test_util.hpp"

using namespace loomora::format;
using basic_project::Bytes;
using export_project::Ids;

namespace {

const char kModified[] = "2026-11-17T10:00:00Z";

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *b = static_cast<Bytes *>(user);
    b->insert(b->end(), data, data + len);
    return 0;
}

Bytes save_memory(loomora_editor *e) {
    Bytes out;
    REQUIRE(loomora_editor_save_stream(e, kModified, 0, collect, &out, nullptr) == 0);
    return out;
}

// An archive read with the ZIP reader.
struct Archive {
    Bytes bytes;
    MemorySource source;
    ZipReader zip;
    explicit Archive(const Bytes &b) : bytes(b), source(bytes.empty() ? nullptr : &bytes[0], bytes.size()), zip(source) {
        zip.open();
    }
    bool has(const std::string &name) const { return zip.find(name) != nullptr; }
    Bytes read(const std::string &name) const {
        const ZipEntry *e = zip.find(name);
        REQUIRE(e != nullptr);
        return zip.read_all(*e, 1ull << 30);
    }
    std::string text(const std::string &name) const {
        const Bytes b = read(name);
        return std::string(b.begin(), b.end());
    }
    std::set<std::string> names() const {
        std::set<std::string> n;
        for (std::size_t i = 0; i < zip.entries().size(); ++i)
            n.insert(zip.entries()[i].name);
        return n;
    }
    bool any_containing(const std::string &part) const {
        const std::set<std::string> n = names();
        for (std::set<std::string>::const_iterator it = n.begin(); it != n.end(); ++it)
            if (it->find(part) != std::string::npos)
                return true;
        return false;
    }
};

// An opened (and therefore validated) project over caller bytes.
struct Proj {
    Bytes bytes;
    loomora_project *p;
    int32_t rc;
    explicit Proj(const Bytes &b) : bytes(b), p(nullptr), rc(0) {
        loomora_report *report = nullptr;
        rc = loomora_project_open_memory(bytes.empty() ? nullptr : &bytes[0], bytes.size(), nullptr, nullptr, &p,
                                         &report);
        loomora_report_free(report);
    }
    ~Proj() { loomora_project_close(p); }
    std::string manifest() const {
        const int64_t n = loomora_project_manifest(p, nullptr, 0);
        REQUIRE(n > 0);
        std::string s(static_cast<std::size_t>(n), '\0');
        REQUIRE(loomora_project_manifest(p, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
        return s;
    }
    std::string item(int32_t kind, std::uint32_t id) const {
        const int64_t n = loomora_project_item_json(p, kind, id, nullptr, 0);
        REQUIRE(n > 0);
        std::string s(static_cast<std::size_t>(n), '\0');
        REQUIRE(loomora_project_item_json(p, kind, id, reinterpret_cast<std::uint8_t *>(&s[0]), s.size()) == n);
        return s;
    }
    std::vector<std::uint32_t> ids(int32_t kind) const {
        std::vector<std::uint32_t> v(static_cast<std::size_t>(loomora_project_count(p, kind)));
        if (!v.empty())
            REQUIRE(loomora_project_ids(p, kind, &v[0], v.size()) == static_cast<int64_t>(v.size()));
        return v;
    }
};

// export to memory: returns the code, `out` holds the archive on success.
int32_t export_memory(const loomora_project *p, int32_t profile, const std::vector<std::uint32_t> &results, Bytes &out,
                      const char *modified = kModified) {
    loomora_report *report = nullptr;
    out.clear();
    const int32_t rc = loomora_project_export_stream(p, profile, results.empty() ? nullptr : &results[0],
                                                     results.size(), modified, 0, collect, &out, &report);
    loomora_report_free(report);
    return rc;
}

std::vector<std::uint32_t> list_of(const rapidjson::Value &v) {
    std::vector<std::uint32_t> out;
    for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
        out.push_back(v[i].GetUint());
    return out;
}

std::vector<std::uint32_t> V(std::uint32_t a) { return std::vector<std::uint32_t>(1, a); }
std::vector<std::uint32_t> V(std::uint32_t a, std::uint32_t b) {
    std::vector<std::uint32_t> v;
    v.push_back(a);
    v.push_back(b);
    return v;
}
std::vector<std::uint32_t> V(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    std::vector<std::uint32_t> v = V(a, b);
    v.push_back(c);
    return v;
}

std::string sha_of(const Bytes &b, std::size_t from, std::size_t len) {
    Sha256 h;
    h.update(&b[from], len);
    return h.hex_digest();
}

struct Fixture {
    Ids ids;
    Bytes full;
    Fixture() {
        loomora_editor *e = nullptr;
        REQUIRE(export_project::build_pixels(&e, &ids) == 0);
        full = save_memory(e);
        loomora_editor_close(e);
    }
};

std::string tag(std::uint32_t id) { return basic_project::num(id); }

// Every PNG chunk type of `png` (empty when it is not a PNG).
std::vector<std::string> chunk_types(const Bytes &png) {
    std::vector<std::string> out;
    std::size_t at = 8;
    while (at + 12 <= png.size()) {
        const std::size_t len = (static_cast<std::size_t>(png[at]) << 24) | (static_cast<std::size_t>(png[at + 1]) << 16) |
                                (static_cast<std::size_t>(png[at + 2]) << 8) | png[at + 3];
        out.push_back(std::string(reinterpret_cast<const char *>(&png[at + 4]), 4));
        at += 12 + len;
    }
    return out;
}

// SC-009: no entry of the archive carries GPS or camera metadata.
void check_no_metadata(const Archive &a) {
    const std::set<std::string> names = a.names();
    for (std::set<std::string>::const_iterator it = names.begin(); it != names.end(); ++it) {
        const std::string &n = *it;
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".png") != 0)
            continue;
        const Bytes png = a.read(n);
        const std::vector<std::string> types = chunk_types(png);
        for (std::size_t i = 0; i < types.size(); ++i) {
            INFO(n << " chunk " << types[i]);
            CHECK((types[i] == "IHDR" || types[i] == "IDAT" || types[i] == "IEND" || types[i] == "PLTE"));
        }
        const std::string text(png.begin(), png.end());
        CHECK(text.find("GPS") == std::string::npos);
        CHECK(text.find("Camera") == std::string::npos);
    }
}

} // namespace

TEST_CASE("a light export drops the heavy entries and keeps the rest", "[format][export][light]") {
    const Fixture f;
    const Archive full(f.full);
    Proj p(f.full);
    REQUIRE(p.rc == 0);
    CHECK(loomora_project_is_light(p.p) == 0);
    REQUIRE(full.has("frames/" + tag(f.ids.frame_3d) + "/mesh-source.stl"));

    Bytes light_bytes;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light_bytes) == 0);
    Proj q(light_bytes);
    REQUIRE(q.rc == 0); // opening validates: a light file is a valid project
    CHECK(loomora_project_is_light(q.p) == 1);
    const Archive light(light_bytes);

    CHECK_FALSE(light.any_containing("/source."));
    CHECK_FALSE(light.any_containing("target.png"));
    CHECK_FALSE(light.any_containing("settings.json"));
    CHECK_FALSE(light.any_containing("mesh-source"));
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(light.has("images/" + tag(f.ids.image[i]) + "/thumb.png"));
        CHECK(light.has("images/" + tag(f.ids.image[i]) + "/image.json"));
    }
    const std::uint32_t rs[5] = {f.ids.result_a1, f.ids.result_a2, f.ids.result_b, f.ids.result_3d,
                                 f.ids.result_shared};
    for (int i = 0; i < 5; ++i) {
        CHECK(light.has("results/" + tag(rs[i]) + "/thumb.png"));
        CHECK(light.has("results/" + tag(rs[i]) + "/sequence.bin"));
        CHECK(light.has("results/" + tag(rs[i]) + "/result.json"));
        CHECK(light.read("results/" + tag(rs[i]) + "/thumb.png") == full.read("results/" + tag(rs[i]) + "/thumb.png"));
    }

    // manifest: light, not editable, the same project, every item kept
    rapidjson::Document m, mf;
    const std::string mt = q.manifest(), mft = p.manifest();
    m.Parse(mt.c_str());
    mf.Parse(mft.c_str());
    REQUIRE(m.IsObject());
    CHECK(std::string(m["profile"].GetString()) == "light");
    CHECK(m["editable"].GetBool() == false);
    CHECK(std::string(m["project_id"].GetString()) == mf["project_id"].GetString());
    CHECK(m["counts"]["images"].GetUint() == 4);
    CHECK(m["counts"]["results"].GetUint() == 5);
    CHECK(m["counts"]["layouts"].GetUint() == 5); // 0 results = all: nothing is pruned
    CHECK(list_of(m["order"]["frames"]) == list_of(mf["order"]["frames"]));
    CHECK(list_of(m["order"]["layouts"]) == list_of(mf["order"]["layouts"]));
    CHECK(m["next_id"]["result"].GetUint() == mf["next_id"]["result"].GetUint());

    // descriptions kept, source and target and settings gone, the frame description without construction
    for (std::size_t i = 0; i < 4; ++i) {
        const std::string a = q.item(LOOMORA_KIND_IMAGE, f.ids.image[i]);
        const std::string b = p.item(LOOMORA_KIND_IMAGE, f.ids.image[i]);
        rapidjson::Document da, db;
        da.Parse(a.c_str());
        db.Parse(b.c_str());
        CHECK(std::string(da["name"].GetString()) == db["name"].GetString());
        CHECK(da["width_px"].GetUint() == db["width_px"].GetUint());
        CHECK(da["color_mode"] == db["color_mode"]);
        CHECK_FALSE(da.HasMember("source"));
        CHECK_FALSE(da.HasMember("target"));
        CHECK(da.HasMember("thumbnail"));
        CHECK(db.HasMember("source"));
    }
    const std::string rl = q.item(LOOMORA_KIND_RESULT, f.ids.result_a1);
    CHECK(rl.find("\"settings\"") == std::string::npos);
    CHECK(p.item(LOOMORA_KIND_RESULT, f.ids.result_a1).find("\"settings\"") != std::string::npos);
    const std::string fl = q.item(LOOMORA_KIND_FRAME, f.ids.frame_3d);
    CHECK(fl.find("construction") == std::string::npos);
    CHECK(fl.find("attached_source") == std::string::npos);
    CHECK(fl.find("cube.stl") != std::string::npos); // the description of the imported file stays
    CHECK(p.item(LOOMORA_KIND_FRAME, f.ids.frame_3d).find("construction") != std::string::npos);
    check_no_metadata(light);
    check_no_metadata(full);
}

TEST_CASE("a light export keeps the mesh geometry identical and drops the UVs", "[format][export][light][mesh]") {
    const Fixture f;
    Proj p(f.full);
    REQUIRE(p.rc == 0);
    Bytes light_bytes;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light_bytes) == 0);
    Proj q(light_bytes);
    REQUIRE(q.rc == 0);
    const std::uint32_t frame = f.ids.frame_3d;

    loomora_mesh_info a = loomora_mesh_info(), b = loomora_mesh_info();
    REQUIRE(loomora_frame_mesh_info(p.p, frame, &a) == 0);
    REQUIRE(loomora_frame_mesh_info(q.p, frame, &b) == 0);
    CHECK((a.flags & LOOMORA_MESH_UVS) != 0);
    CHECK((b.flags & LOOMORA_MESH_UVS) == 0);
    CHECK((b.flags & LOOMORA_MESH_NORMALS) != 0);
    CHECK(a.vertices == b.vertices);
    CHECK(a.triangles == b.triangles);
    float uv[2];
    CHECK(loomora_frame_read_uvs(q.p, frame, 0, 1, uv) == 0);
    CHECK(loomora_frame_read_uvs(p.p, frame, 0, 1, uv) == 1);

    // value by value
    const std::size_t vc = static_cast<std::size_t>(a.vertices), tc = static_cast<std::size_t>(a.triangles);
    std::vector<float> pa(vc * 3), pb(vc * 3), na(vc * 3), nb(vc * 3);
    std::vector<std::uint32_t> ia(tc * 3), ib(tc * 3);
    REQUIRE(loomora_frame_read_vertices(p.p, frame, 0, vc, &pa[0]) == static_cast<int64_t>(vc));
    REQUIRE(loomora_frame_read_vertices(q.p, frame, 0, vc, &pb[0]) == static_cast<int64_t>(vc));
    REQUIRE(loomora_frame_read_triangles(p.p, frame, 0, tc, &ia[0]) == static_cast<int64_t>(tc));
    REQUIRE(loomora_frame_read_triangles(q.p, frame, 0, tc, &ib[0]) == static_cast<int64_t>(tc));
    REQUIRE(loomora_frame_read_normals(p.p, frame, 0, vc, &na[0]) == static_cast<int64_t>(vc));
    REQUIRE(loomora_frame_read_normals(q.p, frame, 0, vc, &nb[0]) == static_cast<int64_t>(vc));
    CHECK(pa == pb);
    CHECK(ia == ib);
    CHECK(na == nb);
    const multi_project::Cube cube;
    CHECK(pb == cube.xyz);
    CHECK(ib == cube.abc);

    // by SHA-256 of the three sections of mesh.bin (header of 24 bytes, then positions, indices, normals)
    const Archive full(f.full), light(light_bytes);
    const std::string name = "frames/" + tag(frame) + "/mesh.bin";
    const Bytes mf = full.read(name), ml = light.read(name);
    CHECK((mf[5] & 2) != 0);
    CHECK((ml[5] & 2) == 0);
    CHECK(sha_of(mf, 0, mf.size()) != sha_of(ml, 0, ml.size()));
    CHECK(ml.size() < mf.size());
    CHECK(sha_of(mf, 24, vc * 12) == sha_of(ml, 24, vc * 12));                           // positions
    CHECK(sha_of(mf, 24 + vc * 12, tc * 12) == sha_of(ml, 24 + vc * 12, tc * 12));       // indices
    CHECK(sha_of(mf, 24 + vc * 12 + tc * 12, vc * 12) == sha_of(ml, 24 + vc * 12 + tc * 12, vc * 12)); // normals
}

TEST_CASE("a subset export holds the chosen results and what they use", "[format][export][subset]") {
    const Fixture f;
    Proj p(f.full);
    REQUIRE(p.rc == 0);
    rapidjson::Document mf;
    const std::string mft = p.manifest();
    mf.Parse(mft.c_str());

    SECTION("light, one 3D result") {
        Bytes b;
        REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, V(f.ids.result_3d), b) == 0);
        Proj q(b);
        REQUIRE(q.rc == 0);
        rapidjson::Document m;
        const std::string mt = q.manifest();
        m.Parse(mt.c_str());
        CHECK(std::string(m["project_id"].GetString()) == mf["project_id"].GetString());
        CHECK(list_of(m["order"]["frames"]) == V(f.ids.frame_3d));
        CHECK(list_of(m["order"]["layouts"]) == V(f.ids.layout_3d));
        CHECK(list_of(m["order"]["images"]) == V(f.ids.image[2], f.ids.image[3]));
        CHECK(list_of(m["order"]["spools"]) == V(f.ids.spool[4], f.ids.spool[5]));
        CHECK(list_of(m["order"]["results"]) == V(f.ids.result_3d));
        CHECK(m["counts"]["frames"].GetUint() == 1);
        CHECK(m["counts"]["layouts"].GetUint() == 1);
        CHECK(m["counts"]["images"].GetUint() == 2);
        CHECK(m["counts"]["spools"].GetUint() == 2);
        CHECK(m["counts"]["results"].GetUint() == 1);
        // next_id still above every identifier, and never lower than the source's (identifiers are not reused)
        CHECK(m["next_id"]["result"].GetUint() == mf["next_id"]["result"].GetUint());
        CHECK(m["next_id"]["spool"].GetUint() > f.ids.spool[5]);
        // 14 entries: mimetype, manifest, spools, frame + mesh, layout + nails, 2 images x 2, result x 3
        const Archive a(b);
        CHECK(a.names().size() == 14);
        CHECK_FALSE(a.any_containing("frames/" + tag(f.ids.frame_a) + "/"));
        CHECK(a.has("frames/" + tag(f.ids.frame_3d) + "/mesh.bin"));
        // the data is the source's data
        std::vector<loomora_step> sa(60), sb(60);
        REQUIRE(loomora_result_read_steps(p.p, f.ids.result_3d, 0, 60, &sa[0]) == 60);
        REQUIRE(loomora_result_read_steps(q.p, f.ids.result_3d, 0, 60, &sb[0]) == 60);
        for (std::size_t i = 0; i < 60; ++i)
            CHECK((sa[i].nail == sb[i].nail && sa[i].spool_index == sb[i].spool_index && sa[i].type == sb[i].type));
        CHECK(a.read("results/" + tag(f.ids.result_3d) + "/sequence.bin") ==
              Archive(f.full).read("results/" + tag(f.ids.result_3d) + "/sequence.bin"));
    }

    SECTION("full, two results, copy-through") {
        Bytes b;
        // given out of order: the file keeps the display order of the source
        REQUIRE(export_memory(p.p, LOOMORA_PROFILE_FULL, V(f.ids.result_shared, f.ids.result_a1), b) == 0);
        Proj q(b);
        REQUIRE(q.rc == 0);
        CHECK(loomora_project_is_light(q.p) == 0);
        rapidjson::Document m;
        const std::string mt = q.manifest();
        m.Parse(mt.c_str());
        CHECK(std::string(m["profile"].GetString()) == "full");
        CHECK(m["editable"].GetBool() == true);
        CHECK(list_of(m["order"]["results"]) == V(f.ids.result_a1, f.ids.result_shared));
        CHECK(list_of(m["order"]["frames"]) == V(f.ids.frame_a));
        CHECK(list_of(m["order"]["layouts"]) == V(f.ids.layout_a1));
        CHECK(list_of(m["order"]["images"]) == V(f.ids.image[0], f.ids.image[2]));
        CHECK(list_of(m["order"]["spools"]) == V(f.ids.spool[0], f.ids.spool[1], f.ids.spool[2]));
        const Archive a(b), orig(f.full);
        const std::string src = "images/" + tag(f.ids.image[0]) + "/source.png";
        const std::string nails = "frames/" + tag(f.ids.frame_a) + "/layouts/" + tag(f.ids.layout_a1) + "/nails.json";
        CHECK(a.has(src));
        CHECK(a.has("results/" + tag(f.ids.result_a1) + "/settings.json"));
        CHECK_FALSE(a.has("images/" + tag(f.ids.image[1]) + "/source.png"));
        // unchanged entries keep their bytes, so their SHA-256 in the manifests is the same
        const rapidjson::Value &ea = m["entries"], &eo = mf["entries"];
        REQUIRE(ea.HasMember(src.c_str()));
        CHECK(std::string(ea[src.c_str()]["sha256"].GetString()) == eo[src.c_str()]["sha256"].GetString());
        CHECK(std::string(ea[nails.c_str()]["sha256"].GetString()) == eo[nails.c_str()]["sha256"].GetString());
        CHECK(a.read(nails) == orig.read(nails));
    }

    SECTION("a spool shared by two results is kept once; an unknown result is refused") {
        Bytes b;
        CHECK(export_memory(p.p, LOOMORA_PROFILE_LIGHT, V(f.ids.result_a1, f.ids.result_a1), b) == 0);
        CHECK(export_memory(p.p, LOOMORA_PROFILE_LIGHT, V(9999), b) == LOOMORA_ERR_NOT_FOUND);
        CHECK(export_memory(p.p, 7, V(f.ids.result_a1), b) == LOOMORA_ERR_INVALID);
        CHECK(export_memory(p.p, 0, std::vector<std::uint32_t>(), b) == LOOMORA_ERR_INVALID);
    }
}

TEST_CASE("a full export of a full project is the same file", "[format][export][full]") {
    const Fixture f;
    Proj p(f.full);
    REQUIRE(p.rc == 0);
    Bytes b;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_FULL, std::vector<std::uint32_t>(), b) == 0);
    CHECK(b == f.full);
}

TEST_CASE("a light project cannot be edited, and only goes to light", "[format][export][light][readonly]") {
    const Fixture f;
    Proj p(f.full);
    Bytes light_bytes;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light_bytes) == 0);
    Proj q(light_bytes);
    REQUIRE(q.rc == 0);

    // the editor refuses it, with the message of the format
    fstest::ScratchDir dir("light-edit");
    const std::string path = dir.file("light.loomora");
    REQUIRE(fstest::write_file(path, light_bytes));
    loomora_editor *e = nullptr;
    loomora_report *report = nullptr;
    CHECK(loomora_editor_open_path(path.c_str(), nullptr, nullptr, &e, &report) == LOOMORA_ERR_READ_ONLY_PROJECT);
    CHECK(e == nullptr);
    bool said = false;
    for (int64_t i = 0; report != nullptr && i < loomora_report_count(report); ++i) {
        const int64_t len = loomora_report_text(report, static_cast<std::size_t>(i), 3, nullptr, 0);
        std::string msg(static_cast<std::size_t>(len), '\0');
        if (len > 0)
            loomora_report_text(report, static_cast<std::size_t>(i), 3, reinterpret_cast<std::uint8_t *>(&msg[0]),
                                msg.size());
        said = said || msg == "This project is a light copy and cannot be edited";
    }
    CHECK(said);
    loomora_report_free(report);

    // a light to full export is refused; nothing is written to the sink
    Bytes out;
    CHECK(export_memory(q.p, LOOMORA_PROFILE_FULL, std::vector<std::uint32_t>(), out) == LOOMORA_ERR_READ_ONLY_PROJECT);
    CHECK(out.empty());
    const std::string target = dir.file("never.loomora");
    CHECK(loomora_project_export_path(q.p, target.c_str(), LOOMORA_PROFILE_FULL, nullptr, 0, kModified, 0, nullptr,
                                      nullptr, nullptr) == LOOMORA_ERR_READ_ONLY_PROJECT);
    CHECK(dir.names().size() == 1); // only light.loomora: no file, no temporary file

    // a light to light export, whole or as a subset, works
    Bytes again;
    REQUIRE(export_memory(q.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), again) == 0);
    CHECK(again == light_bytes);
    Bytes one;
    REQUIRE(export_memory(q.p, LOOMORA_PROFILE_LIGHT, V(f.ids.result_b), one) == 0);
    Proj r(one);
    REQUIRE(r.rc == 0);
    CHECK(loomora_project_is_light(r.p) == 1);
    CHECK(r.ids(LOOMORA_KIND_RESULT) == V(f.ids.result_b));
    CHECK(r.ids(LOOMORA_KIND_FRAME) == V(f.ids.frame_b));
    CHECK(r.ids(LOOMORA_KIND_LAYOUT) == V(f.ids.layout_b));
    CHECK(r.ids(LOOMORA_KIND_IMAGE) == V(f.ids.image[0]));
    CHECK(r.ids(LOOMORA_KIND_SPOOL) == V(f.ids.spool[0], f.ids.spool[3]));
}

TEST_CASE("the light file is at least 50% smaller when the images have pixels", "[format][export][size]") {
    const Fixture f;
    Proj p(f.full);
    Bytes light_bytes;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light_bytes) == 0);
    const double ratio = static_cast<double>(light_bytes.size()) / static_cast<double>(f.full.size());
    std::printf("[size] full %zu bytes, light %zu bytes, ratio %.3f\n", f.full.size(), light_bytes.size(), ratio);
    CHECK(light_bytes.size() * 2 <= f.full.size());

    // one result of the same project: smaller again
    Bytes one;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, V(f.ids.result_b), one) == 0);
    CHECK(one.size() < light_bytes.size());
}

TEST_CASE("a 5,000-step sequence takes under 20 KB in the light file", "[format][export][size]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    const Bytes full = save_memory(e);
    loomora_editor_close(e);
    Proj p(full);
    REQUIRE(p.rc == 0);
    Bytes light_bytes;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light_bytes) == 0);
    Proj q(light_bytes);
    REQUIRE(q.rc == 0);
    CHECK(loomora_result_step_count(q.p, q.ids(LOOMORA_KIND_RESULT)[0]) == 5000);
    const Archive a(light_bytes);
    const std::size_t seq = a.read("results/1/sequence.bin").size();
    std::printf("[size] 5000-step sequence.bin: %zu bytes; basic full %zu bytes, light %zu bytes\n", seq, full.size(),
                light_bytes.size());
    CHECK(seq < 20 * 1024);
    CHECK_FALSE(a.any_containing("/source."));
    check_no_metadata(a);
}

TEST_CASE("an export to a path is atomic and keeps unknown entries only in full", "[format][export][atomic]") {
    const Fixture f;
    Proj p(f.full);
    REQUIRE(p.rc == 0);
    fstest::ScratchDir dir("export-path");
    const std::string path = dir.file("out.loomora");

    // a new file, then replaced; the previous file survives a failure at every step
    REQUIRE(loomora_project_export_path(p.p, path.c_str(), LOOMORA_PROFILE_LIGHT, nullptr, 0, kModified, 0, nullptr,
                                        nullptr, nullptr) == 0);
    Bytes first;
    REQUIRE(fstest::read_file(path, first));
    Proj q(first);
    CHECK(q.rc == 0);
    CHECK(loomora_project_is_light(q.p) == 1);

    // a bad argument and an unknown result change nothing
    CHECK(loomora_project_export_path(p.p, path.c_str(), 9, nullptr, 0, kModified, 0, nullptr, nullptr, nullptr) ==
          LOOMORA_ERR_INVALID);
    const std::uint32_t nope = 4242;
    CHECK(loomora_project_export_path(p.p, path.c_str(), LOOMORA_PROFILE_FULL, &nope, 1, kModified, 0, nullptr,
                                      nullptr, nullptr) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_project_export_path(nullptr, path.c_str(), LOOMORA_PROFILE_FULL, nullptr, 0, kModified, 0, nullptr,
                                      nullptr, nullptr) == LOOMORA_ERR_INVALID);
    CHECK(loomora_project_export_path(p.p, path.c_str(), LOOMORA_PROFILE_FULL, nullptr, 0, "not a time", 0, nullptr,
                                      nullptr, nullptr) == LOOMORA_ERR_INVALID);

    // a failed rename: the old file is intact, no temporary file is left
    struct Rename {
        static bool fail(void *) { return true; }
    };
    hooks().fail_rename = Rename::fail;
    const int32_t rc = loomora_project_export_path(p.p, path.c_str(), LOOMORA_PROFILE_FULL, nullptr, 0, kModified, 0,
                                                   nullptr, nullptr, nullptr);
    hooks().fail_rename = nullptr;
    CHECK(rc != 0);
    Bytes after;
    REQUIRE(fstest::read_file(path, after));
    CHECK(after == first);
    CHECK(dir.names().size() == 1);

    // a cancelled export too
    struct Cancel {
        static int32_t cb(void *, std::uint64_t, std::uint64_t) { return 1; }
    };
    CHECK(loomora_project_export_path(p.p, path.c_str(), LOOMORA_PROFILE_FULL, nullptr, 0, kModified, 0, Cancel::cb,
                                      nullptr, nullptr) == LOOMORA_ERR_CANCELLED);
    CHECK(fstest::read_file(path, after));
    CHECK(after == first);
    CHECK(dir.names().size() == 1);
}

TEST_CASE("unknown entries are kept in a full export and dropped from a light one", "[format][export][unknown]") {
    const std::string corpus = FMT_CORPUS_DIR;
    std::ifstream index((corpus + "/corpus.json").c_str(), std::ios::binary);
    if (corpus.empty() || !index.good()) {
        WARN("the corpus is not present: the unknown-entry export test is skipped");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(index)), std::istreambuf_iterator<char>());
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    REQUIRE(!doc.HasParseError());
    std::string file;
    const rapidjson::Value &files = doc["files"];
    for (rapidjson::Value::ConstMemberIterator it = files.MemberBegin(); it != files.MemberEnd(); ++it)
        if (it->value["code"].IsString() && std::string(it->value["code"].GetString()) == "warn.unknown_entry" &&
            std::string(it->value["kind"].GetString()) == "warning")
            file = it->name.GetString();
    REQUIRE(!file.empty());
    Bytes bytes;
    REQUIRE(fstest::read_file(corpus + "/" + file, bytes));
    Proj p(bytes);
    REQUIRE(p.rc == 0);
    Bytes full, light;
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_FULL, std::vector<std::uint32_t>(), full, "2026-11-18T08:00:00Z") == 0);
    REQUIRE(export_memory(p.p, LOOMORA_PROFILE_LIGHT, std::vector<std::uint32_t>(), light, "2026-11-18T08:00:00Z") == 0);
    CHECK(Archive(full).has("x-acme/note.txt"));
    CHECK(Archive(full).read("x-acme/note.txt") == Archive(bytes).read("x-acme/note.txt"));
    CHECK_FALSE(Archive(light).has("x-acme/note.txt"));
    Proj q(light);
    CHECK(q.rc == 0);
}

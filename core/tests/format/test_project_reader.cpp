// The read side of the .loomora format through the public C API (loomora_project.h, block
// "reader"): the files of tools/gen_invalid_corpus.py (written by Python, independent of the C++
// code) are opened, read and validated. The corpus directory comes from the CMake compile
// definition FMT_CORPUS_DIR; without it the tests say so and return.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../../include/loomora.h"
#include "format/limits.hpp"

#ifndef FMT_CORPUS_DIR
#define FMT_CORPUS_DIR ""
#endif

static_assert(sizeof(loomora_nail) == 88, "loomora_nail is 88 bytes");
static_assert(sizeof(loomora_step) == 12, "loomora_step is 12 bytes");
static_assert(sizeof(loomora_mesh_info) == 24, "loomora_mesh_info is 24 bytes");

namespace {

typedef std::vector<std::uint8_t> Bytes;

std::string corpus_dir() { return FMT_CORPUS_DIR; }

bool have_corpus() {
    std::ifstream f((corpus_dir() + "/corpus.json").c_str(), std::ios::binary);
    if (!f.good()) {
        WARN("the .loomora corpus is not present (python tools/gen_invalid_corpus.py): reader tests skipped");
        return false;
    }
    return true;
}

Bytes read_file(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write_file(const std::string &path, const Bytes &b) {
    std::ofstream f(path.c_str(), std::ios::binary);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct Row {
    std::string file, kind, code, family, severity;
    bool contract_code;
};

std::vector<Row> load_rows() {
    std::vector<Row> rows;
    const Bytes b = read_file(corpus_dir() + "/corpus.json");
    rapidjson::Document d;
    d.Parse(reinterpret_cast<const char *>(b.data()), b.size());
    REQUIRE(!d.HasParseError());
    const rapidjson::Value &files = d["files"];
    for (rapidjson::Value::ConstMemberIterator it = files.MemberBegin(); it != files.MemberEnd(); ++it) {
        Row r;
        r.file = it->name.GetString();
        const rapidjson::Value &v = it->value;
        r.kind = v["kind"].GetString();
        r.code = v["code"].IsString() ? v["code"].GetString() : "";
        r.family = v["family"].IsString() ? v["family"].GetString() : "";
        r.severity = v["severity"].IsString() ? v["severity"].GetString() : "";
        r.contract_code = v["contract_code"].GetBool();
        rows.push_back(r);
    }
    return rows;
}

std::string text_of(const loomora_report *rep, size_t i, int field) {
    const int64_t n = loomora_report_text(rep, i, field, nullptr, 0);
    REQUIRE(n >= 0);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0)
        REQUIRE(loomora_report_text(rep, i, field, reinterpret_cast<uint8_t *>(&s[0]), s.size()) == n);
    return s;
}

std::vector<std::string> codes_of(const loomora_report *rep, int severity) {
    std::vector<std::string> out;
    if (rep == nullptr)
        return out;
    const int64_t n = loomora_report_count(rep);
    for (int64_t i = 0; i < n; ++i) {
        int32_t sev = 0, kind = 0;
        uint32_t id = 0;
        REQUIRE(loomora_report_issue(rep, static_cast<size_t>(i), &sev, &kind, &id) == 0);
        if (severity == 0 || sev == severity)
            out.push_back(text_of(rep, static_cast<size_t>(i), 1));
    }
    return out;
}

bool contains(const std::vector<std::string> &v, const std::string &code, bool prefix) {
    for (size_t i = 0; i < v.size(); ++i)
        if (prefix ? v[i].compare(0, code.size() + 1, code + ".") == 0 : v[i] == code)
            return true;
    return false;
}

struct Opened {
    loomora_project *p;
    loomora_report *r;
    int32_t rc;
    Opened() : p(nullptr), r(nullptr), rc(0) {}
    ~Opened() {
        loomora_project_close(p);
        loomora_report_free(r);
    }
};

std::string as_string(int64_t n, const std::vector<uint8_t> &b) {
    REQUIRE(n >= 0);
    return std::string(b.begin(), b.begin() + n);
}

std::string item_json(const loomora_project *p, int kind, uint32_t id) {
    const int64_t n = loomora_project_item_json(p, kind, id, nullptr, 0);
    REQUIRE(n > 0);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    REQUIRE(loomora_project_item_json(p, kind, id, b.data(), b.size()) == n);
    return as_string(n, b);
}

// ---- the expected result of a corpus row ------------------------------------------------------

bool pending(const std::string &code) {
    // The semantic validator (T068) is a later task: references, nails against the frame, spool
    // lengths and the warnings of thresholds, distances, origin and thumbnails.
    return code.compare(0, 4, "ref.") == 0 || code == "nail.outside_frame" ||
           code.compare(0, 6, "spool.") == 0 || code == "warn.count" || code == "warn.nail_distance" ||
           code == "warn.origin_offset" || code == "warn.thumbnail_size";
}

int32_t expected_rc(const std::string &code) {
    if (code == "version.major")
        return LOOMORA_ERR_UNSUPPORTED_VERSION;
    if (code == "feature.unsupported")
        return LOOMORA_ERR_UNSUPPORTED_FEATURE;
    if (code == "entry.bomb")
        return LOOMORA_ERR_BOMB;
    if (code == "zip.not_zip" || code == "zip.mimetype")
        return LOOMORA_ERR_NOT_A_PROJECT;
    return LOOMORA_ERR_CORRUPT;
}

// Generator sub-codes that the C++ reader names with a contract code of the same family.
std::string reader_code(const Row &r) {
    if (r.code == "schema.number")
        return "num.not_finite";
    return r.code;
}

} // namespace

TEST_CASE("the valid corpus files open with an empty report, from a path and from memory", "[format][reader]") {
    if (!have_corpus())
        return;
    int opened = 0;
    const std::vector<Row> rows = load_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != "valid")
            continue;
        const std::string path = corpus_dir() + "/" + rows[i].file;
        INFO(rows[i].file);
        {
            Opened o;
            o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
            if (o.rc != 0)
                for (const std::string &c : codes_of(o.r, 0))
                    UNSCOPED_INFO("issue " << c);
            REQUIRE(o.rc == 0);
            REQUIRE(o.p != nullptr);
            REQUIRE(o.r != nullptr);
            REQUIRE(loomora_report_count(o.r) == 0);
            REQUIRE(loomora_project_is_light(o.p) == 0);
            REQUIRE(loomora_project_count(o.p, LOOMORA_KIND_FRAME) >= 1);
        }
        {
            const Bytes b = read_file(path);
            Opened o;
            o.rc = loomora_project_open_memory(b.data(), b.size(), nullptr, nullptr, &o.p, nullptr);
            REQUIRE(o.rc == 0);
            REQUIRE(loomora_project_count(o.p, LOOMORA_KIND_RESULT) == 1);
        }
        ++opened;
    }
    REQUIRE(opened >= 8);
}

TEST_CASE("valid-basic: items, entries, nails and steps are served by range", "[format][reader]") {
    if (!have_corpus())
        return;
    Opened o;
    const std::string path = corpus_dir() + "/valid-basic.loomora";
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);

    const int kinds[5] = {LOOMORA_KIND_FRAME, LOOMORA_KIND_LAYOUT, LOOMORA_KIND_IMAGE, LOOMORA_KIND_SPOOL,
                          LOOMORA_KIND_RESULT};
    for (int k = 0; k < 5; ++k) {
        REQUIRE(loomora_project_count(o.p, kinds[k]) == 1);
        uint32_t ids[2] = {0, 0};
        REQUIRE(loomora_project_ids(o.p, kinds[k], nullptr, 0) == 1);
        REQUIRE(loomora_project_ids(o.p, kinds[k], ids, 0) == 1); // too small: nothing written
        REQUIRE(ids[0] == 0);
        REQUIRE(loomora_project_ids(o.p, kinds[k], ids, 2) == 1);
        REQUIRE(ids[0] == 1);
        REQUIRE(item_json(o.p, kinds[k], 1).find("\"id\":1") != std::string::npos);
    }
    REQUIRE(item_json(o.p, LOOMORA_KIND_SPOOL, 1) ==
            "{\"id\":1,\"name\":\"Black\",\"color\":\"#000000\",\"opacity\":1,\"diameter_mm\":0.3,"
            "\"available_length_mm\":200000,\"material\":\"cotton\",\"domain\":\"exterior\"}");
    REQUIRE(loomora_project_item_json(o.p, LOOMORA_KIND_FRAME, 99, nullptr, 0) == LOOMORA_ERR_NOT_FOUND);
    REQUIRE(loomora_project_count(o.p, 77) == LOOMORA_ERR_NOT_FOUND);

    // The manifest, with the size-query convention (nothing written when cap is too small).
    const int64_t mlen = loomora_project_manifest(o.p, nullptr, 0);
    REQUIRE(mlen > 100);
    std::vector<uint8_t> m(static_cast<size_t>(mlen));
    REQUIRE(loomora_project_manifest(o.p, m.data(), m.size() - 1) == mlen);
    REQUIRE(m[0] == 0);
    REQUIRE(loomora_project_manifest(o.p, m.data(), m.size()) == mlen);
    REQUIRE(as_string(mlen, m).compare(0, 10, "{\"format\":") == 0);

    // Entries by range.
    REQUIRE(loomora_project_entry_size(o.p, "images/1/thumb.png") == 71);
    REQUIRE(loomora_project_entry_size(o.p, "nope") == LOOMORA_ERR_NOT_FOUND);
    uint8_t png[8];
    REQUIRE(loomora_project_read_entry(o.p, "images/1/thumb.png", 0, png, 8) == 8);
    REQUIRE(png[1] == 'P');
    REQUIRE(png[2] == 'N');
    REQUIRE(png[3] == 'G');
    uint8_t tail[16];
    REQUIRE(loomora_project_read_entry(o.p, "images/1/thumb.png", 65, tail, 16) == 6); // end clamp
    REQUIRE(loomora_project_read_entry(o.p, "images/1/thumb.png", 71, tail, 16) == 0);
    // A deflated entry (settings.json, JSON) read through a range.
    const int64_t slen = loomora_project_entry_size(o.p, "results/1/settings.json");
    REQUIRE(slen == 47);
    std::vector<uint8_t> whole(47), part(10);
    REQUIRE(loomora_project_read_entry(o.p, "results/1/settings.json", 0, whole.data(), 47) == 47);
    REQUIRE(loomora_project_read_entry(o.p, "results/1/settings.json", 5, part.data(), 10) == 10);
    REQUIRE(std::memcmp(part.data(), whole.data() + 5, 10) == 0);

    // Nails.
    REQUIRE(loomora_layout_nail_count(o.p, 1) == 8);
    REQUIRE(loomora_layout_nail_count(o.p, 9) == LOOMORA_ERR_NOT_FOUND);
    std::vector<loomora_nail> all(8);
    REQUIRE(loomora_layout_read_nails(o.p, 1, 0, 8, all.data()) == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        REQUIRE(all[i].id == i);
        REQUIRE(all[i].kind == 1); // boundary
        REQUIRE(all[i].flags == 0);
        REQUIRE(all[i].diameter == 2.0);
    }
    REQUIRE(all[0].x == -250.0);
    REQUIRE(all[0].y == -250.0);
    loomora_nail some[3];
    REQUIRE(loomora_layout_read_nails(o.p, 1, 3, 3, some) == 3);
    for (int i = 0; i < 3; ++i)
        REQUIRE(std::memcmp(&some[i], &all[3 + i], sizeof(loomora_nail)) == 0);
    REQUIRE(loomora_layout_read_nails(o.p, 1, 6, 5, some) == 2); // end clamp
    REQUIRE(loomora_layout_read_nails(o.p, 1, 8, 5, some) == 0);
    REQUIRE(loomora_layout_nail_label(o.p, 1, 0, nullptr, 0) == 0); // no label
    REQUIRE(loomora_layout_nail_label(o.p, 1, 12345, nullptr, 0) == LOOMORA_ERR_NOT_FOUND);

    // Steps.
    REQUIRE(loomora_result_step_count(o.p, 1) == 7);
    loomora_step steps[7];
    REQUIRE(loomora_result_read_steps(o.p, 1, 0, 7, steps) == 7);
    for (int i = 0; i < 7; ++i) {
        REQUIRE(steps[i].nail < 8);
        REQUIRE(steps[i].spool_index == 0);
        REQUIRE(steps[i].type <= 2);
        REQUIRE(steps[i].rotation <= 1);
    }
    loomora_step range[3];
    REQUIRE(loomora_result_read_steps(o.p, 1, 2, 3, range) == 3);
    for (int i = 0; i < 3; ++i)
        REQUIRE(std::memcmp(&range[i], &steps[2 + i], sizeof(loomora_step)) == 0);
    REQUIRE(loomora_result_read_steps(o.p, 1, 5, 9, range) == 2);

    // A 2D frame has no mesh.
    loomora_mesh_info info;
    REQUIRE(loomora_frame_mesh_info(o.p, 1, &info) == LOOMORA_ERR_NOT_FOUND);
}

TEST_CASE("valid-mesh3d-uv: mesh info and vertex data by range", "[format][reader]") {
    if (!have_corpus())
        return;
    Opened o;
    const std::string path = corpus_dir() + "/valid-mesh3d-uv.loomora";
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);
    loomora_mesh_info info;
    REQUIRE(loomora_frame_mesh_info(o.p, 1, &info) == 0);
    REQUIRE(info.vertices == 4);
    REQUIRE(info.triangles == 4);
    REQUIRE(info.flags == (LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS));

    float xyz[12];
    REQUIRE(loomora_frame_read_vertices(o.p, 1, 0, 4, xyz) == 4);
    float two[6];
    REQUIRE(loomora_frame_read_vertices(o.p, 1, 1, 2, two) == 2);
    REQUIRE(std::memcmp(two, xyz + 3, sizeof two) == 0);
    REQUIRE(loomora_frame_read_vertices(o.p, 1, 3, 9, two) == 1); // end clamp
    uint32_t abc[12];
    REQUIRE(loomora_frame_read_triangles(o.p, 1, 0, 4, abc) == 4);
    for (int i = 0; i < 12; ++i)
        REQUIRE(abc[i] < 4);
    float nrm[12];
    REQUIRE(loomora_frame_read_normals(o.p, 1, 0, 4, nrm) == 4);
    for (int v = 0; v < 4; ++v) {
        const float len2 = nrm[3 * v] * nrm[3 * v] + nrm[3 * v + 1] * nrm[3 * v + 1] + nrm[3 * v + 2] * nrm[3 * v + 2];
        REQUIRE(len2 == Approx(1.0).margin(1e-2));
    }
    float uv[8];
    REQUIRE(loomora_frame_read_uvs(o.p, 1, 0, 4, uv) == 4);
    REQUIRE(loomora_frame_read_vertices(o.p, 7, 0, 4, xyz) == LOOMORA_ERR_NOT_FOUND);

    // The same file without UVs reports none and writes nothing for loomora_frame_read_uvs.
    Opened o2;
    const std::string path2 = corpus_dir() + "/valid-mesh3d.loomora";
    REQUIRE(loomora_project_open_path(path2.c_str(), nullptr, nullptr, &o2.p, nullptr) == 0);
    REQUIRE(loomora_frame_mesh_info(o2.p, 1, &info) == 0);
    REQUIRE(info.flags == LOOMORA_MESH_NORMALS);
    REQUIRE(loomora_frame_read_uvs(o2.p, 1, 0, 4, uv) == 0);
    // 3D nails carry the 3D fields.
    loomora_nail n;
    REQUIRE(loomora_layout_read_nails(o2.p, 1, 0, 1, &n) == 1);
    REQUIRE((n.flags & 1u) == 1u);
    REQUIRE(n.face_triangle < 4);
}

TEST_CASE("valid-newer-minor opens and ignores what it does not know", "[format][reader]") {
    if (!have_corpus())
        return;
    Opened o;
    const std::string path = corpus_dir() + "/valid-newer-minor.loomora";
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);
    REQUIRE(loomora_report_count(o.r) == 0);
}

TEST_CASE("warning corpus files open and report the warnings the reader owns", "[format][reader]") {
    if (!have_corpus())
        return;
    int pending_rows = 0, checked = 0;
    const std::vector<Row> rows = load_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != "warning" && rows[i].kind != "large")
            continue;
        INFO(rows[i].file);
        const std::string path = corpus_dir() + "/" + rows[i].file;
        Opened o;
        o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
        REQUIRE(o.rc == 0); // a warning never blocks
        REQUIRE(loomora_report_error_count(o.r) == 0);
        if (pending(rows[i].code)) {
            ++pending_rows; // T068
            continue;
        }
        REQUIRE(contains(codes_of(o.r, 1), rows[i].code, false));
        if (rows[i].code == "warn.light_project")
            REQUIRE(loomora_project_is_light(o.p) == 1);
        ++checked;
    }
    REQUIRE(checked >= 3);
    if (pending_rows > 0)
        WARN(pending_rows << " warning rows are pending the semantic validator (T068)");
}

TEST_CASE("every invalid and hostile corpus file gives its code", "[format][reader]") {
    if (!have_corpus())
        return;
    int pending_rows = 0, checked = 0;
    const std::vector<Row> rows = load_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != "invalid" && rows[i].kind != "hostile")
            continue;
        INFO(rows[i].file << " expects " << rows[i].code);
        if (pending(rows[i].code)) {
            ++pending_rows; // T068
            continue;
        }
        const std::string path = corpus_dir() + "/" + rows[i].file;
        const std::string want = reader_code(rows[i]);
        const bool exact = rows[i].contract_code || want != rows[i].code;
        {
            Opened o;
            o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
            for (const std::string &c : codes_of(o.r, 0))
                UNSCOPED_INFO("issue " << c);
            REQUIRE(o.rc == expected_rc(rows[i].code));
            REQUIRE(o.p == nullptr);
            if (o.r != nullptr) {
                const std::vector<std::string> errs = codes_of(o.r, 2);
                REQUIRE(contains(errs, exact ? want : rows[i].family, !exact));
            } else {
                // Only a file that is not a ZIP archive may leave no report at all.
                REQUIRE(rows[i].code == "zip.not_zip");
            }
        }
        {
            // The same file through the memory variant and through validate: the same answer.
            const Bytes b = read_file(path);
            Opened o;
            o.rc = loomora_project_open_memory(b.data(), b.size(), nullptr, nullptr, &o.p, &o.r);
            REQUIRE(o.rc == expected_rc(rows[i].code));
            loomora_report *vr = nullptr;
            REQUIRE(loomora_validate_memory(b.data(), b.size(), nullptr, nullptr, &vr) == o.rc);
            REQUIRE(loomora_report_count(vr) == loomora_report_count(o.r));
            loomora_report_free(vr);
            vr = nullptr;
            REQUIRE(loomora_validate_path(path.c_str(), nullptr, nullptr, &vr) == o.rc);
            loomora_report_free(vr);
        }
        ++checked;
    }
    REQUIRE(checked >= 60);
    if (pending_rows > 0)
        WARN(pending_rows << " rows (ref.*, nail.outside_frame, spool.*, warn.*) are pending the semantic validator (T068)");
}

TEST_CASE("the version rule names the version the file needs", "[format][reader]") {
    if (!have_corpus())
        return;
    Opened o;
    const std::string path = corpus_dir() + "/invalid-version.major.loomora";
    o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
    REQUIRE(o.rc == LOOMORA_ERR_UNSUPPORTED_VERSION);
    REQUIRE(loomora_report_count(o.r) == 1);
    REQUIRE(text_of(o.r, 0, 1) == "version.major");
    REQUIRE(text_of(o.r, 0, 3) == "This file needs Loomora format version 2.0 or newer");
}

TEST_CASE("API misuse and missing files give error codes, never an exception", "[format][reader]") {
    loomora_project *p = nullptr;
    loomora_report *r = nullptr;
    REQUIRE(loomora_project_open_path(nullptr, nullptr, nullptr, &p, &r) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_project_open_path("x", nullptr, nullptr, nullptr, &r) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_project_open_memory(nullptr, 5, nullptr, nullptr, &p, &r) == LOOMORA_ERR_INVALID);
    const std::string missing = corpus_dir() + "/no-such-file.loomora";
    REQUIRE(loomora_project_open_path(missing.c_str(), nullptr, nullptr, &p, &r) == LOOMORA_ERR_IO);
    REQUIRE(p == nullptr);
    loomora_report_free(r);
    r = nullptr;
    REQUIRE(loomora_validate_path(missing.c_str(), nullptr, nullptr, &r) == LOOMORA_ERR_IO);
    loomora_report_free(r);
    // Empty and tiny buffers are not projects.
    const uint8_t tiny[3] = {1, 2, 3};
    r = nullptr;
    REQUIRE(loomora_project_open_memory(tiny, sizeof tiny, nullptr, nullptr, &p, &r) == LOOMORA_ERR_NOT_A_PROJECT);
    loomora_report_free(r);
    REQUIRE(loomora_project_count(nullptr, LOOMORA_KIND_FRAME) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_project_is_light(nullptr) == LOOMORA_ERR_INVALID);
    REQUIRE(loomora_report_count(nullptr) == LOOMORA_ERR_INVALID);
    loomora_project_close(nullptr);
    loomora_report_free(nullptr);
    int32_t sev = 0, kind = 0;
    uint32_t id = 0;
    REQUIRE(loomora_report_issue(nullptr, 0, &sev, &kind, &id) == LOOMORA_ERR_INVALID);
}

namespace {
int32_t stop_now(void *user, uint64_t, uint64_t) {
    ++*static_cast<int *>(user);
    return 1;
}
} // namespace

TEST_CASE("the progress callback can cancel an open", "[format][reader]") {
    if (!have_corpus())
        return;
    loomora::format::ScopedLimits restore;
    loomora::format::limits().progress_interval_bytes = 1;
    int calls = 0;
    loomora_project *p = nullptr;
    loomora_report *r = nullptr;
    const std::string path = corpus_dir() + "/valid-basic.loomora";
    REQUIRE(loomora_project_open_path(path.c_str(), stop_now, &calls, &p, &r) == LOOMORA_ERR_CANCELLED);
    REQUIRE(calls >= 1);
    REQUIRE(p == nullptr);
    loomora_report_free(r);
}

// ---- SC-012: use mode never writes ---------------------------------------------------------------

namespace {

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir.c_str());
    if (d != nullptr) {
        while (const dirent *e = readdir(d))
            names.push_back(e->d_name);
        closedir(d);
    }
#endif
    std::sort(names.begin(), names.end());
    return names;
}

bool make_dir(const std::string &dir) {
#ifdef _WIN32
    return _mkdir(dir.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

void set_writable(const std::string &path, bool writable) {
#ifdef _WIN32
    _chmod(path.c_str(), writable ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
#else
    chmod(path.c_str(), writable ? 0644 : 0444);
#endif
}

long long mtime_of(const std::string &path) {
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    return static_cast<long long>(st.st_mtime);
}

bool replace_file(const std::string &from, const std::string &to) {
#ifdef _WIN32
    // Replacing a file that is open is refused by Windows (S-F3): move it aside, then the new one in.
    if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0)
        return true;
    const std::string aside = to + ".old";
    DeleteFileA(aside.c_str());
    return MoveFileExA(to.c_str(), aside.c_str(), 0) != 0 && MoveFileExA(from.c_str(), to.c_str(), 0) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

} // namespace

TEST_CASE("opening and reading never changes the file, its time or its directory (SC-012)", "[format][reader]") {
    if (!have_corpus())
        return;
    const std::string dir = corpus_dir() + "/readonly-test";
    REQUIRE(make_dir(dir));
    const std::string path = dir + "/ro.loomora";
    const Bytes original = read_file(corpus_dir() + "/valid-basic.loomora");
    set_writable(path, true);
    write_file(path, original);
    set_writable(path, false); // a read-only file must open
    const long long t0 = mtime_of(path);
    const std::vector<std::string> before = list_dir(dir);

    {
        Opened o;
        REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);
        std::vector<loomora_nail> nails(8);
        REQUIRE(loomora_layout_read_nails(o.p, 1, 0, 8, nails.data()) == 8);
        loomora_step steps[7];
        REQUIRE(loomora_result_read_steps(o.p, 1, 0, 7, steps) == 7);
        REQUIRE(item_json(o.p, LOOMORA_KIND_FRAME, 1).size() > 10);
        loomora_report *vr = nullptr;
        REQUIRE(loomora_validate_path(path.c_str(), nullptr, nullptr, &vr) == 0);
        loomora_report_free(vr);
    }
    REQUIRE(read_file(path) == original);
    REQUIRE(mtime_of(path) == t0);
    REQUIRE(list_dir(dir) == before);
    set_writable(path, true);
}

TEST_CASE("a project opened before an atomic replace keeps reading the old file", "[format][reader]") {
    if (!have_corpus())
        return;
    const std::string dir = corpus_dir() + "/replace-test";
    REQUIRE(make_dir(dir));
    const std::string path = dir + "/p.loomora";
    const std::string tmp = dir + "/p.loomora.tmp";
    write_file(path, read_file(corpus_dir() + "/valid-basic.loomora"));

    Opened o;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, nullptr) == 0);
    const std::string frame_before = item_json(o.p, LOOMORA_KIND_FRAME, 1);

    write_file(tmp, read_file(corpus_dir() + "/valid-mesh3d.loomora"));
    REQUIRE(replace_file(tmp, path));

    // The old handle still reads the old project...
    REQUIRE(item_json(o.p, LOOMORA_KIND_FRAME, 1) == frame_before);
    REQUIRE(loomora_layout_nail_count(o.p, 1) == 8);
    loomora_step steps[7];
    REQUIRE(loomora_result_read_steps(o.p, 1, 0, 7, steps) == 7);
    // ...and a new open sees the new file.
    Opened n;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &n.p, nullptr) == 0);
    REQUIRE(item_json(n.p, LOOMORA_KIND_FRAME, 1) != frame_before);
}

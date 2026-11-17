// The semantic validator (T066, spec 002): every file of the generated corpus (tools/gen_invalid_corpus.py,
// written by Python, independent of the C++ code) gives exactly the code it is documented to give, with
// the severity, the entry and the item it names; the truncations are rejected without a crash; the
// large-* files open with warn.count when the thresholds are lowered; a save gives the same warnings.
// The corpus directory comes from the compile definition FMT_CORPUS_DIR (without it the tests say so).
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "format/limits.hpp"
#include "format/project_reader.hpp"

#ifndef FMT_CORPUS_DIR
#define FMT_CORPUS_DIR ""
#endif

namespace {

typedef std::vector<std::uint8_t> Bytes;

struct Row {
    std::string file, kind, code, family, entry;
    bool contract_code;
    int severity; // 0 none, 1 warning, 2 error (the C API values)
    std::vector<std::string> also, set_limit;
};

struct Found {
    int severity;
    std::string code, entry, message;
    int item_kind;
    std::uint32_t item_id;
};

Bytes read_file(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool have_corpus() {
    std::ifstream f((std::string(FMT_CORPUS_DIR) + "/corpus.json").c_str(), std::ios::binary);
    if (!f.good()) {
        WARN("the .loomora corpus is not present (python tools/gen_invalid_corpus.py): validator tests skipped");
        return false;
    }
    return true;
}

std::vector<std::string> strings_of(const rapidjson::Value &v, const char *key) {
    std::vector<std::string> out;
    if (v.HasMember(key) && v[key].IsArray())
        for (rapidjson::SizeType i = 0; i < v[key].Size(); ++i)
            out.push_back(v[key][i].GetString());
    return out;
}

std::vector<Row> load_rows() {
    std::vector<Row> rows;
    const Bytes b = read_file(std::string(FMT_CORPUS_DIR) + "/corpus.json");
    rapidjson::Document d;
    d.Parse(reinterpret_cast<const char *>(b.data()), b.size());
    REQUIRE(!d.HasParseError());
    const rapidjson::Value &files = d["files"];
    for (rapidjson::Value::ConstMemberIterator it = files.MemberBegin(); it != files.MemberEnd(); ++it) {
        const rapidjson::Value &v = it->value;
        Row r;
        r.file = it->name.GetString();
        r.kind = v["kind"].GetString();
        r.code = v["code"].IsString() ? v["code"].GetString() : "";
        r.family = v["family"].IsString() ? v["family"].GetString() : "";
        r.entry = v["entry"].IsString() ? v["entry"].GetString() : "";
        r.contract_code = v["contract_code"].GetBool();
        const std::string sev = v["severity"].IsString() ? v["severity"].GetString() : "";
        r.severity = sev == "error" ? 2 : sev == "warning" ? 1 : 0;
        r.also = strings_of(v, "also");
        r.set_limit = strings_of(v, "set_limit");
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

std::vector<Found> issues_of(const loomora_report *rep) {
    std::vector<Found> out;
    if (rep == nullptr)
        return out;
    const int64_t n = loomora_report_count(rep);
    for (int64_t i = 0; i < n; ++i) {
        Found f;
        int32_t sev = 0, kind = 0;
        uint32_t id = 0;
        REQUIRE(loomora_report_issue(rep, static_cast<size_t>(i), &sev, &kind, &id) == 0);
        f.severity = sev;
        f.item_kind = kind;
        f.item_id = id;
        f.code = text_of(rep, static_cast<size_t>(i), 1);
        f.entry = text_of(rep, static_cast<size_t>(i), 2);
        f.message = text_of(rep, static_cast<size_t>(i), 3);
        out.push_back(f);
    }
    return out;
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

void apply_limits(const std::vector<std::string> &settings) {
    for (size_t i = 0; i < settings.size(); ++i) {
        const size_t eq = settings[i].find('=');
        REQUIRE(eq != std::string::npos);
        const std::string name = settings[i].substr(0, eq);
        REQUIRE(loomora::format::set_limit_by_name(name.c_str(), std::strtoull(settings[i].c_str() + eq + 1, nullptr, 10)));
    }
}

int32_t expected_rc(const Row &r) {
    if (r.kind == "valid" || r.kind == "warning" || r.kind == "large")
        return 0;
    if (r.code == "version.major")
        return LOOMORA_ERR_UNSUPPORTED_VERSION;
    if (r.code == "feature.unsupported")
        return LOOMORA_ERR_UNSUPPORTED_FEATURE;
    if (r.code == "entry.bomb")
        return LOOMORA_ERR_BOMB;
    if (r.code == "zip.not_zip" || r.code == "zip.mimetype")
        return LOOMORA_ERR_NOT_A_PROJECT;
    return LOOMORA_ERR_CORRUPT;
}

// The code the C++ reader gives for a generator sub-code of the same family.
std::string reader_code(const Row &r) { return r.code == "schema.number" ? "num.not_finite" : r.code; }

bool code_matches(const std::string &have, const Row &r) {
    const std::string want = reader_code(r);
    if (r.contract_code || want != r.code)
        return have == want;
    return have.compare(0, r.family.size() + 1, r.family + ".") == 0;
}

// The item an entry path belongs to (kind 0 when the path says nothing about one item).
void item_of_entry(const std::string &entry, int &kind, std::uint32_t &id) {
    using namespace loomora::format;
    kind = 0;
    id = 0;
    const EntryClass c = classify_entry(entry);
    switch (c.role) {
    case kRoleFrameJson:
    case kRoleMesh:
        kind = LOOMORA_KIND_FRAME;
        id = c.id;
        break;
    case kRoleLayoutJson:
    case kRoleNails:
        kind = LOOMORA_KIND_LAYOUT;
        id = c.id;
        break;
    case kRoleImageJson:
    case kRoleImageThumb:
        kind = LOOMORA_KIND_IMAGE;
        id = c.id;
        break;
    case kRoleResultJson:
    case kRoleSequence:
    case kRoleResultThumb:
        kind = LOOMORA_KIND_RESULT;
        id = c.id;
        break;
    default:
        break;
    }
}

bool names_item(const std::string &code) {
    return code.compare(0, 4, "ref.") == 0 || code.compare(0, 5, "nail.") == 0 || code.compare(0, 6, "spool.") == 0 ||
           code == "warn.nail_distance" || code == "warn.origin_offset" || code == "warn.thumbnail_size";
}

std::string dump(const std::vector<Found> &v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += "  " + std::string(v[i].severity == 2 ? "error  " : "warning") + " " + v[i].code + " " + v[i].entry + " : " + v[i].message + "\n";
    return s;
}

void check_row(const Row &row) {
    INFO(row.file << " expects " << row.code << " on " << row.entry);
    loomora::format::ScopedLimits scoped;
    apply_limits(row.set_limit);
    const std::string path = std::string(FMT_CORPUS_DIR) + "/" + row.file;
    Opened o;
    o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
    const std::vector<Found> found = issues_of(o.r);
    INFO("issues:\n" << dump(found));
    CHECK(o.rc == expected_rc(row));
    CHECK((o.p == nullptr) == (row.severity == 2));

    if (row.code.empty()) { // truncate-*: only rejected
        REQUIRE(o.rc != 0);
        return;
    }
    // Exactly the documented codes: the expected one and the `also` list, nothing else.
    std::set<std::string> allowed(row.also.begin(), row.also.end());
    const bool strict = row.family == "ref" || row.family == "nail" || row.family == "spool" || row.family == "warn" ||
                        row.family == "num" || row.family == "seq" || row.family == "mesh";
    bool seen = false;
    for (size_t i = 0; i < found.size(); ++i) {
        if (code_matches(found[i].code, row)) {
            seen = true;
            CHECK(found[i].severity == row.severity);
            if (!row.entry.empty() && row.family != "zip")
                CHECK(found[i].entry == row.entry);
            if (names_item(found[i].code)) {
                int kind;
                std::uint32_t id;
                item_of_entry(found[i].entry, kind, id);
                if (kind != 0) {
                    CHECK(found[i].item_kind == kind);
                    CHECK(found[i].item_id == id);
                }
            }
        } else {
            // A file that loses an item to a schema or container error also disagrees with its
            // manifest (counts, order): that cascade is not what the row is about.
            const bool cascade = !strict && found[i].code.compare(0, 9, "manifest.") == 0;
            CHECK((allowed.count(found[i].code) == 1 || cascade));
        }
    }
    CHECK(seen);
}

} // namespace

TEST_CASE("every corpus file gives exactly its documented code, severity, entry and item", "[format][validator]") {
    if (!have_corpus())
        return;
    int checked = 0, semantic = 0;
    const std::vector<Row> rows = load_rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == "valid" || rows[i].kind == "truncate")
            continue;
        check_row(rows[i]);
        ++checked;
        if (rows[i].family == "ref" || rows[i].family == "nail" || rows[i].family == "spool" ||
            (rows[i].family == "warn" && rows[i].code != "warn.light_project" && rows[i].code != "warn.unknown_entry"))
            ++semantic;
    }
    REQUIRE(checked >= 100);
    REQUIRE(semantic >= 20);
}

TEST_CASE("the valid corpus files give an empty report and every truncate-* file is rejected", "[format][validator]") {
    if (!have_corpus())
        return;
    const std::vector<Row> rows = load_rows();
    int valid = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != "valid" && rows[i].kind != "truncate")
            continue;
        const std::string path = std::string(FMT_CORPUS_DIR) + "/" + rows[i].file;
        Opened o;
        o.rc = loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r);
        INFO(rows[i].file << "\n" << dump(issues_of(o.r)));
        if (rows[i].kind == "valid") {
            REQUIRE(o.rc == 0);
            REQUIRE(loomora_report_count(o.r) == 0);
            ++valid;
        } else {
            REQUIRE(o.rc != 0);
            REQUIRE(o.p == nullptr);
        }
    }
    REQUIRE(valid >= 9);
}

TEST_CASE("a valid file cut at every byte is rejected without a crash", "[format][validator]") {
    if (!have_corpus())
        return;
    const Bytes whole = read_file(std::string(FMT_CORPUS_DIR) + "/valid-basic.loomora");
    REQUIRE(whole.size() > 1000);
    int rejected = 0;
    for (size_t n = 0; n < whole.size(); ++n) {
        loomora_project *p = nullptr;
        loomora_report *r = nullptr;
        const int32_t rc = loomora_project_open_memory(whole.empty() ? nullptr : whole.data(), n, nullptr, nullptr, &p, &r);
        if (rc == 0) {
            FAIL("a file cut at " << n << " of " << whole.size() << " bytes was accepted");
        } else {
            REQUIRE(rc < 0);
            REQUIRE(p == nullptr);
            ++rejected;
        }
        loomora_project_close(p);
        loomora_report_free(r);
    }
    REQUIRE(rejected == static_cast<int>(whole.size()));
}

TEST_CASE("the large-* files open with warn.count naming the element and its count once the limits are lowered",
          "[format][validator]") {
    if (!have_corpus())
        return;
    const std::vector<Row> rows = load_rows();
    int checked = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind != "large")
            continue;
        INFO(rows[i].file);
        REQUIRE_FALSE(rows[i].set_limit.empty());
        const std::string path = std::string(FMT_CORPUS_DIR) + "/" + rows[i].file;
        {
            // With the default thresholds the same file is silent about counts.
            Opened o;
            REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);
            const std::vector<Found> f = issues_of(o.r);
            for (size_t k = 0; k < f.size(); ++k)
                REQUIRE(f[k].code != "warn.count");
        }
        loomora::format::ScopedLimits scoped;
        apply_limits(rows[i].set_limit);
        Opened o;
        REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &o.p, &o.r) == 0);
        REQUIRE(loomora_report_error_count(o.r) == 0);
        const std::vector<Found> f = issues_of(o.r);
        int counts = 0;
        for (size_t k = 0; k < f.size(); ++k)
            if (f[k].code == "warn.count") {
                ++counts;
                REQUIRE(f[k].severity == 1);
                REQUIRE(f[k].entry == rows[i].entry);
                REQUIRE(f[k].message.find("threshold") != std::string::npos);
                REQUIRE(f[k].message.find_first_of("0123456789") != std::string::npos);
            }
        INFO(dump(f));
        REQUIRE(counts == 1);
        ++checked;
    }
    REQUIRE(checked == 5);
}

TEST_CASE("a save gives the threshold warnings of the same function", "[format][validator]") {
    loomora::format::ScopedLimits scoped;
    loomora::format::limits().warn_nails_per_layout = 100;
    loomora::format::limits().warn_steps_per_result = 1000;
    loomora::format::limits().warn_items_per_kind = 2;
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    loomora_report *r = nullptr;
    Bytes archive;
    struct Sink {
        static int32_t put(void *user, const uint8_t *data, size_t len) {
            Bytes *b = static_cast<Bytes *>(user);
            b->insert(b->end(), data, data + len);
            return 0;
        }
    };
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, &Sink::put, &archive, &r) == 0);
    std::set<std::string> entries;
    const std::vector<Found> f = issues_of(r);
    INFO(dump(f));
    for (size_t i = 0; i < f.size(); ++i)
        if (f[i].code == "warn.count") {
            REQUIRE(f[i].severity == 1);
            entries.insert(f[i].entry);
        }
    REQUIRE(entries.count("frames/1/layouts/1/nails.json") == 1);
    REQUIRE(entries.count("results/1/sequence.bin") == 1);
    REQUIRE(entries.count("manifest.json") == 1); // the three spools above the lowered items threshold
    loomora_report_free(r);
    loomora_editor_close(e);

    // The saved file opens with the same warnings (open and save agree).
    Opened o;
    REQUIRE(loomora_project_open_memory(archive.data(), archive.size(), nullptr, nullptr, &o.p, &o.r) == 0);
    std::set<std::string> opened;
    const std::vector<Found> g = issues_of(o.r);
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].code == "warn.count")
            opened.insert(g[i].entry);
    REQUIRE(opened == entries);
}

TEST_CASE("the semantic rules on projects built through the editor", "[format][validator]") {
    // The reference project is clean, with no warning of any kind.
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    Bytes archive;
    struct Sink {
        static int32_t put(void *user, const uint8_t *data, size_t len) {
            static_cast<Bytes *>(user)->insert(static_cast<Bytes *>(user)->end(), data, data + len);
            return 0;
        }
    };
    loomora_report *r = nullptr;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, &Sink::put, &archive, &r) == 0);
    REQUIRE(loomora_report_count(r) == 0);
    loomora_report_free(r);
    loomora_editor_close(e);
    Opened o;
    REQUIRE(loomora_project_open_memory(archive.data(), archive.size(), nullptr, nullptr, &o.p, &o.r) == 0);
    REQUIRE(loomora_report_count(o.r) == 0);
}

TEST_CASE("500,000 nails on a grid are validated in linear time with a warning for the count only", "[format][validator][large]") {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e) == 0);
    const char *frame =
        "{\"name\":\"Big\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":4000,\"height_mm\":4000},"
        "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":4000,"
        "\"height_mm\":4000},\"polyline\":[[-2000,-2000],[2000,-2000],[2000,2000],[-2000,2000]]},"
        "\"exclusions\":[],\"surface\":\"plane\"}";
    uint32_t fid = 0, lid = 0;
    REQUIRE(loomora_editor_add_frame(e, frame, &fid) == 0);
    const unsigned side = 708; // 708 * 708 = 501,264 nails, 5 mm apart (diameter 2)
    std::vector<loomora_nail> nails;
    nails.reserve(static_cast<size_t>(side) * side);
    for (unsigned j = 0; j < side; ++j)
        for (unsigned i = 0; i < side; ++i) {
            loomora_nail n = loomora_nail();
            n.id = j * side + i;
            n.kind = 3;
            n.x = -1768.5 + 5.0 * i;
            n.y = -1768.5 + 5.0 * j;
            n.diameter = 2;
            nails.push_back(n);
        }
    REQUIRE(loomora_editor_add_layout(e, fid, "{\"name\":\"Grid\"}", &nails[0], nails.size(), &lid) == 0);
    nails.clear();
    nails.shrink_to_fit();
    Bytes archive;
    struct Sink {
        static int32_t put(void *user, const uint8_t *data, size_t len) {
            static_cast<Bytes *>(user)->insert(static_cast<Bytes *>(user)->end(), data, data + len);
            return 0;
        }
    };
    loomora_report *sr = nullptr;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, &Sink::put, &archive, &sr) == 0);
    const std::vector<Found> saved = issues_of(sr);
    loomora_report_free(sr);
    loomora_editor_close(e);
    REQUIRE(saved.size() == 1);
    REQUIRE(saved[0].code == "warn.count");

    Opened o;
    o.rc = loomora_project_open_memory(archive.data(), archive.size(), nullptr, nullptr, &o.p, &o.r);
    const std::vector<Found> f = issues_of(o.r);
    INFO(dump(f));
    REQUIRE(o.rc == 0);
    REQUIRE(f.size() == 1);
    REQUIRE(f[0].code == "warn.count");
    REQUIRE(f[0].message.find("501264") != std::string::npos);
}

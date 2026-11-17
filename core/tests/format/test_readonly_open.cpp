// T041, User Story 2 (use mode is read-only, SC-012): a file marked read-only opens; opening, listing,
// reading the steps and the thumbnails and validating change neither its bytes, nor its modification
// time (at the finest resolution the platform gives), nor its attributes, and create no file in its
// directory. A reader opened before the project is replaced atomically by the real writer
// (loomora_editor_save_path) keeps reading only the old file, a reader opened after reads only the new
// one: every entry of each is compared with the archive it must come from, so a mix would show.
#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "fs_test_util.hpp"

using basic_project::Bytes;

namespace {

typedef std::map<std::string, Bytes> Entries;

struct Stamp {
    long long seconds;
    long long nanos;
    unsigned mode; // POSIX permission bits, or the Windows attribute bits
    bool operator==(const Stamp &o) const { return seconds == o.seconds && nanos == o.nanos && mode == o.mode; }
};

Stamp stamp_of(const std::string &path) {
    Stamp s = {0, 0, 0};
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    REQUIRE(GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d) != 0);
    const unsigned long long t = (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) |
                                 d.ftLastWriteTime.dwLowDateTime; // 100 ns units
    s.seconds = static_cast<long long>(t / 10000000ull);
    s.nanos = static_cast<long long>(t % 10000000ull) * 100;
    s.mode = d.dwFileAttributes;
#else
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    s.seconds = static_cast<long long>(st.st_mtim.tv_sec);
    s.nanos = static_cast<long long>(st.st_mtim.tv_nsec);
    s.mode = static_cast<unsigned>(st.st_mode & 07777);
#endif
    return s;
}

void set_read_only(const std::string &path, bool read_only) {
#ifdef _WIN32
    REQUIRE(SetFileAttributesA(path.c_str(), read_only ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0);
#else
    REQUIRE(chmod(path.c_str(), read_only ? 0444 : 0644) == 0);
#endif
}

void save_basic(const std::string &path, const char *title, const char *modified) {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    if (title != nullptr)
        REQUIRE(loomora_editor_set_title(e, title) == 0);
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_path(e, path.c_str(), modified, 0, nullptr, nullptr, &report) == 0);
    loomora_report_free(report);
    loomora_editor_close(e);
}

Bytes read_entry(const loomora_project *p, const std::string &name) {
    const std::int64_t size = loomora_project_entry_size(p, name.c_str());
    REQUIRE(size >= 0);
    Bytes b(static_cast<std::size_t>(size));
    if (size > 0)
        REQUIRE(loomora_project_read_entry(p, name.c_str(), 0, &b[0], b.size()) == size);
    return b;
}

// Every entry of the project, read through the API: manifest.json, mimetype and the ones it lists.
Entries all_entries(const loomora_project *p) {
    Entries out;
    out["manifest.json"] = read_entry(p, "manifest.json");
    out["mimetype"] = read_entry(p, "mimetype");
    rapidjson::Document m;
    m.Parse(reinterpret_cast<const char *>(out["manifest.json"].data()), out["manifest.json"].size());
    REQUIRE_FALSE(m.HasParseError());
    const rapidjson::Value &listed = m["entries"];
    for (rapidjson::Value::ConstMemberIterator it = listed.MemberBegin(); it != listed.MemberEnd(); ++it)
        out[it->name.GetString()] = read_entry(p, it->name.GetString());
    return out;
}

Entries entries_of_file(const std::string &path) {
    loomora_project *p = nullptr;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &p, nullptr) == 0);
    const Entries e = all_entries(p);
    loomora_project_close(p);
    return e;
}

std::string title_of(const Entries &e) {
    const Bytes &m = e.find("manifest.json")->second;
    rapidjson::Document d;
    d.Parse(reinterpret_cast<const char *>(m.data()), m.size());
    REQUIRE_FALSE(d.HasParseError());
    return d["title"].GetString();
}

} // namespace

TEST_CASE("use mode: a read-only file opens and nothing changes (SC-012)", "[format][readonly]") {
    fstest::ScratchDir dir("readonly");
    const std::string path = dir.file("ro.loomora");
    save_basic(path, nullptr, basic_project::kModifiedAt);
    Bytes original;
    REQUIRE(fstest::read_file(path, original));
    set_read_only(path, true);
    const Stamp before = stamp_of(path);
    const std::vector<std::string> names = dir.names();
    REQUIRE(names.size() == 1);

    {
        loomora_project *p = nullptr;
        loomora_report *report = nullptr;
        REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &p, &report) == 0);
        CHECK(loomora_report_count(report) == 0);
        loomora_report_free(report);
        // Listing: every item of every kind, with its JSON.
        for (int32_t kind = LOOMORA_KIND_FRAME; kind <= LOOMORA_KIND_RESULT; ++kind) {
            const std::int64_t n = loomora_project_count(p, kind);
            std::vector<std::uint32_t> ids(static_cast<std::size_t>(n) + 1);
            REQUIRE(loomora_project_ids(p, kind, &ids[0], static_cast<std::size_t>(n)) == n);
            for (std::int64_t i = 0; i < n; ++i)
                CHECK(loomora_project_item_json(p, kind, ids[static_cast<std::size_t>(i)], nullptr, 0) > 0);
        }
        // The steps of the result and its thumbnail, the image thumbnail, the nails.
        std::vector<loomora_step> steps(basic_project::kStepCount);
        CHECK(loomora_result_read_steps(p, 1, 0, steps.size(), &steps[0]) ==
              static_cast<std::int64_t>(basic_project::kStepCount));
        CHECK(read_entry(p, "results/1/thumb.png") == basic_project::make_png(4, 4, 200));
        CHECK(read_entry(p, "images/1/thumb.png") == basic_project::make_png(4, 4, 10));
        std::vector<loomora_nail> nails(basic_project::kNailCount);
        CHECK(loomora_layout_read_nails(p, 1, 0, nails.size(), &nails[0]) ==
              static_cast<std::int64_t>(basic_project::kNailCount));
        all_entries(p);
        loomora_project_close(p);
    }
    {
        loomora_report *report = nullptr;
        REQUIRE(loomora_validate_path(path.c_str(), nullptr, nullptr, &report) == 0);
        CHECK(loomora_report_count(report) == 0);
        loomora_report_free(report);
    }

    Bytes after;
    REQUIRE(fstest::read_file(path, after));
    CHECK(after == original);
    CHECK(stamp_of(path) == before); // time to the finest resolution, and the read-only mark
    CHECK(dir.names() == names);     // no temporary, lock or sidecar file
    set_read_only(path, false);
}

TEST_CASE("use mode: a reader opened before an atomic replace reads only the old file", "[format][readonly]") {
    fstest::ScratchDir dir("replace");
    const std::string path = dir.file("p.loomora");
    save_basic(path, "Old title", basic_project::kModifiedAt);
    const std::string old_copy = dir.file("old.copy");
    Bytes old_bytes;
    REQUIRE(fstest::read_file(path, old_bytes));
    REQUIRE(fstest::write_file(old_copy, old_bytes));
    const Entries old_entries = entries_of_file(old_copy);
    fstest::remove_file(old_copy);
    CHECK(title_of(old_entries) == "Old title");

    loomora_project *old_reader = nullptr;
    REQUIRE(loomora_project_open_path(path.c_str(), nullptr, nullptr, &old_reader, nullptr) == 0);
    // Half of the old file has been read when the replacement happens.
    CHECK(read_entry(old_reader, "images/1/source.png") == basic_project::make_png(8, 8, 40));

    save_basic(path, "New title", "2026-11-18T00:00:00Z"); // the real writer replaces the open file

    // The old reader reads, entry by entry, exactly the old archive.
    const Entries seen = all_entries(old_reader);
    CHECK(seen == old_entries);
    CHECK(title_of(seen) == "Old title");
    std::vector<loomora_step> steps(basic_project::kStepCount);
    CHECK(loomora_result_read_steps(old_reader, 1, 0, steps.size(), &steps[0]) ==
          static_cast<std::int64_t>(basic_project::kStepCount));

    // A reader opened after reads exactly the new one.
    const Entries fresh = entries_of_file(path);
    CHECK(title_of(fresh) == "New title");
    CHECK(fresh != old_entries);
    for (Entries::const_iterator it = fresh.begin(); it != fresh.end(); ++it)
        if (it->first != "manifest.json")
            CHECK(it->second == old_entries.find(it->first)->second); // only the manifest differs

    loomora_project_close(old_reader);
    // Nothing is left beside the file once the old reader is closed (no temporary, no moved-aside file).
    CHECK(dir.names() == std::vector<std::string>(1, "p.loomora"));
}

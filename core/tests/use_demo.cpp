// loomora_use_demo: a minimal "use mode" client of the .loomora format (spec 002, User Story 2),
// written only against the public C API (loomora.h). It opens a project, lists it and reads from it;
// it never writes to the project, never creates a file beside it. tools/check_readonly.py runs it on
// a read-only reference file and checks that nothing changed. Host program: not built for the cross
// targets or the self-test builds. `edit` is added by a later story.
//
//   loomora_use_demo list FILE
//   loomora_use_demo result FILE ID [FIRST [COUNT]]     steps of a result (default: the first 5)
//   loomora_use_demo thumb FILE image|result ID OUT     writes the thumbnail to OUT (outside FILE's directory)
//   loomora_use_demo hold FILE                          for the replace test: opens FILE, prints its
//                                                       fingerprint, waits for a line on stdin, prints
//                                                       the fingerprint again (it must not change)
//   loomora_use_demo write-basic FILE TITLE             test support: writes the `basic` reference
//                                                       project, with this title, to FILE (atomic replace)
//   loomora_use_demo write-small FILE                   test support: writes a project of 3 frames (one 3D with
//                                                       UVs), 5 layouts, 4 images with pixels, 6 spools and 5
//                                                       results to FILE
//   loomora_use_demo export FILE OUT full|light [ID...] writes a copy of FILE to OUT (atomic replace) in that
//                                                       profile, with only the results ID... when given
//
// Output is plain lines. Exit status: 0 success, 1 error (message on stderr), 2 usage.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS // fopen of the output file (MSVC runtime)
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <rapidjson/document.h>

#include "../include/loomora.h"
#include "basic_project.hpp"
#include "export_project.hpp"

namespace {

typedef std::vector<std::uint8_t> Bytes;

int fail(const char *what, int32_t code) {
    std::fprintf(stderr, "loomora_use_demo: %s (code %d)\n", what, static_cast<int>(code));
    return 1;
}

struct Project {
    loomora_project *p;
    Project() : p(0) {}
    ~Project() { loomora_project_close(p); }
    int32_t open(const char *path) {
        loomora_report *report = 0;
        const int32_t rc = loomora_project_open_path(path, 0, 0, &p, &report);
        if (report != 0) {
            const std::int64_t n = loomora_report_count(report);
            for (std::int64_t i = 0; i < n; ++i) {
                uint8_t text[512];
                const std::int64_t len = loomora_report_text(report, static_cast<size_t>(i), 3, text, sizeof text - 1);
                if (len > 0 && len < static_cast<std::int64_t>(sizeof text)) {
                    text[len] = 0;
                    std::fprintf(stderr, "report: %s\n", reinterpret_cast<const char *>(text));
                }
            }
            loomora_report_free(report);
        }
        return rc;
    }

  private:
    Project(const Project &);
    Project &operator=(const Project &);
};

bool read_entry(const loomora_project *p, const std::string &name, Bytes &out) {
    const std::int64_t size = loomora_project_entry_size(p, name.c_str());
    if (size < 0)
        return false;
    out.assign(static_cast<std::size_t>(size), 0);
    return size == 0 || loomora_project_read_entry(p, name.c_str(), 0, &out[0], out.size()) == size;
}

bool parse(const Bytes &b, rapidjson::Document &d) {
    d.Parse(reinterpret_cast<const char *>(b.data()), b.size());
    return !d.HasParseError() && d.IsObject();
}

bool manifest(const loomora_project *p, rapidjson::Document &d) {
    const std::int64_t n = loomora_project_manifest(p, 0, 0);
    if (n <= 0)
        return false;
    Bytes b(static_cast<std::size_t>(n));
    return loomora_project_manifest(p, &b[0], b.size()) == n && parse(b, d);
}

std::string str(const rapidjson::Value &v, const char *key) {
    return v.HasMember(key) && v[key].IsString() ? v[key].GetString() : "";
}

const char *kind_name(int32_t kind) {
    return kind == LOOMORA_KIND_FRAME ? "frame" : kind == LOOMORA_KIND_LAYOUT ? "layout"
         : kind == LOOMORA_KIND_IMAGE ? "image" : kind == LOOMORA_KIND_SPOOL ? "spool" : "result";
}

int list(const char *path) {
    Project pr;
    int32_t rc = pr.open(path);
    if (rc != 0)
        return fail("cannot open the project", rc);
    rapidjson::Document m;
    if (!manifest(pr.p, m))
        return fail("unreadable manifest", -1);
    std::printf("title: %s\nmodified_at: %s\nprofile: %s\n", str(m, "title").c_str(), str(m, "modified_at").c_str(),
                str(m, "profile").c_str());
    for (int32_t kind = LOOMORA_KIND_FRAME; kind <= LOOMORA_KIND_RESULT; ++kind) {
        const std::int64_t n = loomora_project_count(pr.p, kind);
        std::printf("%ss: %d\n", kind_name(kind), static_cast<int>(n));
        std::vector<std::uint32_t> ids(static_cast<std::size_t>(n) + 1);
        if (n > 0 && loomora_project_ids(pr.p, kind, &ids[0], static_cast<std::size_t>(n)) != n)
            return fail("cannot list the identifiers", -1);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint32_t id = ids[static_cast<std::size_t>(i)];
            const std::int64_t len = loomora_project_item_json(pr.p, kind, id, 0, 0);
            Bytes b(len > 0 ? static_cast<std::size_t>(len) : 0);
            rapidjson::Document item;
            if (len <= 0 || loomora_project_item_json(pr.p, kind, id, &b[0], b.size()) != len || !parse(b, item))
                return fail("cannot read an item", static_cast<int32_t>(len));
            std::printf("  %s %u: %s\n", kind_name(kind), static_cast<unsigned>(id), str(item, "name").c_str());
        }
    }
    return 0;
}

int result(const char *path, std::uint32_t id, std::uint64_t first, std::size_t count) {
    Project pr;
    int32_t rc = pr.open(path);
    if (rc != 0)
        return fail("cannot open the project", rc);
    const std::int64_t total = loomora_result_step_count(pr.p, id);
    if (total < 0)
        return fail("no such result", static_cast<int32_t>(total));
    std::printf("result %u: %d steps\n", static_cast<unsigned>(id), static_cast<int>(total));
    std::vector<loomora_step> steps(count > 0 ? count : 1);
    const std::int64_t got = loomora_result_read_steps(pr.p, id, first, count, &steps[0]);
    if (got < 0)
        return fail("cannot read the steps", static_cast<int32_t>(got));
    for (std::int64_t i = 0; i < got; ++i) {
        const loomora_step &s = steps[static_cast<std::size_t>(i)];
        std::printf("step %llu: nail=%u spool=%u rotation=%u type=%u\n",
                    static_cast<unsigned long long>(first + static_cast<std::uint64_t>(i)),
                    static_cast<unsigned>(s.nail), static_cast<unsigned>(s.spool_index),
                    static_cast<unsigned>(s.rotation), static_cast<unsigned>(s.type));
    }
    return 0;
}

int thumb(const char *path, const char *kind, std::uint32_t id, const char *out_path) {
    Project pr;
    int32_t rc = pr.open(path);
    if (rc != 0)
        return fail("cannot open the project", rc);
    const std::string entry = std::string(std::strcmp(kind, "image") == 0 ? "images/" : "results/") +
                              basic_project::num(id) + "/thumb.png";
    Bytes b;
    if (std::strcmp(kind, "image") != 0 && std::strcmp(kind, "result") != 0)
        return fail("the kind is image or result", -1);
    if (!read_entry(pr.p, entry, b))
        return fail("no such thumbnail", LOOMORA_ERR_NOT_FOUND);
    std::FILE *f = std::fopen(out_path, "wb");
    if (f == 0)
        return fail("cannot create the output file", -1);
    const bool ok = b.empty() || std::fwrite(&b[0], 1, b.size(), f) == b.size();
    if (std::fclose(f) != 0 || !ok)
        return fail("cannot write the output file", -1);
    std::printf("thumbnail %s: %d bytes\n", entry.c_str(), static_cast<int>(b.size()));
    return 0;
}

// FNV-1a 64 over every entry in name order: name, a zero byte, the entry bytes (manifest.json and
// mimetype included). The same value is computed by tools/check_readonly.py from the archive.
std::string fingerprint(const loomora_project *p, std::string &title) {
    rapidjson::Document m;
    std::map<std::string, Bytes> entries;
    if (!manifest(p, m))
        return "unreadable-manifest";
    title = str(m, "title");
    read_entry(p, "manifest.json", entries["manifest.json"]);
    read_entry(p, "mimetype", entries["mimetype"]);
    const rapidjson::Value &listed = m["entries"];
    for (rapidjson::Value::ConstMemberIterator it = listed.MemberBegin(); it != listed.MemberEnd(); ++it)
        if (!read_entry(p, it->name.GetString(), entries[it->name.GetString()]))
            return std::string("missing-") + it->name.GetString();
    std::uint64_t h = 14695981039346656037ull;
    for (std::map<std::string, Bytes>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
        for (std::size_t i = 0; i <= it->first.size(); ++i) {
            h ^= i < it->first.size() ? static_cast<unsigned char>(it->first[i]) : 0u;
            h *= 1099511628211ull;
        }
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            h ^= it->second[i];
            h *= 1099511628211ull;
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

int hold(const char *path) {
    Project pr;
    int32_t rc = pr.open(path);
    if (rc != 0)
        return fail("cannot open the project", rc);
    std::string title;
    std::string fp = fingerprint(pr.p, title);
    std::printf("opened title=%s fp=%s\n", title.c_str(), fp.c_str());
    std::fflush(stdout);
    char line[16];
    if (std::fgets(line, sizeof line, stdin) == 0)
        return fail("no signal on stdin", -1);
    std::string title2;
    const std::string fp2 = fingerprint(pr.p, title2);
    std::printf("after title=%s fp=%s\n", title2.c_str(), fp2.c_str());
    return fp2 == fp && title2 == title ? 0 : fail("the project changed under the reader", -1);
}

int write_basic(const char *path, const char *title) {
    loomora_editor *e = 0;
    int32_t rc = basic_project::build_basic(&e);
    if (rc != 0)
        return fail("cannot build the project", rc);
    rc = loomora_editor_set_title(e, title);
    if (rc == 0)
        rc = loomora_editor_save_path(e, path, "2026-11-18T00:00:00Z", 0, 0, 0, 0);
    loomora_editor_close(e);
    if (rc != 0)
        return fail("cannot save the project", rc);
    std::printf("wrote %s\n", path);
    return 0;
}

int usage() {
    std::fputs("usage: loomora_use_demo list FILE | result FILE ID [FIRST [COUNT]] | thumb FILE image|result ID OUT |\n"
               "                       hold FILE | write-basic FILE TITLE | write-small FILE |\n"
               "                       export FILE OUT full|light [ID...]\n",
               stderr);
    return 2;
}

int write_small(const char *path) {
    loomora_editor *e = 0;
    int32_t rc = export_project::build_pixels(&e, 0);
    if (rc != 0)
        return fail("cannot build the project", rc);
    rc = loomora_editor_save_path(e, path, "2026-11-18T00:00:00Z", 0, 0, 0, 0);
    loomora_editor_close(e);
    if (rc != 0)
        return fail("cannot save the project", rc);
    std::printf("wrote %s\n", path);
    return 0;
}

// The light or subset copy of a project (loomora_project_export_path), written with the time of write-basic.
int export_copy(const char *path, const char *out_path, const char *profile, int count, char **ids_text) {
    int32_t which = 0;
    if (std::strcmp(profile, "full") == 0)
        which = LOOMORA_PROFILE_FULL;
    else if (std::strcmp(profile, "light") == 0)
        which = LOOMORA_PROFILE_LIGHT;
    else
        return usage();
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < count; ++i)
        ids.push_back(static_cast<std::uint32_t>(std::strtoul(ids_text[i], 0, 10)));
    Project pr;
    int32_t rc = pr.open(path);
    if (rc != 0)
        return fail("cannot open the project", rc);
    loomora_report *report = 0;
    rc = loomora_project_export_path(pr.p, out_path, which, ids.empty() ? 0 : &ids[0], ids.size(),
                                     "2026-11-18T00:00:00Z", 0, 0, 0, &report);
    if (report != 0) {
        const std::int64_t n = loomora_report_count(report);
        for (std::int64_t i = 0; i < n; ++i) {
            uint8_t text[512];
            const std::int64_t len = loomora_report_text(report, static_cast<size_t>(i), 3, text, sizeof text - 1);
            if (len > 0 && len < static_cast<std::int64_t>(sizeof text)) {
                text[len] = 0;
                std::fprintf(stderr, "report: %s\n", reinterpret_cast<const char *>(text));
            }
        }
        loomora_report_free(report);
    }
    if (rc != 0)
        return fail("cannot export the project", rc);
    std::printf("exported %s (%s, %s)\n", out_path, profile, ids.empty() ? "all results" : "chosen results");
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3)
        return usage();
    const std::string cmd = argv[1];
    if (cmd == "export" && argc >= 5)
        return export_copy(argv[2], argv[3], argv[4], argc - 5, argv + 5);
    if (cmd == "write-small" && argc == 3)
        return write_small(argv[2]);
    if (cmd == "list" && argc == 3)
        return list(argv[2]);
    if (cmd == "result" && argc >= 4 && argc <= 6)
        return result(argv[2], static_cast<std::uint32_t>(std::strtoul(argv[3], 0, 10)),
                      argc > 4 ? std::strtoull(argv[4], 0, 10) : 0, argc > 5 ? std::strtoul(argv[5], 0, 10) : 5);
    if (cmd == "thumb" && argc == 6)
        return thumb(argv[2], argv[3], static_cast<std::uint32_t>(std::strtoul(argv[4], 0, 10)), argv[5]);
    if (cmd == "hold" && argc == 3)
        return hold(argv[2]);
    if (cmd == "write-basic" && argc == 4)
        return write_basic(argv[2], argv[3]);
    return usage();
}

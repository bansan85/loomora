// FR-028: on Windows older than 10 1809 the file sink may replace a target that a reader holds
// open by renaming the old file aside (`<target>.old-xxxxxxxx`) and deleting it. When that last
// delete keeps failing, the old file would stay next to the project with nothing said: the save
// succeeds and reports the warning `warn.leftover_file` naming the file.
#include <catch2/catch.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "format/io/sink.hpp"
#include "format/project_writer.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"
#include "fs_test_util.hpp"

#ifdef _WIN32
#include "format/io/file.hpp"
#endif

using namespace loomora::format;

namespace {

// A sink that did not manage to remove a file at its commit.
class LeftoverSink : public Sink {
  public:
    explicit LeftoverSink(const std::string &leftover) : leftover_(leftover) {}
    void write(const void *, std::size_t) override {}
    std::uint64_t position() const override { return 0; }
    void patch_at(std::uint64_t, const void *, std::size_t) override {}
    void commit() override {}
    void abort() noexcept override {}
    std::string leftover_file() const override { return leftover_; }

  private:
    std::string leftover_;
};

} // namespace

TEST_CASE("a file left behind by a commit is a warning naming the file", "[format][leftover]") {
    Report report;
    report_leftover(LeftoverSink("dir/project.loomora.old-0123abcd"), &report);
    REQUIRE(report.count() == 1);
    const Issue &issue = report.at(0);
    CHECK(issue.severity == kWarning);
    CHECK(issue.code == "warn.leftover_file");
    CHECK(issue.message.find("dir/project.loomora.old-0123abcd") != std::string::npos);
    CHECK(report.error_count() == 0);
}

TEST_CASE("a commit that left nothing adds no warning", "[format][leftover]") {
    Report report;
    report_leftover(LeftoverSink(""), &report);
    CHECK(report.count() == 0);
    report_leftover(LeftoverSink("x.old-1"), nullptr); // no report asked: nothing to do, no crash
}

TEST_CASE("the delete hook is null in production", "[format][leftover]") {
    ScopedHooks scope;
    CHECK(!should_fail_delete_aside());
    CHECK(!should_skip_posix_rename());
}

#ifdef _WIN32
namespace {

bool always_fail_delete(void *) { return true; }
bool skip_posix(void *) { return true; }
bool count_delete_attempts(void *user) { // never fails the delete, only counts the attempts
    ++*static_cast<int *>(user);
    return false;
}

// Every issue of `report` whose code is `code`, as its message.
std::vector<std::string> messages_of(loomora_report *report, const std::string &code) {
    std::vector<std::string> out;
    for (int64_t i = 0; report != nullptr && i < loomora_report_count(report); ++i) {
        std::uint8_t text[512] = {0};
        loomora_report_text(report, static_cast<std::size_t>(i), 1, text, sizeof text);
        if (std::string(reinterpret_cast<const char *>(text)) != code)
            continue;
        loomora_report_text(report, static_cast<std::size_t>(i), 3, text, sizeof text);
        out.push_back(reinterpret_cast<const char *>(text));
    }
    return out;
}

std::vector<std::string> aside_files(const fstest::ScratchDir &dir) {
    std::vector<std::string> out;
    const std::vector<std::string> names = dir.names();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i].find(".old-") != std::string::npos)
            out.push_back(names[i]);
    return out;
}

bool bytes_of(const std::string &path, fstest::Bytes &out) { return fstest::read_file(path, out); }

// A project saved to `path`, then saved again while a reader holds `path` open, with the POSIX
// rename skipped so that the rename aside step really runs on every Windows version. The reader
// shares delete, as a use-mode reader does, and keeps reading the old file.
struct AsideFixture {
    fstest::ScratchDir dir;
    std::string path;
    loomora_editor *editor;
    fstest::Bytes old_bytes;
    fstest::Bytes new_bytes; // what a second save must produce
    std::unique_ptr<Source> reader;

    AsideFixture() : dir("leftover"), path(dir.file("project.loomora")), editor(nullptr) {
        REQUIRE(basic_project::build_basic(&editor) == 0);
        REQUIRE(loomora_editor_save_path(editor, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr,
                                         nullptr) == 0);
        REQUIRE(bytes_of(path, old_bytes));
        const std::string reference = dir.file("reference.loomora");
        REQUIRE(loomora_editor_save_path(editor, reference.c_str(), kSecondSave, 0, nullptr, nullptr, nullptr) == 0);
        REQUIRE(bytes_of(reference, new_bytes));
        REQUIRE(fstest::remove_file(reference));
        REQUIRE(new_bytes != old_bytes);
        reader = open_file_source(path); // holds the target open
    }
    ~AsideFixture() {
        reader.reset();
        loomora_editor_close(editor);
        const std::vector<std::string> left = aside_files(dir);
        for (std::size_t i = 0; i < left.size(); ++i)
            fstest::remove_file(dir.file(left[i]));
    }

    static const char *const kSecondSave;

    // The second save; returns its status and its report.
    int save_again(loomora_report **report) {
        return loomora_editor_save_path(editor, path.c_str(), kSecondSave, 0, nullptr, nullptr, report);
    }
    bool reader_sees_old_file() const {
        fstest::Bytes seen(old_bytes.size(), 0);
        return reader->size() == old_bytes.size() &&
               reader->read_at(0, &seen[0], seen.size()) == seen.size() && seen == old_bytes;
    }

  private:
    AsideFixture(const AsideFixture &);
    AsideFixture &operator=(const AsideFixture &);
};

const char *const AsideFixture::kSecondSave = "2026-11-18T08:00:00Z";

} // namespace

// The rename aside step is taken on a system without the POSIX rename (before Windows 10 1809)
// when a reader holds the target; the hook makes it run on every Windows version. The old file is
// deleted, so nothing stays and nothing is reported.
TEST_CASE("a save with a reader open renames the old file aside and deletes it", "[format][leftover][win32]") {
    AsideFixture f;
    ScopedHooks scope;
    int delete_attempts = 0;
    hooks().skip_posix_rename = skip_posix;
    hooks().user = &delete_attempts;
    hooks().fail_delete_aside = count_delete_attempts;
    loomora_report *report = nullptr;
    REQUIRE(f.save_again(&report) == 0);

    CHECK(delete_attempts >= 1); // the aside path ran: its delete was attempted
    fstest::Bytes got;
    REQUIRE(bytes_of(f.path, got));
    CHECK(got == f.new_bytes);
    CHECK(f.reader_sees_old_file()); // the reader kept the complete old file
    CHECK(aside_files(f.dir).empty());
    CHECK(messages_of(report, "warn.leftover_file").empty());
    loomora_report_free(report);
}

TEST_CASE("a save that cannot delete the aside file succeeds and reports it", "[format][leftover][win32]") {
    AsideFixture f;
    ScopedHooks scope;
    hooks().skip_posix_rename = skip_posix;
    hooks().fail_delete_aside = always_fail_delete;
    loomora_report *report = nullptr;
    REQUIRE(f.save_again(&report) == 0);

    fstest::Bytes got;
    REQUIRE(bytes_of(f.path, got));
    CHECK(got == f.new_bytes); // the save itself succeeded
    CHECK(f.reader_sees_old_file());
    const std::vector<std::string> left = aside_files(f.dir);
    REQUIRE(left.size() == 1u); // the old file stays...
    fstest::Bytes old_copy;
    REQUIRE(bytes_of(f.dir.file(left[0]), old_copy));
    CHECK(old_copy == f.old_bytes);
    const std::vector<std::string> warnings = messages_of(report, "warn.leftover_file");
    REQUIRE(warnings.size() == 1u); // ...and is reported, by name
    CHECK(warnings[0].find(left[0]) != std::string::npos);
    CHECK(loomora_report_error_count(report) == 0);
    loomora_report_free(report);
}
#endif

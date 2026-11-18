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
}

#ifdef _WIN32
namespace {
bool always_fail_delete(void *) { return true; }
} // namespace

// The aside rename is only taken on a system without the POSIX rename (before Windows 10 1809)
// when a reader holds the target; elsewhere the delete step is never reached and no file is left.
// Either way the save succeeds and the file, when it stays, is reported.
TEST_CASE("a save that cannot delete the aside file succeeds and reports it", "[format][leftover][win32]") {
    fstest::ScratchDir dir("leftover");
    const std::string path = dir.file("project.loomora");
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    REQUIRE(loomora_editor_save_path(e, path.c_str(), basic_project::kModifiedAt, 0, nullptr, nullptr,
                                     nullptr) == 0);
    std::unique_ptr<Source> reader = open_file_source(path); // holds the target open
    ScopedHooks scope;
    hooks().fail_delete_aside = always_fail_delete;
    loomora_report *report = nullptr;
    REQUIRE(loomora_editor_save_path(e, path.c_str(), "2026-11-18T08:00:00Z", 0, nullptr, nullptr, &report) == 0);
    std::size_t aside = 0;
    const std::vector<std::string> names = dir.names();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i].find(".old-") != std::string::npos)
            ++aside;
    int warnings = 0;
    for (int64_t i = 0; report != nullptr && i < loomora_report_count(report); ++i) {
        std::uint8_t code[64] = {0};
        loomora_report_text(report, static_cast<std::size_t>(i), 1, code, sizeof code);
        if (std::string(reinterpret_cast<const char *>(code)) == "warn.leftover_file")
            ++warnings;
    }
    CHECK(warnings == static_cast<int>(aside));
    loomora_report_free(report);
    reader.reset();
    loomora_editor_close(e);
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i].find(".old-") != std::string::npos)
            fstest::remove_file(dir.file(names[i]));
}
#endif

// SC-008 on a real save (T038): a failure at every write, flush and rename step of
// loomora_editor_save_path leaves the previous file intact and no temporary file; a cancelled
// progress callback does the same and returns LOOMORA_ERR_CANCELLED. The failures are injected with
// the hooks of format/testing.hpp.
#include <catch2/catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../../src/c_api_project_handles.hpp"
#include "../basic_project.hpp"
#include "format/testing.hpp"
#include "fs_test_util.hpp"

using basic_project::Bytes;
using namespace loomora::format;

namespace {

struct Counter {
    unsigned writes, flushes, renames;
    unsigned fail_write_at, fail_flush_at, fail_rename_at; // 1-based, 0 = never
    Counter() : writes(0), flushes(0), renames(0), fail_write_at(0), fail_flush_at(0), fail_rename_at(0) {}
};

bool on_write(void *user, std::uint64_t, std::size_t) {
    Counter *c = static_cast<Counter *>(user);
    return ++c->writes == c->fail_write_at;
}
bool on_flush(void *user) {
    Counter *c = static_cast<Counter *>(user);
    return ++c->flushes == c->fail_flush_at;
}
bool on_rename(void *user) {
    Counter *c = static_cast<Counter *>(user);
    return ++c->renames == c->fail_rename_at;
}

void install(Counter &c) {
    hooks().user = &c;
    hooks().fail_write = on_write;
    hooks().fail_flush = on_flush;
    hooks().fail_rename = on_rename;
}

int32_t save(loomora_editor *e, const std::string &path, const char *modified, loomora_progress_fn cb = nullptr,
             void *user = nullptr) {
    loomora_report *report = nullptr;
    const int32_t rc = loomora_editor_save_path(e, path.c_str(), modified, 0, cb, user, &report);
    delete report;
    return rc;
}

struct Progress {
    unsigned calls;
    std::uint64_t first_done, first_total;
    Progress() : calls(0), first_done(0), first_total(0) {}
};

int32_t cancel_first(void *user, std::uint64_t done, std::uint64_t total) {
    Progress *p = static_cast<Progress *>(user);
    if (p->calls++ == 0) {
        p->first_done = done;
        p->first_total = total;
    }
    return 1;
}

int32_t count_only(void *user, std::uint64_t, std::uint64_t) {
    ++static_cast<Progress *>(user)->calls;
    return 0;
}

// The project whose save is attempted: the `basic` one.
loomora_editor *new_basic() {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    return e;
}

// A project with a source picture of about 4.5 MiB, so that the progress callback is called before
// the end of the save.
loomora_editor *new_big() {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e) == 0);
    const Bytes source = basic_project::make_png(2300, 2000, 7);
    const Bytes thumb = basic_project::make_png(4, 4, 10);
    std::uint32_t id = 0;
    REQUIRE(loomora_editor_add_image(e, "{\"name\":\"Big\",\"color_mode\":\"gray\",\"shape\":\"free\",\"width_px\":2300,\"height_px\":2000}",
                                     &source[0], source.size(), nullptr, 0, &thumb[0], thumb.size(), &id) == 0);
    return e;
}

} // namespace

TEST_CASE("a failure at every write, flush and rename step leaves the previous file intact", "[format][atomic]") {
    fstest::ScratchDir dir("atomic");
    const std::string path = dir.file("project.loomora");

    // The previous file: another project.
    loomora_editor *old = nullptr;
    REQUIRE(basic_project::build_escape(&old) == 0);
    REQUIRE(save(old, path, "2026-11-01T00:00:00Z") == 0);
    loomora_editor_close(old);
    Bytes previous;
    REQUIRE(fstest::read_file(path, previous));
    REQUIRE(!previous.empty());

    loomora_editor *e = new_basic();

    // Count the steps of a save (and check it works with the hooks installed).
    Counter count;
    unsigned writes = 0;
    {
        ScopedHooks scope;
        install(count);
        REQUIRE(save(e, dir.file("count.loomora"), basic_project::kModifiedAt) == 0);
        writes = count.writes;
        CHECK(count.flushes == 1);
        CHECK(count.renames == 1);
    }
    fstest::remove_file(dir.file("count.loomora"));
    REQUIRE(writes > 10);

    for (unsigned k = 1; k <= writes; ++k) {
        INFO("write step " << k << " of " << writes);
        ScopedHooks scope;
        Counter c;
        c.fail_write_at = k;
        install(c);
        CHECK(save(e, path, basic_project::kModifiedAt) == LOOMORA_ERR_IO);
        hooks().fail_write = nullptr;
        Bytes now;
        REQUIRE(fstest::read_file(path, now));
        CHECK(now == previous);
        CHECK(dir.names().size() == 1);
    }
    {
        ScopedHooks scope;
        Counter c;
        c.fail_flush_at = 1;
        install(c);
        CHECK(save(e, path, basic_project::kModifiedAt) == LOOMORA_ERR_IO);
        hooks().fail_flush = nullptr;
        Bytes now;
        REQUIRE(fstest::read_file(path, now));
        CHECK(now == previous);
        CHECK(dir.names().size() == 1);
    }
    {
        ScopedHooks scope;
        Counter c;
        c.fail_rename_at = 1;
        install(c);
        CHECK(save(e, path, basic_project::kModifiedAt) == LOOMORA_ERR_IO);
        hooks().fail_rename = nullptr;
        Bytes now;
        REQUIRE(fstest::read_file(path, now));
        CHECK(now == previous);
        CHECK(dir.names().size() == 1);
    }

    // Without a fault the file is replaced by the new project.
    REQUIRE(save(e, path, basic_project::kModifiedAt) == 0);
    Bytes replaced;
    REQUIRE(fstest::read_file(path, replaced));
    CHECK(replaced != previous);
    CHECK(dir.names().size() == 1);
    loomora_editor_close(e);
}

TEST_CASE("a failed first save leaves no file and no temporary file", "[format][atomic]") {
    fstest::ScratchDir dir("atomic-first");
    loomora_editor *e = new_basic();
    ScopedHooks scope;
    Counter c;
    c.fail_rename_at = 1;
    install(c);
    CHECK(save(e, dir.file("new.loomora"), basic_project::kModifiedAt) == LOOMORA_ERR_IO);
    CHECK(dir.names().empty());
    loomora_editor_close(e);
}

TEST_CASE("a cancelled progress callback leaves the previous file intact", "[format][atomic]") {
    fstest::ScratchDir dir("atomic-cancel");
    const std::string path = dir.file("project.loomora");
    loomora_editor *e = new_basic();
    REQUIRE(save(e, path, "2026-11-01T00:00:00Z") == 0);
    Bytes previous;
    REQUIRE(fstest::read_file(path, previous));

    // A small project: the callback is called once, at the end, and still cancels.
    Progress small;
    CHECK(save(e, path, basic_project::kModifiedAt, cancel_first, &small) == LOOMORA_ERR_CANCELLED);
    CHECK(small.calls == 1);
    CHECK(small.first_total > 0);
    CHECK(small.first_done >= small.first_total);
    Bytes now;
    REQUIRE(fstest::read_file(path, now));
    CHECK(now == previous);
    CHECK(dir.names().size() == 1);
    loomora_editor_close(e);

    // A project over 4 MiB: cancelled before the end of the write.
    loomora_editor *big = new_big();
    Progress mid;
    CHECK(save(big, path, basic_project::kModifiedAt, cancel_first, &mid) == LOOMORA_ERR_CANCELLED);
    CHECK(mid.calls == 1);
    CHECK(mid.first_done < mid.first_total);
    CHECK(mid.first_done >= 4u * 1024u * 1024u);
    REQUIRE(fstest::read_file(path, now));
    CHECK(now == previous);
    CHECK(dir.names().size() == 1);

    // Not cancelled: called more than once, and the save goes through.
    Progress all;
    CHECK(save(big, path, basic_project::kModifiedAt, count_only, &all) == 0);
    CHECK(all.calls >= 2);
    REQUIRE(fstest::read_file(path, now));
    CHECK(now != previous);
    loomora_editor_close(big);
}

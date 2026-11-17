// Issue, Report, FormatError and the mapping of exceptions to LOOMORA_ERR_* codes.
#include <catch2/catch.hpp>

#include <new>
#include <stdexcept>
#include <string>

#include "../../include/loomora.h"
#include "format/report.hpp"

using namespace loomora::format;

namespace {

// Maps the exception thrown by `fn` the way the C boundary does.
template <class F> int code_thrown_by(F fn) {
    try {
        fn();
    } catch (...) {
        return code_of_current_exception();
    }
    return 0;
}

void throw_bad_alloc() { throw std::bad_alloc(); }
void throw_runtime() { throw std::runtime_error("boom"); }
void throw_format() { throw FormatError(LOOMORA_ERR_CORRUPT, "bad entry"); }

} // namespace

TEST_CASE("an empty report has no issue and no error", "[format][report]") {
    Report r;
    REQUIRE(r.count() == 0u);
    REQUIRE(r.error_count() == 0u);
    REQUIRE(r.warning_count() == 0u);
    REQUIRE_FALSE(r.has_errors());
}

TEST_CASE("add counts warnings and errors separately and keeps the order", "[format][report]") {
    Report r;
    r.add(kWarning, "warn.nails", "frames/1/layouts/2/nails.json", "many nails", kItemLayout, 2);
    r.add(kError, "entry.sha256", "images/3/target.png", "hash differs", kItemImage, 3);
    Issue extra;
    extra.severity = kError;
    extra.code = "manifest.schema";
    extra.message = "missing title";
    r.add(extra);

    REQUIRE(r.count() == 3u);
    REQUIRE(r.error_count() == 2u);
    REQUIRE(r.warning_count() == 1u);
    REQUIRE(r.has_errors());

    REQUIRE(r.at(0).severity == kWarning);
    REQUIRE(r.at(0).code == "warn.nails");
    REQUIRE(r.at(0).entry == "frames/1/layouts/2/nails.json");
    REQUIRE(r.at(0).item_kind == kItemLayout);
    REQUIRE(r.at(0).item_id == 2u);
    REQUIRE(r.at(0).message == "many nails");
    REQUIRE(r.at(1).code == "entry.sha256");
    REQUIRE(r.at(2).code == "manifest.schema");
    REQUIRE(r.at(2).item_kind == kItemNone);
    REQUIRE(r.at(2).item_id == 0u);
    REQUIRE(r.issues().size() == 3u);
}

TEST_CASE("a default Issue is a warning about no item", "[format][report]") {
    Issue i;
    REQUIRE(i.severity == kWarning);
    REQUIRE(i.item_kind == kItemNone);
    REQUIRE(i.item_id == 0u);
    REQUIRE(i.code.empty());
}

TEST_CASE("the item kinds equal the LOOMORA_KIND_* values", "[format][report]") {
    REQUIRE(static_cast<int>(kItemFrame) == LOOMORA_KIND_FRAME);
    REQUIRE(static_cast<int>(kItemLayout) == LOOMORA_KIND_LAYOUT);
    REQUIRE(static_cast<int>(kItemImage) == LOOMORA_KIND_IMAGE);
    REQUIRE(static_cast<int>(kItemSpool) == LOOMORA_KIND_SPOOL);
    REQUIRE(static_cast<int>(kItemResult) == LOOMORA_KIND_RESULT);
}

TEST_CASE("merge appends the issues of another report in order", "[format][report]") {
    Report a;
    a.add(kError, "a.one", "e1", "m1");
    Report b;
    b.add(kWarning, "b.one", "e2", "m2");
    b.add(kError, "b.two", "e3", "m3");
    a.merge(b);
    REQUIRE(a.count() == 3u);
    REQUIRE(a.error_count() == 2u);
    REQUIRE(a.warning_count() == 1u);
    REQUIRE(a.at(1).code == "b.one");
    REQUIRE(a.at(2).code == "b.two");
    REQUIRE(b.count() == 2u); // the source is unchanged
    a.merge(a);               // merging a report into itself doubles it, without crashing
    REQUIRE(a.count() == 6u);
}

TEST_CASE("clear empties the report", "[format][report]") {
    Report r;
    r.add(kError, "x", "", "y");
    r.clear();
    REQUIRE(r.count() == 0u);
    REQUIRE(r.error_count() == 0u);
}

TEST_CASE("FormatError carries the code, the message and optionally an issue", "[format][report]") {
    const FormatError plain(LOOMORA_ERR_IO, "disk full");
    REQUIRE(plain.code() == LOOMORA_ERR_IO);
    REQUIRE(std::string(plain.what()) == "disk full");
    REQUIRE_FALSE(plain.has_issue());

    Issue i;
    i.severity = kError;
    i.code = "zip.name";
    i.entry = "../evil";
    i.message = "entry name leaves the archive";
    const FormatError with_issue(LOOMORA_ERR_CORRUPT, i);
    REQUIRE(with_issue.code() == LOOMORA_ERR_CORRUPT);
    REQUIRE(with_issue.has_issue());
    REQUIRE(with_issue.issue().code == "zip.name");
    REQUIRE(with_issue.issue().entry == "../evil");
    REQUIRE(std::string(with_issue.what()) == "entry name leaves the archive");
}

TEST_CASE("exceptions map to LOOMORA_ERR_* codes", "[format][report]") {
    REQUIRE(code_thrown_by(throw_bad_alloc) == LOOMORA_ERR_NO_MEMORY);
    REQUIRE(code_thrown_by(throw_format) == LOOMORA_ERR_CORRUPT);
    REQUIRE(code_thrown_by(throw_runtime) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("code_for_exception maps without rethrowing", "[format][report]") {
    REQUIRE(code_for_exception(std::bad_alloc()) == LOOMORA_ERR_NO_MEMORY);
    REQUIRE(code_for_exception(FormatError(LOOMORA_ERR_BOMB, "bomb")) == LOOMORA_ERR_BOMB);
    REQUIRE(code_for_exception(std::runtime_error("x")) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("a FormatError can be turned into an issue of a report", "[format][report]") {
    Report r;
    r.add(FormatError(LOOMORA_ERR_CORRUPT, "bad"), "container.corrupt", "manifest.json");
    REQUIRE(r.count() == 1u);
    REQUIRE(r.error_count() == 1u);
    REQUIRE(r.at(0).code == "container.corrupt");
    REQUIRE(r.at(0).entry == "manifest.json");
    REQUIRE(r.at(0).message == "bad");
}

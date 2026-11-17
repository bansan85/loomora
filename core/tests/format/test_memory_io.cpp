// MemorySource, MemorySink, CallbackSink and the Progress helper (format/io).
#include <catch2/catch.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "format/io/memory_io.hpp"
#include "format/io/progress.hpp"
#include "format/limits.hpp"
#include "format/report.hpp"
#include "format/testing.hpp"

using namespace loomora::format;

namespace {

const std::uint8_t kData[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

template <class F> int code_thrown_by(F fn) {
    try {
        fn();
    } catch (...) {
        return code_of_current_exception();
    }
    return 0;
}

// Collects what a loomora_sink_fn receives.
struct Collector {
    std::vector<std::uint8_t> bytes;
    std::size_t calls;
    int fail_at_call; // 1-based, 0 = never
    Collector() : calls(0), fail_at_call(0) {}
};

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Collector *c = static_cast<Collector *>(user);
    ++c->calls;
    if (c->fail_at_call != 0 && static_cast<int>(c->calls) == c->fail_at_call)
        return 1;
    c->bytes.insert(c->bytes.end(), data, data + len);
    return 0;
}

bool refuse_growth(void *, std::uint64_t) { return false; }

struct GrowLog {
    std::vector<std::uint64_t> sizes;
};
bool log_growth(void *user, std::uint64_t needed) {
    static_cast<GrowLog *>(user)->sizes.push_back(needed);
    return true;
}

} // namespace

TEST_CASE("MemorySource reads a caller buffer without copying it", "[format][io]") {
    MemorySource src(kData, sizeof kData);
    REQUIRE(src.size() == 10u);
    REQUIRE(src.data() == kData);

    std::uint8_t buf[4] = {0};
    REQUIRE(src.read_at(3, buf, 4) == 4u);
    REQUIRE(buf[0] == 3);
    REQUIRE(buf[3] == 6);
}

TEST_CASE("MemorySource reads short at the end and nothing past it", "[format][io]") {
    MemorySource src(kData, sizeof kData);
    std::uint8_t buf[8] = {0};
    REQUIRE(src.read_at(7, buf, 8) == 3u);
    REQUIRE(buf[2] == 9);
    REQUIRE(src.read_at(10, buf, 8) == 0u);
    REQUIRE(src.read_at(1000, buf, 8) == 0u);
    REQUIRE(src.read_at(0, buf, 0) == 0u);
    REQUIRE(src.read_at(0xFFFFFFFFFFFFFFFFull, buf, 8) == 0u);
}

TEST_CASE("an empty MemorySource may have a null buffer", "[format][io]") {
    MemorySource src(nullptr, 0);
    std::uint8_t b = 0;
    REQUIRE(src.size() == 0u);
    REQUIRE(src.read_at(0, &b, 1) == 0u);
}

TEST_CASE("read_exact reports a short read", "[format][io]") {
    MemorySource src(kData, sizeof kData);
    std::uint8_t buf[8] = {0};
    REQUIRE(read_exact(src, 2, buf, 8));
    REQUIRE(buf[0] == 2);
    REQUIRE_FALSE(read_exact(src, 5, buf, 8));
    REQUIRE(read_exact(src, 5, buf, 0));
}

TEST_CASE("MemorySink appends and reports its position", "[format][io]") {
    MemorySink sink;
    REQUIRE(sink.position() == 0u);
    sink.write("abc", 3);
    sink.write("de", 2);
    REQUIRE(sink.position() == 5u);
    REQUIRE(sink.bytes().size() == 5u);
    REQUIRE(std::string(sink.bytes().begin(), sink.bytes().end()) == "abcde");
    sink.commit();
    REQUIRE(sink.committed());
}

TEST_CASE("MemorySink patch_at overwrites bytes already written", "[format][io]") {
    MemorySink sink;
    sink.write("0123456789", 10);
    sink.patch_at(2, "XY", 2);
    sink.patch_at(8, "ZZ", 2);
    REQUIRE(std::string(sink.bytes().begin(), sink.bytes().end()) == "01XY4567ZZ");
    REQUIRE(sink.position() == 10u); // a patch never moves the position
    REQUIRE(code_thrown_by([&] { sink.patch_at(9, "AB", 2); }) == LOOMORA_ERR_INTERNAL);
    REQUIRE(code_thrown_by([&] { sink.patch_at(11, "A", 1); }) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("MemorySink refuses writes after commit or abort", "[format][io]") {
    MemorySink committed;
    committed.write("a", 1);
    committed.commit();
    REQUIRE(code_thrown_by([&] { committed.write("b", 1); }) == LOOMORA_ERR_INTERNAL);

    MemorySink aborted;
    aborted.write("a", 1);
    aborted.abort();
    REQUIRE(aborted.aborted());
    REQUIRE(aborted.bytes().empty());
    REQUIRE(code_thrown_by([&] { aborted.write("b", 1); }) == LOOMORA_ERR_INTERNAL);
    aborted.abort(); // idempotent
    REQUIRE(code_thrown_by([&] { aborted.commit(); }) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("MemorySink calls its growth callback with the new total size", "[format][io]") {
    GrowLog log;
    MemorySink sink(log_growth, &log);
    sink.write("abcd", 4);
    sink.write("ef", 2);
    REQUIRE(!log.sizes.empty());
    REQUIRE(log.sizes.front() == 4u);
    REQUIRE(log.sizes.back() == 6u);
}

TEST_CASE("a refused growth is an out-of-memory error and keeps what was written", "[format][io]") {
    MemorySink sink(refuse_growth, nullptr);
    REQUIRE(code_thrown_by([&] { sink.write("abcd", 4); }) == LOOMORA_ERR_NO_MEMORY);
    REQUIRE(sink.position() == 0u);
}

TEST_CASE("MemorySink honours the fault-injection hooks", "[format][io]") {
    ScopedHooks scoped;
    struct Count {
        int writes;
    } count = {0};
    hooks().user = &count;
    hooks().fail_write = [](void *u, std::uint64_t, std::size_t) {
        return ++static_cast<Count *>(u)->writes == 2;
    };
    MemorySink sink;
    sink.write("a", 1);
    REQUIRE(code_thrown_by([&] { sink.write("b", 1); }) == LOOMORA_ERR_IO);
    REQUIRE(sink.position() == 1u);

    hooks().fail_write = nullptr;
    hooks().fail_alloc = [](void *, std::size_t) { return true; };
    MemorySink other;
    REQUIRE(code_thrown_by([&] { other.write("x", 1); }) == LOOMORA_ERR_NO_MEMORY);
}

TEST_CASE("CallbackSink delivers the bytes in order at commit, patches included", "[format][io]") {
    Collector c;
    CallbackSink sink(collect, &c, 4); // chunks of at most 4 bytes
    sink.write("0123456789", 10);
    sink.patch_at(0, "AB", 2); // a header patched after the fact
    REQUIRE(sink.position() == 10u);
    REQUIRE(c.calls == 0u); // nothing is delivered before commit
    sink.commit();
    REQUIRE(std::string(c.bytes.begin(), c.bytes.end()) == "AB23456789");
    REQUIRE(c.calls == 3u); // 4 + 4 + 2
}

TEST_CASE("CallbackSink abort delivers nothing", "[format][io]") {
    Collector c;
    CallbackSink sink(collect, &c);
    sink.write("abc", 3);
    sink.abort();
    REQUIRE(c.calls == 0u);
    REQUIRE(code_thrown_by([&] { sink.commit(); }) == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("a failing sink callback ends the commit with an I/O error", "[format][io]") {
    Collector c;
    c.fail_at_call = 2;
    CallbackSink sink(collect, &c, 4);
    sink.write("0123456789", 10);
    REQUIRE(code_thrown_by([&] { sink.commit(); }) == LOOMORA_ERR_IO);
    REQUIRE(c.calls == 2u); // stopped at the first refusal
}

TEST_CASE("an empty CallbackSink commits without calling the callback", "[format][io]") {
    Collector c;
    CallbackSink sink(collect, &c);
    sink.commit();
    REQUIRE(c.calls == 0u);
}

TEST_CASE("Progress calls the callback about every interval and at the end", "[format][progress]") {
    ScopedLimits scoped;
    limits().progress_interval_bytes = 100;

    struct Log {
        std::vector<std::uint64_t> done;
        std::uint64_t total;
        int cancel_after;
        Log() : total(0), cancel_after(0) {}
    } log;
    loomora_progress_fn fn = [](void *u, std::uint64_t done, std::uint64_t total) -> int32_t {
        Log *l = static_cast<Log *>(u);
        l->done.push_back(done);
        l->total = total;
        return (l->cancel_after != 0 && static_cast<int>(l->done.size()) >= l->cancel_after) ? 1 : 0;
    };

    Progress p(fn, &log, 1000);
    p.advance(60);
    REQUIRE(log.done.empty()); // under one interval
    p.advance(60);             // 120 processed: the interval is reached
    REQUIRE(log.done.size() == 1u);
    REQUIRE(log.done[0] == 120u);
    REQUIRE(log.total == 1000u);
    p.advance(50);
    REQUIRE(log.done.size() == 1u);
    p.finish(); // always reports the final figure
    REQUIRE(log.done.size() == 2u);
    REQUIRE(log.done.back() == 170u);
}

TEST_CASE("Progress turns a non-zero return into LOOMORA_ERR_CANCELLED", "[format][progress]") {
    ScopedLimits scoped;
    limits().progress_interval_bytes = 10;
    loomora_progress_fn stop = [](void *, std::uint64_t, std::uint64_t) -> int32_t { return 1; };
    Progress p(stop, nullptr, 0);
    REQUIRE(code_thrown_by([&] { p.advance(10); }) == LOOMORA_ERR_CANCELLED);
}

TEST_CASE("Progress without a callback does nothing", "[format][progress]") {
    Progress p(nullptr, nullptr, 0);
    p.advance(1000000000ull);
    p.finish();
    SUCCEED();
}

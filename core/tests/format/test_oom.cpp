// Out of memory while opening a project (T067, SC-008, FR-026): a replacement operator new, armed
// only around the measured call, fails the n-th allocation of an open (and, in a second series, every
// allocation from the n-th on). Every outcome must be LOOMORA_ERR_NO_MEMORY or success: never another
// code, never a crash, never an exception through the C API, and under ASan or LeakSanitizer never a
// leak. The internal fail_alloc hook of testing.hpp (buffer growth) is driven the same way.
//
// The replacement operator new is the one of the whole loomora_tests binary: it forwards to malloc and
// only fails when `g_armed` is set, which nothing but the measured region does, so Catch2 and the
// other tests are not affected.
#include <catch2/catch.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "format/testing.hpp"

namespace oom {

std::atomic<bool> g_armed(false);
std::atomic<long> g_count(0);   // allocations seen while armed
std::atomic<long> g_fail_at(0); // 1-based index of the allocation to fail; 0 = never
std::atomic<bool> g_persistent(false);
std::atomic<long> g_failed(0);  // allocations that were made to fail

inline void *allocate(std::size_t n, bool may_throw) {
    if (g_armed.load(std::memory_order_relaxed)) {
        const long k = g_count.fetch_add(1) + 1;
        const long at = g_fail_at.load();
        if (at != 0 && (k == at || (g_persistent.load() && k > at))) {
            g_failed.fetch_add(1);
            if (may_throw)
                throw std::bad_alloc();
            return nullptr;
        }
    }
    void *p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr && may_throw)
        throw std::bad_alloc();
    return p;
}

} // namespace oom

void *operator new(std::size_t n) { return oom::allocate(n, true); }
void *operator new[](std::size_t n) { return oom::allocate(n, true); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return oom::allocate(n, false); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return oom::allocate(n, false); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

typedef std::vector<std::uint8_t> Bytes;

struct Sink {
    static int32_t put(void *user, const uint8_t *data, size_t len) {
        static_cast<Bytes *>(user)->insert(static_cast<Bytes *>(user)->end(), data, data + len);
        return 0;
    }
};

Bytes save(loomora_editor *e) {
    Bytes b;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, &Sink::put, &b, nullptr) == 0);
    loomora_editor_close(e);
    return b;
}

Bytes reference_project() {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::build_basic(&e) == 0);
    return save(e);
}

// More items and bytes than the reference: 3,000 nails, 20,000 steps, several spools.
Bytes large_project() {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "Large") == 0);
    REQUIRE(basic_project::add_spools(e, 8) == 0);
    const char *frame =
        "{\"name\":\"Big\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":400,\"height_mm\":400},"
        "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":400,"
        "\"height_mm\":400},\"polyline\":[[-200,-200],[200,-200],[200,200],[-200,200]]},"
        "\"exclusions\":[],\"surface\":\"plane\"}";
    uint32_t fid = 0, lid = 0, iid = 0, rid = 0;
    REQUIRE(loomora_editor_add_frame(e, frame, &fid) == 0);
    std::vector<loomora_nail> nails;
    for (unsigned j = 0; j < 50; ++j)
        for (unsigned i = 0; i < 60; ++i) {
            loomora_nail n = loomora_nail();
            n.id = j * 60 + i;
            n.kind = 3;
            n.x = -177 + 6.0 * i;
            n.y = -147 + 6.0 * j;
            n.diameter = 2;
            nails.push_back(n);
        }
    REQUIRE(loomora_editor_add_layout(e, fid, "{\"name\":\"Grid\"}", &nails[0], nails.size(), &lid) == 0);
    REQUIRE(basic_project::add_image(e, &iid) == 0);
    REQUIRE(basic_project::add_result(e, fid, lid, iid, 8, 1, 20000, &rid) == 0);
    return save(e);
}

// One call of the measured region. fail_at 0: no failure, returns the allocations counted.
struct Measured {
    int32_t rc;
    long allocations;
    long failed;
    bool project_ok; // a successful open must serve its items
};

Measured open_failing(const Bytes &data, long fail_at, bool persistent, bool validate_only) {
    Measured m = {0, 0, 0, true};
    loomora_project *p = nullptr;
    loomora_report *r = nullptr;
    oom::g_count = 0;
    oom::g_failed = 0;
    oom::g_fail_at = fail_at;
    oom::g_persistent = persistent;
    oom::g_armed = true;
    if (validate_only)
        m.rc = loomora_validate_memory(data.data(), data.size(), nullptr, nullptr, &r);
    else
        m.rc = loomora_project_open_memory(data.data(), data.size(), nullptr, nullptr, &p, &r);
    oom::g_armed = false;
    m.allocations = oom::g_count;
    m.failed = oom::g_failed;
    if (m.rc == 0 && !validate_only)
        m.project_ok = p != nullptr && loomora_project_count(p, LOOMORA_KIND_RESULT) == 1;
    if (m.rc != 0 && p != nullptr)
        m.project_ok = false; // a failed open must not hand out a project
    loomora_project_close(p);
    loomora_report_free(r);
    return m;
}

void sweep(const Bytes &data, bool persistent, bool validate_only, long max_runs) {
    const Measured base = open_failing(data, 0, false, validate_only);
    REQUIRE(base.rc == 0);
    REQUIRE(base.allocations > 10);
    const long total = base.allocations;
    const long stride = total <= max_runs ? 1 : (total + max_runs - 1) / max_runs;
    long failures_seen = 0, runs = 0;
    for (long n = 1; n <= total + 1; n += (n < 40 || n > total - 40) ? 1 : stride) {
        const Measured m = open_failing(data, n, persistent, validate_only);
        ++runs;
        INFO("allocation " << n << " of " << total << (persistent ? " and the following" : ""));
        const bool ok = m.rc == 0 || m.rc == LOOMORA_ERR_NO_MEMORY;
        if (!ok)
            FAIL("unexpected result " << m.rc);
        REQUIRE(m.project_ok);
        if (m.failed > 0 && m.rc == LOOMORA_ERR_NO_MEMORY)
            ++failures_seen;
        // A failure that the open survived (a retry, a fallback) is fine; one that it swallowed into
        // another code is not (checked above). A failure beyond the last allocation changes nothing.
        if (n > total)
            REQUIRE(m.rc == 0);
    }
    REQUIRE(runs >= 40);
    REQUIRE(failures_seen > runs / 4); // the failures really reach the code under test
}

} // namespace

TEST_CASE("the replacement operator new only fails inside the measured region", "[format][oom]") {
    // Outside the region nothing fails, inside it the n-th allocation does and later ones do not.
    oom::g_fail_at = 0;
    std::vector<int> v(1000);
    REQUIRE(v.size() == 1000);
    oom::g_count = 0;
    oom::g_fail_at = 2;
    oom::g_persistent = false;
    oom::g_armed = true;
    void *a = nullptr, *b = nullptr, *c = nullptr;
    a = ::operator new(8); // the function itself: a new-expression could be elided by the compiler
    bool threw = false;
    try {
        b = ::operator new(8);
    } catch (const std::bad_alloc &) {
        threw = true;
    }
    c = ::operator new(8);
    oom::g_armed = false;
    oom::g_fail_at = 0;
    REQUIRE(threw);
    REQUIRE(b == nullptr);
    ::operator delete(a);
    ::operator delete(c);
}

TEST_CASE("opening the reference project survives the failure of any allocation", "[format][oom]") {
    const Bytes ref = reference_project();
    sweep(ref, false, false, 1500);
    sweep(ref, true, false, 600); // every allocation from the n-th on fails
    sweep(ref, false, true, 600); // the same through loomora_validate_memory
}

TEST_CASE("opening a large project survives the failure of any allocation", "[format][oom]") {
    const Bytes big = large_project();
    sweep(big, false, false, 400);
    sweep(big, true, false, 200);
}

namespace {
long g_hook_calls, g_hook_fail_at;
bool hook_fail(void *, std::size_t) { return ++g_hook_calls == g_hook_fail_at; }
} // namespace

TEST_CASE("the fail_alloc hook of the format code gives LOOMORA_ERR_NO_MEMORY or success", "[format][oom]") {
    const Bytes ref = reference_project();
    loomora::format::ScopedHooks scoped;
    loomora::format::hooks().fail_alloc = &hook_fail;
    g_hook_fail_at = 0;
    g_hook_calls = 0;
    {
        loomora_project *p = nullptr;
        REQUIRE(loomora_project_open_memory(ref.data(), ref.size(), nullptr, nullptr, &p, nullptr) == 0);
        loomora_project_close(p);
    }
    const long total = g_hook_calls;
    REQUIRE(total >= 1);
    for (long n = 1; n <= total + 1; ++n) {
        g_hook_calls = 0;
        g_hook_fail_at = n;
        loomora_project *p = nullptr;
        loomora_report *r = nullptr;
        const int32_t rc = loomora_project_open_memory(ref.data(), ref.size(), nullptr, nullptr, &p, &r);
        INFO("hook call " << n << " of " << total);
        REQUIRE((rc == 0 || rc == LOOMORA_ERR_NO_MEMORY));
        REQUIRE((rc == 0) == (p != nullptr));
        loomora_project_close(p);
        loomora_report_free(r);
    }
}

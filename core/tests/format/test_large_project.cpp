// Costs that grow with the size of a project (FR-025, SC-005, T106): no quadratic behaviour.
//   - a project with 5,000 items of one kind opens (with the warn.count warning) and every item is read;
//   - 500,000 nails of a deflated nails.json read range by range inflate the entry about once in all,
//     not once per range (asserted with the inflate-byte counter of testing.hpp, not with a clock);
//   - the label of one nail reads the blocks of the nail index that can hold it, not the whole table.
#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "format/testing.hpp"

using basic_project::Bytes;
using basic_project::num;

namespace {

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *b = static_cast<Bytes *>(user);
    b->insert(b->end(), data, data + len);
    return 0;
}

const char kFrame[] =
    "{\"name\":\"Square 500\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
    "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
    "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},"
    "\"exclusions\":[],\"surface\":\"plane\"}";

// A grid of `n` interior nails of diameter 0.5 mm (they never touch); the id of nail i is i * stride.
std::vector<loomora_nail> grid(std::uint32_t n, std::uint32_t stride = 1) {
    std::uint32_t side = 1;
    while (static_cast<std::uint64_t>(side) * side < n)
        ++side;
    const double step = 496.0 / (side > 1 ? side - 1 : 1);
    std::vector<loomora_nail> list(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        loomora_nail nail = loomora_nail();
        nail.id = i * stride;
        nail.kind = 3; // interior
        nail.x = basic_project::round3(-248.0 + step * (i % side));
        nail.y = basic_project::round3(-248.0 + step * (i / side));
        nail.diameter = 0.5;
        list[i] = nail;
    }
    return list;
}

Bytes save(loomora_editor *e) {
    Bytes out;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, nullptr) == 0);
    return out;
}

struct Opened {
    Bytes bytes;
    loomora_project *p;
    loomora_report *report;
    int32_t rc;
    explicit Opened(const Bytes &b) : bytes(b), p(nullptr), report(nullptr), rc(0) {
        rc = loomora_project_open_memory(&bytes[0], bytes.size(), nullptr, nullptr, &p, &report);
    }
    ~Opened() {
        loomora_project_close(p);
        loomora_report_free(report);
    }
};

bool has_warning(const loomora_report *r, const char *code) {
    for (int64_t i = 0; i < loomora_report_count(r); ++i) {
        std::uint8_t buf[64];
        const int64_t n = loomora_report_text(r, static_cast<std::size_t>(i), 1, buf, sizeof buf);
        if (n == static_cast<int64_t>(std::strlen(code)) && std::memcmp(buf, code, static_cast<std::size_t>(n)) == 0)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("a project with 5,000 items of one kind opens, warns and reads every item", "[format][large]") {
    const std::uint32_t kLayouts = 5000;
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "Many layouts") == 0);
    std::uint32_t frame = 0;
    REQUIRE(loomora_editor_add_frame(e, kFrame, &frame) == 0);
    const loomora_nail one = grid(1)[0];
    for (std::uint32_t i = 0; i < kLayouts; ++i) {
        std::uint32_t layout = 0;
        const std::string json = "{\"name\":\"Layout " + num(i) + "\"}";
        REQUIRE(loomora_editor_add_layout(e, frame, json.c_str(), &one, 1, &layout) == 0);
    }
    const Bytes bytes = save(e);
    loomora_editor_close(e);

    typedef std::chrono::steady_clock Clock;
    const Clock::time_point t0 = Clock::now();
    Opened o(bytes);
    REQUIRE(o.rc == 0);
    REQUIRE(loomora_project_count(o.p, LOOMORA_KIND_LAYOUT) == kLayouts);
    CHECK(has_warning(o.report, "warn.count")); // 5,000 > the threshold of 1,000 per kind
    std::vector<std::uint32_t> ids(kLayouts);
    REQUIRE(loomora_project_ids(o.p, LOOMORA_KIND_LAYOUT, &ids[0], ids.size()) == kLayouts);
    for (std::uint32_t i = 0; i < kLayouts; ++i) {
        loomora_nail nail = loomora_nail();
        REQUIRE(loomora_layout_nail_count(o.p, ids[i]) == 1);
        REQUIRE(loomora_layout_read_nails(o.p, ids[i], 0, 1, &nail) == 1);
        CHECK(nail.id == 0u);
    }
    // Generous: the linear scans that this guards against cost seconds here, an index costs a fraction.
    const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    INFO("open and read: " << seconds << " s");
    CHECK(seconds < 60.0);
}

namespace {

struct BigNails {
    Bytes bytes;
    std::uint32_t layout;
    std::uint32_t count;
    std::uint32_t stride;
};

// One layout of `count` nails (ids i * stride), labels on the nails listed in `labelled`.
BigNails big_nails(std::uint32_t count, std::uint32_t stride, const std::vector<std::uint32_t> &labelled) {
    BigNails b;
    b.count = count;
    b.stride = stride;
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "Many nails") == 0);
    std::uint32_t frame = 0;
    REQUIRE(loomora_editor_add_frame(e, kFrame, &frame) == 0);
    const std::vector<loomora_nail> nails = grid(count, stride);
    REQUIRE(loomora_editor_add_layout(e, frame, "{\"name\":\"Grid\"}", &nails[0], nails.size(), &b.layout) == 0);
    for (std::size_t i = 0; i < labelled.size(); ++i) {
        const std::string label = "nail " + num(labelled[i]);
        REQUIRE(loomora_editor_layout_set_label(e, b.layout, labelled[i] * stride, label.c_str()) == 0);
    }
    b.bytes = save(e);
    loomora_editor_close(e);
    return b;
}

} // namespace

TEST_CASE("500,000 nails read range by range inflate the deflated nails.json about once", "[format][large]") {
    const std::uint32_t kNails = 500000;
    const std::size_t kRange = 1024;
    const BigNails b = big_nails(kNails, 1, std::vector<std::uint32_t>());
    Opened o(b.bytes);
    REQUIRE(o.rc == 0);
    char entry[96];
    std::snprintf(entry, sizeof entry, "frames/1/layouts/%u/nails.json", static_cast<unsigned>(b.layout));
    const int64_t size = loomora_project_entry_size(o.p, entry);
    REQUIRE(size > static_cast<int64_t>(kNails) * 40); // tens of MB of JSON
    REQUIRE(loomora_layout_nail_count(o.p, b.layout) == kNails);

    loomora::format::ScopedHooks scoped;
    std::uint64_t inflated = 0;
    loomora::format::hooks().inflated_bytes = &inflated;

    std::vector<loomora_nail> got(kRange);
    std::uint64_t next = 0;
    std::uint64_t calls = 0;
    while (next < kNails) {
        const int64_t n = loomora_layout_read_nails(o.p, b.layout, next, kRange, &got[0]);
        REQUIRE(n == static_cast<int64_t>(std::min<std::uint64_t>(kRange, kNails - next)));
        // Spot checks, so that a wrong resume would show: first, last and a middle nail of the range.
        CHECK(got[0].id == next);
        CHECK(got[static_cast<std::size_t>(n) - 1].id == next + static_cast<std::uint64_t>(n) - 1);
        CHECK(got[static_cast<std::size_t>(n) / 2].id == next + static_cast<std::uint64_t>(n) / 2);
        next += static_cast<std::uint64_t>(n);
        ++calls;
    }
    REQUIRE(inflated > 0); // the entry is deflated: the counter saw the reads
    INFO(calls << " calls inflated " << inflated << " bytes of an entry of " << size);
    // One pass: the entry once, plus at most one block. Re-inflating from byte 0 per call would be
    // about calls / 2 times the entry (hundreds of times).
    CHECK(inflated <= static_cast<std::uint64_t>(size) + static_cast<std::uint64_t>(size) / 10);

    // Ranges of other sizes and offsets, in order, still resume (a range across several blocks, one
    // that starts inside a block, one nail at a time).
    inflated = 0;
    const std::uint64_t starts[5] = {100, 1500, 7000, 7001, 250000};
    const std::size_t counts[5] = {3000, 2500, 1, 5000, 1000};
    for (int k = 0; k < 5; ++k) {
        std::vector<loomora_nail> r(counts[k]);
        REQUIRE(loomora_layout_read_nails(o.p, b.layout, starts[k], r.size(), &r[0]) == static_cast<int64_t>(r.size()));
        for (std::size_t i = 0; i < r.size(); ++i)
            REQUIRE(r[i].id == starts[k] + i);
    }
    CHECK(inflated <= static_cast<std::uint64_t>(size) + static_cast<std::uint64_t>(size) / 10);
    // Going back is allowed (it starts the entry again) and still right.
    loomora_nail first = loomora_nail();
    REQUIRE(loomora_layout_read_nails(o.p, b.layout, 0, 1, &first) == 1);
    CHECK(first.id == 0u);
    // The end of the table: fewer nails than asked.
    std::vector<loomora_nail> tail(10);
    REQUIRE(loomora_layout_read_nails(o.p, b.layout, kNails - 3, tail.size(), &tail[0]) == 3);
    CHECK(tail[2].id == kNails - 1);
    CHECK(loomora_layout_read_nails(o.p, b.layout, kNails, tail.size(), &tail[0]) == 0);
}

TEST_CASE("the label of one nail reads only the blocks that can hold it", "[format][large]") {
    // 60,000 nails with ids 0, 3, 6, ... (not contiguous: the id says nothing about the position).
    const std::uint32_t kNails = 60000;
    std::vector<std::uint32_t> labelled;
    labelled.push_back(0);
    labelled.push_back(1500);
    labelled.push_back(30000);
    labelled.push_back(59999);
    const BigNails b = big_nails(kNails, 3, labelled);
    Opened o(b.bytes);
    REQUIRE(o.rc == 0);
    char entry[96];
    std::snprintf(entry, sizeof entry, "frames/1/layouts/%u/nails.json", static_cast<unsigned>(b.layout));
    const int64_t size = loomora_project_entry_size(o.p, entry);
    REQUIRE(size > 0);

    loomora::format::ScopedHooks scoped;
    std::uint64_t inflated = 0;
    loomora::format::hooks().inflated_bytes = &inflated;

    std::uint8_t buf[64];
    for (std::size_t i = 0; i < labelled.size(); ++i) {
        const std::string want = "nail " + num(labelled[i]);
        const int64_t n = loomora_layout_nail_label(o.p, b.layout, labelled[i] * 3, buf, sizeof buf);
        REQUIRE(n == static_cast<int64_t>(want.size()));
        CHECK(std::string(reinterpret_cast<char *>(buf), static_cast<std::size_t>(n)) == want);
    }
    // Four lookups in increasing order: the entry once (the old code inflated all of it per lookup).
    REQUIRE(inflated > 0);
    INFO("inflated " << inflated << " bytes of an entry of " << size);
    CHECK(inflated <= static_cast<std::uint64_t>(size) + static_cast<std::uint64_t>(size) / 10);

    // A nail without a label, an id that is not in the layout, a size query, a layout that is not there.
    CHECK(loomora_layout_nail_label(o.p, b.layout, 3, buf, sizeof buf) == 0);
    CHECK(loomora_layout_nail_label(o.p, b.layout, 4, buf, sizeof buf) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_layout_nail_label(o.p, b.layout, 3u * kNails, buf, sizeof buf) == LOOMORA_ERR_NOT_FOUND);
    CHECK(loomora_layout_nail_label(o.p, b.layout, 1500u * 3, nullptr, 0) == 9);
    CHECK(loomora_layout_nail_label(o.p, b.layout + 100, 0, buf, sizeof buf) == LOOMORA_ERR_NOT_FOUND);
    // A lookup of the first block after the last one starts the entry again and is still right.
    CHECK(loomora_layout_nail_label(o.p, b.layout, 0, buf, sizeof buf) == 6);
}

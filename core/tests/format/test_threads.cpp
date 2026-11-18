// Read-only handles shared by several threads (contracts/c-api-project.md, "Thread safety"; FR-027).
// ONE open project is read by 8 threads at once, each doing different read calls in a fixed number of
// iterations, and every result is compared with the value read single-threaded before the threads
// start. The project holds what the shared state of the reader is made of: a layout of 20,000 nails in
// a DEFLATED nails.json (the one-slot cursor of ProjectReader, used by loomora_layout_read_nails and
// loomora_layout_nail_label), a result of 6,000 steps, a 3D frame with a mesh and a layout on it, an
// image and thumbnails (the ZIP name index, the entry reads). A second test opens the same file from
// several threads at once (independent handles).
//
// Nothing here depends on timing: the number of iterations is fixed, the random choices come from a
// generator seeded by the thread number, and a mismatch is recorded by the thread and reported by the
// main thread (Catch2 assertions are not thread-safe). The value of the test is under ThreadSanitizer
// (cmake -DLOOMORA_SANITIZE=thread), where a data race in a read call is reported; without it the
// comparisons still catch a result that depended on another thread's call.
#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../include/loomora.h"
#include "../basic_project.hpp"
#include "../multi_project.hpp"
#include "fs_test_util.hpp"

using basic_project::Bytes;
using basic_project::num;

namespace {

const unsigned kThreads = 8;
const unsigned kIterations = 150;     // per thread, on each of the two handles
const std::uint32_t kNails = 20000;
const std::uint32_t kStride = 3;      // the id of nail i is i * kStride: the id says nothing about the position
const std::uint32_t kLabelEvery = 97; // the nail at position i has a label when i % kLabelEvery == 0
const std::uint32_t kSteps = 6000;
const unsigned kSpools = 3;
const unsigned kMeshSide = 40;        // the mesh is a grid of kMeshSide x kMeshSide vertices

int32_t collect(void *user, const std::uint8_t *data, std::size_t len) {
    Bytes *b = static_cast<Bytes *>(user);
    b->insert(b->end(), data, data + len);
    return 0;
}

const char kFrame2D[] =
    "{\"name\":\"Square 500\",\"kind\":\"2d\",\"dimensions\":{\"width_mm\":500,\"height_mm\":500},"
    "\"background\":\"#ffffff\",\"outline\":{\"type\":\"rectangle\",\"params\":{\"width_mm\":500,"
    "\"height_mm\":500},\"polyline\":[[-250,-250],[250,-250],[250,250],[-250,250]]},"
    "\"exclusions\":[],\"surface\":\"plane\"}";

// A flat grid of kMeshSide x kMeshSide vertices on the plane z = 50 (two triangles per cell), with
// normals and UVs, inside the 100 mm box of multi_project::frame3d_json.
struct Mesh {
    std::vector<float> xyz, normals, uvs;
    std::vector<std::uint32_t> abc;
    Mesh() {
        for (unsigned y = 0; y < kMeshSide; ++y) {
            for (unsigned x = 0; x < kMeshSide; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(kMeshSide - 1);
                const float v = static_cast<float>(y) / static_cast<float>(kMeshSide - 1);
                xyz.push_back(-50.0f + 100.0f * u);
                xyz.push_back(-50.0f + 100.0f * v);
                xyz.push_back(50.0f);
                normals.push_back(0.0f);
                normals.push_back(0.0f);
                normals.push_back(1.0f);
                uvs.push_back(u);
                uvs.push_back(v);
            }
        }
        for (unsigned y = 0; y + 1 < kMeshSide; ++y) {
            for (unsigned x = 0; x + 1 < kMeshSide; ++x) {
                const std::uint32_t a = y * kMeshSide + x, b = a + 1, c = a + kMeshSide, d = c + 1;
                const std::uint32_t t[6] = {a, b, d, a, d, c};
                abc.insert(abc.end(), t, t + 6);
            }
        }
    }
    loomora_mesh_info info() const {
        loomora_mesh_info m = loomora_mesh_info();
        m.vertices = xyz.size() / 3;
        m.triangles = abc.size() / 3;
        m.flags = LOOMORA_MESH_NORMALS | LOOMORA_MESH_UVS;
        return m;
    }
};

struct Ids {
    std::uint32_t frame2d, frame3d, layout2d, layout3d, image, result;
    Ids() : frame2d(0), frame3d(0), layout2d(0), layout3d(0), image(0), result(0) {}
};

// The project of the test: spools, a 2D frame with a layout of kNails nails (labels on some), a 3D
// frame with a mesh and a layout of 12 nails, an image, a result of kSteps steps.
Bytes build_project(Ids &ids) {
    loomora_editor *e = nullptr;
    REQUIRE(basic_project::create(&e, "Threads") == 0);
    REQUIRE(basic_project::add_spools(e, kSpools) == 0);
    REQUIRE(loomora_editor_add_frame(e, kFrame2D, &ids.frame2d) == 0);

    std::uint32_t side = 1;
    while (static_cast<std::uint64_t>(side) * side < kNails)
        ++side;
    const double step = 496.0 / (side - 1);
    std::vector<loomora_nail> nails(kNails);
    for (std::uint32_t i = 0; i < kNails; ++i) {
        loomora_nail n = loomora_nail();
        n.id = i * kStride;
        n.kind = 3; // interior
        n.x = basic_project::round3(-248.0 + step * (i % side));
        n.y = basic_project::round3(-248.0 + step * (i / side));
        n.diameter = 0.5;
        nails[i] = n;
    }
    REQUIRE(loomora_editor_add_layout(e, ids.frame2d, "{\"name\":\"Grid\"}", &nails[0], nails.size(), &ids.layout2d) == 0);
    for (std::uint32_t i = 0; i < kNails; i += kLabelEvery) {
        const std::string label = "label of nail " + num(i * kStride);
        REQUIRE(loomora_editor_layout_set_label(e, ids.layout2d, i * kStride, label.c_str()) == 0);
    }

    const Mesh mesh;
    REQUIRE(multi_project::add_frame_json(e, multi_project::frame3d_json("Plate 100"), &ids.frame3d) == 0);
    const loomora_mesh_info info = mesh.info();
    REQUIRE(loomora_editor_frame_set_mesh(e, ids.frame3d, &info, &mesh.xyz[0], &mesh.abc[0], &mesh.normals[0],
                                          &mesh.uvs[0]) == 0);
    REQUIRE(multi_project::add_layout(e, ids.frame3d, "Top ring", multi_project::ring_3d(12), &ids.layout3d) == 0);

    REQUIRE(basic_project::add_image(e, &ids.image) == 0);

    std::string spools;
    for (unsigned i = 0; i < kSpools; ++i)
        spools += std::string(i ? "," : "") + "{\"spool\":" + num(1 + i) + ",\"length_mm\":" + num(1000 + 10 * i) + "}";
    const std::string json = "{\"name\":\"Long result\",\"created_at\":\"" + std::string(basic_project::kResultCreatedAt) +
                             "\",\"frame\":" + num(ids.frame2d) + ",\"layout\":" + num(ids.layout2d) +
                             ",\"images\":[{\"image\":" + num(ids.image) + "}],\"spools\":[" + spools + "]}";
    std::vector<loomora_step> steps(kSteps);
    for (std::uint32_t i = 0; i < kSteps; ++i) {
        loomora_step s = loomora_step();
        const bool first = i < kSpools;
        const bool last = i >= kSteps - kSpools;
        s.spool_index = static_cast<std::uint32_t>(first ? i : last ? i - (kSteps - kSpools) : (i * 5u + 1u) % kSpools);
        s.nail = ((i * 7u + s.spool_index * 13u) % kNails) * kStride;
        s.rotation = static_cast<std::uint8_t>(i & 1u);
        s.type = first ? 1 : last ? 2 : 0;
        steps[i] = s;
    }
    const Bytes thumb = basic_project::make_png(4, 4, 200);
    REQUIRE(loomora_editor_add_result(e, json.c_str(), &steps[0], steps.size(), &thumb[0], thumb.size(),
                                      "{\"algorithm\":\"greedy\",\"seed\":1}", &ids.result) == 0);

    Bytes out;
    REQUIRE(loomora_editor_save_stream(e, basic_project::kModifiedAt, 0, collect, &out, nullptr) == 0);
    loomora_editor_close(e);
    return out;
}

// What the single-threaded reads give: the reference of every comparison.
struct Expected {
    Ids ids;
    Bytes manifest;
    std::vector<loomora_nail> nails;          // layout2d, every nail
    std::map<std::uint32_t, std::string> labels; // nail id -> label (only the nails that have one)
    std::vector<loomora_nail> nails3d;
    std::vector<loomora_step> steps;
    std::vector<std::pair<int32_t, std::uint32_t> > items; // every (kind, id)
    std::map<std::pair<int32_t, std::uint32_t>, Bytes> item_json;
    std::vector<std::string> entry_names;
    std::map<std::string, Bytes> entries;     // the whole uncompressed content of each
    loomora_mesh_info mesh_info;
    std::vector<float> xyz, normals, uvs;
    std::vector<std::uint32_t> abc;
    Expected() : mesh_info(loomora_mesh_info()) {}
};

Bytes read_whole_entry(const loomora_project *p, const std::string &name) {
    const int64_t size = loomora_project_entry_size(p, name.c_str());
    REQUIRE(size >= 0);
    Bytes b(static_cast<std::size_t>(size) + 1);
    REQUIRE(loomora_project_read_entry(p, name.c_str(), 0, &b[0], b.size()) == size);
    b.resize(static_cast<std::size_t>(size));
    return b;
}

std::string entry_of(const char *fmt, std::uint32_t a, std::uint32_t b = 0) {
    char buf[128];
    std::snprintf(buf, sizeof buf, fmt, static_cast<unsigned>(a), static_cast<unsigned>(b));
    return buf;
}

Expected expected_of(const loomora_project *p, const Ids &ids) {
    Expected x;
    x.ids = ids;
    const int64_t msize = loomora_project_manifest(p, nullptr, 0);
    REQUIRE(msize > 0);
    x.manifest.resize(static_cast<std::size_t>(msize));
    REQUIRE(loomora_project_manifest(p, &x.manifest[0], x.manifest.size()) == msize);

    REQUIRE(loomora_layout_nail_count(p, ids.layout2d) == kNails);
    x.nails.resize(kNails);
    REQUIRE(loomora_layout_read_nails(p, ids.layout2d, 0, kNails, &x.nails[0]) == kNails);
    for (std::uint32_t i = 0; i < kNails; i += kLabelEvery) {
        x.labels[i * kStride] = "label of nail " + num(i * kStride);
        REQUIRE((x.nails[i].flags & 2u) != 0u);
    }
    x.nails3d.resize(12);
    REQUIRE(loomora_layout_read_nails(p, ids.layout3d, 0, 12, &x.nails3d[0]) == 12);
    REQUIRE(loomora_result_step_count(p, ids.result) == kSteps);
    x.steps.resize(kSteps);
    REQUIRE(loomora_result_read_steps(p, ids.result, 0, kSteps, &x.steps[0]) == kSteps);

    for (int32_t kind = LOOMORA_KIND_FRAME; kind <= LOOMORA_KIND_RESULT; ++kind) {
        const int64_t n = loomora_project_count(p, kind);
        REQUIRE(n >= 0);
        std::vector<std::uint32_t> list(static_cast<std::size_t>(n) + 1);
        REQUIRE(loomora_project_ids(p, kind, &list[0], list.size()) == n);
        for (int64_t i = 0; i < n; ++i) {
            const std::pair<int32_t, std::uint32_t> key(kind, list[static_cast<std::size_t>(i)]);
            const int64_t size = loomora_project_item_json(p, kind, key.second, nullptr, 0);
            REQUIRE(size > 0);
            Bytes j(static_cast<std::size_t>(size));
            REQUIRE(loomora_project_item_json(p, kind, key.second, &j[0], j.size()) == size);
            x.items.push_back(key);
            x.item_json[key] = j;
        }
    }

    x.entry_names.push_back(entry_of("frames/%u/layouts/%u/nails.json", ids.frame2d, ids.layout2d));
    x.entry_names.push_back(entry_of("frames/%u/layouts/%u/layout.json", ids.frame2d, ids.layout2d));
    x.entry_names.push_back(entry_of("frames/%u/mesh.bin", ids.frame3d));
    x.entry_names.push_back(entry_of("images/%u/thumb.png", ids.image));
    x.entry_names.push_back(entry_of("images/%u/source.png", ids.image));
    x.entry_names.push_back(entry_of("results/%u/thumb.png", ids.result));
    x.entry_names.push_back(entry_of("results/%u/settings.json", ids.result));
    x.entry_names.push_back(entry_of("results/%u/sequence.bin", ids.result));
    x.entry_names.push_back("spools.json");
    for (std::size_t i = 0; i < x.entry_names.size(); ++i)
        x.entries[x.entry_names[i]] = read_whole_entry(p, x.entry_names[i]);

    REQUIRE(loomora_frame_mesh_info(p, ids.frame3d, &x.mesh_info) == 0);
    const std::size_t v = static_cast<std::size_t>(x.mesh_info.vertices), t = static_cast<std::size_t>(x.mesh_info.triangles);
    REQUIRE(v == kMeshSide * kMeshSide);
    x.xyz.resize(v * 3);
    x.normals.resize(v * 3);
    x.uvs.resize(v * 2);
    x.abc.resize(t * 3);
    REQUIRE(loomora_frame_read_vertices(p, ids.frame3d, 0, v, &x.xyz[0]) == static_cast<int64_t>(v));
    REQUIRE(loomora_frame_read_normals(p, ids.frame3d, 0, v, &x.normals[0]) == static_cast<int64_t>(v));
    REQUIRE(loomora_frame_read_uvs(p, ids.frame3d, 0, v, &x.uvs[0]) == static_cast<int64_t>(v));
    REQUIRE(loomora_frame_read_triangles(p, ids.frame3d, 0, t, &x.abc[0]) == static_cast<int64_t>(t));
    return x;
}

// The mismatches seen by the threads: the count and the first description.
class Failures {
  public:
    Failures() : count_(0) {}
    void add(const std::string &what) {
        if (count_.fetch_add(1) == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            first_ = what;
        }
    }
    unsigned count() const { return count_.load(); }
    std::string first() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_;
    }

  private:
    std::atomic<unsigned> count_;
    mutable std::mutex mutex_;
    std::string first_;
};

// A start gate: no thread begins before all of them exist, so that they really overlap (no sleep, no
// timing assumption: the last one to arrive opens the gate).
class Gate {
  public:
    explicit Gate(unsigned n) : n_(n), arrived_(0) {}
    void wait() {
        arrived_.fetch_add(1);
        while (arrived_.load() < n_)
            std::this_thread::yield();
    }

  private:
    unsigned n_;
    std::atomic<unsigned> arrived_;
};

#define CHECK_T(failures, cond, what)                                                                \
    do {                                                                                             \
        if (!(cond))                                                                                 \
            (failures).add(std::string(what) + " (line " + num(__LINE__) + ")");                     \
    } while (false)

bool same_bytes(const void *a, const void *b, std::size_t n) { return n == 0 || std::memcmp(a, b, n) == 0; }

std::uint32_t pick(std::mt19937 &rng, std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); }

// One thread: `iterations` read calls of a mix of kinds on the shared handle `p`.
void worker(const loomora_project *p, const Expected &x, unsigned thread, unsigned iterations, Failures &f) {
    std::mt19937 rng(1000u + thread);
    const Ids &ids = x.ids;
    std::uint64_t seq_next = static_cast<std::uint64_t>(thread) * 2048u; // this thread's sequential scan
    for (unsigned it = 0; it < iterations; ++it) {
        switch (pick(rng, 10)) {
        case 0:
        case 1: { // a range of nails at a random offset and size (may start before the previous one)
            const std::uint64_t first = pick(rng, kNails + 50);
            const std::size_t count = 1 + pick(rng, 3000);
            const std::uint64_t want = first >= kNails ? 0 : std::min<std::uint64_t>(count, kNails - first);
            std::vector<loomora_nail> out(count);
            const int64_t n = loomora_layout_read_nails(p, ids.layout2d, first, count, &out[0]);
            CHECK_T(f, n == static_cast<int64_t>(want), "read_nails count");
            if (n == static_cast<int64_t>(want))
                CHECK_T(f, same_bytes(&out[0], &x.nails[static_cast<std::size_t>(first)], static_cast<std::size_t>(want) * sizeof(loomora_nail)),
                        "read_nails content");
            break;
        }
        case 2: { // the next range of this thread's sequential scan (the case the cursor is made for)
            if (seq_next >= kNails)
                seq_next = 0;
            const std::size_t count = 1024;
            const std::uint64_t want = std::min<std::uint64_t>(count, kNails - seq_next);
            std::vector<loomora_nail> out(count);
            const int64_t n = loomora_layout_read_nails(p, ids.layout2d, seq_next, count, &out[0]);
            CHECK_T(f, n == static_cast<int64_t>(want), "sequential read_nails count");
            if (n == static_cast<int64_t>(want))
                CHECK_T(f, same_bytes(&out[0], &x.nails[static_cast<std::size_t>(seq_next)], static_cast<std::size_t>(want) * sizeof(loomora_nail)),
                        "sequential read_nails content");
            seq_next += count;
            break;
        }
        case 3:
        case 4: { // the label of a nail: with a label, without, or not in the layout
            const std::uint32_t position = pick(rng, kNails + 30);
            const bool exists = position < kNails;
            const std::uint32_t id = exists ? x.nails[position].id : position * kStride + 1;
            std::uint8_t buf[128];
            const int64_t n = loomora_layout_nail_label(p, ids.layout2d, id, buf, sizeof buf);
            const std::map<std::uint32_t, std::string>::const_iterator l = x.labels.find(id);
            if (!exists) {
                CHECK_T(f, n == LOOMORA_ERR_NOT_FOUND, "label of an unknown nail");
            } else if (l == x.labels.end()) {
                CHECK_T(f, n == 0, "label of a nail without one");
            } else {
                CHECK_T(f, n == static_cast<int64_t>(l->second.size()) &&
                               std::string(reinterpret_cast<const char *>(buf), static_cast<std::size_t>(n < 0 ? 0 : n)) == l->second,
                        "label content");
            }
            break;
        }
        case 5: { // steps at a random offset and size
            const std::uint64_t first = pick(rng, kSteps + 20);
            const std::size_t count = 1 + pick(rng, 700);
            const std::uint64_t want = first >= kSteps ? 0 : std::min<std::uint64_t>(count, kSteps - first);
            std::vector<loomora_step> out(count);
            const int64_t n = loomora_result_read_steps(p, ids.result, first, count, &out[0]);
            CHECK_T(f, n == static_cast<int64_t>(want), "read_steps count");
            if (n == static_cast<int64_t>(want))
                CHECK_T(f, same_bytes(&out[0], &x.steps[static_cast<std::size_t>(first)], static_cast<std::size_t>(want) * sizeof(loomora_step)),
                        "read_steps content");
            break;
        }
        case 6: { // a range of any entry (nails.json is deflated, the PNGs are stored), and its size
            const std::string &name = x.entry_names[pick(rng, static_cast<std::uint32_t>(x.entry_names.size()))];
            const Bytes &whole = x.entries.find(name)->second;
            CHECK_T(f, loomora_project_entry_size(p, name.c_str()) == static_cast<int64_t>(whole.size()), "entry_size");
            const std::uint64_t offset = pick(rng, static_cast<std::uint32_t>(whole.size()) + 10);
            const std::size_t cap = 1 + pick(rng, 5000);
            const std::uint64_t want = offset >= whole.size() ? 0 : std::min<std::uint64_t>(cap, whole.size() - offset);
            Bytes out(cap);
            const int64_t n = loomora_project_read_entry(p, name.c_str(), offset, &out[0], cap);
            CHECK_T(f, n == static_cast<int64_t>(want), "read_entry count");
            if (n == static_cast<int64_t>(want) && want > 0)
                CHECK_T(f, same_bytes(&out[0], &whole[static_cast<std::size_t>(offset)], static_cast<std::size_t>(want)), "read_entry content");
            break;
        }
        case 7: { // the JSON of an item, the manifest, the counts and the ids
            const std::pair<int32_t, std::uint32_t> &key = x.items[pick(rng, static_cast<std::uint32_t>(x.items.size()))];
            const Bytes &want = x.item_json.find(key)->second;
            Bytes out(want.size());
            CHECK_T(f, loomora_project_item_json(p, key.first, key.second, &out[0], out.size()) == static_cast<int64_t>(want.size()) &&
                           out == want,
                    "item_json");
            Bytes m(x.manifest.size());
            CHECK_T(f, loomora_project_manifest(p, &m[0], m.size()) == static_cast<int64_t>(m.size()) && m == x.manifest, "manifest");
            CHECK_T(f, loomora_project_count(p, key.first) > 0 && loomora_project_is_light(p) == 0, "count and is_light");
            break;
        }
        case 8: { // mesh ranges
            const std::uint64_t v = x.mesh_info.vertices, t = x.mesh_info.triangles;
            const std::uint64_t first = pick(rng, static_cast<std::uint32_t>(v));
            const std::size_t count = 1 + pick(rng, 400);
            const std::uint64_t want = std::min<std::uint64_t>(count, v - first);
            std::vector<float> a(count * 3), b(count * 3), c(count * 2);
            CHECK_T(f, loomora_frame_read_vertices(p, ids.frame3d, first, count, &a[0]) == static_cast<int64_t>(want) &&
                           same_bytes(&a[0], &x.xyz[static_cast<std::size_t>(first) * 3], static_cast<std::size_t>(want) * 12),
                    "vertices");
            CHECK_T(f, loomora_frame_read_normals(p, ids.frame3d, first, count, &b[0]) == static_cast<int64_t>(want) &&
                           same_bytes(&b[0], &x.normals[static_cast<std::size_t>(first) * 3], static_cast<std::size_t>(want) * 12),
                    "normals");
            CHECK_T(f, loomora_frame_read_uvs(p, ids.frame3d, first, count, &c[0]) == static_cast<int64_t>(want) &&
                           same_bytes(&c[0], &x.uvs[static_cast<std::size_t>(first) * 2], static_cast<std::size_t>(want) * 8),
                    "uvs");
            const std::uint64_t tfirst = pick(rng, static_cast<std::uint32_t>(t));
            const std::uint64_t twant = std::min<std::uint64_t>(count, t - tfirst);
            std::vector<std::uint32_t> d(count * 3);
            CHECK_T(f, loomora_frame_read_triangles(p, ids.frame3d, tfirst, count, &d[0]) == static_cast<int64_t>(twant) &&
                           same_bytes(&d[0], &x.abc[static_cast<std::size_t>(tfirst) * 3], static_cast<std::size_t>(twant) * 12),
                    "triangles");
            loomora_mesh_info info = loomora_mesh_info();
            CHECK_T(f, loomora_frame_mesh_info(p, ids.frame3d, &info) == 0 && info.vertices == v && info.triangles == t, "mesh_info");
            break;
        }
        default: { // the 3D layout (a small deflated nails.json: the cursor changes layout under the others)
            std::vector<loomora_nail> out(12);
            const int64_t n = loomora_layout_read_nails(p, ids.layout3d, 0, out.size(), &out[0]);
            CHECK_T(f, n == 12 && same_bytes(&out[0], &x.nails3d[0], 12 * sizeof(loomora_nail)), "3D nails");
            std::uint8_t buf[8];
            CHECK_T(f, loomora_layout_nail_label(p, ids.layout3d, x.nails3d[5].id, buf, sizeof buf) == 0, "3D label");
            break;
        }
        }
    }
}

// `kThreads` threads on one handle.
void run_shared(const loomora_project *p, const Expected &x, Failures &f) {
    Gate gate(kThreads);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t)
        threads.push_back(std::thread([&, t]() {
            gate.wait();
            try {
                worker(p, x, t, kIterations, f);
            } catch (...) {
                f.add("an exception left a worker thread");
            }
        }));
    for (std::size_t t = 0; t < threads.size(); ++t)
        threads[t].join();
}

// The fixture is built once for every test case of this file: an editor save of 20,000 nails.
struct Fixture {
    Ids ids;
    Bytes bytes;
    fstest::ScratchDir dir;
    std::string path;
    Fixture() : dir("threads") {
        bytes = build_project(ids);
        path = dir.file("threads.loomora");
        REQUIRE(fstest::write_file(path, bytes));
    }
};

Fixture &fixture() {
    static Fixture f;
    return f;
}

} // namespace

TEST_CASE("one project handle read by 8 threads gives the single-threaded values", "[format][threads]") {
    Fixture &fx = fixture();

    SECTION("a handle opened from a file") {
        loomora_project *p = nullptr;
        REQUIRE(loomora_project_open_path(fx.path.c_str(), nullptr, nullptr, &p, nullptr) == 0);
        const Expected x = expected_of(p, fx.ids);
        Failures f;
        run_shared(p, x, f);
        INFO("first mismatch: " << f.first());
        CHECK(f.count() == 0u);
        // After the threads, the handle still gives the same values.
        std::vector<loomora_nail> again(kNails);
        CHECK(loomora_layout_read_nails(p, fx.ids.layout2d, 0, kNails, &again[0]) == kNails);
        CHECK(same_bytes(&again[0], &x.nails[0], kNails * sizeof(loomora_nail)));
        loomora_project_close(p);
    }

    SECTION("a handle opened from a buffer") {
        Bytes copy = fx.bytes; // the buffer must outlive the handle
        loomora_project *p = nullptr;
        REQUIRE(loomora_project_open_memory(&copy[0], copy.size(), nullptr, nullptr, &p, nullptr) == 0);
        const Expected x = expected_of(p, fx.ids);
        Failures f;
        run_shared(p, x, f);
        INFO("first mismatch: " << f.first());
        CHECK(f.count() == 0u);
        loomora_project_close(p);
    }
}

TEST_CASE("the same file opened from 8 threads at once gives independent handles", "[format][threads]") {
    Fixture &fx = fixture();
    loomora_project *ref = nullptr;
    REQUIRE(loomora_project_open_path(fx.path.c_str(), nullptr, nullptr, &ref, nullptr) == 0);
    const Expected x = expected_of(ref, fx.ids);
    loomora_project_close(ref);

    Failures f;
    Gate gate(kThreads);
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t)
        threads.push_back(std::thread([&, t]() {
            gate.wait();
            try {
                for (unsigned round = 0; round < 3; ++round) {
                    loomora_project *p = nullptr;
                    loomora_report *report = nullptr;
                    const int32_t rc = loomora_project_open_path(fx.path.c_str(), nullptr, nullptr, &p, &report);
                    CHECK_T(f, rc == 0 && p != nullptr, "open_path from a thread");
                    if (rc == 0 && p != nullptr) {
                        // Each handle has its own cursor: read a few ranges in order, then close it.
                        for (std::uint32_t first = (t * 1024u) % kNails; first < kNails && first < (t * 1024u) % kNails + 4096; first += 1024) {
                            std::vector<loomora_nail> out(1024);
                            const int64_t n = loomora_layout_read_nails(p, fx.ids.layout2d, first, out.size(), &out[0]);
                            const std::uint64_t want = std::min<std::uint64_t>(1024, kNails - first);
                            CHECK_T(f, n == static_cast<int64_t>(want) &&
                                           same_bytes(&out[0], &x.nails[first], static_cast<std::size_t>(want) * sizeof(loomora_nail)),
                                    "nails of an independent handle");
                        }
                        worker(p, x, 100u + t, 15, f);
                    }
                    loomora_project_close(p);
                    loomora_report_free(report);
                }
            } catch (...) {
                f.add("an exception left an opener thread");
            }
        }));
    for (std::size_t t = 0; t < threads.size(); ++t)
        threads[t].join();
    INFO("first mismatch: " << f.first());
    CHECK(f.count() == 0u);
}

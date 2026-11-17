// mesh.bin (contracts/project-format.md 5.2): layout, block CRC table, range reads, the light
// rewrite without UVs, rejections and byte-order-independent vectors.
#include <catch2/catch.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "zlib.h"
#include "../../include/loomora.h"
#include "../../include/loomora_project.h"
#include "format/codec/mesh_codec.hpp"
#include "format/endian.hpp"
#include "format/io/memory_io.hpp"
#include "format/report.hpp"

using namespace loomora::format;
typedef std::vector<std::uint8_t> Bytes;

namespace {

// The tetrahedron of the byte vectors below (positions, indices, normals, uvs).
MeshData tetra(bool normals, bool uvs) {
    MeshData m;
    const float pos[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::uint32_t idx[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    const float nrm[12] = {0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, -1};
    const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    m.positions.assign(pos, pos + 12);
    m.indices.assign(idx, idx + 12);
    if (normals)
        m.normals.assign(nrm, nrm + 12);
    if (uvs)
        m.uvs.assign(uv, uv + 8);
    return m;
}

// Fixed bytes of the tetrahedron (computed independently of the codec, with Python's struct and
// zlib), so that the test does not depend on the host byte order or on the codec's own layout.
const unsigned char kTetraFull[208] = {
    0x4C, 0x4D, 0x4D, 0x53, 0x01, 0x03, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00,
    0x80, 0x3F, 0x1C, 0x6B, 0x67, 0x60, 0xC2, 0x8F, 0x07, 0x18};
const unsigned char kTetraLight[176] = {
    0x4C, 0x4D, 0x4D, 0x53, 0x01, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x74, 0x69, 0x07, 0xD8, 0x59, 0xD4, 0x56, 0x45};

Bytes encode(const MeshData &m) {
    MemorySink sink;
    write_mesh(sink, m);
    sink.commit();
    return sink.take();
}

// Fixes the block table of a (modified) file whose data part is `data_len` bytes.
void reseal(Bytes &b, std::size_t data_len) {
    b.resize(data_len);
    std::vector<std::uint32_t> crcs;
    for (std::size_t off = 0; off < data_len; off += kMeshBlockSize) {
        const std::size_t n = data_len - off < kMeshBlockSize ? data_len - off : kMeshBlockSize;
        crcs.push_back(static_cast<std::uint32_t>(crc32(0, &b[off], static_cast<unsigned>(n))));
    }
    Bytes table(crcs.size() * 4);
    for (std::size_t i = 0; i < crcs.size(); ++i)
        write_u32(&table[i * 4], crcs[i]);
    const std::uint32_t tcrc =
        static_cast<std::uint32_t>(crc32(0, &table[0], static_cast<unsigned>(table.size())));
    b.insert(b.end(), table.begin(), table.end());
    b.resize(b.size() + 4);
    write_u32(&b[b.size() - 4], tcrc);
}

template <class F> std::string issue_code(F fn, int *err = nullptr) {
    try {
        fn();
    } catch (const FormatError &e) {
        if (err != nullptr)
            *err = e.code();
        return e.has_issue() ? e.issue().code : std::string();
    }
    return "no throw";
}

std::string open_code(const Bytes &b) {
    return issue_code([&b] {
        MemorySource src(b.empty() ? nullptr : &b[0], b.size());
        MeshReader r(src);
    });
}
std::string verify_code(const Bytes &b) {
    return issue_code([&b] {
        MemorySource src(&b[0], b.size());
        MeshReader r(src);
        r.verify();
    });
}

// A mesh big enough for several blocks: a grid of n x n vertices, two triangles per cell.
MeshData grid(unsigned n, bool normals, bool uvs) {
    MeshData m;
    for (unsigned y = 0; y < n; ++y)
        for (unsigned x = 0; x < n; ++x) {
            m.positions.push_back(static_cast<float>(x) * 0.5f);
            m.positions.push_back(static_cast<float>(y) * 0.25f);
            m.positions.push_back(static_cast<float>((x * 7 + y * 3) % 11) - 5.0f);
            if (normals) {
                m.normals.push_back(0.0f);
                m.normals.push_back(0.6f);
                m.normals.push_back(0.8f);
            }
            if (uvs) {
                m.uvs.push_back(static_cast<float>(x) / static_cast<float>(n));
                m.uvs.push_back(static_cast<float>(y) / static_cast<float>(n));
            }
        }
    for (unsigned y = 0; y + 1 < n; ++y)
        for (unsigned x = 0; x + 1 < n; ++x) {
            const std::uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            const std::uint32_t t[6] = {a, b, d, a, d, c};
            m.indices.insert(m.indices.end(), t, t + 6);
        }
    return m;
}

} // namespace

TEST_CASE("mesh.bin is byte exact (fixed vectors, host byte order independent)", "[mesh]") {
    const Bytes full(kTetraFull, kTetraFull + sizeof kTetraFull);
    const Bytes light(kTetraLight, kTetraLight + sizeof kTetraLight);
    CHECK(encode(tetra(true, true)) == full);
    CHECK(encode(tetra(true, false)) == light);
    CHECK(mesh_entry_size(4, 4, true, true) == full.size());
    CHECK(mesh_entry_size(4, 4, true, false) == light.size());
    CHECK(mesh_entry_size(4, 4, false, false) == light.size() - 48);

    // and the fixed bytes decode to the values
    MemorySource src(&full[0], full.size());
    MeshData back;
    read_mesh(src, &back);
    const MeshData want = tetra(true, true);
    CHECK(back.positions == want.positions);
    CHECK(back.indices == want.indices);
    CHECK(back.normals == want.normals);
    CHECK(back.uvs == want.uvs);
}

TEST_CASE("header fields and flags", "[mesh]") {
    const Bytes none = encode(tetra(false, false));
    CHECK(none[5] == 0);
    CHECK(read_u64(&none[8]) == 4);
    CHECK(read_u64(&none[16]) == 4);
    const Bytes n = encode(tetra(true, false));
    CHECK(n[5] == 1);
    const Bytes nu = encode(tetra(true, true));
    CHECK(nu[5] == 3);
    MemorySource src(&nu[0], nu.size());
    MeshReader r(src);
    CHECK(r.vertex_count() == 4);
    CHECK(r.triangle_count() == 4);
    CHECK(r.has_normals());
    CHECK(r.has_uvs());
    CHECK(r.block_count() == 1);
}

TEST_CASE("an empty mesh round trips", "[mesh]") {
    MeshData m;
    const Bytes b = encode(m);
    CHECK(b.size() == 24 + 4 + 4);
    MemorySource src(&b[0], b.size());
    MeshReader r(src);
    CHECK(r.vertex_count() == 0);
    r.verify();
}

TEST_CASE("multi-block mesh: table, ranges across blocks, full verification", "[mesh]") {
    const MeshData m = grid(300, true, true); // 90000 vertices, ~180000 triangles: about 4.7 MB
    const Bytes b = encode(m);
    REQUIRE(b.size() > 3 * kMeshBlockSize);
    MemorySource src(&b[0], b.size());
    MeshReader r(src);
    CHECK(r.vertex_count() == m.vertex_count());
    const std::uint64_t vc = m.vertex_count(), tc = m.triangle_count();
    const std::uint64_t data_len = 24 + 12 * vc + 12 * tc + 12 * vc + 8 * vc;
    CHECK(r.block_count() == (data_len + kMeshBlockSize - 1) / kMeshBlockSize);
    CHECK(b.size() == data_len + 4 * r.block_count() + 4);
    CHECK(r.block_count() >= 4);
    r.verify();

    // a range spanning the first block boundary (positions go to byte 24 + 12 * 90000 > 1 MiB)
    const std::uint64_t first = kMeshBlockSize / 12 - 100;
    std::vector<float> pos(3 * 300);
    r.read_positions(first, 300, &pos[0]);
    for (std::size_t i = 0; i < pos.size(); ++i)
        CHECK(pos[i] == m.positions[3 * first + i]);

    std::vector<std::uint32_t> idx(3 * 1000);
    r.read_indices(77777, 1000, &idx[0]);
    for (std::size_t i = 0; i < idx.size(); ++i)
        CHECK(idx[i] == m.indices[3 * 77777 + i]);

    std::vector<float> nrm(3 * 50), uv(2 * 50);
    r.read_normals(89950, 50, &nrm[0]);
    r.read_uvs(89950, 50, &uv[0]);
    CHECK(nrm[0] == 0.0f);
    CHECK(nrm[1] == 0.6f);
    CHECK(uv[2 * 49] == m.uvs[2 * (89950 + 49)]);
    CHECK(uv[2 * 49 + 1] == m.uvs[2 * (89950 + 49) + 1]);

    // out of range is a caller bug
    int err = 0;
    CHECK(issue_code([&] { r.read_positions(89999, 2, &pos[0]); }, &err) == "");
    CHECK(err == LOOMORA_ERR_INTERNAL);
}

TEST_CASE("range reads verify the CRC of the block they read, and only that block", "[mesh]") {
    const MeshData m = grid(300, true, false);
    Bytes b = encode(m);
    // damage a byte of the third block (inside the indices)
    const std::size_t victim = 2 * kMeshBlockSize + 1000;
    REQUIRE(victim < b.size() - 8);
    b[victim] ^= 0x20;
    MemorySource src(&b[0], b.size());
    MeshReader r(src); // the table itself is intact
    std::vector<float> pos(3 * 10);
    r.read_positions(0, 10, &pos[0]); // block 0: fine
    CHECK(pos[3] == 0.5f);
    // positions end at 24 + 12 * 90000 = 1080024 (block 1); indices start there; the victim is in
    // block 2, which the indices of the middle of the mesh live in
    std::vector<std::uint32_t> idx(3 * 10);
    int err = 0;
    const std::uint64_t tri = (victim - (24 + 12 * 90000)) / 12;
    CHECK(issue_code([&] { r.read_indices(tri, 10, &idx[0]); }, &err) == "mesh.crc");
    CHECK(err == LOOMORA_ERR_CORRUPT);
    CHECK(issue_code([&] { r.read_indices(0, 10, &idx[0]); }) == "no throw");
    CHECK(verify_code(b) == "mesh.crc");
}

TEST_CASE("light rewrite: same positions, indices and normals, no UVs, table recomputed", "[mesh]") {
    SECTION("tetrahedron: equal to a mesh written without UVs") {
        const Bytes full = encode(tetra(true, true));
        MemorySource src(&full[0], full.size());
        MemorySink out;
        rewrite_mesh_without_uvs(src, out);
        out.commit();
        const Bytes light = out.take();
        CHECK(light == Bytes(kTetraLight, kTetraLight + sizeof kTetraLight));
        CHECK(light.size() == full.size() - 32);
        CHECK((light[5] & kMeshFlagUvs) == 0);
        CHECK((light[5] & kMeshFlagNormals) != 0);
    }
    SECTION("multi-block mesh") {
        const MeshData m = grid(300, true, true);
        const Bytes full = encode(m);
        MemorySource src(&full[0], full.size());
        MemorySink out;
        rewrite_mesh_without_uvs(src, out);
        out.commit();
        const Bytes light = out.take();
        MeshData noUv = m;
        noUv.uvs.clear();
        CHECK(light == encode(noUv));
        CHECK(light != full);
        MemorySource lsrc(&light[0], light.size());
        MeshData back;
        read_mesh(lsrc, &back);
        CHECK(back.positions == m.positions);
        CHECK(back.indices == m.indices);
        CHECK(back.normals == m.normals);
        CHECK(back.uvs.empty());
        MeshReader lr(lsrc);
        CHECK(!lr.has_uvs());
    }
    SECTION("a mesh without UVs is rewritten to the same bytes") {
        const Bytes light = encode(tetra(true, false));
        MemorySource src(&light[0], light.size());
        MemorySink out;
        rewrite_mesh_without_uvs(src, out);
        out.commit();
        CHECK(out.take() == light);
    }
    SECTION("a damaged source is not rewritten") {
        Bytes full = encode(tetra(true, true));
        full[60] ^= 1;
        MemorySource src(&full[0], full.size());
        MemorySink out;
        CHECK(issue_code([&] { rewrite_mesh_without_uvs(src, out); }) == "mesh.crc");
    }
}

TEST_CASE("counts and flags against frame.json", "[mesh]") {
    const Bytes b = encode(tetra(true, false));
    MemorySource src(&b[0], b.size());
    MeshReader r(src);
    r.check_counts(4, 4, true, false);
    int err = 0;
    CHECK(issue_code([&] { r.check_counts(5, 4, true, false); }, &err) == "mesh.count");
    CHECK(err == LOOMORA_ERR_CORRUPT);
    CHECK(issue_code([&] { r.check_counts(4, 3, true, false); }) == "mesh.count");
    CHECK(issue_code([&] { r.check_counts(4, 4, false, false); }) == "mesh.count");
    CHECK(issue_code([&] { r.check_counts(4, 4, true, true); }) == "mesh.count");
}

TEST_CASE("header rejections", "[mesh]") {
    const Bytes ok = encode(tetra(true, true));
    Bytes b = ok;
    b[0] = 'X';
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[4] = 2;
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[5] = 0x04; // reserved flag
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[6] = 1; // reserved u16
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[8] = 5; // vertex_count no longer matches the size
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[15] = 0x7F; // absurd vertex count
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b[23] = 0x7F; // absurd triangle count
    CHECK(open_code(b) == "mesh.header");
    b = ok;
    b.push_back(0);
    CHECK(open_code(b) == "mesh.header");
    CHECK(open_code(Bytes()) == "mesh.header");
    CHECK(open_code(Bytes(ok.begin(), ok.begin() + 20)) == "mesh.header");
    int err = 0;
    b = ok;
    b[0] = 'X';
    CHECK(issue_code([&b] {
              MemorySource src(&b[0], b.size());
              MeshReader r(src);
          },
          &err) == "mesh.header");
    CHECK(err == LOOMORA_ERR_CORRUPT);
}

TEST_CASE("table CRC and block CRC rejections", "[mesh]") {
    const Bytes ok = encode(tetra(true, true));
    Bytes b = ok;
    b[b.size() - 1] ^= 0x01; // table CRC
    CHECK(open_code(b) == "mesh.crc");
    b = ok;
    b[b.size() - 6] ^= 0x01; // a block CRC: the table CRC no longer matches
    CHECK(open_code(b) == "mesh.crc");
    b = ok;
    b[100] ^= 0x01; // data
    CHECK(open_code(b) == "no throw");
    CHECK(verify_code(b) == "mesh.crc");
}

TEST_CASE("every truncation of a mesh is rejected", "[mesh]") {
    const Bytes ok = encode(tetra(true, true));
    for (std::size_t n = 0; n < ok.size(); ++n) {
        Bytes cut(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(n));
        const std::string code = open_code(cut);
        CHECK((code == "mesh.header" || code == "mesh.crc"));
    }
}

TEST_CASE("index range and value checks on read (valid CRC)", "[mesh]") {
    const MeshData m = tetra(true, true);
    const std::size_t data_len = 24 + 48 + 48 + 48 + 32;
    const std::size_t idx_off = 24 + 48;
    const std::size_t nrm_off = idx_off + 48;
    const std::size_t uv_off = nrm_off + 48;
    const Bytes ok = encode(m);
    int err = 0;

    SECTION("index not below vertex_count") {
        Bytes b = ok;
        write_u32(&b[idx_off + 8], 4);
        reseal(b, data_len);
        CHECK(verify_code(b) == "mesh.index_range");
        MemorySource src(&b[0], b.size());
        MeshReader r(src);
        std::uint32_t out[12];
        CHECK(issue_code([&] { r.read_indices(0, 4, out); }, &err) == "mesh.index_range");
        CHECK(err == LOOMORA_ERR_CORRUPT);
    }
    SECTION("position not finite") {
        Bytes b = ok;
        write_f32(&b[24 + 4], std::numeric_limits<float>::infinity());
        reseal(b, data_len);
        CHECK(verify_code(b) == "num.not_finite");
    }
    SECTION("texture coordinate NaN") {
        Bytes b = ok;
        write_f32(&b[uv_off], std::numeric_limits<float>::quiet_NaN());
        reseal(b, data_len);
        CHECK(verify_code(b) == "num.not_finite");
    }
    SECTION("normal not unit") {
        Bytes b = ok;
        write_f32(&b[nrm_off + 8], 1.01f);
        reseal(b, data_len);
        CHECK(verify_code(b) == "mesh.normal");
        write_f32(&b[nrm_off + 8], 1.0005f); // within 1e-3
        reseal(b, data_len);
        CHECK(verify_code(b) == "no throw");
    }
    SECTION("normal not finite") {
        Bytes b = ok;
        write_f32(&b[nrm_off], std::numeric_limits<float>::infinity());
        reseal(b, data_len);
        CHECK(verify_code(b) == "num.not_finite");
    }
}

TEST_CASE("the writer refuses invalid data and misuse", "[mesh]") {
    int err = 0;
    MeshData bad = tetra(true, true);

    bad.indices[3] = 4;
    CHECK(issue_code([&] { encode(bad); }, &err) == "mesh.index_range");
    CHECK(err == LOOMORA_ERR_INVALID_ITEM);

    bad = tetra(true, true);
    bad.positions[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(issue_code([&] { encode(bad); }, &err) == "num.not_finite");
    CHECK(err == LOOMORA_ERR_INVALID_ITEM);

    bad = tetra(true, true);
    bad.uvs[1] = std::numeric_limits<float>::infinity();
    CHECK(issue_code([&] { encode(bad); }) == "num.not_finite");

    bad = tetra(true, true);
    bad.normals[2] = 2.0f;
    CHECK(issue_code([&] { encode(bad); }) == "mesh.normal");

    bad = tetra(true, true);
    bad.positions.pop_back(); // not a multiple of 3
    CHECK(issue_code([&] { encode(bad); }, &err) == "");
    CHECK(err == LOOMORA_ERR_INVALID_ITEM);

    bad = tetra(true, true);
    bad.uvs.pop_back();
    CHECK(issue_code([&] { encode(bad); }, &err) == "");
    CHECK(err == LOOMORA_ERR_INVALID_ITEM);

    // streaming misuse
    MemorySink sink;
    MeshWriter w(sink, 4, 4, false, false);
    const MeshData t = tetra(false, false);
    CHECK(issue_code([&] { w.write_indices(&t.indices[0], 4); }, &err) == ""); // positions first
    CHECK(err == LOOMORA_ERR_INTERNAL);
    w.write_positions(&t.positions[0], 4);
    CHECK(issue_code([&] { w.write_positions(&t.positions[0], 1); }, &err) == ""); // too many
    CHECK(err == LOOMORA_ERR_INTERNAL);
    CHECK(issue_code([&] { w.finish(); }, &err) == ""); // indices missing
    CHECK(err == LOOMORA_ERR_INTERNAL);
    w.write_indices(&t.indices[0], 2);
    w.write_indices(&t.indices[6], 2); // sections can be written in pieces
    w.finish();
    sink.commit();
    CHECK(sink.bytes() == encode(t));
}

TEST_CASE("the writer streams in pieces to the same bytes", "[mesh]") {
    const MeshData m = grid(300, true, true);
    MemorySink sink;
    MeshWriter w(sink, m.vertex_count(), m.triangle_count(), true, true);
    const std::size_t nv = static_cast<std::size_t>(m.vertex_count());
    const std::size_t nt = static_cast<std::size_t>(m.triangle_count());
    for (std::size_t i = 0; i < nv; i += 4099)
        w.write_positions(&m.positions[3 * i], i + 4099 <= nv ? 4099 : nv - i);
    for (std::size_t i = 0; i < nt; i += 10007)
        w.write_indices(&m.indices[3 * i], i + 10007 <= nt ? 10007 : nt - i);
    for (std::size_t i = 0; i < nv; i += 50000)
        w.write_normals(&m.normals[3 * i], i + 50000 <= nv ? 50000 : nv - i);
    w.write_uvs(&m.uvs[0], nv);
    w.finish();
    sink.commit();
    CHECK(sink.bytes() == encode(m));
}

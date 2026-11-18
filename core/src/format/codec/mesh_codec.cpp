// mesh.bin codec (mesh_codec.hpp).
#include "format/codec/mesh_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "zlib.h"
#include "format/codec/codec_error.hpp"
#include "format/endian.hpp"

namespace loomora {
namespace format {

namespace {

const unsigned char kMagic[4] = {'L', 'M', 'M', 'S'};
const std::size_t kChunkItems = 4096;               // vertices or triangles per working chunk
const std::uint64_t kMaxVertices = 0xFFFFFFFFull + 1; // indices are u32
const std::uint64_t kMaxTriangles = 1ull << 40;
const double kNormalTolerance = 1e-3;

unsigned long crc_update(unsigned long crc, const unsigned char *p, std::size_t len) {
    while (len > 0) {
        const std::size_t n = len > 0x40000000u ? 0x40000000u : len;
        crc = crc32(crc, p, static_cast<uInt>(n));
        p += n;
        len -= n;
    }
    return crc;
}

unsigned long crc_init() { return crc32(0L, Z_NULL, 0); }

bool finite_value(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}

bool unit_length(float x, float y, float z) {
    const double dx = x, dy = y, dz = z;
    return std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - 1.0) <= kNormalTolerance;
}

// Section sizes in bytes for these counts.
struct Layout {
    std::uint64_t positions, indices, normals, uvs, data;
};

bool compute_layout(std::uint64_t vertices, std::uint64_t triangles, bool normals, bool uvs, Layout *l) {
    if (vertices > kMaxVertices || triangles > kMaxTriangles)
        return false;
    l->positions = vertices * 12;
    l->indices = triangles * 12;
    l->normals = normals ? vertices * 12 : 0;
    l->uvs = uvs ? vertices * 8 : 0;
    l->data = kMeshHeaderSize + l->positions + l->indices + l->normals + l->uvs;
    return true;
}

std::uint64_t blocks_for(std::uint64_t data) { return (data + kMeshBlockSize - 1) / kMeshBlockSize; }

} // namespace

std::uint64_t mesh_entry_size(std::uint64_t vertices, std::uint64_t triangles, bool normals, bool uvs) {
    Layout l;
    if (!compute_layout(vertices, triangles, normals, uvs, &l))
        return 0;
    return l.data + 4 * blocks_for(l.data) + 4;
}

// ---- MeshWriter ----------------------------------------------------------------------------------

MeshWriter::MeshWriter(Sink &sink, std::uint64_t vertex_count, std::uint64_t triangle_count,
                       bool normals, bool uvs)
    : sink_(sink), vertices_(vertex_count), triangles_(triangle_count), normals_(normals), uvs_(uvs),
      section_(kPositions), left_(vertex_count), block_crc_(crc_init()), in_block_(0), finished_(false) {
    Layout l;
    if (!compute_layout(vertex_count, triangle_count, normals, uvs, &l))
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "mesh has too many vertices or triangles");
    unsigned char head[kMeshHeaderSize];
    std::memcpy(head, kMagic, 4);
    head[4] = 1;
    head[5] = static_cast<unsigned char>((normals ? kMeshFlagNormals : 0) | (uvs ? kMeshFlagUvs : 0));
    write_u16(head + 6, 0);
    write_u64(head + 8, vertex_count);
    write_u64(head + 16, triangle_count);
    emit(head, sizeof head);
}

void MeshWriter::emit(const unsigned char *data, std::size_t len) {
    while (len > 0) {
        const std::size_t room = kMeshBlockSize - in_block_;
        const std::size_t take = len < room ? len : room;
        block_crc_ = crc_update(block_crc_, data, take);
        sink_.write(data, take);
        in_block_ += take;
        data += take;
        len -= take;
        if (in_block_ == kMeshBlockSize) {
            table_.push_back(static_cast<std::uint32_t>(block_crc_ & 0xFFFFFFFFul));
            block_crc_ = crc_init();
            in_block_ = 0;
        }
    }
}

void MeshWriter::advance_section() {
    // positions -> indices -> normals (if any) -> uvs (if any) -> done
    if (section_ == kPositions) {
        section_ = kIndices;
        left_ = triangles_;
    } else if (section_ == kIndices && normals_) {
        section_ = kNormals;
        left_ = vertices_;
    } else if ((section_ == kIndices || section_ == kNormals) && uvs_) {
        section_ = kUvs;
        left_ = vertices_;
    } else {
        section_ = kDone;
        left_ = 0;
    }
}

void MeshWriter::begin(Section section, std::uint64_t items) {
    if (finished_)
        codec::fail_internal("mesh writer used after finish()");
    while (section_ != section) {
        if (section_ == kDone)
            codec::fail_internal("mesh section is not part of this mesh");
        if (left_ != 0)
            codec::fail_internal("mesh sections written out of order or before the previous one is complete");
        advance_section();
    }
    if (items > left_)
        codec::fail_internal("more values than the mesh counts declare");
    left_ -= items;
}

void MeshWriter::write_positions(const float *xyz, std::size_t vertices) {
    begin(kPositions, vertices);
    unsigned char buf[kChunkItems * 12];
    for (std::size_t i = 0; i < vertices; i += kChunkItems) {
        const std::size_t n = std::min(kChunkItems, vertices - i);
        for (std::size_t k = 0; k < n * 3; ++k) {
            if (!finite_value(xyz[i * 3 + k]))
                codec::fail_invalid("num.not_finite", "a vertex position is NaN or Infinity");
            write_f32(buf + 4 * k, xyz[i * 3 + k]);
        }
        emit(buf, n * 12);
    }
}

void MeshWriter::write_indices(const std::uint32_t *abc, std::size_t triangles) {
    begin(kIndices, triangles);
    unsigned char buf[kChunkItems * 12];
    for (std::size_t i = 0; i < triangles; i += kChunkItems) {
        const std::size_t n = std::min(kChunkItems, triangles - i);
        for (std::size_t k = 0; k < n * 3; ++k) {
            if (abc[i * 3 + k] >= vertices_)
                codec::fail_invalid("mesh.index_range", "a triangle index is not below the vertex count");
            write_u32(buf + 4 * k, abc[i * 3 + k]);
        }
        emit(buf, n * 12);
    }
}

void MeshWriter::write_normals(const float *xyz, std::size_t vertices) {
    begin(kNormals, vertices);
    unsigned char buf[kChunkItems * 12];
    for (std::size_t i = 0; i < vertices; i += kChunkItems) {
        const std::size_t n = std::min(kChunkItems, vertices - i);
        for (std::size_t v = 0; v < n; ++v) {
            const float *p = xyz + (i + v) * 3;
            if (!finite_value(p[0]) || !finite_value(p[1]) || !finite_value(p[2]))
                codec::fail_invalid("num.not_finite", "a vertex normal is NaN or Infinity");
            if (!unit_length(p[0], p[1], p[2]))
                codec::fail_invalid("mesh.normal", "a vertex normal is not of unit length (within 1e-3)");
            write_f32(buf + 12 * v, p[0]);
            write_f32(buf + 12 * v + 4, p[1]);
            write_f32(buf + 12 * v + 8, p[2]);
        }
        emit(buf, n * 12);
    }
}

void MeshWriter::write_uvs(const float *uv, std::size_t vertices) {
    begin(kUvs, vertices);
    unsigned char buf[kChunkItems * 8];
    for (std::size_t i = 0; i < vertices; i += kChunkItems) {
        const std::size_t n = std::min(kChunkItems, vertices - i);
        for (std::size_t k = 0; k < n * 2; ++k) {
            if (!finite_value(uv[i * 2 + k]))
                codec::fail_invalid("num.not_finite", "a texture coordinate is NaN or Infinity");
            write_f32(buf + 4 * k, uv[i * 2 + k]);
        }
        emit(buf, n * 8);
    }
}

void MeshWriter::finish() {
    if (finished_)
        codec::fail_internal("mesh writer finished twice");
    while (section_ != kDone) {
        if (left_ != 0)
            codec::fail_internal("mesh writer finished before every section is complete");
        advance_section();
    }
    if (in_block_ > 0) {
        table_.push_back(static_cast<std::uint32_t>(block_crc_ & 0xFFFFFFFFul));
        in_block_ = 0;
    }
    std::vector<unsigned char> table(table_.size() * 4 + 4);
    for (std::size_t i = 0; i < table_.size(); ++i)
        write_u32(&table[i * 4], table_[i]);
    const unsigned long tcrc = crc_update(crc_init(), &table[0], table_.size() * 4);
    write_u32(&table[table_.size() * 4], static_cast<std::uint32_t>(tcrc & 0xFFFFFFFFul));
    sink_.write(&table[0], table.size());
    finished_ = true;
}

void write_mesh(Sink &sink, const MeshData &mesh) {
    if (mesh.positions.size() % 3 != 0 || mesh.indices.size() % 3 != 0)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "positions and indices must come in groups of 3");
    const std::uint64_t vertices = mesh.vertex_count();
    if (!mesh.normals.empty() && mesh.normals.size() != vertices * 3)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "normals do not match the vertex count");
    if (!mesh.uvs.empty() && mesh.uvs.size() != vertices * 2)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "texture coordinates do not match the vertex count");
    const bool normals = !mesh.normals.empty(), uvs = !mesh.uvs.empty();
    MeshWriter w(sink, vertices, mesh.triangle_count(), normals, uvs);
    if (vertices > 0)
        w.write_positions(&mesh.positions[0], static_cast<std::size_t>(vertices));
    if (mesh.triangle_count() > 0)
        w.write_indices(&mesh.indices[0], static_cast<std::size_t>(mesh.triangle_count()));
    if (normals)
        w.write_normals(&mesh.normals[0], static_cast<std::size_t>(vertices));
    if (uvs)
        w.write_uvs(&mesh.uvs[0], static_cast<std::size_t>(vertices));
    w.finish();
}

// ---- MeshReader ----------------------------------------------------------------------------------

MeshReader::MeshReader(const Source &source)
    : source_(source), vertices_(0), triangles_(0), normals_(false), uvs_(false), data_size_(0),
      off_positions_(0), off_indices_(0), off_normals_(0), off_uvs_(0), block_index_(0), block_valid_(false) {
    if (source.size() < kMeshHeaderSize + 8)
        codec::fail_corrupt("mesh.header", "mesh entry is too short");
    unsigned char head[kMeshHeaderSize];
    if (!read_exact(source, 0, head, sizeof head))
        throw FormatError(LOOMORA_ERR_IO, "short read of mesh entry");
    if (std::memcmp(head, kMagic, 4) != 0)
        codec::fail_corrupt("mesh.header", "bad magic (not a Loomora mesh)");
    if (head[4] != 1)
        codec::fail_corrupt("mesh.header", "unsupported mesh version");
    if ((head[5] & ~(kMeshFlagNormals | kMeshFlagUvs)) != 0)
        codec::fail_corrupt("mesh.header", "reserved mesh flag set");
    if (read_u16(head + 6) != 0)
        codec::fail_corrupt("mesh.header", "reserved mesh header field is not zero");
    normals_ = (head[5] & kMeshFlagNormals) != 0;
    uvs_ = (head[5] & kMeshFlagUvs) != 0;
    vertices_ = read_u64(head + 8);
    triangles_ = read_u64(head + 16);

    Layout l;
    if (!compute_layout(vertices_, triangles_, normals_, uvs_, &l))
        codec::fail_corrupt("mesh.header", "vertex or triangle count is out of range");
    const std::uint64_t blocks = blocks_for(l.data);
    if (l.data + 4 * blocks + 4 != source.size())
        codec::fail_corrupt("mesh.header", "entry size does not match the counts and flags");
    data_size_ = l.data;
    off_positions_ = kMeshHeaderSize;
    off_indices_ = off_positions_ + l.positions;
    off_normals_ = off_indices_ + l.indices;
    off_uvs_ = off_normals_ + l.normals;

    // The size equals what the counts give, so the table is at most as large as the entry.
    std::vector<unsigned char> raw(static_cast<std::size_t>(blocks * 4 + 4));
    if (!read_exact(source, data_size_, &raw[0], raw.size()))
        throw FormatError(LOOMORA_ERR_IO, "short read of mesh entry");
    const unsigned long tcrc = crc_update(crc_init(), &raw[0], static_cast<std::size_t>(blocks * 4));
    if (read_u32(&raw[static_cast<std::size_t>(blocks * 4)]) != static_cast<std::uint32_t>(tcrc & 0xFFFFFFFFul))
        codec::fail_corrupt("mesh.crc", "CRC-32 of the mesh block table does not match");
    table_.resize(static_cast<std::size_t>(blocks));
    for (std::size_t i = 0; i < table_.size(); ++i)
        table_[i] = read_u32(&raw[i * 4]);
}

void MeshReader::check_counts(std::uint64_t vertices, std::uint64_t triangles, bool normals, bool uvs) const {
    if (vertices != vertices_ || triangles != triangles_ || normals != normals_ || uvs != uvs_)
        codec::fail_corrupt("mesh.count", "mesh counts or flags differ from the frame's description");
}

const unsigned char *MeshReader::load_block(std::uint64_t index) const {
    if (block_valid_ && block_index_ == index)
        return &block_[0];
    block_valid_ = false;
    if (block_.empty())
        block_.resize(kMeshBlockSize);
    const std::uint64_t start = index * kMeshBlockSize;
    const std::uint64_t left = data_size_ - start;
    const std::size_t len = static_cast<std::size_t>(left < kMeshBlockSize ? left : kMeshBlockSize);
    if (!read_exact(source_, start, &block_[0], len))
        throw FormatError(LOOMORA_ERR_IO, "short read of mesh entry");
    const unsigned long crc = crc_update(crc_init(), &block_[0], len);
    if (static_cast<std::uint32_t>(crc & 0xFFFFFFFFul) != table_[static_cast<std::size_t>(index)])
        codec::fail_corrupt("mesh.crc", "CRC-32 of a mesh block does not match");
    block_index_ = index;
    block_valid_ = true;
    return &block_[0];
}

void MeshReader::read_bytes(std::uint64_t offset, std::size_t len, unsigned char *out) const {
    while (len > 0) {
        const std::uint64_t index = offset / kMeshBlockSize;
        const unsigned char *blk = load_block(index);
        const std::size_t inside = static_cast<std::size_t>(offset - index * kMeshBlockSize);
        const std::uint64_t left = data_size_ - index * kMeshBlockSize;
        const std::size_t blen = static_cast<std::size_t>(left < kMeshBlockSize ? left : kMeshBlockSize);
        const std::size_t n = std::min(len, blen - inside);
        std::memcpy(out, blk + inside, n);
        out += n;
        offset += n;
        len -= n;
    }
}

void MeshReader::check_range(std::uint64_t first, std::uint64_t count, std::uint64_t total,
                             const char *what) const {
    if (first > total || count > total - first)
        codec::fail_internal(std::string("mesh range read outside the ") + what);
}

void MeshReader::read_positions(std::uint64_t first, std::uint64_t count, float *out) const {
    check_range(first, count, vertices_, "vertices");
    unsigned char buf[kChunkItems * 12];
    for (std::uint64_t i = 0; i < count; i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, count - i));
        read_bytes(off_positions_ + (first + i) * 12, n * 12, buf);
        for (std::size_t k = 0; k < n * 3; ++k) {
            if (!f32_is_finite_bits(buf + 4 * k))
                codec::fail_corrupt("num.not_finite", "a vertex position is NaN or Infinity");
            out[i * 3 + k] = read_f32(buf + 4 * k);
        }
    }
}

void MeshReader::read_indices(std::uint64_t first, std::uint64_t count, std::uint32_t *out) const {
    check_range(first, count, triangles_, "triangles");
    unsigned char buf[kChunkItems * 12];
    for (std::uint64_t i = 0; i < count; i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, count - i));
        read_bytes(off_indices_ + (first + i) * 12, n * 12, buf);
        for (std::size_t k = 0; k < n * 3; ++k) {
            const std::uint32_t v = read_u32(buf + 4 * k);
            if (v >= vertices_)
                codec::fail_corrupt("mesh.index_range", "a triangle index is not below the vertex count");
            out[i * 3 + k] = v;
        }
    }
}

void MeshReader::read_normals(std::uint64_t first, std::uint64_t count, float *out) const {
    if (!normals_)
        codec::fail_internal("mesh has no normals");
    check_range(first, count, vertices_, "vertices");
    unsigned char buf[kChunkItems * 12];
    for (std::uint64_t i = 0; i < count; i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, count - i));
        read_bytes(off_normals_ + (first + i) * 12, n * 12, buf);
        for (std::size_t v = 0; v < n; ++v) {
            const unsigned char *p = buf + 12 * v;
            if (!f32_is_finite_bits(p) || !f32_is_finite_bits(p + 4) || !f32_is_finite_bits(p + 8))
                codec::fail_corrupt("num.not_finite", "a vertex normal is NaN or Infinity");
            const float x = read_f32(p), y = read_f32(p + 4), z = read_f32(p + 8);
            if (!unit_length(x, y, z))
                codec::fail_corrupt("mesh.normal", "a vertex normal is not of unit length (within 1e-3)");
            float *o = out + (i + v) * 3;
            o[0] = x;
            o[1] = y;
            o[2] = z;
        }
    }
}

void MeshReader::read_uvs(std::uint64_t first, std::uint64_t count, float *out) const {
    if (!uvs_)
        codec::fail_internal("mesh has no texture coordinates");
    check_range(first, count, vertices_, "vertices");
    unsigned char buf[kChunkItems * 8];
    for (std::uint64_t i = 0; i < count; i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, count - i));
        read_bytes(off_uvs_ + (first + i) * 8, n * 8, buf);
        for (std::size_t k = 0; k < n * 2; ++k) {
            if (!f32_is_finite_bits(buf + 4 * k))
                codec::fail_corrupt("num.not_finite", "a texture coordinate is NaN or Infinity");
            out[i * 2 + k] = read_f32(buf + 4 * k);
        }
    }
}

void MeshReader::verify() const {
    load_block(0); // the header block, even when there is no section
    std::vector<float> f(kChunkItems * 3);
    std::vector<std::uint32_t> t(kChunkItems * 3);
    for (std::uint64_t i = 0; i < vertices_; i += kChunkItems)
        read_positions(i, std::min<std::uint64_t>(kChunkItems, vertices_ - i), &f[0]);
    for (std::uint64_t i = 0; i < triangles_; i += kChunkItems)
        read_indices(i, std::min<std::uint64_t>(kChunkItems, triangles_ - i), &t[0]);
    if (normals_)
        for (std::uint64_t i = 0; i < vertices_; i += kChunkItems)
            read_normals(i, std::min<std::uint64_t>(kChunkItems, vertices_ - i), &f[0]);
    if (uvs_)
        for (std::uint64_t i = 0; i < vertices_; i += kChunkItems)
            read_uvs(i, std::min<std::uint64_t>(kChunkItems, vertices_ - i), &f[0]);
}

void read_mesh(const Source &source, MeshData *mesh) {
    MeshReader r(source);
    // The entry size equals the counts, so these vectors are no larger than the entry itself.
    if (r.vertex_count() > ~static_cast<std::size_t>(0) / 12 || r.triangle_count() > ~static_cast<std::size_t>(0) / 12)
        throw FormatError(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM, "mesh is too large for this platform");
    const std::size_t nv = static_cast<std::size_t>(r.vertex_count());
    const std::size_t nt = static_cast<std::size_t>(r.triangle_count());
    MeshData m;
    m.positions.resize(nv * 3);
    m.indices.resize(nt * 3);
    if (r.has_normals())
        m.normals.resize(nv * 3);
    if (r.has_uvs())
        m.uvs.resize(nv * 2);
    r.verify();
    if (nv > 0)
        r.read_positions(0, nv, &m.positions[0]);
    if (nt > 0)
        r.read_indices(0, nt, &m.indices[0]);
    if (nv > 0 && r.has_normals())
        r.read_normals(0, nv, &m.normals[0]);
    if (nv > 0 && r.has_uvs())
        r.read_uvs(0, nv, &m.uvs[0]);
    *mesh = m;
}

void rewrite_mesh_without_uvs(const Source &in, Sink &out) {
    MeshReader r(in);
    MeshWriter w(out, r.vertex_count(), r.triangle_count(), r.has_normals(), false);
    std::vector<float> f(kChunkItems * 3);
    std::vector<std::uint32_t> t(kChunkItems * 3);
    for (std::uint64_t i = 0; i < r.vertex_count(); i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, r.vertex_count() - i));
        r.read_positions(i, n, &f[0]);
        w.write_positions(&f[0], n);
    }
    for (std::uint64_t i = 0; i < r.triangle_count(); i += kChunkItems) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, r.triangle_count() - i));
        r.read_indices(i, n, &t[0]);
        w.write_indices(&t[0], n);
    }
    if (r.has_normals())
        for (std::uint64_t i = 0; i < r.vertex_count(); i += kChunkItems) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkItems, r.vertex_count() - i));
            r.read_normals(i, n, &f[0]);
            w.write_normals(&f[0], n);
        }
    if (r.has_uvs()) // not copied, but verified like the rest of the source
        for (std::uint64_t i = 0; i < r.vertex_count(); i += kChunkItems)
            r.read_uvs(i, std::min<std::uint64_t>(kChunkItems, r.vertex_count() - i), &f[0]);
    w.finish();
}

} // namespace format
} // namespace loomora

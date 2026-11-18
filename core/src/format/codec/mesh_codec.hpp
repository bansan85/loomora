// Codec of `mesh.bin`, the triangle mesh of a 3D frame (contracts/project-format.md 5.2).
//
//   "LMMS" | version u8 (=1) | flags u8 (bit 0 normals, bit 1 texture coordinates) | reserved u16 (=0)
//   vertex_count u64 | triangle_count u64                                    (24-byte header)
//   positions: vertex_count x 3 x f32 (mm)
//   indices:   triangle_count x 3 x u32, each < vertex_count
//   normals:   vertex_count x 3 x f32, unit length within 1e-3              (only with bit 0)
//   uvs:       vertex_count x 2 x f32, finite                                (only with bit 1)
//   block table: CRC-32 u32 of each 1 MiB block of everything above (the header included, the last
//                block shorter), then the CRC-32 u32 of the table's bytes
// All integers and floats are little-endian and read and written byte by byte.
//
// Winding (FR-015): triangle (a, b, c) is counter-clockwise seen from the outside (front view,
// right-handed, Z toward the viewer); the codec stores indices and positions as given and never
// reorders or mirrors them.
//
// Memory is bounded: MeshWriter streams to a Sink with a small buffer, MeshReader reads ranges
// from a Source through one cached 1 MiB block that is CRC-checked whenever it is loaded. Neither
// holds the mesh.
//
// Errors on read are FormatError(LOOMORA_ERR_CORRUPT) carrying an Issue with one of:
//   mesh.crc          a block CRC or the table CRC does not match;
//   mesh.index_range  an index is not below vertex_count;
//   mesh.header       bad magic, version, reserved flag or field, absurd counts, or an entry size that
//                     does not equal the size the counts and flags give;
//   mesh.count        counts or flags differ from the expected ones (frame.json; MeshReader::check_counts);
//   mesh.normal       a normal whose length is not 1 within 1e-3;
//   num.not_finite    a position, normal or texture coordinate that is NaN or Infinity.
// The writer refuses such data with FormatError(LOOMORA_ERR_INVALID_ITEM) carrying the same
// issue code (not mesh.crc / mesh.header / mesh.count), and misuse (sections out of order, too
// many values, finish too early) with LOOMORA_ERR_INTERNAL. mesh.header, mesh.count, mesh.normal are
// codes beyond the list of contracts/cli-validate.md; they are stable and documented here.
//
// Readers are not thread safe (they cache a block): use one per thread over the same Source.
#ifndef LOOMORA_FORMAT_CODEC_MESH_CODEC_HPP
#define LOOMORA_FORMAT_CODEC_MESH_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "format/io/sink.hpp"
#include "format/io/source.hpp"

namespace loomora {
namespace format {

const std::size_t kMeshBlockSize = 1024 * 1024; // block of the CRC table
const std::size_t kMeshHeaderSize = 24;
const unsigned kMeshFlagNormals = 1;
const unsigned kMeshFlagUvs = 2;

// Size in bytes of the whole entry for these counts (header, sections, block table), or 0 when the
// counts are not representable (vertices above 2^32, or a size beyond 2^63).
std::uint64_t mesh_entry_size(std::uint64_t vertices, std::uint64_t triangles, bool normals, bool uvs);

// A whole mesh in memory, for small meshes, tests and the editor; big meshes are streamed.
struct MeshData {
    std::vector<float> positions;        // 3 per vertex
    std::vector<std::uint32_t> indices;  // 3 per triangle
    std::vector<float> normals;          // 3 per vertex, or empty (no normals)
    std::vector<float> uvs;              // 2 per vertex, or empty (no texture coordinates)

    std::uint64_t vertex_count() const { return positions.size() / 3; }
    std::uint64_t triangle_count() const { return indices.size() / 3; }
};

// Streams one mesh.bin to a Sink (not committed or aborted here). The sections must be written in
// order (positions, indices, normals if any, uvs if any), each in as many calls as wanted, each
// complete before the next starts; finish() writes the block table.
class MeshWriter {
  public:
    // Writes the header. `uvs` false is the light profile (or a mesh that has none).
    MeshWriter(Sink &sink, std::uint64_t vertex_count, std::uint64_t triangle_count, bool normals,
               bool uvs);

    void write_positions(const float *xyz, std::size_t vertices);        // 3 floats each
    void write_indices(const std::uint32_t *abc, std::size_t triangles); // 3 indices each
    void write_normals(const float *xyz, std::size_t vertices);          // 3 floats each
    void write_uvs(const float *uv, std::size_t vertices);               // 2 floats each

    // Checks that every declared section is complete, then writes the table and its CRC.
    void finish();

  private:
    enum Section { kPositions, kIndices, kNormals, kUvs, kDone };
    void begin(Section section, std::uint64_t items);
    void emit(const unsigned char *data, std::size_t len);
    void advance_section();

    Sink &sink_;
    std::uint64_t vertices_, triangles_;
    bool normals_, uvs_;
    Section section_;
    std::uint64_t left_;                 // items still expected in the current section
    unsigned long block_crc_;
    std::size_t in_block_;
    std::vector<std::uint32_t> table_;
    bool finished_;
};

// Writes a whole mesh (sizes checked: positions % 3, indices % 3, normals and uvs empty or sized
// for the vertices).
void write_mesh(Sink &sink, const MeshData &mesh);

// Range reader over a Source.
class MeshReader {
  public:
    // Reads and checks the header, the size and the table CRC (mesh.header, mesh.crc); does not
    // read the sections.
    explicit MeshReader(const Source &source);

    std::uint64_t vertex_count() const { return vertices_; }
    std::uint64_t triangle_count() const { return triangles_; }
    bool has_normals() const { return normals_; }
    bool has_uvs() const { return uvs_; }
    std::uint64_t block_count() const { return table_.size(); }

    // mesh.count unless the counts and the flags equal frame.json's (vertices, triangles,
    // has_normals, has_uvs).
    void check_counts(std::uint64_t vertices, std::uint64_t triangles, bool normals, bool uvs) const;

    // Ranges (`first` and `count` in vertices or triangles; FormatError(LOOMORA_ERR_INTERNAL) when
    // out of range, or when the section is absent for normals and uvs). Every block that is read
    // has its CRC checked (mesh.crc). `out` holds 3 * count floats, 3 * count indices, 3 * count
    // floats, 2 * count floats.
    void read_positions(std::uint64_t first, std::uint64_t count, float *out) const;
    void read_indices(std::uint64_t first, std::uint64_t count, std::uint32_t *out) const;
    void read_normals(std::uint64_t first, std::uint64_t count, float *out) const;
    void read_uvs(std::uint64_t first, std::uint64_t count, float *out) const;

    // The whole validation pass: every block CRC, every value (finite, index range, unit normals).
    void verify() const;

  private:
    const unsigned char *load_block(std::uint64_t index) const;
    void read_bytes(std::uint64_t offset, std::size_t len, unsigned char *out) const;
    void check_range(std::uint64_t first, std::uint64_t count, std::uint64_t total, const char *what) const;

    const Source &source_;
    std::uint64_t vertices_, triangles_;
    bool normals_, uvs_;
    std::uint64_t data_size_;     // bytes covered by the block table
    std::uint64_t off_positions_, off_indices_, off_normals_, off_uvs_;
    std::vector<std::uint32_t> table_;
    mutable std::vector<unsigned char> block_;
    mutable std::uint64_t block_index_;
    mutable bool block_valid_;
};

// Reads a whole mesh into memory (verified like verify()); for small meshes and tests.
void read_mesh(const Source &source, MeshData *mesh);

// The light-profile rewrite (FR-017): copies `in` to `out` without its texture coordinates: bit 1
// cleared, positions, indices and normals the same values, block table recomputed. `in` is fully
// verified while it is read (as verify()); an `in` without UVs gives the same bytes. Streams with
// bounded memory; `out` is not committed or aborted here.
void rewrite_mesh_without_uvs(const Source &in, Sink &out);

} // namespace format
} // namespace loomora

#endif

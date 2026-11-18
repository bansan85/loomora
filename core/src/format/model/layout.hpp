// Layout (layout.json) and its nails (nails.json), data-model.md "Layout (nail layout)".
//
// A layout is small (id, name, frame, nail_count). Its nails can be many (500,000 and more), so
// they are held in a NailTable: parallel arrays ("structure of arrays", about 30 bytes per 2D nail
// and 90 per 3D nail), with the optional labels kept apart, sorted by nail position. The nails
// codec (codec/nails_codec.hpp) fills a table from nails.json and writes it back. `Nail` is the
// one-nail view used to add and read nails without knowing the layout of the arrays.
#ifndef LOOMORA_FORMAT_MODEL_LAYOUT_HPP
#define LOOMORA_FORMAT_MODEL_LAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "format/model/common.hpp"

namespace loomora {
namespace format {
namespace model {

enum NailKind { kNailBoundary = 0, kNailExclusionBoundary = 1, kNailInterior = 2 };
enum NailSide { kSideExterior = 0, kSideInterior = 1 };

const char *to_string(NailKind k);
const char *to_string(NailSide s);
bool parse_nail_kind(const std::string &text, NailKind &out);
bool parse_nail_side(const std::string &text, NailSide &out);

// JSON key order of layout.json: id, name, frame, nail_count.
struct Layout {
    std::uint32_t id = 0;
    std::string name;                // up to 256 bytes
    std::uint32_t frame = 0;         // fixed for the life of the layout
    std::uint64_t nail_count = 0;    // equals the number of nails in nails.json (checked on reading)
};

void check_layout(const Layout &layout, const std::string &entry, Report &report);

// One nail. JSON key order: id, x, y, z, diameter, kind, label, axis, length, side, face{triangle, u, v}.
// The 3D fields (z, axis, length, side, triangle, u, v) are meaningful only in a 3D table.
struct Nail {
    std::uint32_t id = 0;            // unique within the layout, any u32
    double x = 0, y = 0, z = 0;
    double diameter = 0;             // > 0
    NailKind kind = kNailBoundary;
    bool has_label = false;
    std::string label;               // optional, up to 64 bytes
    double axis[3] = {0, 0, 1};      // unit vector within 1e-6
    double length = 0;               // >= 0
    NailSide side = kSideExterior;
    std::uint32_t triangle = 0;      // face.triangle, below the triangle count of the frame's mesh
    double u = 0, v = 0;             // face.u, face.v: u >= -1e-6, v >= -1e-6, u + v <= 1 + 1e-6
};

// One entry of the range-read index per this many nails.
const std::size_t kNailsPerIndexEntry = 1024;

class NailTable {
  public:
    explicit NailTable(bool is_3d = false) : is_3d_(is_3d) {}

    bool is_3d() const { return is_3d_; }
    std::size_t size() const { return id.size(); }
    // Removes every nail (keeps the dimension).
    void clear();
    void reserve(std::size_t count);
    // Appends a nail; the 3D fields of `n` are ignored in a 2D table.
    void add(const Nail &n);
    // The nail at `index` (throws std::out_of_range when index >= size()). In a 2D table the 3D
    // fields have their defaults.
    Nail get(std::size_t index) const;
    // The label of the nail at `index`, or null.
    const std::string *label_at(std::size_t index) const;
    std::size_t label_count() const { return label_index.size(); }

    // The arrays, all of length size() (3D ones empty in a 2D table; axis has 3 values per nail).
    std::vector<std::uint32_t> id;
    std::vector<double> x, y, z, diameter;
    std::vector<std::uint8_t> kind;       // NailKind
    std::vector<double> axis;             // 3 per nail
    std::vector<double> length;
    std::vector<std::uint8_t> side;       // NailSide
    std::vector<std::uint32_t> triangle;
    std::vector<double> u, v;
    // The labels: label_index[i] is the position (increasing) of the nail with label_text[i].
    std::vector<std::uint32_t> label_index;
    std::vector<std::string> label_text;
    // Range-read index filled by the codec when it parses: offsets[k] is the byte offset in
    // nails.json of the `{` of nail k * kNailsPerIndexEntry.
    std::vector<std::uint64_t> offsets;

  private:
    bool is_3d_;
};

// "No bound": the triangle count to give when the frame's mesh is not known.
const std::uint64_t kAnyTriangleCount = ~static_cast<std::uint64_t>(0);

// Rules of the nails of layout `layout_id`: finite numbers, diameter > 0, id unique
// (nail.duplicate_id), label up to 64 bytes, kind and side valid, and for a 3D table the axis a
// unit vector within 1e-6, length >= 0, face.u and face.v >= -1e-6, u + v <= 1 + 1e-6 and
// face.triangle < `triangle_count`. At most 1,000 issues are added (a table of 500,000 bad
// nails does not make a report of 500,000 lines): the last one then says that more were left out.
void check_nails(const NailTable &nails, const std::string &entry, std::uint32_t layout_id, Report &report,
                 std::uint64_t triangle_count = kAnyTriangleCount);

} // namespace model
} // namespace format
} // namespace loomora

#endif

// Geometry of the semantic validator: a polygon tester that stays fast on large outlines, and the
// search of the closest pair of nails on a uniform grid (linear time for nails that do not overlap).
#ifndef LOOMORA_FORMAT_VALIDATE_GEOMETRY_HPP
#define LOOMORA_FORMAT_VALIDATE_GEOMETRY_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "format/model/frame.hpp"
#include "format/model/layout.hpp"

namespace loomora {
namespace format {

// A closed polygon (the last point joins the first). The edges are bucketed by their y range, so
// a point test looks at the edges of one bucket only.
class Polygon {
  public:
    explicit Polygon(const std::vector<model::Point> &points);

    std::size_t size() const { return pts_.size(); }
    // Bounding box of the points (meaningless when empty).
    double min_x() const { return minx_; }
    double max_x() const { return maxx_; }
    double min_y() const { return miny_; }
    double max_y() const { return maxy_; }
    // True when the point is inside (even-odd rule).
    bool contains(double x, double y) const;
    // True when the point is at most `tol` from an edge.
    bool near_boundary(double x, double y, double tol) const;

  private:
    std::size_t bucket_of(double y) const;
    std::vector<model::Point> pts_;
    double minx_, maxx_, miny_, maxy_;
    double inv_height_;
    std::size_t buckets_;
    std::vector<std::uint32_t> start_; // buckets_ + 1 offsets into items_
    std::vector<std::uint32_t> items_; // edge index (edge i joins point i and i + 1)
};

// The first pair of nails of `t` closer than the sum of their radii minus `tolerance` (the exact
// distance test, 3D distance for a 3D table). Returns false when there is none. Memory and time
// are linear in the number of nails as long as the nails do not overlap; the search stops at the
// first pair found.
bool find_close_nails(const model::NailTable &t, double tolerance, std::size_t &first, std::size_t &second,
                      double &distance);

} // namespace format
} // namespace loomora

#endif

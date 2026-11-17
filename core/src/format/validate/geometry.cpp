#include "format/validate/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace loomora {
namespace format {

namespace {

const std::size_t kMaxBuckets = 4096;
const std::size_t kMaxBucketEntries = 8u * 1024 * 1024;

double segment_distance2(double px, double py, const model::Point &a, const model::Point &b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = 0;
    if (len2 > 0) {
        t = ((px - a.x) * dx + (py - a.y) * dy) / len2;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
    }
    const double cx = a.x + t * dx - px, cy = a.y + t * dy - py;
    return cx * cx + cy * cy;
}

} // namespace

Polygon::Polygon(const std::vector<model::Point> &points)
    : pts_(points), minx_(0), maxx_(0), miny_(0), maxy_(0), inv_height_(0), buckets_(1) {
    if (pts_.empty()) {
        start_.assign(2, 0);
        return;
    }
    minx_ = maxx_ = pts_[0].x;
    miny_ = maxy_ = pts_[0].y;
    for (std::size_t i = 1; i < pts_.size(); ++i) {
        minx_ = std::min(minx_, pts_[i].x);
        maxx_ = std::max(maxx_, pts_[i].x);
        miny_ = std::min(miny_, pts_[i].y);
        maxy_ = std::max(maxy_, pts_[i].y);
    }
    const std::size_t edges = pts_.size();
    buckets_ = std::min<std::size_t>(std::max<std::size_t>(edges / 4, 1), kMaxBuckets);
    // An edge is listed in every bucket its y range touches: halve the buckets until the lists fit.
    for (;;) {
        inv_height_ = (maxy_ > miny_) ? static_cast<double>(buckets_) / (maxy_ - miny_) : 0;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < edges; ++i) {
            const model::Point &a = pts_[i], &b = pts_[(i + 1) % edges];
            total += bucket_of(std::max(a.y, b.y)) - bucket_of(std::min(a.y, b.y)) + 1;
        }
        if (total <= kMaxBucketEntries || buckets_ == 1) {
            start_.assign(buckets_ + 1, 0);
            for (std::size_t i = 0; i < edges; ++i) {
                const model::Point &a = pts_[i], &b = pts_[(i + 1) % edges];
                for (std::size_t k = bucket_of(std::min(a.y, b.y)); k <= bucket_of(std::max(a.y, b.y)); ++k)
                    ++start_[k + 1];
            }
            for (std::size_t k = 0; k < buckets_; ++k)
                start_[k + 1] += start_[k];
            items_.assign(start_[buckets_], 0);
            std::vector<std::uint32_t> fill(start_.begin(), start_.end() - 1);
            for (std::size_t i = 0; i < edges; ++i) {
                const model::Point &a = pts_[i], &b = pts_[(i + 1) % edges];
                for (std::size_t k = bucket_of(std::min(a.y, b.y)); k <= bucket_of(std::max(a.y, b.y)); ++k)
                    items_[fill[k]++] = static_cast<std::uint32_t>(i);
            }
            return;
        }
        buckets_ /= 2;
    }
}

std::size_t Polygon::bucket_of(double y) const {
    const double f = (y - miny_) * inv_height_;
    if (!(f > 0))
        return 0;
    if (f >= static_cast<double>(buckets_))
        return buckets_ - 1;
    return static_cast<std::size_t>(f);
}

bool Polygon::contains(double x, double y) const {
    if (pts_.empty() || y < miny_ || y > maxy_ || x < minx_ || x > maxx_)
        return false;
    const std::size_t b = bucket_of(y);
    bool inside = false;
    for (std::uint32_t k = start_[b]; k < start_[b + 1]; ++k) {
        const model::Point &a = pts_[items_[k]];
        const model::Point &c = pts_[(items_[k] + 1) % pts_.size()];
        if ((a.y > y) != (c.y > y)) {
            const double xi = a.x + (y - a.y) * (c.x - a.x) / (c.y - a.y);
            if (x < xi)
                inside = !inside;
        }
    }
    return inside;
}

bool Polygon::near_boundary(double x, double y, double tol) const {
    if (pts_.empty() || y < miny_ - tol || y > maxy_ + tol || x < minx_ - tol || x > maxx_ + tol)
        return false;
    const double tol2 = tol * tol;
    for (std::size_t b = bucket_of(y - tol), last = bucket_of(y + tol); b <= last; ++b)
        for (std::uint32_t k = start_[b]; k < start_[b + 1]; ++k)
            if (segment_distance2(x, y, pts_[items_[k]], pts_[(items_[k] + 1) % pts_.size()]) <= tol2)
                return true;
    return false;
}

// ---- closest pair of nails ------------------------------------------------------------------------

namespace {

// Nails are split by diameter class k (diameter in [2^k, 2^(k+1))), each class on its own grid of
// cell 2^(k+1). Two nails of classes a <= b are closer than the sum of their radii only when they
// are in cells at most one apart on the grid of class b.
struct NailGrid {
    struct Slot {
        std::uint64_t key;
        std::uint32_t head; // first nail + 1 of the chain, 0 when the slot is free
        Slot() : key(0), head(0) {}
    };
    std::vector<Slot> slots;
    std::vector<std::uint32_t> next; // per nail: the next nail + 1 of the same cell
    std::size_t mask;

    explicit NailGrid(std::size_t n) : next(n, 0), mask(0) {
        std::size_t cap = 16;
        while (cap < 2 * n)
            cap *= 2;
        slots.resize(cap);
        mask = cap - 1;
    }
    static std::uint64_t mix(std::uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }
    void insert(std::uint64_t key, std::uint32_t nail) {
        std::size_t i = static_cast<std::size_t>(mix(key)) & mask;
        while (slots[i].head != 0 && slots[i].key != key)
            i = (i + 1) & mask;
        slots[i].key = key;
        next[nail] = slots[i].head;
        slots[i].head = nail + 1;
    }
    std::uint32_t first(std::uint64_t key) const { // nail + 1, 0 when the cell is empty
        std::size_t i = static_cast<std::size_t>(mix(key)) & mask;
        while (slots[i].head != 0) {
            if (slots[i].key == key)
                return slots[i].head;
            i = (i + 1) & mask;
        }
        return 0;
    }
};

std::int64_t cell_of(double v, int cell_exp) {
    double f = std::floor(std::ldexp(v, -cell_exp));
    const double lim = 4.0e15;
    f = f < -lim ? -lim : (f > lim ? lim : f);
    return static_cast<std::int64_t>(f);
}

std::uint64_t cell_key(int cls, std::int64_t ix, std::int64_t iy, std::int64_t iz) {
    std::uint64_t k = static_cast<std::uint64_t>(static_cast<std::int64_t>(cls) + 4096);
    k = k * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(ix);
    k = k * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(iy);
    k = k * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(iz);
    return k;
}

} // namespace

bool find_close_nails(const model::NailTable &t, double tolerance, std::size_t &first, std::size_t &second,
                      double &distance) {
    const std::size_t n = t.size();
    if (n < 2 || n >= 0xFFFFFFFEu)
        return false;
    const bool is3d = t.is_3d() && t.z.size() == n;
    std::vector<int> cls(n);
    std::map<int, std::size_t> classes;
    for (std::size_t i = 0; i < n; ++i) {
        int e = 0;
        std::frexp(t.diameter[i], &e);
        cls[i] = e - 1;
        ++classes[cls[i]];
    }
    NailGrid grid(n);
    for (std::size_t i = 0; i < n; ++i) {
        const int c = cls[i];
        const std::int64_t iz = is3d ? cell_of(t.z[i], c + 1) : 0;
        grid.insert(cell_key(c, cell_of(t.x[i], c + 1), cell_of(t.y[i], c + 1), iz), static_cast<std::uint32_t>(i));
    }
    const int zr = is3d ? 1 : 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ri = t.diameter[i] / 2;
        for (std::map<int, std::size_t>::const_iterator it = classes.lower_bound(cls[i]); it != classes.end(); ++it) {
            const int b = it->first;
            const std::int64_t cx = cell_of(t.x[i], b + 1), cy = cell_of(t.y[i], b + 1);
            const std::int64_t cz = is3d ? cell_of(t.z[i], b + 1) : 0;
            for (int dz = -zr; dz <= zr; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        std::uint32_t j1 = grid.first(cell_key(b, cx + dx, cy + dy, cz + dz));
                        for (; j1 != 0; j1 = grid.next[j1 - 1]) {
                            const std::size_t j = j1 - 1;
                            if (j == i)
                                continue;
                            const double reach = ri + t.diameter[j] / 2 - tolerance;
                            if (!(reach > 0))
                                continue;
                            const double ex = t.x[i] - t.x[j], ey = t.y[i] - t.y[j];
                            const double ez = is3d ? t.z[i] - t.z[j] : 0;
                            const double d2 = ex * ex + ey * ey + ez * ez;
                            if (d2 < reach * reach) {
                                first = std::min(i, j);
                                second = std::max(i, j);
                                distance = std::sqrt(d2);
                                return true;
                            }
                        }
                    }
        }
    }
    return false;
}

} // namespace format
} // namespace loomora

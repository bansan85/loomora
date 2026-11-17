#include "format/model/layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace loomora {
namespace format {
namespace model {

namespace {

const char *const kKindNames[] = {"boundary", "exclusion_boundary", "interior"};
const char *const kSideNames[] = {"exterior", "interior"};

const std::size_t kMaxNailIssues = 1000;
const std::size_t kNoPosition = ~static_cast<std::size_t>(0);

std::string decimal(std::uint64_t v) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

// Adds issues up to the cap, then one last line saying that more were left out.
class CappedReport {
  public:
    CappedReport(Report &r, const std::string &entry, std::uint32_t layout) : r_(r), entry_(entry), layout_(layout), n_(0) {}
    void add(const char *code, std::size_t index, std::uint32_t nail_id, const std::string &what) {
        if (n_ < kMaxNailIssues) {
            add_error(r_, code, entry_, kItemLayout, layout_,
                      "nail " + decimal(nail_id) +
                          (index == kNoPosition ? std::string() : " (position " + decimal(index) + ")") + ": " + what);
        } else if (n_ == kMaxNailIssues) {
            add_error(r_, "schema.value", entry_, kItemLayout, layout_,
                      "more than " + decimal(kMaxNailIssues) + " nail issues: the others are not listed");
        }
        ++n_;
    }
    bool finite(std::size_t i, std::uint32_t id, const char *what, double v) {
        if (is_finite(v))
            return true;
        add("num.not_finite", i, id, std::string(what) + " is not finite");
        return false;
    }

  private:
    Report &r_;
    const std::string &entry_;
    std::uint32_t layout_;
    std::size_t n_;
};

} // namespace

const char *to_string(NailKind k) {
    const int i = static_cast<int>(k);
    return (i >= 0 && i < 3) ? kKindNames[i] : "";
}

const char *to_string(NailSide s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < 2) ? kSideNames[i] : "";
}

bool parse_nail_kind(const std::string &text, NailKind &out) {
    int i;
    if (!lookup_name(kKindNames, 3, text, i))
        return false;
    out = static_cast<NailKind>(i);
    return true;
}

bool parse_nail_side(const std::string &text, NailSide &out) {
    int i;
    if (!lookup_name(kSideNames, 2, text, i))
        return false;
    out = static_cast<NailSide>(i);
    return true;
}

void check_layout(const Layout &l, const std::string &entry, Report &report) {
    check_length(report, entry, kItemLayout, l.id, "name", l.name, kMaxNameBytes);
}

void NailTable::clear() {
    id.clear();
    x.clear();
    y.clear();
    z.clear();
    diameter.clear();
    kind.clear();
    axis.clear();
    length.clear();
    side.clear();
    triangle.clear();
    u.clear();
    v.clear();
    label_index.clear();
    label_text.clear();
    offsets.clear();
}

void NailTable::reserve(std::size_t n) {
    id.reserve(n);
    x.reserve(n);
    y.reserve(n);
    diameter.reserve(n);
    kind.reserve(n);
    if (is_3d_) {
        z.reserve(n);
        axis.reserve(3 * n);
        length.reserve(n);
        side.reserve(n);
        triangle.reserve(n);
        u.reserve(n);
        v.reserve(n);
    }
}

void NailTable::add(const Nail &n) {
    id.push_back(n.id);
    x.push_back(n.x);
    y.push_back(n.y);
    diameter.push_back(n.diameter);
    kind.push_back(static_cast<std::uint8_t>(n.kind));
    if (is_3d_) {
        z.push_back(n.z);
        axis.push_back(n.axis[0]);
        axis.push_back(n.axis[1]);
        axis.push_back(n.axis[2]);
        length.push_back(n.length);
        side.push_back(static_cast<std::uint8_t>(n.side));
        triangle.push_back(n.triangle);
        u.push_back(n.u);
        v.push_back(n.v);
    }
    if (n.has_label) {
        label_index.push_back(static_cast<std::uint32_t>(id.size() - 1));
        label_text.push_back(n.label);
    }
}

const std::string *NailTable::label_at(std::size_t index) const {
    const std::uint32_t key = static_cast<std::uint32_t>(index);
    std::vector<std::uint32_t>::const_iterator it = std::lower_bound(label_index.begin(), label_index.end(), key);
    if (it == label_index.end() || *it != key)
        return nullptr;
    return &label_text[static_cast<std::size_t>(it - label_index.begin())];
}

Nail NailTable::get(std::size_t i) const {
    if (i >= id.size())
        throw std::out_of_range("nail index");
    Nail n;
    n.id = id[i];
    n.x = x[i];
    n.y = y[i];
    n.diameter = diameter[i];
    n.kind = static_cast<NailKind>(kind[i]);
    if (is_3d_) {
        n.z = z[i];
        n.axis[0] = axis[3 * i];
        n.axis[1] = axis[3 * i + 1];
        n.axis[2] = axis[3 * i + 2];
        n.length = length[i];
        n.side = static_cast<NailSide>(side[i]);
        n.triangle = triangle[i];
        n.u = u[i];
        n.v = v[i];
    }
    const std::string *label = label_at(i);
    if (label != nullptr) {
        n.has_label = true;
        n.label = *label;
    }
    return n;
}

void check_nails(const NailTable &t, const std::string &entry, std::uint32_t layout_id, Report &report,
                 std::uint64_t triangle_count) {
    CappedReport out(report, entry, layout_id);
    const bool is3d = t.is_3d();
    for (std::size_t i = 0; i < t.size(); ++i) {
        const std::uint32_t id = t.id[i];
        out.finite(i, id, "x", t.x[i]);
        out.finite(i, id, "y", t.y[i]);
        if (out.finite(i, id, "diameter", t.diameter[i]) && !(t.diameter[i] > 0))
            out.add("schema.value", i, id, "diameter must be greater than 0");
        if (t.kind[i] > kNailInterior)
            out.add("schema.value", i, id, "kind must be boundary, exclusion_boundary or interior");
        if (!is3d)
            continue;
        out.finite(i, id, "z", t.z[i]);
        const double ax = t.axis[3 * i], ay = t.axis[3 * i + 1], az = t.axis[3 * i + 2];
        const bool fx = out.finite(i, id, "axis", ax), fy = out.finite(i, id, "axis", ay),
                   fz = out.finite(i, id, "axis", az);
        if (fx && fy && fz) {
            const double norm = std::sqrt(ax * ax + ay * ay + az * az);
            if (!(std::fabs(norm - 1.0) <= kAxisTolerance))
                out.add("schema.value", i, id, "axis must be a unit vector within 1e-6");
        }
        if (out.finite(i, id, "length", t.length[i]) && t.length[i] < 0)
            out.add("schema.value", i, id, "length must not be negative");
        if (t.side[i] > kSideInterior)
            out.add("schema.value", i, id, "side must be exterior or interior");
        const double fu = t.u[i], fv = t.v[i];
        const bool fin_u = out.finite(i, id, "face.u", fu), fin_v = out.finite(i, id, "face.v", fv);
        if (fin_u && fin_v) {
            if (fu < -kFaceTolerance || fv < -kFaceTolerance)
                out.add("schema.value", i, id, "face.u and face.v must be at least -1e-6");
            else if (fu + fv > 1.0 + kFaceTolerance)
                out.add("schema.value", i, id, "face.u + face.v must be at most 1 + 1e-6");
        }
        if (triangle_count != kAnyTriangleCount && t.triangle[i] >= triangle_count)
            out.add("schema.value", i, id, "face.triangle is not a triangle of the mesh");
    }
    for (std::size_t k = 0; k < t.label_text.size(); ++k)
        if (t.label_text[k].size() > kMaxLabelBytes)
            out.add("schema.value", t.label_index[k], t.id[t.label_index[k]], "label is longer than 64 bytes");

    std::vector<std::uint32_t> ids(t.id);
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 1; i < ids.size(); ++i)
        if (ids[i] == ids[i - 1] && (i == 1 || ids[i] != ids[i - 2]))
            out.add("nail.duplicate_id", kNoPosition, ids[i], "identifier used twice in the layout");
}

} // namespace model
} // namespace format
} // namespace loomora

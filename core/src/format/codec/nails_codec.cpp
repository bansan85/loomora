#include "format/codec/nails_codec.hpp"

#include <cmath>
#include <cstring>
#include <utility>

#include "format/json/json_out.hpp"
#include "loomora.h"

namespace loomora {
namespace format {
namespace codec {

using model::Nail;
using model::NailTable;

namespace {

enum Field {
    F_ID, F_X, F_Y, F_Z, F_DIAMETER, F_KIND, F_LABEL, F_AXIS, F_LENGTH, F_SIDE, F_FACE,
    F_TRIANGLE, F_U, F_V,
    F_NAILS, // the root member "nails"
    F_SKIP,  // an ignored member: its value is skipped
    F_NONE
};

const char *const kFieldNames[] = {"id", "x", "y", "z", "diameter", "kind", "label", "axis", "length", "side",
                                   "face", "face.triangle", "face.u", "face.v", "nails", "", ""};

inline std::uint32_t bit(Field f) { return static_cast<std::uint32_t>(1) << static_cast<unsigned>(f); }

const std::uint32_t kRequired2d = (1u << F_ID) | (1u << F_X) | (1u << F_Y) | (1u << F_DIAMETER) | (1u << F_KIND);
const std::uint32_t kRequired3d = kRequired2d | (1u << F_Z) | (1u << F_AXIS) | (1u << F_LENGTH) | (1u << F_SIDE) | (1u << F_FACE);
const std::uint32_t kRequiredFace = (1u << F_TRIANGLE) | (1u << F_U) | (1u << F_V);

enum Where { W_START, W_ROOT_KEYS, W_NAILS_ARRAY, W_NAIL_KEYS, W_AXIS, W_FACE_KEYS, W_DONE };

struct Num {
    double d;
    bool is_uint;
    std::uint64_t u;
};

bool name_is(const char *s, rapidjson::SizeType n, const char *lit) {
    const std::size_t len = std::strlen(lit);
    return n == len && std::memcmp(s, lit, len) == 0;
}

// The SAX handler of nails.json (full entry or a fragment inside an artificial array).
class NailsHandler {
  public:
    enum Mode { kFull, kRange };

    NailsHandler(NailTable &table, bool is_3d, Mode mode, std::size_t limit, const json::ReaderStream *stream)
        : table_(table), is_3d_(is_3d), mode_(mode), limit_(limit), stream_(stream),
          where_(W_START), skip_(0), pending_(F_NONE), seen_nails_(false),
          mask_(0), face_mask_(0), axis_n_(0), index_(0), done_(false), failed_(false) {}

    bool failed() const { return failed_; }
    bool done() const { return done_; }
    bool finished() const { return where_ == W_DONE; }
    const std::string &code() const { return code_; }
    const std::string &message() const { return message_; }
    std::size_t count() const { return index_; }

    // ---- scalars
    bool Null() { return other(); }
    bool Bool(bool) { return other(); }
    bool Int(int i) { return i >= 0 ? number(uint_num(static_cast<std::uint64_t>(i))) : number(dbl_num(static_cast<double>(i))); }
    bool Uint(unsigned u) { return number(uint_num(u)); }
    bool Int64(std::int64_t i) {
        return i >= 0 ? number(uint_num(static_cast<std::uint64_t>(i))) : number(dbl_num(static_cast<double>(i)));
    }
    bool Uint64(std::uint64_t u) { return number(uint_num(u)); }
    bool Double(double d) { return number(dbl_num(d)); }
    bool RawNumber(const char *, rapidjson::SizeType, bool) { return true; }

    bool String(const char *s, rapidjson::SizeType n, bool) {
        if (skip_ > 0)
            return true;
        if (pending_ == F_SKIP) {
            pending_ = F_NONE;
            return true;
        }
        if (where_ == W_NAIL_KEYS) {
            if (pending_ == F_KIND) {
                model::NailKind k;
                if (!model::parse_nail_kind(std::string(s, n), k))
                    return fail("schema.value", "kind must be boundary, exclusion_boundary or interior");
                nail_.kind = k;
                pending_ = F_NONE;
                return true;
            }
            if (pending_ == F_SIDE) {
                model::NailSide side;
                if (!model::parse_nail_side(std::string(s, n), side))
                    return fail("schema.value", "side must be exterior or interior");
                nail_.side = side;
                pending_ = F_NONE;
                return true;
            }
            if (pending_ == F_LABEL) {
                nail_.has_label = true;
                nail_.label.assign(s, n);
                pending_ = F_NONE;
                return true;
            }
        }
        return wrong_type();
    }

    // ---- structure
    bool StartObject() {
        if (skip_ > 0) {
            ++skip_;
            return true;
        }
        if (pending_ == F_SKIP) {
            pending_ = F_NONE;
            skip_ = 1;
            return true;
        }
        switch (where_) {
        case W_START:
            if (mode_ == kRange)
                break; // a fragment is parsed inside an array: an object here is wrong
            where_ = W_ROOT_KEYS;
            return true;
        case W_NAILS_ARRAY:
            nail_ = Nail();
            mask_ = 0;
            where_ = W_NAIL_KEYS;
            if (mode_ == kFull && index_ % model::kNailsPerIndexEntry == 0 && stream_ != nullptr)
                table_.offsets.push_back(static_cast<std::uint64_t>(stream_->Tell())); // the iterative parser raises the event before it takes the `{`
            return true;
        case W_NAIL_KEYS:
            if (pending_ == F_FACE) {
                where_ = W_FACE_KEYS;
                face_mask_ = 0;
                pending_ = F_NONE;
                return true;
            }
            break;
        default:
            break;
        }
        return wrong_type();
    }

    bool Key(const char *s, rapidjson::SizeType n, bool) {
        if (skip_ > 0)
            return true;
        switch (where_) {
        case W_ROOT_KEYS:
            if (name_is(s, n, "nails")) {
                if (seen_nails_)
                    return fail("schema.duplicate_key", "duplicate key \"nails\" in the root object");
                seen_nails_ = true;
                pending_ = F_NAILS;
            } else {
                pending_ = F_SKIP;
            }
            return true;
        case W_NAIL_KEYS:
            pending_ = nail_field(s, n);
            if (pending_ != F_SKIP) {
                if (mask_ & bit(pending_))
                    return duplicate(kFieldNames[pending_]);
                mask_ |= bit(pending_);
            }
            return true;
        case W_FACE_KEYS:
            if (name_is(s, n, "triangle"))
                pending_ = F_TRIANGLE;
            else if (name_is(s, n, "u"))
                pending_ = F_U;
            else if (name_is(s, n, "v"))
                pending_ = F_V;
            else
                pending_ = F_SKIP;
            if (pending_ != F_SKIP) {
                if (face_mask_ & bit(pending_))
                    return duplicate(kFieldNames[pending_]);
                face_mask_ |= bit(pending_);
            }
            return true;
        default:
            return fail("schema.syntax", "unexpected member name");
        }
    }

    bool EndObject(rapidjson::SizeType) {
        if (skip_ > 0) {
            --skip_;
            return true;
        }
        switch (where_) {
        case W_FACE_KEYS: {
            const std::uint32_t missing = kRequiredFace & ~face_mask_;
            if (missing != 0)
                return fail("schema.missing", "required member " + path() + "." + kFieldNames[first_bit(missing)] +
                                                  " is missing");
            where_ = W_NAIL_KEYS;
            return true;
        }
        case W_NAIL_KEYS: {
            const std::uint32_t missing = (is_3d_ ? kRequired3d : kRequired2d) & ~mask_;
            if (missing != 0)
                return fail("schema.missing", "required member " + path() + "." + kFieldNames[first_bit(missing)] +
                                                  " is missing");
            table_.add(nail_);
            ++index_;
            where_ = W_NAILS_ARRAY;
            if (mode_ == kRange && index_ >= limit_) {
                done_ = true;
                return false; // enough nails: stop
            }
            return true;
        }
        case W_ROOT_KEYS:
            if (!seen_nails_)
                return fail("schema.missing", "required member nails is missing");
            where_ = W_DONE;
            return true;
        default:
            return fail("schema.syntax", "unexpected end of object");
        }
    }

    bool StartArray() {
        if (skip_ > 0) {
            ++skip_;
            return true;
        }
        if (pending_ == F_SKIP) {
            pending_ = F_NONE;
            skip_ = 1;
            return true;
        }
        if (where_ == W_START && mode_ == kRange) {
            where_ = W_NAILS_ARRAY;
            return true;
        }
        if (where_ == W_ROOT_KEYS && pending_ == F_NAILS) {
            where_ = W_NAILS_ARRAY;
            pending_ = F_NONE;
            return true;
        }
        if (where_ == W_NAIL_KEYS && pending_ == F_AXIS) {
            where_ = W_AXIS;
            axis_n_ = 0;
            pending_ = F_NONE;
            return true;
        }
        return wrong_type();
    }

    bool EndArray(rapidjson::SizeType) {
        if (skip_ > 0) {
            --skip_;
            return true;
        }
        if (where_ == W_AXIS) {
            if (axis_n_ != 3)
                return fail("schema.value", path() + ".axis must hold exactly 3 numbers");
            where_ = W_NAIL_KEYS;
            return true;
        }
        if (where_ == W_NAILS_ARRAY) {
            if (mode_ == kRange) {
                done_ = true; // the end of the array: fewer nails than asked
                return false;
            }
            where_ = W_ROOT_KEYS;
            return true;
        }
        return fail("schema.syntax", "unexpected end of array");
    }

  private:
    static Num uint_num(std::uint64_t u) {
        Num n;
        n.d = static_cast<double>(u);
        n.is_uint = true;
        n.u = u;
        return n;
    }
    static Num dbl_num(double d) {
        Num n;
        n.d = d;
        n.is_uint = false;
        n.u = 0;
        return n;
    }
    static unsigned first_bit(std::uint32_t mask) {
        unsigned i = 0;
        while (((mask >> i) & 1u) == 0)
            ++i;
        return i;
    }

    std::string path() const { return "nails[" + json::dec(index_) + "]"; }

    Field nail_field(const char *s, rapidjson::SizeType n) const {
        if (name_is(s, n, "id")) return F_ID;
        if (name_is(s, n, "x")) return F_X;
        if (name_is(s, n, "y")) return F_Y;
        if (name_is(s, n, "diameter")) return F_DIAMETER;
        if (name_is(s, n, "kind")) return F_KIND;
        if (name_is(s, n, "label")) return F_LABEL;
        if (is_3d_) { // the 3D members are ignored in a 2D table
            if (name_is(s, n, "z")) return F_Z;
            if (name_is(s, n, "axis")) return F_AXIS;
            if (name_is(s, n, "length")) return F_LENGTH;
            if (name_is(s, n, "side")) return F_SIDE;
            if (name_is(s, n, "face")) return F_FACE;
        }
        return F_SKIP;
    }

    bool fail(const char *code, const std::string &message) {
        if (!failed_) {
            failed_ = true;
            code_ = code;
            message_ = message;
        }
        return false;
    }
    bool duplicate(const char *name) { return fail("schema.duplicate_key", "duplicate key \"" + std::string(name) + "\" in " + path()); }
    bool wrong_type() {
        if (where_ == W_AXIS)
            return fail("schema.type", path() + ".axis must hold numbers");
        if (where_ == W_NAIL_KEYS || where_ == W_FACE_KEYS)
            return fail("schema.type", path() + "." + kFieldNames[pending_ < F_NONE ? pending_ : F_ID] + " has the wrong type");
        if (where_ == W_NAILS_ARRAY)
            return fail("schema.type", path() + " must be an object");
        if (where_ == W_ROOT_KEYS)
            return fail("schema.type", "nails must be an array");
        return fail("schema.type", "the root of nails.json must be an object");
    }

    // A null or a boolean.
    bool other() {
        if (skip_ > 0)
            return true;
        if (pending_ == F_SKIP) {
            pending_ = F_NONE;
            return true;
        }
        return wrong_type();
    }

    bool as_u32(const Num &n, std::uint32_t &out) {
        if (n.is_uint) {
            if (n.u <= 4294967295ull) {
                out = static_cast<std::uint32_t>(n.u);
                return true;
            }
        } else if (n.d >= 0 && n.d <= 4294967295.0 && n.d == std::floor(n.d)) {
            out = static_cast<std::uint32_t>(n.d);
            return true;
        }
        return false;
    }

    bool number(const Num &n) {
        if (skip_ > 0)
            return true;
        if (pending_ == F_SKIP) {
            pending_ = F_NONE;
            return true;
        }
        if (where_ == W_AXIS) {
            if (axis_n_ >= 3)
                return fail("schema.value", path() + ".axis must hold exactly 3 numbers");
            nail_.axis[axis_n_++] = n.d;
            return true;
        }
        if (where_ != W_NAIL_KEYS && where_ != W_FACE_KEYS)
            return wrong_type();
        std::uint32_t u32 = 0;
        switch (pending_) {
        case F_ID:
        case F_TRIANGLE:
            if (!as_u32(n, u32))
                return fail("schema.value", path() + "." + kFieldNames[pending_] + " must be an integer from 0 to 4294967295");
            if (pending_ == F_ID)
                nail_.id = u32;
            else
                nail_.triangle = u32;
            break;
        case F_X: nail_.x = n.d; break;
        case F_Y: nail_.y = n.d; break;
        case F_Z: nail_.z = n.d; break;
        case F_DIAMETER: nail_.diameter = n.d; break;
        case F_LENGTH: nail_.length = n.d; break;
        case F_U: nail_.u = n.d; break;
        case F_V: nail_.v = n.d; break;
        default:
            return wrong_type();
        }
        pending_ = F_NONE;
        return true;
    }

    NailTable &table_;
    bool is_3d_;
    Mode mode_;
    std::size_t limit_;
    const json::ReaderStream *stream_;
    Where where_;
    unsigned skip_;
    Field pending_;
    bool seen_nails_;
    std::uint32_t mask_, face_mask_;
    unsigned axis_n_;
    std::size_t index_;
    bool done_, failed_;
    Nail nail_;
    std::string code_, message_;
};

} // namespace

void read_nails(json::ByteReader &in, bool is_3d, NailTable &out, const std::string &entry,
                std::uint64_t expected_count) {
    NailTable table(is_3d);
    json::ReaderStream stream(in);
    NailsHandler handler(table, is_3d, NailsHandler::kFull, 0, &stream);
    const bool ok = json::parse_sax_stream(stream, handler, entry);
    if (!ok) {
        if (handler.failed())
            json::fail(entry, handler.code().c_str(), handler.message());
        json::fail(entry, "schema.syntax", "unexpected end of nails.json");
    }
    if (!handler.finished())
        json::fail(entry, "schema.syntax", "nails.json is not complete");
    if (expected_count != kAnyNailCount && table.size() != expected_count)
        json::fail(entry, "schema.nail_count",
                   "nail_count is " + json::dec(expected_count) + " but nails.json holds " + json::dec(table.size()));
    out = std::move(table);
}

void read_nail_range(json::ByteReader &in, bool is_3d, std::size_t count, NailTable &out, const std::string &entry) {
    NailTable table(is_3d);
    if (count != 0) {
        json::PrefixByteReader prefixed('[', in);
        json::ReaderStream stream(prefixed);
        NailsHandler handler(table, is_3d, NailsHandler::kRange, count, &stream);
        const bool ok = json::parse_sax_stream(stream, handler, entry);
        if (!ok && handler.failed())
            json::fail(entry, handler.code().c_str(), handler.message());
        if (!ok && !handler.done())
            json::fail(entry, "schema.syntax", "unexpected end of the nails");
        if (ok)
            json::fail(entry, "schema.syntax", "the nails fragment ends inside the array");
    }
    out = std::move(table);
}

void write_nails(const NailTable &t, std::string &out) {
    out.reserve(out.size() + 32 + t.size() * (t.is_3d() ? 190 : 70));
    json::JsonWriter w(out);
    const bool is3d = t.is_3d();
    std::size_t next_label = 0;
    w.begin_object();
    w.key("nails");
    w.begin_array();
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t.kind[i] > model::kNailInterior || (is3d && t.side[i] > model::kSideInterior)) {
            Issue issue;
            issue.severity = kError;
            issue.code = "schema.value";
            issue.message = "nail " + json::dec(t.id[i]) + " has a kind or side out of range";
            throw FormatError(LOOMORA_ERR_INVALID_ITEM, issue);
        }
        w.begin_object();
        w.key("id");
        w.uint(t.id[i]);
        w.key("x");
        w.number(t.x[i]);
        w.key("y");
        w.number(t.y[i]);
        if (is3d) {
            w.key("z");
            w.number(t.z[i]);
        }
        w.key("diameter");
        w.number(t.diameter[i]);
        w.key("kind");
        w.string(model::to_string(static_cast<model::NailKind>(t.kind[i])));
        if (next_label < t.label_index.size() && t.label_index[next_label] == i) {
            w.key("label");
            w.string(t.label_text[next_label]);
            ++next_label;
        }
        if (is3d) {
            w.key("axis");
            w.begin_array();
            w.number(t.axis[3 * i]);
            w.number(t.axis[3 * i + 1]);
            w.number(t.axis[3 * i + 2]);
            w.end_array();
            w.key("length");
            w.number(t.length[i]);
            w.key("side");
            w.string(model::to_string(static_cast<model::NailSide>(t.side[i])));
            w.key("face");
            w.begin_object();
            w.key("triangle");
            w.uint(t.triangle[i]);
            w.key("u");
            w.number(t.u[i]);
            w.key("v");
            w.number(t.v[i]);
            w.end_object();
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

} // namespace codec
} // namespace format
} // namespace loomora

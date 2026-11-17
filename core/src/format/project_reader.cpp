#include "format/project_reader.hpp"

#include <algorithm>
#include <new>
#include <set>

#include "format/codec/mesh_codec.hpp"
#include "format/codec/nails_codec.hpp"
#include "format/io/progress.hpp"
#include "format/json/json_in.hpp"
#include "format/model/model_json.hpp"
#include "format/sha256.hpp"
#include "format/validate/validator.hpp"

namespace loomora {
namespace format {

namespace {

const std::uint64_t kManifestMaxBytes = 256ull * 1024 * 1024;
const std::uint64_t kItemJsonMaxBytes = 64ull * 1024 * 1024;
const char kManifestName[] = "manifest.json";

// ---- entry names ---------------------------------------------------------------------------

bool parse_id(const std::string &s, std::uint32_t &out) {
    if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0'))
        return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }
    if (v > 0xFFFFFFFFull)
        return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

bool ends_with_ext(const std::string &name, const char *stem, const char *const *exts, std::size_t n) {
    const std::string s(stem);
    if (name.compare(0, s.size(), s) != 0)
        return false;
    for (std::size_t i = 0; i < n; ++i)
        if (name.size() == s.size() + std::string(exts[i]).size() && name.compare(s.size(), std::string::npos, exts[i]) == 0)
            return true;
    return false;
}

std::string dec(std::uint64_t v) { return json::dec(v); }

std::string id_text(std::uint32_t id) { return dec(id); }

// ---- stream adapters ------------------------------------------------------------------------

class StreamByteReader : public json::ByteReader {
  public:
    explicit StreamByteReader(ZipEntryStream &s) : s_(s) {}
    std::size_t read(char *buf, std::size_t cap) override { return s_.read(buf, cap); }

  private:
    ZipEntryStream &s_;
};

void skip_stream(ZipEntryStream &s, std::uint64_t bytes) {
    char scratch[16 * 1024];
    while (bytes > 0) {
        const std::size_t want = bytes < sizeof scratch ? static_cast<std::size_t>(bytes) : sizeof scratch;
        const std::size_t n = s.read(scratch, want);
        if (n == 0)
            throw FormatError(LOOMORA_ERR_CORRUPT, "the entry ended before the requested offset");
        bytes -= n;
    }
}

[[noreturn]] void not_found(const std::string &what) {
    throw FormatError(LOOMORA_ERR_NOT_FOUND, what);
}

} // namespace

EntryClass classify_entry(const std::string &name) {
    EntryClass c;
    std::vector<std::string> p;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = name.find('/', start);
        if (slash == std::string::npos) {
            p.push_back(name.substr(start));
            break;
        }
        p.push_back(name.substr(start, slash - start));
        start = slash + 1;
    }
    static const char *const kSourceExts[] = {".png", ".jpg", ".jpeg", ".webp"};
    if (p.size() == 1) {
        if (name == kMimetypeName)
            c.role = kRoleMimetype;
        else if (name == kManifestName)
            c.role = kRoleManifest;
        else if (name == "spools.json")
            c.role = kRoleSpools;
    } else if (p.size() == 3 && p[0] == "frames" && parse_id(p[1], c.id)) {
        if (p[2] == "frame.json")
            c.role = kRoleFrameJson;
        else if (p[2] == "mesh.bin")
            c.role = kRoleMesh;
        else if (p[2].size() > 12 && p[2].compare(0, 12, "mesh-source.") == 0 &&
                 p[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 12) ==
                     std::string::npos)
            c.role = kRoleMeshSource;
    } else if (p.size() == 5 && p[0] == "frames" && p[2] == "layouts" && parse_id(p[1], c.parent) &&
               parse_id(p[3], c.id)) {
        if (p[4] == "layout.json")
            c.role = kRoleLayoutJson;
        else if (p[4] == "nails.json")
            c.role = kRoleNails;
    } else if (p.size() == 3 && p[0] == "images" && parse_id(p[1], c.id)) {
        if (p[2] == "image.json")
            c.role = kRoleImageJson;
        else if (ends_with_ext(p[2], "source", kSourceExts, 4))
            c.role = kRoleImageSource;
        else if (p[2] == "target.png")
            c.role = kRoleImageTarget;
        else if (p[2] == "thumb.png")
            c.role = kRoleImageThumb;
    } else if (p.size() == 3 && p[0] == "results" && parse_id(p[1], c.id)) {
        if (p[2] == "result.json")
            c.role = kRoleResultJson;
        else if (p[2] == "sequence.bin")
            c.role = kRoleSequence;
        else if (p[2] == "thumb.png")
            c.role = kRoleResultThumb;
        else if (p[2] == "settings.json")
            c.role = kRoleSettings;
    }
    if (c.role == kRoleUnknown) {
        c.id = 0;
        c.parent = 0;
    }
    return c;
}

// ---- EntrySource ----------------------------------------------------------------------------

EntrySource::EntrySource(const ZipReader &zip, const ZipEntry &entry) : zip_(zip), entry_(entry), pos_(0) {}
EntrySource::~EntrySource() {}

std::size_t EntrySource::read_at(std::uint64_t offset, void *buf, std::size_t len) const {
    if (offset >= entry_.size || len == 0)
        return 0;
    if (static_cast<std::uint64_t>(len) > entry_.size - offset)
        len = static_cast<std::size_t>(entry_.size - offset);
    if (entry_.method == 0)
        return zip_.read_range(entry_, offset, buf, len);
    if (!stream_ || offset < pos_) {
        stream_ = zip_.open_entry(entry_, nullptr);
        pos_ = 0;
    }
    if (offset > pos_) {
        skip_stream(*stream_, offset - pos_);
        pos_ = offset;
    }
    std::size_t total = 0;
    unsigned char *out = static_cast<unsigned char *>(buf);
    while (total < len) {
        const std::size_t n = stream_->read(out + total, len - total);
        if (n == 0)
            break;
        total += n;
        pos_ += n;
    }
    return total;
}

// ---- ProjectReader: accessors -----------------------------------------------------------------

ProjectReader::ProjectReader(std::unique_ptr<Source> source)
    : source_(std::move(source)), zip_(*source_), complete_(false) {}

ProjectReader::~ProjectReader() {}

const std::vector<std::uint32_t> &ProjectReader::ids(int kind) const {
    static const std::vector<std::uint32_t> kEmpty;
    switch (kind) {
    case LOOMORA_KIND_FRAME: return manifest_.order.frames;
    case LOOMORA_KIND_LAYOUT: return manifest_.order.layouts;
    case LOOMORA_KIND_IMAGE: return manifest_.order.images;
    case LOOMORA_KIND_SPOOL: return manifest_.order.spools;
    case LOOMORA_KIND_RESULT: return manifest_.order.results;
    default: return kEmpty;
    }
}

namespace {
template <typename Map>
const typename Map::mapped_type *find_in(const Map &m, std::uint32_t id) {
    const typename Map::const_iterator it = m.find(id);
    return it == m.end() ? nullptr : &it->second;
}
} // namespace

const FrameRecord *ProjectReader::frame(std::uint32_t id) const { return find_in(frames_, id); }
const LayoutRecord *ProjectReader::layout(std::uint32_t id) const { return find_in(layouts_, id); }
const ImageRecord *ProjectReader::image(std::uint32_t id) const { return find_in(images_, id); }
const SpoolRecord *ProjectReader::spool(std::uint32_t id) const { return find_in(spools_, id); }
const ResultRecord *ProjectReader::result(std::uint32_t id) const { return find_in(results_, id); }

std::unique_ptr<Source> ProjectReader::open_entry_source(const std::string &name) const {
    const ZipEntry *e = zip_.find(name);
    if (e == nullptr)
        not_found("no entry " + name);
    return std::unique_ptr<Source>(new EntrySource(zip_, *e));
}

std::vector<std::uint8_t> ProjectReader::read_item_json(int kind, std::uint32_t id) const {
    std::string entry;
    switch (kind) {
    case LOOMORA_KIND_FRAME:
        if (const FrameRecord *r = frame(id)) entry = r->entry;
        break;
    case LOOMORA_KIND_LAYOUT:
        if (const LayoutRecord *r = layout(id)) entry = r->entry;
        break;
    case LOOMORA_KIND_IMAGE:
        if (const ImageRecord *r = image(id)) entry = r->entry;
        break;
    case LOOMORA_KIND_RESULT:
        if (const ResultRecord *r = result(id)) entry = r->entry;
        break;
    case LOOMORA_KIND_SPOOL:
        if (const SpoolRecord *r = spool(id))
            return std::vector<std::uint8_t>(r->json.begin(), r->json.end());
        break;
    default:
        break;
    }
    const ZipEntry *e = entry.empty() ? nullptr : zip_.find(entry);
    if (e == nullptr)
        not_found("no such item");
    return zip_.read_all(*e, kItemJsonMaxBytes);
}

std::size_t ProjectReader::read_entry(const std::string &name, std::uint64_t offset, void *buf,
                                      std::size_t len) const {
    const ZipEntry *e = zip_.find(name);
    if (e == nullptr)
        not_found("no entry " + name);
    EntrySource src(zip_, *e);
    return src.read_at(offset, buf, len);
}

namespace {
bool layout_is_3d(const ProjectReader &r, const LayoutRecord &l) {
    const FrameRecord *f = r.frame(l.layout.frame);
    if (f == nullptr)
        f = r.frame(l.folder_frame);
    return f != nullptr && f->frame.kind == model::kFrame3D;
}
} // namespace

void ProjectReader::load_nails(std::uint32_t layout_id, model::NailTable &out) const {
    const LayoutRecord *l = layout(layout_id);
    const ZipEntry *e = (l == nullptr || l->nails_entry.empty()) ? nullptr : zip_.find(l->nails_entry);
    if (e == nullptr)
        not_found("no nails for this layout");
    const bool is3d = layout_is_3d(*this, *l);
    std::unique_ptr<ZipEntryStream> s = zip_.open_entry(*e, nullptr);
    StreamByteReader br(*s);
    model::NailTable t(is3d);
    codec::read_nails(br, is3d, t, e->name, codec::kAnyNailCount);
    out = std::move(t);
}

void ProjectReader::read_nails(std::uint32_t layout_id, std::uint64_t first, std::size_t count,
                               model::NailTable &out) const {
    const LayoutRecord *l = layout(layout_id);
    const ZipEntry *e = (l == nullptr || l->nails_entry.empty()) ? nullptr : zip_.find(l->nails_entry);
    if (e == nullptr)
        not_found("no nails for this layout");
    const bool is3d = layout_is_3d(*this, *l);
    out = model::NailTable(is3d);
    const std::uint64_t total = l->nails_parsed;
    if (first >= total || count == 0)
        return;
    if (static_cast<std::uint64_t>(count) > total - first)
        count = static_cast<std::size_t>(total - first);
    const std::uint64_t k = first / model::kNailsPerIndexEntry;
    if (k >= l->nail_offsets.size())
        throw FormatError(LOOMORA_ERR_INTERNAL, "the nail index is shorter than the nail count");
    const std::uint64_t head = first - k * model::kNailsPerIndexEntry;
    const std::uint64_t at = l->nail_offsets[static_cast<std::size_t>(k)];
    model::NailTable t(is3d);
    if (e->method == 0) {
        json::SourceByteReader br(zip_.source(), e->data_offset + at, e->size > at ? e->size - at : 0);
        codec::read_nail_range(br, is3d, static_cast<std::size_t>(head) + count, t, e->name);
    } else {
        std::unique_ptr<ZipEntryStream> s = zip_.open_entry(*e, nullptr);
        skip_stream(*s, at);
        StreamByteReader br(*s);
        codec::read_nail_range(br, is3d, static_cast<std::size_t>(head) + count, t, e->name);
    }
    out.reserve(count);
    for (std::size_t i = static_cast<std::size_t>(head); i < t.size(); ++i)
        out.add(t.get(i));
}

std::size_t ProjectReader::read_steps(std::uint32_t result_id, std::uint64_t first, std::size_t count,
                                      std::vector<SequenceStep> &out) const {
    out.clear();
    const ResultRecord *r = result(result_id);
    const ZipEntry *e = (r == nullptr || r->sequence_entry.empty()) ? nullptr : zip_.find(r->sequence_entry);
    if (e == nullptr)
        not_found("no sequence for this result");
    EntrySource src(zip_, *e);
    SequenceReader sr(src);
    SequenceStep st;
    for (std::uint64_t i = 0; i < first; ++i)
        if (!sr.next(&st))
            return 0;
    while (out.size() < count && sr.next(&st))
        out.push_back(st);
    return out.size();
}

// ---- the open pass ----------------------------------------------------------------------------

struct ReaderPass {
    ProjectReader &r;
    Report &report;
    loomora_progress_fn cb;
    void *user;
    std::set<std::string> bad;       // entries whose stream failed
    std::set<std::string> missing;   // entry.missing already reported
    std::map<std::string, const model::ManifestEntry *> listed;
    bool complete;
    std::uint64_t stream_total;

    ReaderPass(ProjectReader &reader, Report &rep, loomora_progress_fn c, void *u)
        : r(reader), report(rep), cb(c), user(u), complete(true), stream_total(0) {}

    void error(const char *code, const std::string &entry, const std::string &message,
               ItemKind kind = kItemNone, std::uint32_t id = 0) {
        report.add(kError, code, entry, message, kind, id);
    }

    // True when `e` is an error the pass recovers from (it is added to the report).
    bool recover(const FormatError &e, const char *fallback, const std::string &entry, ItemKind kind,
                 std::uint32_t id) {
        if (e.code() != LOOMORA_ERR_CORRUPT)
            return false;
        Issue is;
        if (e.has_issue()) {
            is = e.issue();
        } else {
            is.message = e.what();
        }
        is.severity = kError;
        if (is.code.empty())
            is.code = fallback;
        if (is.entry.empty())
            is.entry = entry;
        if (is.item_kind == kItemNone) {
            is.item_kind = kind;
            is.item_id = kind == kItemNone ? 0 : id;
        }
        report.add(is);
        return true;
    }

    void report_missing(const std::string &name) {
        if (missing.insert(name).second)
            error("entry.missing", name, "the entry " + name + " is not in the archive");
        complete = false;
    }

    const ZipEntry *usable(const std::string &name) {
        const ZipEntry *e = r.zip_.find(name);
        return (e == nullptr || bad.count(name) != 0) ? nullptr : e;
    }

    // Parses the small JSON entry `e`; false (and an issue) when it cannot be read or is not JSON.
    bool load_json(const ZipEntry &e, std::uint64_t max, json::Document &doc, ItemKind kind, std::uint32_t id) {
        try {
            const std::vector<std::uint8_t> bytes = r.zip_.read_all(e, max);
            json::parse_document(reinterpret_cast<const char *>(bytes.data()), bytes.size(), doc, e.name);
            return true;
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.syntax", e.name, kind, id))
                throw;
            complete = false;
            return false;
        }
    }

    // ---- step 2: manifest ----------------------------------------------------------------------
    void read_manifest();
    void check_manifest_fields();
    // ---- step 3: entries -------------------------------------------------------------------------
    void check_entries();
    // ---- step 4 and 5: items -----------------------------------------------------------------------
    void check_free_json();
    void load_spools();
    void load_frames();
    void load_layouts();
    void load_images();
    void load_results();
    void verify_mesh(FrameRecord &fr);
    void verify_sequence(ResultRecord &rr);
    // ---- step 6: the manifest against the content -------------------------------------------------
    void check_content();
};

namespace {

bool peek_versions(const json::Value &root, std::uint32_t &major, std::uint32_t &minor,
                   model::Version &min_reader, bool &have_min) {
    major = minor = 0;
    have_min = false;
    if (!root.IsObject())
        return false;
    json::Value::ConstMemberIterator f = root.FindMember("format");
    if (f == root.MemberEnd() || !f->value.IsObject())
        return false;
    json::Value::ConstMemberIterator ma = f->value.FindMember("major");
    json::Value::ConstMemberIterator mi = f->value.FindMember("minor");
    if (ma == f->value.MemberEnd() || !ma->value.IsUint())
        return false;
    major = ma->value.GetUint();
    minor = (mi != f->value.MemberEnd() && mi->value.IsUint()) ? mi->value.GetUint() : 0;
    json::Value::ConstMemberIterator mr = root.FindMember("min_reader");
    if (mr != root.MemberEnd() && mr->value.IsString())
        have_min = model::parse_version(std::string(mr->value.GetString(), mr->value.GetStringLength()), min_reader);
    return true;
}

} // namespace

void ReaderPass::read_manifest() {
    const ZipEntry *me = r.zip_.find(kManifestName);
    if (me == nullptr) {
        error("manifest.missing", kManifestName, "the archive has no manifest.json");
        throw FormatError(LOOMORA_ERR_CORRUPT, "no manifest.json");
    }
    json::Document doc;
    try {
        const std::vector<std::uint8_t> bytes = r.zip_.read_all(*me, kManifestMaxBytes);
        r.manifest_json_.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        json::parse_document(reinterpret_cast<const char *>(bytes.data()), bytes.size(), doc, kManifestName);
    } catch (const FormatError &ex) {
        if (!recover(ex, "schema.syntax", kManifestName, kItemNone, 0))
            throw;
        throw FormatError(LOOMORA_ERR_CORRUPT, "manifest.json cannot be read");
    }

    // The version rule comes before the strict parse: a newer file may be shaped differently.
    std::uint32_t major = 0, minor = 0;
    model::Version min_reader;
    bool have_min = false;
    if (peek_versions(doc, major, minor, min_reader, have_min)) {
        model::Version lib;
        model::Version format;
        format.major = major;
        format.minor = minor;
        const bool inconsistent = have_min && model::version_above(min_reader, format);
        const bool too_new = major > model::kFormatMajor || (have_min && model::version_above(min_reader, lib));
        if (too_new && !inconsistent) {
            const model::Version need = (have_min && model::version_above(min_reader, lib)) ? min_reader : format;
            Issue is;
            is.severity = kError;
            is.code = "version.major";
            is.entry = kManifestName;
            is.message = "This file needs Loomora format version " + model::version_text(need) + " or newer";
            throw FormatError(LOOMORA_ERR_UNSUPPORTED_VERSION, is);
        }
    }

    try {
        model::parse_manifest(doc, r.manifest_, kManifestName);
    } catch (const FormatError &ex) {
        if (!recover(ex, "schema.type", kManifestName, kItemNone, 0))
            throw;
        throw FormatError(LOOMORA_ERR_CORRUPT, "manifest.json is not valid");
    }

    for (std::size_t i = 0; i < r.manifest_.required_features.size(); ++i) {
        const std::string &f = r.manifest_.required_features[i];
        if (f != model::kFeatureSurfaceMesh && f != model::kFeatureStringLevels &&
            f != model::kFeatureMultiViewTarget) {
            Issue is;
            is.severity = kError;
            is.code = "feature.unsupported";
            is.entry = kManifestName;
            is.message = "This file needs the feature \"" + f + "\", which this version of Loomora does not support";
            throw FormatError(LOOMORA_ERR_UNSUPPORTED_FEATURE, is);
        }
    }
    for (std::size_t i = 0; i < r.manifest_.entries.size(); ++i)
        listed[r.manifest_.entries[i].path] = &r.manifest_.entries[i];
}

void ReaderPass::check_manifest_fields() {
    const model::Manifest &m = r.manifest_;
    const std::string e = kManifestName;
    if (m.format.name != "loomora")
        error("manifest.value", e, "format.name must be loomora");
    model::Version format;
    format.major = m.format.major;
    format.minor = m.format.minor;
    if (model::version_above(m.min_reader, format))
        error("manifest.min_reader", e, "min_reader is above the format version");
    if (!model::is_uuid4(m.project_id))
        error("manifest.value", e, "project_id must be a lowercase UUID v4");
    if (m.title.size() > model::kMaxTitleBytes)
        error("manifest.value", e, "title is longer than 1,024 bytes");
    if (!model::is_iso8601_utc(m.created_at))
        error("manifest.value", e, "created_at must be an ISO 8601 UTC time");
    if (!model::is_iso8601_utc(m.modified_at))
        error("manifest.value", e, "modified_at must be an ISO 8601 UTC time");
    if ((m.profile == model::kProfileLight) == m.editable)
        error("manifest.profile", e, "editable must be false for a light project and true for a full one");
    {
        std::vector<std::string> f(m.required_features);
        std::sort(f.begin(), f.end());
        if (std::adjacent_find(f.begin(), f.end()) != f.end())
            error("manifest.features", e, "required_features repeats a feature");
    }
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
        const model::ManifestEntry &me = m.entries[i];
        if (!model::is_sha256_hex(me.sha256))
            error("manifest.entries", e, "entries[" + me.path + "].sha256 must be 64 lowercase hex characters");
        if (me.path == kMimetypeName || me.path == kManifestName)
            error("manifest.entries", e, "entries must not list " + me.path);
    }
}

void ReaderPass::check_entries() {
    Progress progress(cb, user, stream_total);
    std::vector<unsigned char> buf(64 * 1024);
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    std::set<std::string> in_archive;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        in_archive.insert(e.name);
        if (e.name == kMimetypeName || e.name == kManifestName)
            continue;
        const std::map<std::string, const model::ManifestEntry *>::const_iterator li = listed.find(e.name);
        const model::ManifestEntry *me = li == listed.end() ? nullptr : li->second;
        if (classify_entry(e.name).role == kRoleUnknown)
            report.add(kWarning, "warn.unknown_entry", e.name,
                       "the entry " + e.name + " is not defined by this version of the format; it is kept");
        if (me == nullptr) {
            error("entry.unlisted", e.name, "the entry is in the archive but not in the manifest");
            complete = false;
        }
        try {
            std::unique_ptr<ZipEntryStream> s = r.zip_.open_entry(e, &progress);
            Sha256 h;
            std::uint64_t total = 0;
            for (;;) {
                const std::size_t n = s->read(buf.data(), buf.size());
                if (n == 0)
                    break;
                h.update(buf.data(), n);
                total += n;
            }
            if (me != nullptr) {
                if (total != me->size) {
                    error("entry.size", e.name, "the manifest says " + dec(me->size) + " bytes, the entry has " + dec(total));
                    complete = false;
                }
                const std::string hex = h.hex_digest();
                if (hex != me->sha256) {
                    error("entry.sha256", e.name, "SHA-256 mismatch: expected " + me->sha256 + ", found " + hex);
                    complete = false;
                }
            }
        } catch (const FormatError &ex) {
            if (!recover(ex, "entry.crc", e.name, kItemNone, 0))
                throw;
            bad.insert(e.name);
            complete = false;
        }
    }
    for (std::size_t i = 0; i < r.manifest_.entries.size(); ++i) {
        const std::string &path = r.manifest_.entries[i].path;
        if (in_archive.count(path) == 0)
            report_missing(path);
    }
    progress.finish();
}

namespace {
// Accepts every event: used to check the syntax and the depth of a JSON entry nobody interprets.
struct NullHandler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, NullHandler> {};
} // namespace

// settings.json is a free JSON object, but section 4 still rejects bad syntax, depth over 64 and non-finite numbers in every JSON entry.
void ReaderPass::check_free_json() {
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const EntryClass c = classify_entry(e.name);
        if (c.role != kRoleSettings || bad.count(e.name) != 0)
            continue;
        try {
            std::unique_ptr<ZipEntryStream> s = r.zip_.open_entry(e, nullptr);
            StreamByteReader br(*s);
            NullHandler h;
            json::parse_sax(br, h, e.name);
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.syntax", e.name, kItemResult, c.id))
                throw;
            complete = false;
        }
    }
}

void ReaderPass::load_spools() {
    const ZipEntry *e = usable("spools.json");
    if (e == nullptr) {
        if (bad.count("spools.json") == 0)
            report_missing("spools.json");
        complete = false;
        return;
    }
    json::Document doc;
    if (!load_json(*e, kItemJsonMaxBytes, doc, kItemSpool, 0))
        return;
    const std::size_t errors = report.error_count();
    try {
        std::vector<model::Spool> list;
        std::vector<std::string> texts;
        model::parse_spools(doc, list, &texts, e->name);
        model::check_spools(list, e->name, report);
        for (std::size_t i = 0; i < list.size(); ++i) {
            SpoolRecord rec;
            rec.spool = list[i];
            rec.json = texts[i];
            r.spools_.insert(std::make_pair(list[i].id, rec));
        }
    } catch (const FormatError &ex) {
        if (!recover(ex, "schema.type", e->name, kItemSpool, 0))
            throw;
    }
    if (report.error_count() != errors)
        complete = false;
}

void ReaderPass::load_frames() {
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const EntryClass c = classify_entry(e.name);
        if (c.role != kRoleFrameJson || bad.count(e.name) != 0)
            continue;
        json::Document doc;
        if (!load_json(e, kItemJsonMaxBytes, doc, kItemFrame, c.id))
            continue;
        const std::size_t errors = report.error_count();
        try {
            FrameRecord rec;
            model::parse_frame(doc, rec.frame, e.name);
            rec.entry = e.name;
            if (rec.frame.id != c.id)
                error("schema.value", e.name, "id " + id_text(rec.frame.id) + " differs from the folder " + id_text(c.id),
                      kItemFrame, c.id);
            model::check_frame(rec.frame, e.name, report);
            r.frames_.insert(std::make_pair(c.id, rec));
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.type", e.name, kItemFrame, c.id))
                throw;
        }
        if (report.error_count() != errors)
            complete = false;
    }
    for (std::map<std::uint32_t, FrameRecord>::iterator it = r.frames_.begin(); it != r.frames_.end(); ++it) {
        FrameRecord &fr = it->second;
        if (!fr.frame.has_mesh)
            continue;
        const std::string name = "frames/" + id_text(it->first) + "/mesh.bin";
        if (r.zip_.find(name) == nullptr) {
            report_missing(name);
            continue;
        }
        fr.mesh_entry = name;
        if (bad.count(name) == 0)
            verify_mesh(fr);
    }
}

void ReaderPass::verify_mesh(FrameRecord &fr) {
    const ZipEntry *e = r.zip_.find(fr.mesh_entry);
    const std::size_t errors = report.error_count();
    try {
        EntrySource src(r.zip_, *e);
        MeshReader mr(src);
        mr.check_counts(fr.frame.mesh.vertices, fr.frame.mesh.triangles, fr.frame.mesh.has_normals,
                        fr.frame.mesh.has_uvs);
        mr.verify();
    } catch (const FormatError &ex) {
        if (!recover(ex, "mesh.header", fr.mesh_entry, kItemFrame, fr.frame.id))
            throw;
    }
    if (report.error_count() != errors)
        complete = false;
}

void ReaderPass::load_layouts() {
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const EntryClass c = classify_entry(e.name);
        if (c.role != kRoleLayoutJson || bad.count(e.name) != 0)
            continue;
        json::Document doc;
        if (!load_json(e, kItemJsonMaxBytes, doc, kItemLayout, c.id))
            continue;
        const std::size_t errors = report.error_count();
        try {
            LayoutRecord rec;
            model::parse_layout(doc, rec.layout, e.name);
            rec.entry = e.name;
            rec.folder_frame = c.parent;
            if (rec.layout.id != c.id)
                error("schema.value", e.name, "id " + id_text(rec.layout.id) + " differs from the folder " + id_text(c.id),
                      kItemLayout, c.id);
            model::check_layout(rec.layout, e.name, report);
            const std::string nails = "frames/" + id_text(c.parent) + "/layouts/" + id_text(c.id) + "/nails.json";
            if (r.zip_.find(nails) == nullptr) {
                report_missing(nails);
            } else {
                rec.nails_entry = nails;
                if (bad.count(nails) == 0) {
                    const FrameRecord *fr = r.frame(rec.layout.frame);
                    if (fr == nullptr)
                        fr = r.frame(c.parent);
                    const bool is3d = fr != nullptr && fr->frame.kind == model::kFrame3D;
                    // face.triangle against the mesh is a rule of the validator (nail.outside_frame)
                    const std::uint64_t triangles = model::kAnyTriangleCount;
                    const ZipEntry *ne = r.zip_.find(nails);
                    try {
                        std::unique_ptr<ZipEntryStream> s = r.zip_.open_entry(*ne, nullptr);
                        StreamByteReader br(*s);
                        model::NailTable t(is3d);
                        codec::read_nails(br, is3d, t, nails, rec.layout.nail_count);
                        rec.nail_offsets.swap(t.offsets);
                        rec.nails_parsed = t.size();
                        model::check_nails(t, nails, rec.layout.id, report, triangles);
                    } catch (const FormatError &ex) {
                        if (!recover(ex, "schema.nail_count", nails, kItemLayout, c.id))
                            throw;
                    }
                }
            }
            r.layouts_.insert(std::make_pair(c.id, rec));
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.type", e.name, kItemLayout, c.id))
                throw;
        }
        if (report.error_count() != errors)
            complete = false;
    }
}

void ReaderPass::load_images() {
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const EntryClass c = classify_entry(e.name);
        if (c.role != kRoleImageJson || bad.count(e.name) != 0)
            continue;
        json::Document doc;
        if (!load_json(e, kItemJsonMaxBytes, doc, kItemImage, c.id))
            continue;
        const std::size_t errors = report.error_count();
        try {
            ImageRecord rec;
            model::parse_image(doc, rec.image, e.name);
            rec.entry = e.name;
            if (rec.image.id != c.id)
                error("schema.value", e.name, "id " + id_text(rec.image.id) + " differs from the folder " + id_text(c.id),
                      kItemImage, c.id);
            model::check_image(rec.image, r.manifest_.profile, e.name, report);
            r.images_.insert(std::make_pair(c.id, rec));
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.type", e.name, kItemImage, c.id))
                throw;
        }
        if (report.error_count() != errors)
            complete = false;
    }
}

void ReaderPass::load_results() {
    const std::vector<ZipEntry> &entries = r.zip_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry &e = entries[i];
        const EntryClass c = classify_entry(e.name);
        if (c.role != kRoleResultJson || bad.count(e.name) != 0)
            continue;
        json::Document doc;
        if (!load_json(e, kItemJsonMaxBytes, doc, kItemResult, c.id))
            continue;
        const std::size_t errors = report.error_count();
        try {
            ResultRecord rec;
            model::parse_result(doc, rec.result, e.name);
            rec.entry = e.name;
            if (rec.result.id != c.id)
                error("schema.value", e.name, "id " + id_text(rec.result.id) + " differs from the folder " + id_text(c.id),
                      kItemResult, c.id);
            const FrameRecord *fr = r.frame(rec.result.frame);
            bool is3d = false;
            if (fr != nullptr) {
                is3d = fr->frame.kind == model::kFrame3D;
            } else {
                for (std::size_t k = 0; k < rec.result.images.size(); ++k)
                    is3d = is3d || rec.result.images[k].has_view;
            }
            model::check_result(rec.result, is3d ? model::kFrame3D : model::kFrame2D, r.manifest_.profile, e.name,
                                report);
            const std::string seq = "results/" + id_text(c.id) + "/sequence.bin";
            if (r.zip_.find(seq) == nullptr) {
                report_missing(seq);
            } else {
                rec.sequence_entry = seq;
            }
            std::map<std::uint32_t, ResultRecord>::iterator it =
                r.results_.insert(std::make_pair(c.id, rec)).first;
            if (!it->second.sequence_entry.empty() && bad.count(seq) == 0)
                verify_sequence(it->second);
        } catch (const FormatError &ex) {
            if (!recover(ex, "schema.type", e.name, kItemResult, c.id))
                throw;
        }
        if (report.error_count() != errors)
            complete = false;
    }
}

void ReaderPass::verify_sequence(ResultRecord &rr) {
    const ZipEntry *e = r.zip_.find(rr.sequence_entry);
    try {
        EntrySource src(r.zip_, *e);
        SequenceReader sr(src);
        if (sr.count() != rr.result.steps)
            error("seq.count", rr.sequence_entry,
                  "the sequence holds " + dec(sr.count()) + " records but result.json says steps = " + dec(rr.result.steps),
                  kItemResult, rr.result.id);
        if (sr.has_levels() != rr.result.levels)
            error("seq.header", rr.sequence_entry, "the levels flag of the header differs from result.json",
                  kItemResult, rr.result.id);
        SequenceStep st;
        while (sr.next(&st)) {
        }
        sr.finish();
    } catch (const FormatError &ex) {
        if (!recover(ex, "seq.header", rr.sequence_entry, kItemResult, rr.result.id))
            throw;
    }
}

void ReaderPass::check_content() {
    const model::Manifest &m = r.manifest_;
    const std::string e = kManifestName;
    struct Row {
        const char *name;
        const std::vector<std::uint32_t> *order;
        std::uint64_t count;
        std::uint32_t next;
        std::vector<std::uint32_t> found;
    };
    Row rows[5];
    const char *const names[5] = {"frames", "layouts", "images", "spools", "results"};
    const std::vector<std::uint32_t> *orders[5] = {&m.order.frames, &m.order.layouts, &m.order.images,
                                                   &m.order.spools, &m.order.results};
    const std::uint64_t counts[5] = {m.counts.frames, m.counts.layouts, m.counts.images, m.counts.spools,
                                     m.counts.results};
    const std::uint32_t nexts[5] = {m.next_id.frame, m.next_id.layout, m.next_id.image, m.next_id.spool,
                                    m.next_id.result};
    for (int k = 0; k < 5; ++k) {
        rows[k].name = names[k];
        rows[k].order = orders[k];
        rows[k].count = counts[k];
        rows[k].next = nexts[k];
    }
    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = r.frames_.begin(); it != r.frames_.end(); ++it)
        rows[0].found.push_back(it->first);
    for (std::map<std::uint32_t, LayoutRecord>::const_iterator it = r.layouts_.begin(); it != r.layouts_.end(); ++it)
        rows[1].found.push_back(it->first);
    for (std::map<std::uint32_t, ImageRecord>::const_iterator it = r.images_.begin(); it != r.images_.end(); ++it)
        rows[2].found.push_back(it->first);
    for (std::map<std::uint32_t, SpoolRecord>::const_iterator it = r.spools_.begin(); it != r.spools_.end(); ++it)
        rows[3].found.push_back(it->first);
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = r.results_.begin(); it != r.results_.end(); ++it)
        rows[4].found.push_back(it->first);
    for (int k = 0; k < 5; ++k) {
        const Row &row = rows[k];
        const std::string n = row.name;
        if (row.count != row.found.size())
            error("manifest.counts", e, "counts." + n + " is " + dec(row.count) + " but the project holds " + dec(row.found.size()));
        std::vector<std::uint32_t> sorted(*row.order);
        std::sort(sorted.begin(), sorted.end());
        if (sorted != row.found)
            error("manifest.order", e, "order." + n + " is not a permutation of the items of the project");
        std::uint32_t top = 0;
        bool any = false;
        if (!sorted.empty()) {
            top = sorted.back();
            any = true;
        }
        if (!row.found.empty() && (!any || row.found.back() > top)) {
            top = row.found.back();
            any = true;
        }
        if (any && row.next <= top)
            error("manifest.next_id", e, "next_id of " + n.substr(0, n.size() - 1) + " is not above the identifier " + id_text(top));
    }

    bool mesh = false, levels = false, multi = false;
    for (std::map<std::uint32_t, FrameRecord>::const_iterator it = r.frames_.begin(); it != r.frames_.end(); ++it)
        mesh = mesh || (it->second.frame.kind == model::kFrame3D && it->second.frame.has_mesh);
    for (std::map<std::uint32_t, ResultRecord>::const_iterator it = r.results_.begin(); it != r.results_.end(); ++it) {
        levels = levels || it->second.result.levels;
        multi = multi || it->second.result.images.size() > 1;
    }
    const bool want[3] = {mesh, levels, multi};
    const char *const feature[3] = {model::kFeatureSurfaceMesh, model::kFeatureStringLevels,
                                    model::kFeatureMultiViewTarget};
    for (int k = 0; k < 3; ++k) {
        const bool have = std::find(m.required_features.begin(), m.required_features.end(), std::string(feature[k])) !=
                          m.required_features.end();
        if (have != want[k])
            error("manifest.features", e,
                  std::string("required_features ") + (want[k] ? "lacks " : "lists ") + feature[k] +
                      (want[k] ? ", which the content needs" : ", which the content does not need"));
    }
}

int ProjectReader::open(std::unique_ptr<Source> source, loomora_progress_fn progress, void *user,
                        std::unique_ptr<ProjectReader> *out, Report &report) {
    out->reset();
    std::unique_ptr<ProjectReader> reader(new ProjectReader(std::move(source)));
    int code = 0;
    try {
        ReaderPass pass(*reader, report, progress, user);
        {
            Progress p(progress, user, reader->source_->size());
            reader->zip_.open(&p);
        }
        pass.read_manifest();
        pass.check_manifest_fields();
        for (std::size_t i = 0; i < reader->zip_.entries().size(); ++i)
            pass.stream_total += reader->zip_.entries()[i].size;
        pass.check_entries();
        pass.check_free_json();
        pass.load_spools();
        pass.load_frames();
        pass.load_layouts();
        pass.load_images();
        pass.load_results();
        pass.check_content();
        if (reader->manifest_.profile == model::kProfileLight)
            report.add(kWarning, "warn.light_project", kManifestName,
                       "this is a light copy: it has no source images and no settings and cannot be edited");
        reader->complete_ = pass.complete;
        validate_semantic(*reader, report); // every rule that the records already read allow
    } catch (const FormatError &e) {
        code = e.code();
        if (e.has_issue())
            report.add(e.issue());
        else if (code == LOOMORA_ERR_CORRUPT && !report.has_errors())
            report.add(kError, "zip.central", "", e.what());
    } catch (const std::bad_alloc &) {
        code = LOOMORA_ERR_NO_MEMORY;
    }
    if (code == 0 && report.has_errors())
        code = LOOMORA_ERR_CORRUPT;
    if (code == 0)
        *out = std::move(reader);
    return code;
}

} // namespace format
} // namespace loomora

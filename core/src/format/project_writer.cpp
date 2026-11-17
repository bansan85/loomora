#include "format/project_writer.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "format/codec/nails_codec.hpp"
#include "format/io/file.hpp"
#include "format/io/memory_io.hpp"
#include "format/io/progress.hpp"
#include "format/item_json.hpp"
#include "format/limits.hpp"
#include "format/sha256.hpp"
#include "format/zip/zip_writer.hpp"

namespace loomora {
namespace format {

namespace {

const std::size_t kNailsPerChunk = 2048;

std::string dec(std::uint64_t value) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

// Where an entry's bytes go: the first pass hashes them, the second writes them.
class Emitter {
  public:
    virtual ~Emitter() {}
    virtual void put(const void *data, std::size_t len) = 0;
};

class HashEmitter : public Emitter {
  public:
    HashEmitter() : size_(0) {}
    void put(const void *data, std::size_t len) override {
        hash_.update(data, len);
        size_ += len;
    }
    std::uint64_t size() const { return size_; }
    std::string hex() { return hash_.hex_digest(); }

  private:
    Sha256 hash_;
    std::uint64_t size_;
};

class ZipEmitter : public Emitter {
  public:
    explicit ZipEmitter(ZipWriter &zip) : zip_(zip) {}
    void put(const void *data, std::size_t len) override { zip_.write(data, len); }

  private:
    ZipWriter &zip_;
};

// kArchive: an entry of the old archive, copied as it is (the editor's "kept" parts).
enum Content { kText, kBlob, kNails, kArchive };

// One entry of the archive, in canonical order.
struct Planned {
    std::string name;
    Content content;
    std::string text;                       // kText
    const std::vector<std::uint8_t> *blob;  // kBlob
    const model::NailTable *nails;          // kNails
    std::string archive_name;               // kArchive: the entry of the old archive
    std::uint64_t size;
    std::string sha256;

    Planned() : content(kText), blob(nullptr), nails(nullptr), size(0) {}
};

// nails.json in pieces: `{"nails":[` + the nails of each chunk, comma separated, + `]}`. The text of
// a chunk is what write_nails gives for a small table, without its `{"nails":[` prefix (10 bytes)
// and `]}` suffix (2 bytes).
void emit_nails(const model::NailTable &t, Emitter &out) {
    out.put("{\"nails\":[", 10);
    std::string text;
    for (std::size_t first = 0; first < t.size(); first += kNailsPerChunk) {
        const std::size_t last = first + kNailsPerChunk < t.size() ? first + kNailsPerChunk : t.size();
        model::NailTable part(t.is_3d());
        part.reserve(last - first);
        for (std::size_t i = first; i < last; ++i)
            part.add(t.get(i));
        text.clear();
        codec::write_nails(part, text);
        if (first != 0)
            out.put(",", 1);
        out.put(text.data() + 10, text.size() - 12);
    }
    out.put("]}", 2);
}

// The uncompressed bytes of an entry of the old archive, in pieces.
void emit_archive(const ProjectReader &base, const std::string &name, Emitter &out) {
    std::unique_ptr<Source> src = base.open_entry_source(name);
    std::vector<std::uint8_t> buf(64 * 1024);
    std::uint64_t at = 0;
    const std::uint64_t total = src->size();
    while (at < total) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), total - at));
        const std::size_t got = src->read_at(at, &buf[0], want);
        if (got == 0)
            throw FormatError(LOOMORA_ERR_IO, "the old archive ended early while it was copied");
        out.put(&buf[0], got);
        at += got;
    }
}

void produce(const ProjectReader *base, const Planned &p, Emitter &out) {
    switch (p.content) {
    case kText:
        out.put(p.text.data(), p.text.size());
        break;
    case kBlob:
        out.put(p.blob->data(), p.blob->size());
        break;
    case kNails:
        emit_nails(*p.nails, out);
        break;
    case kArchive:
        emit_archive(*base, p.archive_name, out);
        break;
    }
}

bool ends_with(const std::string &s, const char *suffix) {
    const std::size_t n = std::string(suffix).size();
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::uint16_t method_of(const std::string &name, bool store_all) {
    return (!store_all && ends_with(name, ".json")) ? 8 : 0;
}

void add_text(std::vector<Planned> &plan, const std::string &name, const std::string &text) {
    Planned p;
    p.name = name;
    p.content = kText;
    p.text = text;
    plan.push_back(p);
}

void add_blob(std::vector<Planned> &plan, const std::string &name, const std::vector<std::uint8_t> &blob) {
    Planned p;
    p.name = name;
    p.content = kBlob;
    p.blob = &blob;
    plan.push_back(p);
}

void add_archive(std::vector<Planned> &plan, const std::string &name, const std::string &archive_name) {
    Planned p;
    p.name = name;
    p.content = kArchive;
    p.archive_name = archive_name;
    plan.push_back(p);
}

// A piece of bulk data: the untouched old entry or the staged bytes.
void add_blob(std::vector<Planned> &plan, const std::string &name, const Blob &blob) {
    if (blob.in_archive())
        add_archive(plan, name, blob.from_archive);
    else
        add_blob(plan, name, blob.bytes);
}

// The entries in the canonical order of project-format.md section 2 (after mimetype and manifest).
void build_plan(const ProjectEditor &e, std::vector<Planned> &plan) {
    if (e.spools_kept())
        add_archive(plan, "spools.json", "spools.json");
    else
        add_text(plan, "spools.json", spools_json(e.spools_in_order()));

    std::map<std::uint32_t, std::vector<std::uint32_t> > layouts_of;
    for (std::map<std::uint32_t, StagedLayout>::const_iterator it = e.layouts().begin(); it != e.layouts().end(); ++it)
        layouts_of[it->second.layout.frame].push_back(it->first);

    for (std::map<std::uint32_t, StagedFrame>::const_iterator fit = e.frames().begin(); fit != e.frames().end();
         ++fit) {
        const std::uint32_t fid = fit->first;
        const StagedFrame &sf = fit->second;
        if (sf.json_kept)
            add_archive(plan, frame_entry(fid), frame_entry(fid));
        else
            add_text(plan, frame_entry(fid), frame_json(sf.frame));
        if (sf.frame.has_mesh) {
            add_blob(plan, mesh_entry_name(fid), sf.mesh);
            if (!sf.frame.mesh.attached_source.empty())
                add_blob(plan, sf.frame.mesh.attached_source, sf.attached);
        }
        const std::vector<std::uint32_t> &ids = layouts_of[fid];
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const StagedLayout &l = e.layouts().find(ids[i])->second;
            if (l.json_kept)
                add_archive(plan, layout_entry(fid, ids[i]), layout_entry(fid, ids[i]));
            else
                add_text(plan, layout_entry(fid, ids[i]), layout_json(l.layout));
            if (l.nails_kept) {
                add_archive(plan, nails_entry(fid, ids[i]), nails_entry(fid, ids[i]));
            } else {
                Planned p;
                p.name = nails_entry(fid, ids[i]);
                p.content = kNails;
                p.nails = &l.nails;
                plan.push_back(p);
            }
        }
    }
    for (std::map<std::uint32_t, StagedImage>::const_iterator it = e.images().begin(); it != e.images().end(); ++it) {
        const StagedImage &img = it->second;
        if (img.json_kept)
            add_archive(plan, image_entry(it->first), image_entry(it->first));
        else
            add_text(plan, image_entry(it->first), image_json(img.image));
        add_blob(plan, img.image.source.entry, img.source);
        if (img.image.has_target)
            add_blob(plan, img.image.target.entry, img.target);
        add_blob(plan, img.image.thumbnail.entry, img.thumbnail);
    }
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = e.results().begin(); it != e.results().end();
         ++it) {
        const StagedResult &r = it->second;
        if (r.json_kept)
            add_archive(plan, result_entry(it->first), result_entry(it->first));
        else
            add_text(plan, result_entry(it->first), result_json(r.result));
        add_blob(plan, result_sequence_entry(it->first), r.sequence);
        add_blob(plan, r.result.thumbnail.entry, r.thumbnail);
        if (r.result.has_settings)
            add_blob(plan, r.result.settings.entry, r.settings);
    }
    for (std::size_t i = 0; i < e.unknown_entries().size(); ++i)
        add_archive(plan, e.unknown_entries()[i], e.unknown_entries()[i]);
}

void warn_count(Report *report, const std::string &what, std::uint64_t count, std::uint64_t limit,
                const std::string &entry, ItemKind kind, std::uint32_t id) {
    if (report == nullptr || count <= limit)
        return;
    report->add(kWarning, "warn.count", entry,
                what + ": " + dec(count) + " is above the threshold of " + dec(limit), kind, id);
}

void collect_warnings(const ProjectEditor &e, std::uint64_t total_bytes, Report *report) {
    if (report == nullptr)
        return;
    const Limits &l = limits();
    warn_count(report, "frames", e.frames().size(), l.warn_items_per_kind, "", kItemNone, 0);
    warn_count(report, "layouts", e.layouts().size(), l.warn_items_per_kind, "", kItemNone, 0);
    warn_count(report, "images", e.images().size(), l.warn_items_per_kind, "", kItemNone, 0);
    warn_count(report, "spools", e.spools().size(), l.warn_items_per_kind, "", kItemNone, 0);
    warn_count(report, "results", e.results().size(), l.warn_items_per_kind, "", kItemNone, 0);
    warn_count(report, "decompressed bytes of the project", total_bytes, l.warn_decompressed_total, "", kItemNone, 0);
    for (std::map<std::uint32_t, StagedLayout>::const_iterator it = e.layouts().begin(); it != e.layouts().end(); ++it)
        warn_count(report, "nails of layout " + dec(it->first), it->second.layout.nail_count, l.warn_nails_per_layout,
                   nails_entry(it->second.layout.frame, it->first), kItemLayout, it->first);
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = e.results().begin(); it != e.results().end(); ++it)
        warn_count(report, "steps of result " + dec(it->first), it->second.result.steps, l.warn_steps_per_result,
                   result_sequence_entry(it->first), kItemResult, it->first);
}

} // namespace

void write_project(const ProjectEditor &editor, const std::string &modified_at, std::uint32_t options, Sink &sink,
                   loomora_progress_fn progress_cb, void *progress_user, Report *report) {
    if ((options & ~kSaveKnownOptions) != 0)
        throw FormatError(LOOMORA_ERR_INVALID, "unknown save option bits");
    if (!model::is_iso8601_utc(modified_at))
        throw FormatError(LOOMORA_ERR_INVALID, "modified_at must be an ISO 8601 UTC time (YYYY-MM-DDTHH:MM:SSZ)");
    const bool store_all = (options & kSaveStoreAll) != 0;

    editor.check_ready();
    std::vector<Planned> plan;
    build_plan(editor, plan);
    const ProjectReader *base = editor.base();
    std::map<std::string, const model::ManifestEntry *> old_entries; // the sums of the old archive
    if (base != nullptr)
        for (std::size_t i = 0; i < base->manifest().entries.size(); ++i)
            old_entries[base->manifest().entries[i].path] = &base->manifest().entries[i];

    // Pass 1: size and SHA-256 of every entry, for the manifest.
    model::Manifest manifest = editor.manifest();
    manifest.modified_at = modified_at;
    manifest.counts.frames = manifest.order.frames.size();
    manifest.counts.layouts = manifest.order.layouts.size();
    manifest.counts.images = manifest.order.images.size();
    manifest.counts.spools = manifest.order.spools.size();
    manifest.counts.results = manifest.order.results.size();
    manifest.required_features = editor.required_features();
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        std::map<std::string, const model::ManifestEntry *>::const_iterator known;
        if (plan[i].content == kArchive && (known = old_entries.find(plan[i].archive_name)) != old_entries.end()) {
            // Copied as it is: the size and the SHA-256 were verified when the archive was opened.
            plan[i].size = known->second->size;
            plan[i].sha256 = known->second->sha256;
        } else {
            HashEmitter hash;
            produce(base, plan[i], hash);
            plan[i].size = hash.size();
            plan[i].sha256 = hash.hex();
        }
        total += plan[i].size;
        model::ManifestEntry me;
        me.path = plan[i].name;
        me.size = plan[i].size;
        me.sha256 = plan[i].sha256;
        manifest.entries.push_back(me);
    }
    Report own;
    model::check_manifest(manifest, own);
    for (std::size_t i = 0; i < own.count(); ++i)
        if (own.at(i).severity == kError)
            throw FormatError(LOOMORA_ERR_INVALID_ITEM, own.at(i));
    const std::string manifest_text = manifest_json(manifest);
    total += manifest_text.size();

    // Pass 2: the archive.
    Progress progress(progress_cb, progress_user, total);
    ZipWriter zip(sink, &progress);
    zip.add_mimetype();
    if (store_all)
        zip.add_stored("manifest.json", manifest_text.data(), manifest_text.size());
    else
        zip.add_deflate("manifest.json", manifest_text.data(), manifest_text.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].content == kArchive) {
            const ZipEntry *old = base->find_entry(plan[i].archive_name);
            // Copy-through: the compressed bytes as they are (SC-013), unless "store everything" asks
            // for an uncompressed entry and this one is deflated.
            if (old != nullptr && (!store_all || old->method == 0)) {
                zip.add_raw(plan[i].name, base->zip(), *old);
                continue;
            }
        }
        zip.begin_entry(plan[i].name, method_of(plan[i].name, store_all), plan[i].size);
        ZipEmitter out(zip);
        produce(base, plan[i], out);
        zip.end_entry();
    }
    zip.finish();
    progress.finish();
    collect_warnings(editor, total, report);
}

void save_project_to_path(const ProjectEditor &editor, const std::string &utf8_path, const std::string &modified_at,
                          std::uint32_t options, loomora_progress_fn progress, void *progress_user, Report *report) {
    std::unique_ptr<Sink> sink = create_file_sink(utf8_path);
    try {
        write_project(editor, modified_at, options, *sink, progress, progress_user, report);
        sink->commit();
    } catch (...) {
        sink->abort();
        throw;
    }
}

void save_project_to_callback(const ProjectEditor &editor, const std::string &modified_at, std::uint32_t options,
                              loomora_sink_fn sink_fn, void *sink_user, loomora_progress_fn progress,
                              void *progress_user, Report *report) {
    CallbackSink sink(sink_fn, sink_user);
    try {
        write_project(editor, modified_at, options, sink, progress, progress_user, report);
        sink.commit();
    } catch (...) {
        sink.abort();
        throw;
    }
}

} // namespace format
} // namespace loomora

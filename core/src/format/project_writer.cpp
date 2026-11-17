#include "format/project_writer.hpp"

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
#include "format/validate/validator.hpp"
#include "format/zip/zip_writer.hpp"

namespace loomora {
namespace format {

namespace {

const std::size_t kNailsPerChunk = 2048;

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

enum Content { kText, kBlob, kString, kNails };

// One entry of the archive, in canonical order.
struct Planned {
    std::string name;
    Content content;
    std::string text;                       // kText
    const std::vector<std::uint8_t> *blob;  // kBlob
    const std::string *str;                 // kString
    const model::NailTable *nails;          // kNails
    std::uint64_t size;
    std::string sha256;

    Planned() : content(kText), blob(nullptr), str(nullptr), nails(nullptr), size(0) {}
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

void produce(const Planned &p, Emitter &out) {
    switch (p.content) {
    case kText:
        out.put(p.text.data(), p.text.size());
        break;
    case kBlob:
        out.put(p.blob->data(), p.blob->size());
        break;
    case kString:
        out.put(p.str->data(), p.str->size());
        break;
    case kNails:
        emit_nails(*p.nails, out);
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

// The entries in the canonical order of project-format.md section 2 (after mimetype and manifest).
void build_plan(const ProjectEditor &e, std::vector<Planned> &plan) {
    add_text(plan, "spools.json", spools_json(e.spools_in_order()));

    std::map<std::uint32_t, std::vector<std::uint32_t> > layouts_of;
    for (std::map<std::uint32_t, StagedLayout>::const_iterator it = e.layouts().begin(); it != e.layouts().end(); ++it)
        layouts_of[it->second.layout.frame].push_back(it->first);

    for (std::map<std::uint32_t, StagedFrame>::const_iterator fit = e.frames().begin(); fit != e.frames().end();
         ++fit) {
        const std::uint32_t fid = fit->first;
        add_text(plan, frame_entry(fid), frame_json(fit->second.frame));
        const std::vector<std::uint32_t> &ids = layouts_of[fid];
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const StagedLayout &l = e.layouts().find(ids[i])->second;
            add_text(plan, layout_entry(fid, ids[i]), layout_json(l.layout));
            Planned p;
            p.name = nails_entry(fid, ids[i]);
            p.content = kNails;
            p.nails = &l.nails;
            plan.push_back(p);
        }
    }
    for (std::map<std::uint32_t, StagedImage>::const_iterator it = e.images().begin(); it != e.images().end(); ++it) {
        const StagedImage &img = it->second;
        add_text(plan, image_entry(it->first), image_json(img.image));
        add_blob(plan, img.image.source.entry, img.source);
        if (img.image.has_target)
            add_blob(plan, img.image.target.entry, img.target);
        add_blob(plan, img.image.thumbnail.entry, img.thumbnail);
    }
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = e.results().begin(); it != e.results().end();
         ++it) {
        const StagedResult &r = it->second;
        add_text(plan, result_entry(it->first), result_json(r.result));
        add_blob(plan, result_sequence_entry(it->first), r.sequence);
        add_blob(plan, r.result.thumbnail.entry, r.thumbnail);
        if (r.result.has_settings) {
            Planned p;
            p.name = r.result.settings.entry;
            p.content = kString;
            p.str = &r.settings;
            plan.push_back(p);
        }
    }
}

void collect_warnings(const ProjectEditor &e, std::uint64_t total_bytes, Report *report) {
    if (report == nullptr)
        return;
    ThresholdFacts facts;
    facts.frames = e.frames().size();
    facts.layout_items = e.layouts().size();
    facts.images = e.images().size();
    facts.spools = e.spools().size();
    facts.results = e.results().size();
    facts.total_bytes = total_bytes;
    for (std::map<std::uint32_t, StagedLayout>::const_iterator it = e.layouts().begin(); it != e.layouts().end(); ++it) {
        ThresholdFacts::Layout l;
        l.id = it->first;
        l.frame = it->second.layout.frame;
        l.nails = it->second.nails.size();
        facts.layouts.push_back(l);
    }
    for (std::map<std::uint32_t, StagedResult>::const_iterator it = e.results().begin(); it != e.results().end(); ++it) {
        ThresholdFacts::Result r;
        r.id = it->first;
        r.steps = it->second.result.steps;
        facts.result_list.push_back(r);
    }
    for (std::map<std::uint32_t, StagedFrame>::const_iterator it = e.frames().begin(); it != e.frames().end(); ++it)
        if (it->second.frame.has_mesh) {
            ThresholdFacts::Mesh m;
            m.frame = it->first;
            m.triangles = it->second.frame.mesh.triangles;
            facts.meshes.push_back(m);
        }
    check_thresholds(facts, *report); // the same function as the reader's: one file, the same warnings
}

} // namespace

void write_project(const ProjectEditor &editor, const std::string &modified_at, std::uint32_t options, Sink &sink,
                   loomora_progress_fn progress_cb, void *progress_user, Report *report) {
    if ((options & ~kSaveKnownOptions) != 0)
        throw FormatError(LOOMORA_ERR_INVALID, "unknown save option bits");
    if (!model::is_iso8601_utc(modified_at))
        throw FormatError(LOOMORA_ERR_INVALID, "modified_at must be an ISO 8601 UTC time (YYYY-MM-DDTHH:MM:SSZ)");
    const bool store_all = (options & kSaveStoreAll) != 0;

    std::vector<Planned> plan;
    build_plan(editor, plan);

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
        HashEmitter hash;
        produce(plan[i], hash);
        plan[i].size = hash.size();
        plan[i].sha256 = hash.hex();
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
        zip.begin_entry(plan[i].name, method_of(plan[i].name, store_all), plan[i].size);
        ZipEmitter out(zip);
        produce(plan[i], out);
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

// The read side of the .loomora format: opens a project from a Source (a file or a caller's
// buffer, never copied), validates it while streaming and then serves its items and its bulk data
// by range. Read-only: nothing here writes, creates a file or changes a time.
//
// What open() does (contracts/project-format.md section 8, research R11), in order:
//   1. the ZIP container (ZipReader::open: every rule of section 1, the ratio guard);
//   2. manifest.json: parse, version rule (LOOMORA_ERR_UNSUPPORTED_VERSION, message "This file
//      needs Loomora format version X.Y or newer"), required features (LOOMORA_ERR_UNSUPPORTED_FEATURE),
//      the rules of the manifest on its own (manifest.* codes);
//   3. every archive entry streamed once through SHA-256 and the ZIP CRC-32, against the manifest
//      (entry.size, entry.sha256, entry.crc, entry.missing, entry.unlisted, warn.unknown_entry);
//   4. every item parsed into the model and checked by the model rules (schema.*, id.duplicate,
//      num.not_finite, nail.duplicate_id ...); the folder identifier must equal the item's `id`;
//   5. the binary entries: sequence.bin (seq.*: header, count against result.steps, CRC) and
//      mesh.bin (mesh.*: header, counts against frame.json, every block CRC, index range);
//   6. the manifest against the content (manifest.counts, manifest.order, manifest.next_id,
//      manifest.features);
//   7. warn.light_project for a light copy;
//   8. validate_semantic (validate/validator.hpp): the cross-item rules (references, nails against the
//      frame, spool lengths, thresholds, distances, origin, thumbnails). It always runs; when an item
//      could not be read (complete() is false) it keeps to the rules that the records read allow.
// An error does not stop the pass when it is safe to continue; every issue goes to the Report.
// The memory held by an open project is the manifest, the parsed small items and one index entry
// per 1,024 nails of each layout; nails, steps and mesh data are read by range from the archive.
//
// Thread safety: every const member function may be called from several threads at once (each
// call opens its own streams); open() itself runs once.
#ifndef LOOMORA_FORMAT_PROJECT_READER_HPP
#define LOOMORA_FORMAT_PROJECT_READER_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "loomora.h"
#include "format/codec/sequence_codec.hpp"
#include "format/io/source.hpp"
#include "format/model/frame.hpp"
#include "format/model/image.hpp"
#include "format/model/layout.hpp"
#include "format/model/project.hpp"
#include "format/model/result.hpp"
#include "format/model/spool.hpp"
#include "format/report.hpp"
#include "format/zip/zip_reader.hpp"

namespace loomora {
namespace format {

// The role of an archive entry in the layout of project-format.md section 2.
enum EntryRole {
    kRoleUnknown = 0,   // not defined by the format
    kRoleMimetype,
    kRoleManifest,
    kRoleSpools,        // spools.json
    kRoleFrameJson,     // frames/<fid>/frame.json            id = fid
    kRoleMesh,          // frames/<fid>/mesh.bin              id = fid
    kRoleMeshSource,    // frames/<fid>/mesh-source.<ext>     id = fid
    kRoleLayoutJson,    // frames/<fid>/layouts/<lid>/layout.json   id = lid, parent = fid
    kRoleNails,         // frames/<fid>/layouts/<lid>/nails.json    id = lid, parent = fid
    kRoleImageJson,     // images/<iid>/image.json            id = iid
    kRoleImageSource,   // images/<iid>/source.<png|jpg|jpeg|webp>
    kRoleImageTarget,   // images/<iid>/target.png
    kRoleImageThumb,    // images/<iid>/thumb.png
    kRoleResultJson,    // results/<rid>/result.json          id = rid
    kRoleSequence,      // results/<rid>/sequence.bin
    kRoleResultThumb,   // results/<rid>/thumb.png
    kRoleSettings       // results/<rid>/settings.json
};

struct EntryClass {
    EntryRole role;
    std::uint32_t id;     // the identifier in the path (0 for roles without one)
    std::uint32_t parent; // the frame identifier of a layout entry
    EntryClass() : role(kRoleUnknown), id(0), parent(0) {}
};

// Classifies an archive entry name. An identifier is a decimal number without leading zeros.
EntryClass classify_entry(const std::string &name);

struct FrameRecord {
    model::Frame frame;
    std::string entry;      // frames/<id>/frame.json
    std::string mesh_entry; // frames/<id>/mesh.bin when the frame has a mesh and the archive has it, else ""
};

struct LayoutRecord {
    model::Layout layout;
    std::uint32_t folder_frame; // the <fid> of the path (layout.frame may differ: a reference error)
    std::string entry;          // layout.json
    std::string nails_entry;    // nails.json, "" when the archive has none
    std::uint64_t nails_parsed; // nails actually found in nails.json (0 when it could not be read)
    std::vector<std::uint64_t> nail_offsets; // byte offset in nails.json of nail k * kNailsPerIndexEntry
    LayoutRecord() : folder_frame(0), nails_parsed(0) {}
};

struct ImageRecord {
    model::Image image;
    std::string entry; // image.json
};

struct SpoolRecord {
    model::Spool spool;
    std::string json; // the object of spools.json, compact
};

struct ResultRecord {
    model::Result result;
    std::string entry;          // result.json
    std::string sequence_entry; // sequence.bin, "" when the archive has none
};

// A random-access view of the uncompressed content of one entry, for the codecs that need a
// Source (SequenceReader, MeshReader). Stored entries are read in place; a deflated entry is
// inflated through a stream that restarts when a read goes backwards. Own one per thread.
class EntrySource : public Source {
  public:
    EntrySource(const ZipReader &zip, const ZipEntry &entry);
    ~EntrySource() override;
    std::uint64_t size() const override { return entry_.size; }
    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t len) const override;

  private:
    EntrySource(const EntrySource &);
    EntrySource &operator=(const EntrySource &);
    const ZipReader &zip_;
    ZipEntry entry_;
    mutable std::unique_ptr<ZipEntryStream> stream_;
    mutable std::uint64_t pos_; // uncompressed offset of the stream
};

class ProjectReader {
  public:
    ~ProjectReader();

    // Opens and validates. Returns 0 when the project has no error (warnings may be in `report`),
    // else the LOOMORA_ERR_* code (LOOMORA_ERR_CORRUPT or a specific one) with every issue found in
    // `report`; `*out` is then null. `source` is owned by the reader. `report` is not cleared.
    static int open(std::unique_ptr<Source> source, loomora_progress_fn progress, void *user,
                    std::unique_ptr<ProjectReader> *out, Report &report);

    // ---- the manifest and the items ----------------------------------------------------------
    const model::Manifest &manifest() const { return manifest_; }
    const std::string &manifest_json() const { return manifest_json_; } // manifest.json as stored
    bool is_light() const { return manifest_.profile == model::kProfileLight; }

    // The identifiers of a kind in display order (manifest `order`); `kind` is LOOMORA_KIND_*.
    const std::vector<std::uint32_t> &ids(int kind) const;
    std::uint64_t count(int kind) const { return ids(kind).size(); }

    const FrameRecord *frame(std::uint32_t id) const;
    const LayoutRecord *layout(std::uint32_t id) const;
    const ImageRecord *image(std::uint32_t id) const;
    const SpoolRecord *spool(std::uint32_t id) const;
    const ResultRecord *result(std::uint32_t id) const;

    const std::map<std::uint32_t, FrameRecord> &frames() const { return frames_; }
    const std::map<std::uint32_t, LayoutRecord> &layouts() const { return layouts_; }
    const std::map<std::uint32_t, ImageRecord> &images() const { return images_; }
    const std::map<std::uint32_t, SpoolRecord> &spools() const { return spools_; }
    const std::map<std::uint32_t, ResultRecord> &results() const { return results_; }

    // True when the manifest and every item could be read (a validator can rely on the records).
    bool complete() const { return complete_; }

    // ---- the archive -------------------------------------------------------------------------
    const ZipReader &zip() const { return zip_; }
    // The archive entry of that exact name, or null.
    const ZipEntry *find_entry(const std::string &name) const { return zip_.find(name); }
    // A Source over the uncompressed entry (throws FormatError(LOOMORA_ERR_NOT_FOUND) when absent).
    std::unique_ptr<Source> open_entry_source(const std::string &name) const;

    // ---- reading (every one throws FormatError: LOOMORA_ERR_NOT_FOUND for an unknown item) -----
    // The whole JSON text of an item; a spool gives its object of spools.json.
    std::vector<std::uint8_t> read_item_json(int kind, std::uint32_t id) const;
    // Up to `len` bytes of the uncompressed entry from `offset`; returns how many (0 at or past
    // the end). Throws LOOMORA_ERR_NOT_FOUND when the entry is absent.
    std::size_t read_entry(const std::string &name, std::uint64_t offset, void *buf, std::size_t len) const;
    // The nails [first, first + count) of a layout (fewer at the end) into `out`, which is replaced.
    // Cost: the entry is inflated up to the nearest index entry before `first`.
    void read_nails(std::uint32_t layout, std::uint64_t first, std::size_t count, model::NailTable &out) const;
    // Every nail of a layout (for the validator: memory proportional to the nail count).
    void load_nails(std::uint32_t layout, model::NailTable &out) const;
    // Steps [first, first + count) of a result's sequence, in file order, without verifying the
    // CRC again (the open pass did). Returns the number of steps read.
    std::size_t read_steps(std::uint32_t result, std::uint64_t first, std::size_t count,
                           std::vector<SequenceStep> &out) const;

  private:
    explicit ProjectReader(std::unique_ptr<Source> source);
    ProjectReader(const ProjectReader &);
    ProjectReader &operator=(const ProjectReader &);

    std::unique_ptr<Source> source_;
    ZipReader zip_;
    model::Manifest manifest_;
    std::string manifest_json_;
    std::map<std::uint32_t, FrameRecord> frames_;
    std::map<std::uint32_t, LayoutRecord> layouts_;
    std::map<std::uint32_t, ImageRecord> images_;
    std::map<std::uint32_t, SpoolRecord> spools_;
    std::map<std::uint32_t, ResultRecord> results_;
    bool complete_;
    friend struct ReaderPass;
};

} // namespace format
} // namespace loomora

#endif

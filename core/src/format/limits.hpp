// Every threshold, guard and tolerance of the .loomora format, in one place (data-model.md,
// "Constants"). The writer and the reader use the same header; all values are tunable without a
// format change.
//
// The values live in one mutable internal object, limits(). It is changed only by the tests and by
// the hidden CLI option `loomora validate --set-limit name=value`; it is never exported from the
// shared library (nothing in the C API touches it) and never read from the environment. A test
// that changes it uses ScopedLimits, which restores the defaults when it goes out of scope.
#ifndef LOOMORA_FORMAT_LIMITS_HPP
#define LOOMORA_FORMAT_LIMITS_HPP

#include <cstdint>

namespace loomora {
namespace format {

struct Limits {
    // Warning thresholds (FR-025): above them the file is valid but a warning is reported.
    std::uint64_t warn_nails_per_layout = 100000;
    std::uint64_t warn_steps_per_result = 5000000;
    std::uint64_t warn_triangles_per_mesh = 5000000;
    std::uint64_t warn_decompressed_total = 256ull * 1024 * 1024; // 256 MiB
    std::uint64_t warn_items_per_kind = 1000;

    // Decompression-bomb guard (FR-026): an entry is rejected when its decompressed size exceeds
    // bomb_ratio times its compressed size, once it has produced more than bomb_min_bytes.
    std::uint64_t bomb_ratio = 250;                 // 250 : 1
    std::uint64_t bomb_min_bytes = 1024ull * 1024;  // checked after 1 MiB

    // Thumbnails (FR-011, FR-017).
    std::uint32_t thumb_max_side = 256;              // pixels
    std::uint64_t thumb_max_bytes = 64ull * 1024;    // 64 KiB

    // Geometric tolerances in millimetres (FR-015, FR-024).
    double nail_in_frame_tolerance_mm = 0.01;
    double nail_overlap_tolerance_mm = 1e-6;
    double frame_origin_tolerance_mm = 0.01; // bounding-box centre, a warning

    // JSON (FR-027): nesting depth, and size of one entry (SAX is used for large arrays).
    std::uint32_t json_depth = 64;
    std::uint64_t json_entry_max_bytes = 1024ull * 1024 * 1024; // 1 GiB

    // ZIP entry name length in bytes (FR-026).
    std::uint32_t entry_name_max_bytes = 512;

    // Progress callback: called about every this many bytes processed (R11).
    std::uint64_t progress_interval_bytes = 4ull * 1024 * 1024; // 4 MiB

    // ZIP64 thresholds: ZIP64 structures are written (and required) when a size or an offset
    // reaches the first two, or the entry count the third. The defaults are the limits of the
    // classic ZIP format; tests lower them to force ZIP64 on small archives.
    std::uint64_t zip64_size_threshold = 0xFFFFFFFFull;
    std::uint64_t zip64_offset_threshold = 0xFFFFFFFFull;
    std::uint32_t zip64_count_threshold = 0xFFFF;
};

// The one mutable object. Not thread safe: change it only when nothing else runs.
Limits &limits();

// Puts the defaults back.
void reset_limits();

// Sets one limit by the name of the `--set-limit` option of the CLI: warn_nails, warn_steps,
// warn_triangles, warn_total_bytes, warn_items, bomb_ratio. Returns false for an unknown name.
bool set_limit_by_name(const char *name, std::uint64_t value);

// Restores the default limits when it is destroyed (tests).
class ScopedLimits {
  public:
    ScopedLimits() {}
    ~ScopedLimits() { reset_limits(); }

  private:
    ScopedLimits(const ScopedLimits &);
    ScopedLimits &operator=(const ScopedLimits &);
};

} // namespace format
} // namespace loomora

#endif

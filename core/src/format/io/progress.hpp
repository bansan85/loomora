// Progress reporting through a loomora_progress_fn: called about every limits().progress_interval_bytes
// (4 MiB by default) processed. A non-zero return from the callback ends the operation with
// FormatError(LOOMORA_ERR_CANCELLED). Used by the reader, the validator and the writer.
#ifndef LOOMORA_FORMAT_IO_PROGRESS_HPP
#define LOOMORA_FORMAT_IO_PROGRESS_HPP

#include <cstdint>

#include "loomora.h"
#include "format/limits.hpp"
#include "format/report.hpp"

namespace loomora {
namespace format {

class Progress {
  public:
    // `callback` may be null (then every call is a no-op); `total` is the bytes expected, 0 if unknown.
    Progress(loomora_progress_fn callback, void *user, std::uint64_t total)
        : callback_(callback), user_(user), total_(total), done_(0), reported_(0) {}

    // Adds `bytes` processed; calls the callback when the interval since the last call is reached.
    // Throws FormatError(LOOMORA_ERR_CANCELLED) when the callback asks to stop.
    void advance(std::uint64_t bytes) {
        done_ += bytes;
        if (callback_ != nullptr && done_ - reported_ >= limits().progress_interval_bytes)
            report();
    }

    // Calls the callback once more with the final figure (same cancellation rule).
    void finish() {
        if (callback_ != nullptr)
            report();
    }

    std::uint64_t done() const { return done_; }

  private:
    void report() {
        reported_ = done_;
        if (callback_(user_, done_, total_) != 0)
            throw FormatError(LOOMORA_ERR_CANCELLED, "operation cancelled by the caller");
    }

    loomora_progress_fn callback_;
    void *user_;
    std::uint64_t total_;
    std::uint64_t done_;
    std::uint64_t reported_;
};

} // namespace format
} // namespace loomora

#endif

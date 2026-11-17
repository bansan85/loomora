// In-memory implementations of Source and Sink (memory_io.cpp):
//   MemorySource   reads a caller's buffer without copying it (loomora_project_open_memory);
//   MemorySink     grows a std::vector, with an optional growth callback (cap or accounting);
//   CallbackSink   the sink of loomora_editor_save_stream: it keeps the archive in memory (the
//                  ZIP writer must patch earlier bytes, which a forward-only callback cannot do)
//                  and hands it to the loomora_sink_fn, in order, at commit().
#ifndef LOOMORA_FORMAT_IO_MEMORY_IO_HPP
#define LOOMORA_FORMAT_IO_MEMORY_IO_HPP

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "loomora.h"
#include "format/io/sink.hpp"
#include "format/io/source.hpp"

namespace loomora {
namespace format {

class MemorySource : public Source {
  public:
    // `data` is not copied and must stay valid as long as the source is used (the contract of
    // loomora_project_open_memory). It may be null when len is 0.
    MemorySource(const std::uint8_t *data, std::size_t len) : data_(data), len_(len) {}

    std::uint64_t size() const override { return len_; }
    std::size_t read_at(std::uint64_t offset, void *buf, std::size_t len) const override;

    // The whole buffer, for callers that can read in place.
    const std::uint8_t *data() const { return data_; }

  private:
    const std::uint8_t *data_;
    std::size_t len_;
};

// Called before the buffer must grow to hold `needed_total` bytes; returning false refuses the
// growth (the write then fails with LOOMORA_ERR_NO_MEMORY and nothing is added).
typedef bool (*GrowFn)(void *user, std::uint64_t needed_total);

class MemorySink : public Sink {
  public:
    MemorySink() : grow_(nullptr), grow_user_(nullptr), state_(kOpen) {}
    MemorySink(GrowFn grow, void *user) : grow_(grow), grow_user_(user), state_(kOpen) {}

    void write(const void *data, std::size_t len) override;
    std::uint64_t position() const override { return bytes_.size(); }
    void patch_at(std::uint64_t offset, const void *data, std::size_t len) override;
    void commit() override;
    void abort() noexcept override;

    // What was written (empty after abort).
    const std::vector<std::uint8_t> &bytes() const { return bytes_; }
    // Moves the written bytes out (after commit()).
    std::vector<std::uint8_t> take() { return std::move(bytes_); }
    bool committed() const { return state_ == kCommitted; }
    bool aborted() const { return state_ == kAborted; }

  private:
    enum State { kOpen, kCommitted, kAborted };
    void require_open() const;

    std::vector<std::uint8_t> bytes_;
    GrowFn grow_;
    void *grow_user_;
    State state_;
};

class CallbackSink : public Sink {
  public:
    // `chunk` is the largest piece handed to the callback at commit (at least 1).
    CallbackSink(loomora_sink_fn callback, void *user, std::size_t chunk = 64 * 1024)
        : callback_(callback), user_(user), chunk_(chunk == 0 ? 1 : chunk) {}

    void write(const void *data, std::size_t len) override { buffer_.write(data, len); }
    std::uint64_t position() const override { return buffer_.position(); }
    void patch_at(std::uint64_t offset, const void *data, std::size_t len) override {
        buffer_.patch_at(offset, data, len);
    }
    // Delivers everything, in order, in pieces of at most `chunk` bytes; a non-zero return of the
    // callback stops the delivery with FormatError(LOOMORA_ERR_IO).
    void commit() override;
    void abort() noexcept override { buffer_.abort(); }

  private:
    loomora_sink_fn callback_;
    void *user_;
    std::size_t chunk_;
    MemorySink buffer_;
};

} // namespace format
} // namespace loomora

#endif

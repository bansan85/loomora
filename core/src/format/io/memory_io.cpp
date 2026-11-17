#include "format/io/memory_io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "format/report.hpp"
#include "format/testing.hpp"

namespace loomora {
namespace format {

bool read_exact(const Source &source, std::uint64_t offset, void *buf, std::size_t len) {
    std::uint8_t *out = static_cast<std::uint8_t *>(buf);
    std::size_t done = 0;
    while (done < len) {
        const std::size_t got = source.read_at(offset + done, out + done, len - done);
        if (got == 0)
            return false;
        done += got;
    }
    return true;
}

std::size_t MemorySource::read_at(std::uint64_t offset, void *buf, std::size_t len) const {
    if (len == 0 || offset >= len_)
        return 0;
    const std::size_t available = len_ - static_cast<std::size_t>(offset);
    const std::size_t n = std::min(len, available);
    std::memcpy(buf, data_ + static_cast<std::size_t>(offset), n);
    return n;
}

void MemorySink::require_open() const {
    if (state_ != kOpen)
        throw FormatError(LOOMORA_ERR_INTERNAL, "the sink is already committed or aborted");
}

void MemorySink::write(const void *data, std::size_t len) {
    require_open();
    if (should_fail_write(bytes_.size(), len))
        throw FormatError(LOOMORA_ERR_IO, "write failed (injected)");
    if (len == 0)
        return;
    const std::uint64_t needed = static_cast<std::uint64_t>(bytes_.size()) + len;
    if (needed > std::numeric_limits<std::size_t>::max() ||
        (grow_ != nullptr && !grow_(grow_user_, needed)) || should_fail_alloc(len))
        throw FormatError(LOOMORA_ERR_NO_MEMORY, "the memory buffer cannot grow");
    const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
    bytes_.insert(bytes_.end(), p, p + len); // std::bad_alloc maps to LOOMORA_ERR_NO_MEMORY
}

void MemorySink::patch_at(std::uint64_t offset, const void *data, std::size_t len) {
    require_open();
    if (offset > bytes_.size() || len > bytes_.size() - static_cast<std::size_t>(offset))
        throw FormatError(LOOMORA_ERR_INTERNAL, "patch outside the bytes already written");
    if (len != 0)
        std::memcpy(&bytes_[static_cast<std::size_t>(offset)], data, len);
}

void MemorySink::commit() {
    require_open();
    state_ = kCommitted;
}

void MemorySink::abort() noexcept {
    state_ = kAborted;
    std::vector<std::uint8_t>().swap(bytes_);
}

void CallbackSink::commit() {
    buffer_.commit();
    const std::vector<std::uint8_t> &all = buffer_.bytes();
    std::size_t done = 0;
    while (done < all.size()) {
        const std::size_t n = std::min(chunk_, all.size() - done);
        if (callback_(user_, &all[done], n) != 0)
            throw FormatError(LOOMORA_ERR_IO, "the output callback refused the data");
        done += n;
    }
}

} // namespace format
} // namespace loomora

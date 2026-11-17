// sequence.bin codec (sequence_codec.hpp).
#include "format/codec/sequence_codec.hpp"

#include <algorithm>
#include <cstring>

#include "zlib.h"
#include "format/codec/codec_error.hpp"
#include "format/endian.hpp"

namespace loomora {
namespace format {

namespace {

const std::size_t kBufferSize = 64 * 1024;
const std::size_t kMaxRecord = 12;   // 5 (nail) + 1 (flags) + 5 (escaped spool) + 1 (level)
const std::size_t kMinFile = 4 + 1 + 1 + 1 + 4; // magic, version, flags, count, CRC
const unsigned char kMagic[4] = {'L', 'M', 'S', 'Q'};
const std::uint32_t kEscape = 31;

unsigned long crc_update(unsigned long crc, const unsigned char *p, std::size_t len) {
    while (len > 0) {
        const std::size_t n = len > 0x40000000u ? 0x40000000u : len;
        crc = crc32(crc, p, static_cast<uInt>(n));
        p += n;
        len -= n;
    }
    return crc;
}

// 1: parsed (`*used` bytes), 0: the data ends in the middle of the varint, -1: malformed
// (longer than 5 bytes, above 2^32 - 1, or not minimal).
int parse_varint(const unsigned char *p, std::size_t avail, std::uint32_t *value, std::size_t *used) {
    const std::size_t limit = avail < 5 ? avail : 5;
    for (std::size_t i = 0; i < limit; ++i) {
        if ((p[i] & 0x80u) == 0) {
            const std::size_t n = read_varint_u32(p, i + 1, value);
            if (n == 0)
                return -1;
            *used = n;
            return 1;
        }
    }
    return avail < 5 ? 0 : -1;
}

} // namespace

// ---- SequenceWriter ------------------------------------------------------------------------------

SequenceWriter::SequenceWriter(Sink &sink, bool levels, std::uint32_t count)
    : sink_(sink), levels_(levels), count_(count), added_(0), crc_(crc32(0L, Z_NULL, 0)), finished_(false) {
    buf_.reserve(kBufferSize + kMaxRecord);
    unsigned char head[6 + 5];
    std::memcpy(head, kMagic, 4);
    head[4] = 1;
    head[5] = levels ? 1 : 0;
    const std::size_t n = write_varint_u32(head + 6, count);
    put(head, 6 + n);
}

void SequenceWriter::put(const unsigned char *data, std::size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    if (buf_.size() >= kBufferSize)
        flush();
}

void SequenceWriter::flush() {
    if (buf_.empty())
        return;
    crc_ = crc_update(crc_, &buf_[0], buf_.size());
    sink_.write(&buf_[0], buf_.size());
    buf_.clear();
}

void SequenceWriter::add(const SequenceStep &step) {
    if (finished_)
        codec::fail_internal("sequence writer used after finish()");
    if (added_ >= count_)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "more records than the declared count");
    if (static_cast<unsigned>(step.type) > static_cast<unsigned>(kStepEnd))
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "reserved record type");
    if (!levels_ && step.level != 0)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "a level is set but the sequence has no levels");

    unsigned char rec[kMaxRecord];
    std::size_t n = write_varint_u32(rec, step.nail_id);
    unsigned flags = (step.ccw ? 1u : 0u) | (static_cast<unsigned>(step.type) << 1);
    if (step.spool_index < kEscape) {
        flags |= step.spool_index << 3;
        rec[n++] = static_cast<unsigned char>(flags);
    } else {
        flags |= kEscape << 3;
        rec[n++] = static_cast<unsigned char>(flags);
        n += write_varint_u32(rec + n, step.spool_index);
    }
    if (levels_)
        rec[n++] = step.level;
    put(rec, n);
    ++added_;
}

void SequenceWriter::finish() {
    if (finished_)
        codec::fail_internal("sequence writer finished twice");
    if (added_ != count_)
        codec::fail_internal("sequence writer finished before all declared records were added");
    flush();
    unsigned char t[4];
    write_u32(t, static_cast<std::uint32_t>(crc_ & 0xFFFFFFFFul));
    sink_.write(t, 4);
    finished_ = true;
}

void encode_sequence(Sink &sink, const std::vector<SequenceStep> &steps, bool levels) {
    if (steps.size() > 0xFFFFFFFFull)
        throw FormatError(LOOMORA_ERR_INVALID_ITEM, "more than 2^32 - 1 records");
    SequenceWriter w(sink, levels, static_cast<std::uint32_t>(steps.size()));
    for (std::size_t i = 0; i < steps.size(); ++i)
        w.add(steps[i]);
    w.finish();
}

// ---- SequenceReader ------------------------------------------------------------------------------

SequenceReader::SequenceReader(const Source &source)
    : source_(source), region_end_(0), file_pos_(0), buf_(kBufferSize), pos_(0), end_(0),
      crc_(crc32(0L, Z_NULL, 0)), count_(0), delivered_(0), levels_(false) {
    if (source.size() < kMinFile)
        codec::fail_corrupt("seq.header", "sequence entry is too short");
    region_end_ = source.size() - 4;

    ensure(6 + 5);
    if (end_ - pos_ < 6 + 1)
        codec::fail_corrupt("seq.header", "sequence entry is too short");
    const unsigned char *p = &buf_[pos_];
    if (std::memcmp(p, kMagic, 4) != 0)
        codec::fail_corrupt("seq.header", "bad magic (not a Loomora sequence)");
    if (p[4] != 1)
        codec::fail_corrupt("seq.header", "unsupported sequence version");
    if ((p[5] & ~1u) != 0)
        codec::fail_corrupt("seq.header", "reserved header flag set");
    levels_ = (p[5] & 1u) != 0;
    std::size_t used = 0;
    if (parse_varint(p + 6, end_ - pos_ - 6, &count_, &used) != 1)
        codec::fail_corrupt("seq.header", "malformed or truncated record count");
    consume(6 + used);
}

void SequenceReader::ensure(std::size_t n) {
    if (end_ - pos_ >= n)
        return;
    const std::size_t keep = end_ - pos_;
    if (pos_ > 0 && keep > 0)
        std::memmove(&buf_[0], &buf_[pos_], keep);
    pos_ = 0;
    end_ = keep;
    const std::uint64_t left = region_end_ - file_pos_;
    const std::uint64_t room = buf_.size() - end_;
    const std::size_t want = static_cast<std::size_t>(left < room ? left : room);
    if (want > 0) {
        if (!read_exact(source_, file_pos_, &buf_[end_], want))
            throw FormatError(LOOMORA_ERR_IO, "short read of sequence entry");
        file_pos_ += want;
        end_ += want;
    }
}

void SequenceReader::consume(std::size_t n) {
    crc_ = crc_update(crc_, &buf_[pos_], n);
    pos_ += n;
}

bool SequenceReader::next(SequenceStep *step) {
    if (delivered_ >= count_)
        return false;
    ensure(kMaxRecord);
    const std::size_t avail = end_ - pos_;
    const unsigned char *p = &buf_[pos_];
    const char *const kShort = "the entry ends before the declared number of records";

    std::uint32_t nail = 0, spool = 0;
    std::size_t used = 0, off = 0;
    int rc = parse_varint(p, avail, &nail, &used);
    if (rc == 0)
        codec::fail_corrupt("seq.count", kShort);
    if (rc < 0)
        codec::fail_corrupt("seq.header", "malformed or non-minimal nail id");
    off = used;
    if (off >= avail)
        codec::fail_corrupt("seq.count", kShort);
    const unsigned flags = p[off++];
    const unsigned type = (flags >> 1) & 3u;
    if (type == 3)
        codec::fail_corrupt("seq.header", "reserved record type 3");
    spool = flags >> 3;
    if (spool == kEscape) {
        rc = parse_varint(p + off, avail - off, &spool, &used);
        if (rc == 0)
            codec::fail_corrupt("seq.count", kShort);
        if (rc < 0)
            codec::fail_corrupt("seq.header", "malformed or non-minimal spool index");
        if (spool < kEscape)
            codec::fail_corrupt("seq.header", "escaped spool index below 31");
        off += used;
    }
    unsigned level = 0;
    if (levels_) {
        if (off >= avail)
            codec::fail_corrupt("seq.count", kShort);
        level = p[off++];
    }
    consume(off);
    ++delivered_;
    step->nail_id = nail;
    step->ccw = (flags & 1u) != 0;
    step->type = static_cast<StepType>(type);
    step->spool_index = spool;
    step->level = static_cast<std::uint8_t>(level);
    return true;
}

void SequenceReader::finish() {
    if (delivered_ < count_)
        codec::fail_internal("sequence reader finished before all records were read");
    if (pos_ != end_ || file_pos_ != region_end_)
        codec::fail_corrupt("seq.count", "bytes remain after the declared number of records");
    unsigned char t[4];
    if (!read_exact(source_, region_end_, t, 4))
        throw FormatError(LOOMORA_ERR_IO, "short read of sequence entry");
    if (read_u32(t) != static_cast<std::uint32_t>(crc_ & 0xFFFFFFFFul))
        codec::fail_corrupt("seq.crc", "CRC-32 of the sequence does not match");
}

void decode_sequence(const Source &source, std::vector<SequenceStep> *out, std::uint64_t first,
                     std::uint64_t max_count, std::uint32_t *total_count, bool *levels) {
    out->clear();
    SequenceReader reader(source);
    if (total_count != nullptr)
        *total_count = reader.count();
    if (levels != nullptr)
        *levels = reader.has_levels();
    // Each record takes at least 2 bytes: a count above that is caught as seq.count while reading.
    std::uint64_t reserve = max_count;
    if (reserve > reader.count())
        reserve = reader.count();
    if (reserve > source.size() / 2)
        reserve = source.size() / 2;
    out->reserve(static_cast<std::size_t>(reserve));

    SequenceStep s;
    std::uint64_t index = 0;
    while (reader.next(&s)) {
        if (index >= first && out->size() < max_count)
            out->push_back(s);
        ++index;
    }
    reader.finish();
}

// ---- Machine steps -------------------------------------------------------------------------------

MachineStepBuilder::MachineStepBuilder(const std::vector<std::uint32_t> &spool_ids,
                                       const SegmentLength &length)
    : spool_ids_(spool_ids), length_(length), state_(spool_ids.size()), steps_(0) {}

bool MachineStepBuilder::add(const SequenceStep &step, MachineStep *out) {
    if (step.spool_index >= state_.size())
        codec::fail_corrupt("seq.spool", "spool index outside the result's spool selection");
    State &st = state_[step.spool_index];
    switch (step.type) {
    case kStepStart:
        st.active = true;
        st.nail = step.nail_id;
        st.ccw = step.ccw;
        return false;
    case kStepEnd:
        if (!st.active)
            codec::fail_corrupt("seq.order", "end for a spool whose string is not attached");
        st.active = false;
        return false;
    case kStepSegment:
        break;
    }
    if (!st.active)
        codec::fail_corrupt("seq.order", "segment for a spool whose string is not attached");
    out->step = ++steps_;
    out->from_nail = st.nail;
    out->from_ccw = st.ccw;
    out->to_nail = step.nail_id;
    out->to_ccw = step.ccw;
    out->spool_id = spool_ids_[step.spool_index];
    out->length_mm = length_ ? length_(st.nail, st.ccw, step.nail_id, step.ccw) : 0.0;
    st.nail = step.nail_id;
    st.ccw = step.ccw;
    return true;
}

void rebuild_machine_steps(const std::vector<SequenceStep> &steps,
                           const std::vector<std::uint32_t> &spool_ids, const SegmentLength &length,
                           std::vector<MachineStep> *out) {
    out->clear();
    MachineStepBuilder builder(spool_ids, length);
    MachineStep m;
    for (std::size_t i = 0; i < steps.size(); ++i)
        if (builder.add(steps[i], &m))
            out->push_back(m);
}

} // namespace format
} // namespace loomora

// Codec of `sequence.bin`, the weaving sequence of a result (contracts/project-format.md 5.1).
//
//   "LMSQ" | version u8 (=1) | header flags u8 (bit 0: levels present) | count varint |
//   records... | CRC-32 u32 (of every preceding byte, zlib polynomial)
//   record: nail_id varint | flags u8 | level u8 (only with header bit 0)
//   flags:  bit 0 = rotation (0 CW, 1 CCW, seen from the nail tip in the front view, FR-015: the
//           codec stores and returns it as is, nothing is mirrored), bits 1-2 = type (0 segment,
//           1 start, 2 end, 3 reserved and rejected), bits 3-7 = spool index 0-30 into the result's
//           spool selection, 31 = escape followed by a varint index (>= 31).
//
// Encoding: SequenceWriter (or encode_sequence) streams to a Sink; the count must be known first
// (it is written before the records) and is checked at finish(). Decoding: SequenceReader streams
// records from a Source through a small fixed buffer (memory does not grow with the sequence);
// decode_sequence collects a range. The CRC is at the end of the entry, so records returned by
// SequenceReader::next are unverified until finish() returns: use decode_sequence, or call
// finish() and drop the records when it throws.
//
// Errors are FormatError(LOOMORA_ERR_CORRUPT) carrying an Issue with these codes:
//   seq.header  bad magic, version, reserved header flag, entry too short, malformed or non-minimal
//               varint, reserved type 3, escaped spool index below 31;
//   seq.count   `count` differs from the number of records (more or fewer bytes than count says);
//   seq.crc     the trailing CRC-32 does not match;
// plus, for rebuild_machine_steps (semantics, not bytes): seq.order (a segment or end for a spool
// whose string is not attached) and seq.spool (a spool index outside the selection). The checks that
// need the project (nail_id belongs to the layout, count == result.steps) are the caller's.
// An encoder refuses invalid input with FormatError(LOOMORA_ERR_INVALID_ITEM).
#ifndef LOOMORA_FORMAT_CODEC_SEQUENCE_CODEC_HPP
#define LOOMORA_FORMAT_CODEC_SEQUENCE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "format/io/sink.hpp"
#include "format/io/source.hpp"

namespace loomora {
namespace format {

enum StepType { kStepSegment = 0, kStepStart = 1, kStepEnd = 2 };

struct SequenceStep {
    std::uint32_t nail_id;
    bool ccw;                  // false = CW, true = CCW (rotation bit 0)
    StepType type;
    std::uint32_t spool_index; // index into the result's spool selection
    std::uint8_t level;        // pass height, 0 = base; meaningful only when the sequence has levels

    SequenceStep() : nail_id(0), ccw(false), type(kStepSegment), spool_index(0), level(0) {}
};

// Streams one sequence.bin to a Sink. The sink is neither committed nor aborted here.
class SequenceWriter {
  public:
    // Writes the header. `count` is the number of records that add() will receive.
    SequenceWriter(Sink &sink, bool levels, std::uint32_t count);

    // Appends one record. FormatError(LOOMORA_ERR_INVALID_ITEM) for a level set while the sequence
    // has no levels, or more records than `count`.
    void add(const SequenceStep &step);

    // Flushes and writes the CRC-32. FormatError(LOOMORA_ERR_INTERNAL) when fewer than `count`
    // records were added.
    void finish();

  private:
    void flush();
    void put(const unsigned char *data, std::size_t len);

    Sink &sink_;
    bool levels_;
    std::uint32_t count_;
    std::uint32_t added_;
    unsigned long crc_;
    bool finished_;
    std::vector<unsigned char> buf_;
};

// Writes a whole sequence (count = steps.size(), which must fit in u32).
void encode_sequence(Sink &sink, const std::vector<SequenceStep> &steps, bool levels);

// Streams records out of a Source.
class SequenceReader {
  public:
    // Reads and checks the header (seq.header). Does not read the records.
    explicit SequenceReader(const Source &source);

    std::uint32_t count() const { return count_; }
    bool has_levels() const { return levels_; }

    // Delivers the next record; false once `count` records were delivered. seq.count when the
    // data ends before `count` records, seq.header for a malformed record.
    bool next(SequenceStep *step);

    // After next() returned false: checks that no byte is left between the last record and the
    // CRC (seq.count) and the CRC itself (seq.crc). FormatError(LOOMORA_ERR_INTERNAL) when called
    // before all records were delivered.
    void finish();

  private:
    void ensure(std::size_t n);
    void consume(std::size_t n);

    const Source &source_;
    std::uint64_t region_end_;  // offset of the trailing CRC
    std::uint64_t file_pos_;    // offset of the first byte not yet loaded in buf_
    std::vector<unsigned char> buf_;
    std::size_t pos_, end_;
    unsigned long crc_;
    std::uint32_t count_;
    std::uint32_t delivered_;
    bool levels_;
};

// Decodes and fully verifies a sequence (every record is parsed and the CRC checked), keeping
// only `max_count` records from `first` (record index) in `*out` (cleared first), so a huge
// sequence can be inspected with bounded memory. `*total_count` and `*levels`, when not null,
// receive the header's count and levels flag.
void decode_sequence(const Source &source, std::vector<SequenceStep> *out, std::uint64_t first = 0,
                     std::uint64_t max_count = ~static_cast<std::uint64_t>(0),
                     std::uint32_t *total_count = nullptr, bool *levels = nullptr);

// ---- Machine step list (requirements 8.3, contracts/project-format.md 5.1) ----------------------

// One element of the "Machine JSON / CSV" schema: {step, from_nail, from_side, to_nail, to_side,
// spool_id, length_mm}; the sides are CW (false) or CCW (true), as stored, never mirrored.
struct MachineStep {
    std::uint64_t step;        // 1-based counter of segments
    std::uint32_t from_nail;
    bool from_ccw;             // rotation at the starting nail: that of the spool's previous record
    std::uint32_t to_nail;
    bool to_ccw;               // rotation at the arrival nail: that of the segment record
    std::uint32_t spool_id;    // project spool identifier (looked up through the selection)
    double length_mm;

    MachineStep() : step(0), from_nail(0), from_ccw(false), to_nail(0), to_ccw(false), spool_id(0),
                    length_mm(0.0) {}
};

// Length of the string between two nails with their winding sides (needs the nail positions and
// diameters, which the sequence does not have). An empty function gives length 0 for every step.
typedef std::function<double(std::uint32_t from_nail, bool from_ccw, std::uint32_t to_nail, bool to_ccw)>
    SegmentLength;

// Turns records in weaving order into machine steps, one at a time (so a sequence of millions of
// steps never needs to be in memory). Per spool: start attaches the string at its nail with the
// record's rotation; segment goes from the spool's previous record (start or segment) to its own
// nail; end detaches the string (no machine step, the end nail is where it is cut or fixed). A
// start while the string is attached just re-attaches it.
class MachineStepBuilder {
  public:
    // `spool_ids[i]` is the project spool identifier of selection index i.
    MachineStepBuilder(const std::vector<std::uint32_t> &spool_ids, const SegmentLength &length);

    // Feeds the next record. Returns true and fills `*out` when the record is a segment.
    // seq.order / seq.spool as described in the header comment.
    bool add(const SequenceStep &step, MachineStep *out);

  private:
    struct State {
        bool active;
        std::uint32_t nail;
        bool ccw;
        State() : active(false), nail(0), ccw(false) {}
    };
    std::vector<std::uint32_t> spool_ids_;
    SegmentLength length_;
    std::vector<State> state_;
    std::uint64_t steps_;
};

// The whole list in one call (`*out` is cleared first).
void rebuild_machine_steps(const std::vector<SequenceStep> &steps,
                           const std::vector<std::uint32_t> &spool_ids, const SegmentLength &length,
                           std::vector<MachineStep> *out);

} // namespace format
} // namespace loomora

#endif

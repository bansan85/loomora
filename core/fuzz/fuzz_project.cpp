// libFuzzer entry of the .loomora reader (T072): the input is an archive in memory; it is validated,
// opened and every item is read through the public C API. The reader must never crash, hang, leak
// or leave undefined behaviour whatever the bytes are; every failure is an error code and a report.
//
//   clang++ -fsanitize=fuzzer,address,undefined ...   (CMake target loomora_fuzz_project)
//   loomora_fuzz_project -timeout=5 -max_len=262144 corpus-dir seed-dir...
//
// -timeout=5 is the per-input limit (SC-004). The structure-aware mutator below keeps the ZIP CRC-32
// and the manifest SHA-256 right after a mutation of the stored entries, so mutations reach the
// parsers and the semantic validator. The same entry point is called by the mutation loop
// (fuzz_loop.cpp) on hosts without libFuzzer.
#include <cstddef>
#include <cstdint>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    fuzz::exercise(data, size);
    return 0;
}

#ifdef FUZZ_LIBFUZZER
extern "C" std::size_t LLVMFuzzerMutate(std::uint8_t *data, std::size_t size, std::size_t max_size);

// Mutates one stored entry when the input is an archive of stored entries (then fixes the checksums),
// else the whole input like the default mutator.
extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t *data, std::size_t size, std::size_t max_size,
                                               unsigned int seed) {
    fuzz::Bytes a(data, data + size);
    const std::vector<fuzz::StoredEntry> es = fuzz::stored_entries(a);
    if (es.empty() || (seed % 4) == 0)
        return LLVMFuzzerMutate(data, size, max_size);
    const fuzz::StoredEntry &e = es[(seed / 4) % es.size()];
    if (e.size == 0)
        return size;
    LLVMFuzzerMutate(data + e.data, e.size, e.size); // same length: the layout of the archive stays
    fuzz::Bytes patched(data, data + size);
    fuzz::fix_up(patched);
    for (std::size_t i = 0; i < size; ++i)
        data[i] = patched[i];
    return size;
}
#endif

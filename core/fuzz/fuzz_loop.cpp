// A mutation loop over corpus files, for hosts without libFuzzer (T072): it reads the seed files,
// mutates them (bit flips, byte changes, ranges, truncation, splices, structure-aware edits of one
// stored entry with the checksums repaired) and feeds each result to LLVMFuzzerTestOneInput of
// fuzz_project.cpp. Build it with -fsanitize=address,undefined (CMake target loomora_fuzz_loop): the
// first crash, sanitizer report or input slower than 5 seconds saves the input as crash-<seed>-<n>.bin
// in the current directory and ends the run with a non-zero status.
//
//   loomora_fuzz_loop [--seconds S] [--iterations N] [--seed N] [--max-len BYTES] [--out DIR] seed-dir-or-file...
//
// The mutator is deterministic for a given --seed, so a crash is reproduced by running the saved file
// through `loomora_fuzz_loop --replay file`.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);
extern "C" void __sanitizer_set_death_callback(void (*)()) __attribute__((weak));

namespace {

using fuzz::Bytes;

// ---- a small deterministic random generator (xorshift64*) -------------------------------------------
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 2685821657736338717ULL + 0x9E3779B97F4A7C15ULL) {
        if (s == 0)
            s = 1;
    }
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }
};

// ---- the input being run, saved when the process dies --------------------------------------------------
const Bytes *g_current = nullptr;
std::string g_out_dir = ".";
std::uint64_t g_seed = 0, g_iteration = 0;

void save_current(const char *why) {
    if (g_current == nullptr)
        return;
    char name[256];
    std::snprintf(name, sizeof name, "%s/crash-%llu-%llu.bin", g_out_dir.c_str(), static_cast<unsigned long long>(g_seed),
                  static_cast<unsigned long long>(g_iteration));
    FILE *f = std::fopen(name, "wb");
    if (f != nullptr) {
        if (!g_current->empty())
            std::fwrite(&(*g_current)[0], 1, g_current->size(), f);
        std::fclose(f);
        std::fprintf(stderr, "%s: the input was saved as %s (%zu bytes)\n", why, name, g_current->size());
    }
}

void on_death() { save_current("sanitizer report"); }

void on_signal(int sig) {
    save_current(sig == SIGABRT ? "abort" : sig == SIGSEGV ? "segmentation fault" : sig == SIGFPE ? "arithmetic error" : "signal");
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// ---- seeds ---------------------------------------------------------------------------------------------------

bool read_file(const std::string &path, Bytes &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    out.clear();
    unsigned char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void collect(const std::string &path, std::vector<std::string> &files) {
#ifdef _WIN32
    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        files.push_back(path);
        return;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((path + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            files.push_back(path + "/" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        files.push_back(path);
        return;
    }
    DIR *d = opendir(path.c_str());
    if (d == nullptr)
        return;
    while (struct dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name[0] == '.')
            continue;
        const std::string full = path + "/" + name;
        if (stat(full.c_str(), &st) == 0 && !S_ISDIR(st.st_mode))
            files.push_back(full);
    }
    closedir(d);
#endif
    std::sort(files.begin(), files.end());
}

// ---- mutations ---------------------------------------------------------------------------------------------------

const std::uint8_t kInteresting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x20, 0x22, 0x2D, 0x30, 0x39, 0x5B, 0x5D, 0x7B, 0x7D, 0x2C, 0x3A};
const std::uint32_t kWords[] = {0u, 1u, 2u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u,
                                0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x3FFFFFFFu};

// One mutation of the bytes in [lo, hi) (the whole input for a raw mutation).
void mutate_range(Bytes &b, std::size_t lo, std::size_t hi, Rng &rng, bool keep_size) {
    if (hi <= lo || hi > b.size())
        return;
    const std::size_t len = hi - lo;
    // Inside a JSON entry most edits change a number (an id, a count, a coordinate): the file stays
    // well formed, so the edit reaches the model and the semantic rules instead of the JSON parser.
    const bool json = keep_size && (b[lo] == '{' || b[lo] == '[');
    switch (json && rng.chance(85) ? 4 : rng.below(keep_size ? 8 : 12)) {
    case 0: // flip a bit
        b[lo + rng.below(len)] ^= static_cast<std::uint8_t>(1u << rng.below(8));
        break;
    case 1: // random byte
        b[lo + rng.below(len)] = static_cast<std::uint8_t>(rng.next());
        break;
    case 2: // interesting byte
        b[lo + rng.below(len)] = kInteresting[rng.below(sizeof kInteresting)];
        break;
    case 3: { // a 32-bit word with an interesting value
        if (len < 4)
            break;
        const std::size_t at = lo + rng.below(len - 3);
        const std::uint32_t v = kWords[rng.below(sizeof kWords / sizeof kWords[0])];
        for (int i = 0; i < 4; ++i)
            b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        break;
    }
    case 4: { // a run of digits becomes other digits (ids, counts, coordinates in JSON)
        for (int tries = 0; tries < 8; ++tries) {
            const std::size_t at = lo + rng.below(len);
            if (b[at] >= '0' && b[at] <= '9') {
                std::size_t end = at;
                while (end < hi && b[end] >= '0' && b[end] <= '9')
                    ++end;
                const std::uint8_t digit = rng.chance(50) ? '9' : static_cast<std::uint8_t>('0' + rng.below(10));
                for (std::size_t i = at; i < end; ++i)
                    b[i] = rng.chance(70) ? digit : static_cast<std::uint8_t>('0' + rng.below(10));
                break;
            }
        }
        break;
    }
    case 5: { // swap two bytes
        std::swap(b[lo + rng.below(len)], b[lo + rng.below(len)]);
        break;
    }
    case 6: { // overwrite a range with a copy of another range of the same input
        const std::size_t n = 1 + rng.below(std::min<std::size_t>(len, 32));
        const std::size_t from = lo + rng.below(len - n + 1), to = lo + rng.below(len - n + 1);
        std::vector<std::uint8_t> tmp(b.begin() + static_cast<std::ptrdiff_t>(from), b.begin() + static_cast<std::ptrdiff_t>(from + n));
        std::copy(tmp.begin(), tmp.end(), b.begin() + static_cast<std::ptrdiff_t>(to));
        break;
    }
    case 7: { // fill a range
        const std::size_t n = 1 + rng.below(std::min<std::size_t>(len, 16));
        const std::size_t at = lo + rng.below(len - n + 1);
        std::fill(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n),
                  kInteresting[rng.below(sizeof kInteresting)]);
        break;
    }
    case 8: { // delete a range
        const std::size_t n = 1 + rng.below(std::min<std::size_t>(len, 64));
        const std::size_t at = lo + rng.below(len - n + 1);
        b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
        break;
    }
    case 9: { // insert random bytes
        const std::size_t n = 1 + rng.below(16);
        const std::size_t at = lo + rng.below(len + 1);
        std::vector<std::uint8_t> ins(n);
        for (std::size_t i = 0; i < n; ++i)
            ins[i] = static_cast<std::uint8_t>(rng.next());
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
        break;
    }
    case 10: { // duplicate a range in place
        const std::size_t n = 1 + rng.below(std::min<std::size_t>(len, 64));
        const std::size_t at = lo + rng.below(len - n + 1);
        std::vector<std::uint8_t> dup(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), dup.begin(), dup.end());
        break;
    }
    default: // truncate
        b.resize(lo + rng.below(len));
        break;
    }
}

void mutate(Bytes &b, const std::vector<Bytes> &seeds, Rng &rng, std::size_t max_len) {
    const std::size_t rounds = 1 + rng.below(3);
    for (std::size_t r = 0; r < rounds; ++r) {
        if (b.empty()) {
            b.push_back(static_cast<std::uint8_t>(rng.next()));
            continue;
        }
        const std::size_t kind = rng.below(100);
        if (kind < 55) {
            // Structure-aware: edit one stored entry in place, then repair the checksums.
            const std::vector<fuzz::StoredEntry> es = fuzz::stored_entries(b);
            if (!es.empty()) {
                const fuzz::StoredEntry &e = es[rng.below(es.size())];
                if (e.size > 0) {
                    const std::size_t edits = 1 + rng.below(4);
                    for (std::size_t i = 0; i < edits; ++i)
                        mutate_range(b, e.data, e.data + e.size, rng, true);
                    fuzz::fix_up(b);
                    continue;
                }
            }
        }
        if (kind < 70 && !seeds.empty()) {
            // Splice a piece of another seed over a piece of this input.
            const Bytes &o = seeds[rng.below(seeds.size())];
            if (!o.empty()) {
                const std::size_t n = 1 + rng.below(std::min<std::size_t>(o.size(), 256));
                const std::size_t from = rng.below(o.size() - n + 1), to = rng.below(b.size());
                const std::size_t take = std::min(n, b.size() - to);
                std::copy(o.begin() + static_cast<std::ptrdiff_t>(from), o.begin() + static_cast<std::ptrdiff_t>(from + take),
                          b.begin() + static_cast<std::ptrdiff_t>(to));
                continue;
            }
        }
        mutate_range(b, 0, b.size(), rng, false); // anywhere: headers, directory, names
    }
    if (b.size() > max_len)
        b.resize(max_len);
}

} // namespace

int main(int argc, char **argv) {
    double seconds = 60;
    std::uint64_t iterations = 0, seed = 1;
    std::size_t max_len = 1u << 20;
    std::vector<std::string> inputs;
    bool replay = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (a == "--iterations" && i + 1 < argc)
            iterations = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seed" && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--max-len" && i + 1 < argc)
            max_len = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--out" && i + 1 < argc)
            g_out_dir = argv[++i];
        else if (a == "--replay")
            replay = true;
        else
            inputs.push_back(a);
    }
    std::vector<std::string> files;
    for (std::size_t i = 0; i < inputs.size(); ++i)
        collect(inputs[i], files);
    std::vector<Bytes> seeds;
    for (std::size_t i = 0; i < files.size(); ++i) {
        Bytes b;
        if (read_file(files[i], b) && b.size() <= max_len)
            seeds.push_back(b);
    }
    if (seeds.empty()) {
        std::fprintf(stderr, "usage: loomora_fuzz_loop [--seconds S] [--iterations N] [--seed N] [--max-len BYTES] [--out DIR] [--replay] seed-dir-or-file...\n");
        return 2;
    }
    std::signal(SIGABRT, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGFPE, on_signal);
    std::signal(SIGILL, on_signal);
    if (__sanitizer_set_death_callback != nullptr)
        __sanitizer_set_death_callback(on_death);

    if (replay) {
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            g_current = &seeds[i];
            LLVMFuzzerTestOneInput(seeds[i].empty() ? nullptr : &seeds[i][0], seeds[i].size());
        }
        std::printf("replayed %zu input(s)\n", seeds.size());
        return 0;
    }

    // Structured seeds: every seed that opens becomes an all-stored copy too, so the structure-aware
    // mutation can reach every entry of it.
    const std::size_t plain = seeds.size();
    for (std::size_t i = 0; i < plain; ++i) {
        Bytes s = fuzz::to_stored(seeds[i]);
        if (!s.empty() && s.size() <= max_len)
            seeds.push_back(s);
    }
    std::printf("%zu seed(s) (%zu plain, %zu all-stored), seed %llu, max length %zu bytes\n", seeds.size(), plain,
                seeds.size() - plain, static_cast<unsigned long long>(seed), max_len);

    g_seed = seed;
    Rng rng(seed);
    typedef std::chrono::steady_clock Clock;
    const Clock::time_point start = Clock::now();
    Clock::time_point last_report = start;
    std::uint64_t executed = 0;
    double slowest = 0;
    Bytes input;
    // Run every seed once, unmutated.
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        g_iteration = executed++;
        g_current = &seeds[i];
        LLVMFuzzerTestOneInput(seeds[i].empty() ? nullptr : &seeds[i][0], seeds[i].size());
    }
    for (;;) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if ((iterations != 0 && executed >= iterations) || (iterations == 0 && elapsed >= seconds))
            break;
        // Two thirds of the inputs start from an all-stored seed: every entry of it can be edited in place.
        input = seeds[(seeds.size() > plain && rng.chance(65)) ? plain + rng.below(seeds.size() - plain) : rng.below(seeds.size())];
        mutate(input, seeds, rng, max_len);
        g_current = &input;
        g_iteration = executed;
        const Clock::time_point t0 = Clock::now();
        LLVMFuzzerTestOneInput(input.empty() ? nullptr : &input[0], input.size());
        const double took = std::chrono::duration<double>(Clock::now() - t0).count();
        ++executed;
        slowest = std::max(slowest, took);
        if (took > 5.0) {
            save_current("input slower than 5 seconds");
            std::fprintf(stderr, "time limit: %.1f s for %zu bytes\n", took, input.size());
            return 1;
        }
        if (std::chrono::duration<double>(Clock::now() - last_report).count() >= 30) {
            last_report = Clock::now();
            std::printf("  %llu inputs in %.0f s (%.0f/s), slowest %.3f s\n", static_cast<unsigned long long>(executed),
                        elapsed, executed / std::max(elapsed, 0.001), slowest);
            std::fflush(stdout);
        }
    }
    const double total = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("done: %llu inputs in %.1f s = %.0f inputs/s, slowest %.3f s, no crash\n",
                static_cast<unsigned long long>(executed), total, executed / std::max(total, 0.001), slowest);
    const fuzz::Stats &st = fuzz::stats();
    std::printf("opened %lu, rejected %lu; most frequent first errors:", st.opened, st.rejected);
    std::vector<std::pair<unsigned long, std::string> > top;
    for (std::map<std::string, unsigned long>::const_iterator it = st.first_error.begin(); it != st.first_error.end(); ++it)
        top.push_back(std::make_pair(it->second, it->first));
    std::sort(top.rbegin(), top.rend());
    for (std::size_t i = 0; i < top.size() && i < 12; ++i)
        std::printf(" %s=%lu", top[i].second.c_str(), top[i].first);
    std::printf("\n");
    return 0;
}


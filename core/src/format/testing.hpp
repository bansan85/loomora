// Fault-injection hooks of the .loomora format code: an internal table of function pointers, all
// null in production, that the tests set to make one step fail (SC-008: a failed write, flush or
// rename must leave the previous file intact and no temporary file; an allocation failure must
// end in LOOMORA_ERR_NO_MEMORY). The production code calls the should_fail_* helpers at each such
// step; with null hooks they cost one comparison. Nothing here is exported from the shared library
// and nothing reads the environment.
//
// A hook returns true to make the step fail; the step then fails as a real failure would
// (FormatError with LOOMORA_ERR_IO for write, flush and rename; LOOMORA_ERR_NO_MEMORY for an
// allocation). `user` is passed to every hook. A test sets the hooks through ScopedHooks, which
// clears them when it goes out of scope, so one test never leaks into the next.
#ifndef LOOMORA_FORMAT_TESTING_HPP
#define LOOMORA_FORMAT_TESTING_HPP

#include <cstddef>
#include <cstdint>

namespace loomora {
namespace format {

struct Hooks {
    void *user;
    // Before every write of a sink (file or memory): `offset` is the position of the write.
    bool (*fail_write)(void *user, std::uint64_t offset, std::size_t len);
    // Before the flush to stable storage (fsync, FlushFileBuffers) of a file sink.
    bool (*fail_flush)(void *user);
    // Before the rename that replaces the target file.
    bool (*fail_rename)(void *user);
    // Before each attempt to delete the old file renamed aside by the Windows file sink (its
    // `<target>.old-xxxxxxxx` copy): true makes that attempt fail.
    bool (*fail_delete_aside)(void *user);
    // Before an allocation of `bytes` made by the format code worth injecting (buffer growth).
    bool (*fail_alloc)(void *user, std::size_t bytes);

    Hooks() : user(nullptr), fail_write(nullptr), fail_flush(nullptr), fail_rename(nullptr),
              fail_delete_aside(nullptr), fail_alloc(nullptr) {}
};

// The one table. Null hooks in production.
Hooks &hooks();

// Clears every hook.
void reset_hooks();

inline bool should_fail_write(std::uint64_t offset, std::size_t len) {
    const Hooks &h = hooks();
    return h.fail_write != nullptr && h.fail_write(h.user, offset, len);
}
inline bool should_fail_flush() {
    const Hooks &h = hooks();
    return h.fail_flush != nullptr && h.fail_flush(h.user);
}
inline bool should_fail_rename() {
    const Hooks &h = hooks();
    return h.fail_rename != nullptr && h.fail_rename(h.user);
}
inline bool should_fail_delete_aside() {
    const Hooks &h = hooks();
    return h.fail_delete_aside != nullptr && h.fail_delete_aside(h.user);
}
inline bool should_fail_alloc(std::size_t bytes) {
    const Hooks &h = hooks();
    return h.fail_alloc != nullptr && h.fail_alloc(h.user, bytes);
}

// Clears every hook when it is destroyed (tests).
class ScopedHooks {
  public:
    ScopedHooks() { reset_hooks(); }
    ~ScopedHooks() { reset_hooks(); }

  private:
    ScopedHooks(const ScopedHooks &);
    ScopedHooks &operator=(const ScopedHooks &);
};

} // namespace format
} // namespace loomora

#endif

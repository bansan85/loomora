// Factories of the file source and the file sink. Declared here so that the readers and writers
// can be written against them; defined, one per platform, in file_posix.cpp (POSIX) and
// file_win32.cpp (Windows). Each platform file is #if-guarded: it compiles to nothing elsewhere,
// so exactly one definition exists in any build.
//
// Paths are UTF-8 (the C API convention); the Windows file converts them to UTF-16.
#ifndef LOOMORA_FORMAT_IO_FILE_HPP
#define LOOMORA_FORMAT_IO_FILE_HPP

#include <memory>
#include <string>

#include "format/io/sink.hpp"
#include "format/io/source.hpp"

namespace loomora {
namespace format {

// Opens `utf8_path` read-only (never writes, never creates, shares read, write and delete on
// Windows so a concurrent atomic replace of the file succeeds: a reader opened before a save keeps
// reading the complete old file). Throws FormatError(LOOMORA_ERR_IO) when it cannot be opened or
// read, FormatError(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM) when the file is over 2 GiB on a target
// with LOOMORA_FMT_NO_LARGE_FILES.
std::unique_ptr<Source> open_file_source(const std::string &utf8_path);

// Creates a sink that writes `<utf8_target_path>.tmp-<random>` in the same directory as the
// target. commit() flushes it to stable storage (fsync, F_FULLFSYNC on macOS, FlushFileBuffers)
// and replaces the target atomically (rename, MoveFileExW); abort() (or destruction before commit)
// removes the temporary file. The target is created or replaced only by a successful commit().
// Honours the hooks of testing.hpp. Throws FormatError(LOOMORA_ERR_IO) when the temporary file
// cannot be created.
std::unique_ptr<Sink> create_file_sink(const std::string &utf8_target_path);

} // namespace format
} // namespace loomora

#endif

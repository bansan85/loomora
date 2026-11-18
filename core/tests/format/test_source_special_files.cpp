// open_file_source refuses a file that is not a regular file, and never waits for one. On POSIX a
// plain open(O_RDONLY) of a named pipe blocks until another process opens the pipe for writing,
// so a path that names a FIFO would hang the caller before the "not a regular file" check ran.
// (Windows has no equivalent path to a FIFO; the test is compiled for POSIX hosts only.)
#include <catch2/catch.hpp>

#ifndef _WIN32

#include <chrono>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../include/loomora.h"
#include "format/io/file.hpp"
#include "format/report.hpp"
#include "fs_test_util.hpp"

using namespace loomora::format;

TEST_CASE("open_file_source on a named pipe fails at once instead of waiting for a writer",
          "[format][io][file][fifo]") {
    fstest::ScratchDir dir("fifo");
    const std::string fifo = dir.file("pipe.loomora");
    REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);

    // A helper process that opens the pipe for writing after 600 ms. A blocking open in the code
    // under test would only return once that happened.
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        usleep(600 * 1000);
        const int fd = open(fifo.c_str(), O_WRONLY);
        if (fd >= 0)
            close(fd);
        _exit(0);
    }

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int code = 0;
    try {
        open_file_source(fifo);
    } catch (const FormatError &e) {
        code = e.code();
    }
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // Hold a reader open so the helper's blocking open of the writing end can finish, then reap it.
    const int fd = open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    int status = 0;
    waitpid(child, &status, 0);
    if (fd >= 0)
        close(fd);

    CHECK(code == LOOMORA_ERR_IO);
    CHECK(ms < 300);
}

#endif // !_WIN32

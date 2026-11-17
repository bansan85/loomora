// Compiled as C++98 (-std=c++98 -pedantic-errors, see core/tests/CMakeLists.txt) to prove that the
// public header is valid C++98. No standard library header: the header must stand alone.
#include "loomora.h"

extern "C" int loomora_header_cpp98_check(void);

extern "C" int loomora_header_cpp98_check(void) {
    loomora_project *project = 0;
    loomora_editor *editor = 0;
    loomora_report *report = 0;
    loomora_progress_fn progress = 0;
    loomora_sink_fn sink = 0;
    int32_t major = 0;
    int32_t minor = 0;
    uint8_t random16[16];
    char uuid[37];
    for (int i = 0; i < 16; ++i)
        random16[i] = static_cast<uint8_t>(i);
    (void)project;
    (void)editor;
    (void)report;
    (void)progress;
    (void)sink;
    if (loomora_format_version(&major, &minor) != 1u)
        return 1;
    if (LOOMORA_ERR_IO != -10 || LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM != -22)
        return 2;
    if (loomora_uuid4_from_random(random16, uuid) != 0)
        return 3;
    return loomora_abi_version() == 1u ? 0 : 4;
}

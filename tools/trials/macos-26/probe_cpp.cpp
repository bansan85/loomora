// Spike S11 (T132): C++11 side of the probe. It uses what the Loomora backend uses from the library
// (std::string, containers, <memory>, <functional>, a lambda, std::to_string), so that the link
// really needs the C++ runtime: otool -L then shows whether libc++ comes from a dylib.
#include "probe_cpp.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" int probe_cpp_run(char *out, unsigned long size)
{
    std::unique_ptr<std::vector<int> > values(new std::vector<int>{1, 2, 3, 4});
    std::function<int(int)> twice = [](int x) { return 2 * x; };
    int sum = 0;
    for (auto v : *values) {
        sum += twice(v);
    }
    std::reverse(values->begin(), values->end());
    const std::string text = "cpp11 sum=" + std::to_string(sum) + " first=" + std::to_string(values->front());
    std::snprintf(out, static_cast<size_t>(size), "%s", text.c_str());
    return sum;
}

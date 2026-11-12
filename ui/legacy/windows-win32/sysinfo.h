// System information for the Windows Legacy UI: Windows version, end-of-life test, platform id.
#ifndef LOOMORA_WIN32_SYSINFO_H
#define LOOMORA_WIN32_SYSINFO_H

#include <string>

namespace sysinfo {

// True when the backend says this Windows version is no longer maintained (loomora_is_eol). The
// version is the NT version, except that Windows 11 (NT 10.0, build 22000 or later) is passed as
// 11.0, which is what the backend expects. False when the version cannot be read.
bool IsEndOfLife();

// Platform id of the running system for the probe record: "windows-<system>-<architecture>", for
// example "windows-11-x64" or "windows-2000-x86". The architecture is the one of the operating
// system, not of this process (a 32-bit executable under WOW64 reports "x64"), the same way the
// Linux UIs report the machine of the kernel. Matches ^[a-z0-9][a-z0-9._-]*$.
std::string PlatformId();

} // namespace sysinfo

#endif

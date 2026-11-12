#include "sysinfo.h"

#include <windows.h>

#include "dynload.h"
#include "loomora.h"

namespace sysinfo {

namespace {

struct Version {
    bool valid;
    int major;
    int minor;
    unsigned long build;
};

typedef LONG(NTAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
typedef BOOL(WINAPI *GetVersionExFn)(OSVERSIONINFOW *);

// ntdll!RtlGetVersion reports the true version on every NT system (it exists since Windows 2000
// and is not subject to the compatibility lie of GetVersionEx on Windows 8.1 and later). Loaded
// at run time because it is not an import of the Win32 API; GetVersionExW (deprecated, also looked
// up at run time) is the fallback: it returns 6.2 on Windows 8.1 and later for an executable
// without a matching manifest, which is only acceptable because ntdll always has RtlGetVersion
// there.
Version ReadVersion() {
    Version version = Version();
    OSVERSIONINFOW info = OSVERSIONINFOW();
    info.dwOSVersionInfoSize = sizeof(info);

    const RtlGetVersionFn rtl =
        dynload::Resolve<RtlGetVersionFn>(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    bool ok = rtl != NULL && rtl(&info) >= 0; // NTSTATUS: negative = failure
    if (!ok) {
        info = OSVERSIONINFOW();
        info.dwOSVersionInfoSize = sizeof(info);
        const GetVersionExFn getVersionEx =
            dynload::Resolve<GetVersionExFn>(GetModuleHandleW(L"kernel32.dll"), "GetVersionExW");
        ok = getVersionEx != NULL && getVersionEx(&info) != 0;
    }
    if (ok) {
        version.valid = true;
        version.major = static_cast<int>(info.dwMajorVersion);
        version.minor = static_cast<int>(info.dwMinorVersion);
        version.build = info.dwBuildNumber;
    }
    return version;
}

bool IsWindows11(const Version &version) {
    return version.major == 10 && version.minor == 0 && version.build >= 22000;
}

// "x86", "x64", "arm64", ... of the operating system. GetNativeSystemInfo (Windows XP and later)
// sees through WOW64; on Windows 2000, which has no WOW64, GetSystemInfo is already native.
const char *NativeArchitecture() {
    typedef void(WINAPI * NativeInfoFn)(SYSTEM_INFO *);
    SYSTEM_INFO info = SYSTEM_INFO();
    const NativeInfoFn native =
        dynload::Resolve<NativeInfoFn>(GetModuleHandleW(L"kernel32.dll"), "GetNativeSystemInfo");
    if (native != NULL)
        native(&info);
    else
        GetSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";
    case PROCESSOR_ARCHITECTURE_IA64:
        return "ia64";
    case 5: // PROCESSOR_ARCHITECTURE_ARM, not in the Windows 2000 headers
        return "arm";
    case 12: // PROCESSOR_ARCHITECTURE_ARM64
        return "arm64";
    }
    return "unknown";
}

std::string Number(int value) { return std::to_string(static_cast<long long>(value)); }

// The system part of the platform id. Server editions share the number of their client edition.
std::string SystemName(const Version &version) {
    if (!version.valid)
        return "unknown";
    if (IsWindows11(version))
        return "11";
    switch (version.major * 100 + version.minor) {
    case 500:
        return "2000";
    case 501:
        return "xp";
    case 600:
        return "vista";
    case 601:
        return "7";
    case 602:
        return "8";
    case 603:
        return "8.1";
    case 1000:
        return "10";
    }
    // Windows XP x64 and Windows Server 2003 are both NT 5.2, and so on for versions to come.
    return "nt" + Number(version.major) + "." + Number(version.minor);
}

} // namespace

// The version is read on each call rather than cached in a function-local static: the thread-safe
// initialization of such a static pulls the condition variable functions of libstdc++ (Windows
// Vista and later) into the import table. Keep the whole UI free of them.
bool IsEndOfLife() {
    const Version version = ReadVersion();
    if (!version.valid)
        return false;
    const int major = IsWindows11(version) ? 11 : version.major;
    const int minor = IsWindows11(version) ? 0 : version.minor;
    return loomora_is_eol(LOOMORA_FAMILY_WINDOWS, major, minor) == 1;
}

std::string PlatformId() {
    return "windows-" + SystemName(ReadVersion()) + "-" + NativeArchitecture();
}

} // namespace sysinfo

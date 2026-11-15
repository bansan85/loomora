// The Windows version and architecture, in the forms the backend and the probe record expect:
// loomora_is_eol takes (family, major, minor) and the platform id is "windows-<system>-<arch>".
// Reading the system is all this file does; the end-of-life verdict comes from the backend.
using System;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Loomora.WinUI;

internal static class SystemInfo
{
    // .NET reports the true version (it does not apply the compatibility lie of GetVersionEx).
    private static bool IsWindows11(Version version) =>
        version.Major == 10 && version.Minor == 0 && version.Build >= 22000;

    private static string SystemName(Version version)
    {
        if (IsWindows11(version))
            return "11";
        if (version.Major == 10 && version.Minor == 0)
            return "10";
        return "nt" + version.Major.ToString(CultureInfo.InvariantCulture) + "." +
               version.Minor.ToString(CultureInfo.InvariantCulture);
    }

    private static string Architecture() => RuntimeInformation.OSArchitecture switch
    {
        System.Runtime.InteropServices.Architecture.X64 => "x64",
        System.Runtime.InteropServices.Architecture.Arm64 => "arm64",
        System.Runtime.InteropServices.Architecture.X86 => "x86",
        System.Runtime.InteropServices.Architecture.Arm => "arm",
        _ => "unknown",
    };

    // The OS architecture, like the Legacy UI: an x64 build under emulation on ARM64 reports arm64.
    internal static string PlatformId() =>
        "windows-" + SystemName(Environment.OSVersion.Version) + "-" + Architecture();

    internal static bool IsEndOfLife()
    {
        Version version = Environment.OSVersion.Version;
        int major = IsWindows11(version) ? 11 : version.Major;
        int minor = IsWindows11(version) ? 0 : version.Minor;
        return Native.IsEol(Native.FamilyWindows, major, minor) == 1;
    }
}

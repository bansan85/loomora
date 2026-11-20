// Windows family and version for the backend's end-of-life rule (LOOMORA_FAMILY_WINDOWS). Same
// reading as the WinUI 3 Modern UI: .NET reports the true version, and Windows 11 is 10.0 build 22000+.
namespace Loomora.Maui;

internal static class PlatformSystem
{
    internal const int Family = 1; // LOOMORA_FAMILY_WINDOWS

    internal static (int Major, int Minor) Version()
    {
        System.Version version = Environment.OSVersion.Version;
        if (version.Major == 10 && version.Minor == 0 && version.Build >= 22000)
            return (11, 0);
        return (version.Major, version.Minor);
    }
}

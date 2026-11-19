// The end-of-life verdict of the running system. The version and the family come from the platform
// (Platforms/<os>/PlatformSystem.cs); the verdict itself comes from the backend.
namespace Loomora.Maui;

internal static class SystemInfo
{
    internal static bool IsEndOfLife()
    {
        (int major, int minor) = PlatformSystem.Version();
        return Native.IsEol(PlatformSystem.Family, major, minor) == 1;
    }
}

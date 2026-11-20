// Android family and version for the backend's end-of-life rule (LOOMORA_FAMILY_ANDROID), read from
// Build.VERSION.RELEASE as the Android Modern UI does: major and minor, -1 when not numeric.
using Android.OS;

namespace Loomora.Maui;

internal static class PlatformSystem
{
    internal const int Family = 4; // LOOMORA_FAMILY_ANDROID

    internal static (int Major, int Minor) Version()
    {
        string[] parts = (Build.VERSION.Release ?? string.Empty).Split('.');
        int major = int.TryParse(parts[0], out int m) ? m : -1;
        int minor = parts.Length > 1 && int.TryParse(parts[1], out int n) ? n : -1;
        return (major, minor);
    }
}

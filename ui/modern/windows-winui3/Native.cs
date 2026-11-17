// The C API of the C++11 backend (core/include/loomora.h) as seen from C#: P/Invoke generated at
// compile time by LibraryImport, no C++/CLI and no C++/WinRT. Strings are UTF-8 on the native side.
// This file only converts between UTF-16 (.NET) and UTF-8 (backend); it builds no greeting itself.
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Loomora.WinUI;

internal static unsafe partial class Native
{
    internal const uint ExpectedAbiVersion = 1; // LOOMORA_ABI_VERSION

    // loomora_* family and variant constants of loomora.h
    internal const int FamilyWindows = 1;
    internal const int UiModern = 2;
    internal const int CoreNative = 1;

    private const string Library = "loomora";

    [LibraryImport(Library, EntryPoint = "loomora_abi_version")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial uint AbiVersion();

    [LibraryImport(Library, EntryPoint = "loomora_greet")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    private static partial int GreetRaw(byte* input, nuint inputLength, byte* output, nuint capacity);

    [LibraryImport(Library, EntryPoint = "loomora_is_eol")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial int IsEol(int family, int major, int minor);

    // The text is static and owned by the backend: it is read, never freed (hence the raw pointer).
    [LibraryImport(Library, EntryPoint = "loomora_eol_warning")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    private static partial nint EolWarningRaw();

    [LibraryImport(Library, EntryPoint = "loomora_build_compiler")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    private static partial nint BuildCompilerRaw();

    [LibraryImport(Library, EntryPoint = "loomora_probe_record")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    private static partial nuint ProbeRecordRaw(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string platformId, int uiVariant, int coreKind,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string displayedText, int displayed, uint displayMs,
        long rssKb, int eolWarningShown, [MarshalAs(UnmanagedType.LPUTF8Str)] string disabledFeatures,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string timestampUtc, byte* buffer, nuint capacity);

    // ---- the .loomora project C API (loomora_project.h), used by the project probe modes only ----
    // Handles (loomora_project*, loomora_report*) are opaque: nint. The callbacks (progress and
    // sink) are passed as native function pointers (nint); the sink comes from a delegate kept
    // alive by the caller for the length of the call.

    internal const int ErrIo = -10;                  // LOOMORA_ERR_IO
    internal const int ErrNoMemory = -16;            // LOOMORA_ERR_NO_MEMORY
    internal const int ErrInvalid = -3;              // LOOMORA_ERR_INVALID
    internal const int ErrInternal = -4;             // LOOMORA_ERR_INTERNAL
    internal const int ErrTooLargeForPlatform = -22; // LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM
    internal const int ProfileFull = 1;              // LOOMORA_PROFILE_FULL
    internal const int ProfileLight = 2;             // LOOMORA_PROFILE_LIGHT

    // int32_t (*loomora_sink_fn)(void *user, const uint8_t *data, size_t len): 0 to go on.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int SinkCallback(nint user, byte* data, nuint length);

    [LibraryImport(Library, EntryPoint = "loomora_project_open_path")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial int ProjectOpenPath([MarshalAs(UnmanagedType.LPUTF8Str)] string path,
                                                nint progress, nint user, out nint project,
                                                out nint report);

    [LibraryImport(Library, EntryPoint = "loomora_project_close")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial void ProjectClose(nint project);

    [LibraryImport(Library, EntryPoint = "loomora_project_is_light")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial int ProjectIsLight(nint project);

    [LibraryImport(Library, EntryPoint = "loomora_validate_memory")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial int ValidateMemory(byte* data, nuint length, nint progress, nint user,
                                               out nint report);

    [LibraryImport(Library, EntryPoint = "loomora_project_export_stream")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    private static partial int ProjectExportStreamRaw(nint project, int profile, uint* resultIds,
                                                      nuint resultCount,
                                                      [MarshalAs(UnmanagedType.LPUTF8Str)] string modifiedAt,
                                                      uint options, nint sink, nint user,
                                                      out nint report);

    [LibraryImport(Library, EntryPoint = "loomora_report_count")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial long ReportCount(nint report);

    [LibraryImport(Library, EntryPoint = "loomora_report_error_count")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial long ReportErrorCount(nint report);

    [LibraryImport(Library, EntryPoint = "loomora_report_free")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory)]
    internal static partial void ReportFree(nint report);

    // Every result of the project (no subset, no options) into `sink`, called once per chunk. The
    // delegate is kept alive until the native call returns.
    internal static int ProjectExportStream(nint project, int profile, string modifiedAt,
                                            SinkCallback sink, out nint report)
    {
        nint callback = Marshal.GetFunctionPointerForDelegate(sink);
        int rc = ProjectExportStreamRaw(project, profile, null, 0, modifiedAt, 0, callback, 0,
                                        out report);
        GC.KeepAlive(sink);
        return rc;
    }

    // "Bonjour " + input, computed by the backend; null when the backend reports an error.
    internal static string? Greet(string input)
    {
        byte[] utf8 = Encoding.UTF8.GetBytes(input);
        byte[] output = new byte[utf8.Length + 64]; // "Bonjour " + input
        fixed (byte* inputPointer = utf8.Length > 0 ? utf8 : new byte[1]) // never a null pointer
        fixed (byte* outputPointer = output)
        {
            int written = GreetRaw(inputPointer, (nuint)utf8.Length, outputPointer, (nuint)output.Length);
            return written < 0 ? null : Encoding.UTF8.GetString(output, 0, written);
        }
    }

    internal static string EolWarning() => Marshal.PtrToStringUTF8(EolWarningRaw()) ?? string.Empty;

    internal static string BuildCompiler() => Marshal.PtrToStringUTF8(BuildCompilerRaw()) ?? string.Empty;

    // The ProbeResult JSON line (ASCII, without a line end), or null if the backend cannot build it.
    internal static string? ProbeRecord(string platformId, string input, string displayedText,
                                        bool displayed, uint displayMs, long rssKb,
                                        bool eolWarningShown, string disabledFeatures,
                                        string timestampUtc)
    {
        // First call with no room measures, second one fills the buffer.
        nuint needed = ProbeRecordRaw(platformId, UiModern, CoreNative, input, displayedText,
                                      displayed ? 1 : 0, displayMs, rssKb, eolWarningShown ? 1 : 0,
                                      disabledFeatures, timestampUtc, null, 0);
        if (needed == 0)
            return null;
        byte[] line = new byte[(int)needed];
        fixed (byte* linePointer = line)
        {
            nuint written = ProbeRecordRaw(platformId, UiModern, CoreNative, input, displayedText,
                                           displayed ? 1 : 0, displayMs, rssKb,
                                           eolWarningShown ? 1 : 0, disabledFeatures, timestampUtc,
                                           linePointer, needed);
            return written == needed ? Encoding.UTF8.GetString(line) : null;
        }
    }
}

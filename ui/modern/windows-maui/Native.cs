// The C API of the C++11 backend (core/include/loomora.h) as seen from C#, through LibraryImport:
// the same calls as the WinUI 3 Modern UI (ui/modern/windows-winui3/Native.cs). Strings are UTF-8
// on the native side. This file only converts between UTF-16 (.NET) and UTF-8 (backend).
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Loomora.Maui;

internal static unsafe partial class Native
{
    private const string Library = "loomora";

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
}

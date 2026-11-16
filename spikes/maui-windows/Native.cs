// Spike: the three calls of loomora.h used by the WinUI 3 Modern UI (ui/modern/windows-winui3/Native.cs),
// through LibraryImport. No logic here: the greeting is built by the backend.
using System;
using System.Text;

namespace LoomoraMauiSpike;

internal static unsafe partial class Native
{
    private const string Library = "loomora";

    [System.Runtime.InteropServices.LibraryImport(Library, EntryPoint = "loomora_abi_version")]
    [System.Runtime.InteropServices.DefaultDllImportSearchPaths(System.Runtime.InteropServices.DllImportSearchPath.AssemblyDirectory)]
    internal static partial uint AbiVersion();

    [System.Runtime.InteropServices.LibraryImport(Library, EntryPoint = "loomora_greet")]
    [System.Runtime.InteropServices.DefaultDllImportSearchPaths(System.Runtime.InteropServices.DllImportSearchPath.AssemblyDirectory)]
    private static partial int GreetRaw(byte* input, nuint inputLength, byte* output, nuint capacity);

    [System.Runtime.InteropServices.LibraryImport(Library, EntryPoint = "loomora_is_eol")]
    [System.Runtime.InteropServices.DefaultDllImportSearchPaths(System.Runtime.InteropServices.DllImportSearchPath.AssemblyDirectory)]
    internal static partial int IsEol(int family, int major, int minor);

    internal static string? Greet(string input)
    {
        byte[] utf8 = Encoding.UTF8.GetBytes(input);
        byte[] output = new byte[utf8.Length + 64];
        fixed (byte* inputPointer = utf8.Length > 0 ? utf8 : new byte[1])
        fixed (byte* outputPointer = output)
        {
            int written = GreetRaw(inputPointer, (nuint)utf8.Length, outputPointer, (nuint)output.Length);
            return written < 0 ? null : Encoding.UTF8.GetString(output, 0, written);
        }
    }
}

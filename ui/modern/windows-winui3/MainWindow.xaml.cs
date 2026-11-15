// Main window of the Windows Modern UI: one TextBox pre-filled with "Coucou" (limited to 1,000
// characters), a wrapping TextBlock with the greeting of the backend, an InfoBar for the
// end-of-life warning. No application logic: the greeting, the end-of-life verdict and its text come
// from Native (the C API of the backend); this file wires controls and converts text.
using System;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Windows.Graphics;

namespace Loomora.WinUI;

public sealed partial class MainWindow : Window
{
    private const int MaxChars = 1000;
    private const string DefaultInput = "Coucou";
    private const string GreetingError = "(error)";

    // Size of the window at 96 DPI.
    private const int WindowWidth = 560;
    private const int WindowHeight = 360;

    internal bool EolWarningShown { get; private set; }

    // The text of the field as the backend gets it: WinUI keeps a line break as a lone carriage
    // return, converted back to the line feed that was set.
    internal string InputText => InputBox.Text.Replace("\r\n", "\n").Replace('\r', '\n');

    internal string GreetingTextValue => GreetingText.Text;

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr window);

    public MainWindow(string? initialInput)
    {
        InitializeComponent();
        ResizeForDpi();

        InputBox.Text = Truncated(initialInput ?? DefaultInput); // raises TextChanged: greeting shown
        UpdateGreeting();

        if (SystemInfo.IsEndOfLife())
        {
            EolWarning.Message = Native.EolWarning();
            EolWarning.IsOpen = true;
            EolWarningShown = true;
        }
    }

    private void ResizeForDpi()
    {
        IntPtr handle = Win32Interop.GetWindowFromWindowId(AppWindow.Id);
        uint dpi = GetDpiForWindow(handle);
        double scale = dpi == 0 ? 1.0 : dpi / 96.0;
        AppWindow.Resize(new SizeInt32((int)(WindowWidth * scale), (int)(WindowHeight * scale)));
    }

    // At most MaxChars UTF-16 units (what MaxLength lets a user type, which does not apply to text
    // set by the program), never cutting a surrogate pair in two.
    private static string Truncated(string text)
    {
        if (text.Length <= MaxChars)
            return text;
        int length = char.IsHighSurrogate(text[MaxChars - 1]) ? MaxChars - 1 : MaxChars;
        return text.Substring(0, length);
    }

    // Asks the backend for the greeting of the field content and shows it.
    private void UpdateGreeting() => GreetingText.Text = Native.Greet(InputText) ?? GreetingError;

    private void InputBox_TextChanged(object sender, TextChangedEventArgs e) => UpdateGreeting();

    // The field behaves as a single-line field: Enter inserts nothing.
    private void InputBox_PreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Enter)
            e.Handled = true;
    }
}

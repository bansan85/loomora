// Main page of the MAUI Windows and Android UI: see MainPage.xaml. No application logic: the
// greeting and the end-of-life verdict come from Native (the C API of the backend); this file wires
// controls and shows what the backend returns.
namespace Loomora.Maui;

public partial class MainPage : ContentPage
{
    private const string DefaultInput = "Coucou";
    private const string GreetingError = "(error)";

    public MainPage()
    {
        InitializeComponent();

        InputBox.Text = DefaultInput; // raises TextChanged: greeting shown
        UpdateGreeting();

        if (SystemInfo.IsEndOfLife())
        {
            EolText.Text = Native.EolWarning();
            EolWarning.IsVisible = true;
        }
    }

    // Asks the backend for the greeting of the field content and shows it.
    private void UpdateGreeting() => GreetingText.Text = Native.Greet(InputBox.Text ?? string.Empty) ?? GreetingError;

    private void OnInputChanged(object? sender, TextChangedEventArgs e) => UpdateGreeting();

    private void OnDismissWarning(object? sender, EventArgs e) => EolWarning.IsVisible = false;
}

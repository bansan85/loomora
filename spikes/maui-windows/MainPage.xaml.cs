namespace LoomoraMauiSpike;

public partial class MainPage : ContentPage
{
    public MainPage()
    {
        InitializeComponent();
        AbiLabel.Text = $"ABI loomora: {Native.AbiVersion()} (attendu 1)";
    }

    private void OnGreetClicked(object? sender, EventArgs e)
    {
        GreetLabel.Text = Native.Greet(NameEntry.Text ?? string.Empty) ?? "erreur backend";
    }
}

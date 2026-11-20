using Avalonia.Controls;
using Avalonia.Interactivity;
using SkyRecompiled.Launcher.ViewModels;

namespace SkyRecompiled.Launcher.Views;

public partial class PlayView : UserControl
{
    public PlayView()
    {
        InitializeComponent();
    }

    private async void OnBrowseRomClicked(object? sender, RoutedEventArgs e)
    {
        if (DataContext is MainViewModel vm)
        {
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel != null)
            {
                await vm.PickRomAsync(topLevel.StorageProvider);
            }
        }
    }
}

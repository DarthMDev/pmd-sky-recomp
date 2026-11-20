using System;
using System.Threading.Tasks;
using Avalonia.Controls;
using SkyRecompiled.Launcher.ViewModels;

namespace SkyRecompiled.Launcher.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (DataContext is MainViewModel vm)
        {
            vm.WireClipboard(CopyTextToClipboardAsync);
        }
    }

    private async Task<bool> CopyTextToClipboardAsync(string text)
    {
        var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
        if (clipboard == null)
            return false;

        try
        {
            await clipboard.SetTextAsync(text);
            return true;
        }
        catch
        {
            return false;
        }
    }
}

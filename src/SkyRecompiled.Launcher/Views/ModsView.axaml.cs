using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using SkyRecompiled.Launcher.ViewModels;

namespace SkyRecompiled.Launcher.Views;

public partial class ModsView : UserControl
{
    public ModsView()
    {
        InitializeComponent();
    }

    private async void OnImportModClicked(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ModsViewModel vm)
        {
            var topLevel = TopLevel.GetTopLevel(this);
            if (topLevel == null) return;

            var folders = await topLevel.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = "Select a Mod Folder (containing romfs/ and optional mod.toml)",
                AllowMultiple = false,
            });
            if (folders.Count > 0)
            {
                vm.ImportModFolder(folders[0].Path.LocalPath);
            }
        }
    }
}

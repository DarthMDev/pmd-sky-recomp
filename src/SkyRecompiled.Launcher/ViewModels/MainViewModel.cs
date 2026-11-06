using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Platform.Storage;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    [ObservableProperty]
    private string? _romPath;

    [ObservableProperty]
    private string _romRegion = "None";

    [ObservableProperty]
    private string _romSha1 = "";

    [ObservableProperty]
    private bool _isRomValid;

    [ObservableProperty]
    private bool _isRomLoaded;

    [ObservableProperty]
    private bool _isProcessing;

    [ObservableProperty]
    private double _progressValue;

    [ObservableProperty]
    private string _progressText = "Ready";

    [ObservableProperty]
    private string _statusMessage = "Select your Explorers of Sky Nintendo DS ROM (.nds) to begin.";

    public MainViewModel()
    {
        try
        {
            // Check for previously remembered ROM
            var remembered = RomPreparationService.LoadSavedRomPath();
            if (!string.IsNullOrEmpty(remembered) && File.Exists(remembered))
            {
                _ = InitializeSavedRomAsync(remembered);
            }
        }
        catch
        {
            // Gracefully ignore startup read errors
        }
    }

    private async Task InitializeSavedRomAsync(string path)
    {
        IsProcessing = true;
        ProgressText = "Verifying remembered ROM...";
        ProgressValue = 10;

        var progress = new Progress<(double Progress, string Status)>(p =>
        {
            ProgressValue = p.Progress;
            ProgressText = p.Status;
        });

        var info = await RomPreparationService.VerifyRomAsync(path, progress);
        ApplyRomInfo(info);
        IsProcessing = false;
    }

    private void ApplyRomInfo(RomInfo info)
    {
        RomPath = info.Path;
        RomRegion = info.Region;
        RomSha1 = info.Sha1;
        IsRomValid = info.IsValid;
        IsRomLoaded = info.IsValid;

        if (info.IsValid)
        {
            StatusMessage = $"Verified Clean ROM: Pokémon Mystery Dungeon: Explorers of Sky [{info.Region}]";
            ProgressText = "Ready to Play";
            ProgressValue = 100;
            RomPreparationService.SaveRomPath(info.Path);
        }
        else
        {
            StatusMessage = "Invalid ROM: Selected file is not a verified clean USA or Europe Explorers of Sky dump.";
            ProgressText = "Verification Failed";
            ProgressValue = 0;
        }
    }

    [RelayCommand]
    public async Task PickRomAsync(IStorageProvider? storageProvider)
    {
        if (storageProvider == null) return;

        var fileTypes = new FilePickerFileType[]
        {
            new("Nintendo DS ROM (*.nds)")
            {
                Patterns = new[] { "*.nds", "*.NDS" }
            },
            new("All Files (*.*)")
            {
                Patterns = new[] { "*.*" }
            }
        };

        var files = await storageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky ROM",
            AllowMultiple = false,
            FileTypeFilter = fileTypes
        });

        if (files.Count > 0)
        {
            var chosenPath = files[0].Path.LocalPath;
            IsProcessing = true;
            ProgressValue = 0;
            ProgressText = "Starting ROM verification...";

            var progress = new Progress<(double Progress, string Status)>(p =>
            {
                ProgressValue = p.Progress;
                ProgressText = p.Status;
            });

            try
            {
                var info = await RomPreparationService.VerifyRomAsync(chosenPath, progress);
                ApplyRomInfo(info);
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error verifying ROM: {ex.Message}";
                ProgressText = "Error";
                IsRomValid = false;
            }
            finally
            {
                IsProcessing = false;
            }
        }
    }

    [RelayCommand]
    public async Task LaunchGameAsync()
    {
        if (string.IsNullOrEmpty(RomPath) || !IsRomValid) return;

        IsProcessing = true;
        ProgressValue = 20;
        ProgressText = "Configuring static recompilation environment...";
        await Task.Delay(250);

        ProgressValue = 60;
        ProgressText = "Launching game engine (nds_runner)...";
        await Task.Delay(250);

        try
        {
            var proc = RomPreparationService.LaunchGame(RomPath);
            ProgressValue = 100;
            ProgressText = "Game Running";
            StatusMessage = "SkyRecompiled is active. Enjoy your dungeon exploration!";

            // Optionally minimize or monitor
        }
        catch (Exception ex)
        {
            StatusMessage = $"Failed to launch engine: {ex.Message}";
            ProgressText = "Launch Error";
        }
        finally
        {
            IsProcessing = false;
        }
    }

    [RelayCommand]
    public void OpenSaveFolder()
    {
        var dir = RomPreparationService.GetUserStorageDir();
        Directory.CreateDirectory(dir);

        if (OperatingSystem.IsMacOS())
            Process.Start("open", $"\"{dir}\"");
        else if (OperatingSystem.IsWindows())
            Process.Start("explorer.exe", $"\"{dir}\"");
        else
            Process.Start("xdg-open", $"\"{dir}\"");
    }

    [RelayCommand]
    public void ResetRom()
    {
        RomPath = null;
        IsRomLoaded = false;
        IsRomValid = false;
        ProgressValue = 0;
        ProgressText = "Ready";
        StatusMessage = "Select your Explorers of Sky Nintendo DS ROM (.nds) to begin.";
    }
}

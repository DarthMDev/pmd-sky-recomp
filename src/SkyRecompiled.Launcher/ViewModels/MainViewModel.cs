using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private Process? _activeProcess;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private string? _romPath;

    [ObservableProperty]
    private string _romRegion = "None";

    [ObservableProperty]
    private string _romSha1 = "";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private bool _isRomValid;

    [ObservableProperty]
    private bool _isRomLoaded;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private bool _isProcessing;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private bool _isGameRunning;

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

    private bool CanLaunchGame() => !IsGameRunning && !IsProcessing && IsRomValid && !string.IsNullOrEmpty(RomPath);

    [RelayCommand(CanExecute = nameof(CanLaunchGame))]
    public Task LaunchGameAsync()
    {
        if (string.IsNullOrEmpty(RomPath) || !IsRomValid) return Task.CompletedTask;
        if (IsGameRunning || (_activeProcess is { HasExited: false })) return Task.CompletedTask;

        IsProcessing = true;
        ProgressValue = 50;
        ProgressText = "Launching game engine (nds_runner)...";

        try
        {
            var proc = RomPreparationService.LaunchGame(RomPath);
            _activeProcess = proc;
            IsGameRunning = true;
            int exitedHandled = 0;

            void HandleExit()
            {
                if (Interlocked.Exchange(ref exitedHandled, 1) != 0) return;
                var exitCode = 0;
                try { exitCode = proc.ExitCode; } catch { }
                void ApplyExitUi()
                {
                    if (ReferenceEquals(_activeProcess, proc))
                        _activeProcess = null;
                    IsGameRunning = false;
                    ProgressValue = exitCode == 0 ? 100 : 0;
                    ProgressText = exitCode == 0 ? "Ready to Play" : $"Engine Exited ({exitCode})";
                    StatusMessage = exitCode == 0
                        ? $"Verified Clean ROM: Pokémon Mystery Dungeon: Explorers of Sky [{RomRegion}]"
                        : $"nds_runner exited with code {exitCode}. Check nds_runner.log for details.";
                }
                if (Application.Current != null)
                    Dispatcher.UIThread.Post(ApplyExitUi);
                else
                    ApplyExitUi();
            }

            proc.EnableRaisingEvents = true;
            proc.Exited += (_, _) => HandleExit();
            if (proc.HasExited)
                HandleExit();

            ProgressValue = 100;
            ProgressText = "Game Running";
            StatusMessage = "SkyRecompiled is active. Enjoy your dungeon exploration!";
        }
        catch (Exception ex)
        {
            IsGameRunning = false;
            _activeProcess = null;
            StatusMessage = $"Failed to launch engine: {ex.Message}";
            ProgressText = "Launch Error";
        }
        finally
        {
            IsProcessing = false;
        }
        return Task.CompletedTask;
    }

    [RelayCommand]
    public void OpenSaveFolder()
    {
        var dir = RomPreparationService.GetUserStorageDir();
        Directory.CreateDirectory(dir);

        var fileName = OperatingSystem.IsMacOS() ? "open"
            : OperatingSystem.IsWindows() ? "explorer.exe"
            : "xdg-open";
        var psi = new ProcessStartInfo
        {
            FileName = fileName,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add(dir);
        Process.Start(psi);
    }

    [RelayCommand]
    public void ResetRom()
    {
        RomPreparationService.ClearSavedRomPath();
        RomPath = null;
        RomRegion = "None";
        RomSha1 = "";
        IsRomLoaded = false;
        IsRomValid = false;
        ProgressValue = 0;
        ProgressText = "Ready";
        StatusMessage = "Select your Explorers of Sky Nintendo DS ROM (.nds) to begin.";
    }
}

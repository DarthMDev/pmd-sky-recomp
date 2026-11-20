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
using SkyRecompiled.Launcher.Models;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private Process? _activeProcess;
    private CancellationTokenSource? _verifyCts;

    public MainViewModel()
    {
        Mods = new ModsViewModel();
        Settings = new SettingsViewModel();
        LogsSaves = new LogsSavesViewModel();

        EnabledModCount = Mods.EnabledCount;
        Mods.OnEnabledModCountChanged = count => EnabledModCount = count;

        Settings.OnSettingsSaved = RefreshEnvironmentSummary;
        LogsSaves.OnSaveStateChanged = RefreshEnvironmentSummary;

        RefreshEnvironmentSummary();

        try
        {
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

    public ModsViewModel Mods { get; }
    public SettingsViewModel Settings { get; }
    public LogsSavesViewModel LogsSaves { get; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPlayTab))]
    [NotifyPropertyChangedFor(nameof(IsModsTab))]
    [NotifyPropertyChangedFor(nameof(IsSettingsTab))]
    [NotifyPropertyChangedFor(nameof(IsLogsSavesTab))]
    private int _selectedTabIndex;

    public bool IsPlayTab => SelectedTabIndex == 0;
    public bool IsModsTab => SelectedTabIndex == 1;
    public bool IsSettingsTab => SelectedTabIndex == 2;
    public bool IsLogsSavesTab => SelectedTabIndex == 3;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private string? _romPath;

    [ObservableProperty]
    private string _romRegion = "None";

    [ObservableProperty]
    private string _romSha1 = "";

    [ObservableProperty]
    private string _romSizeText = "—";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    private bool _isRomValid;

    [ObservableProperty]
    private bool _isRomLoaded;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    [NotifyCanExecuteChangedFor(nameof(CancelVerificationCommand))]
    private bool _isProcessing;

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(LaunchGameCommand))]
    [NotifyCanExecuteChangedFor(nameof(StopGameCommand))]
    private bool _isGameRunning;

    [ObservableProperty]
    private double _progressValue;

    [ObservableProperty]
    private string _progressText = "Ready";

    [ObservableProperty]
    private string _statusMessage = "Select your Explorers of Sky Nintendo DS ROM (.nds) to begin.";

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(HasEnabledMods))]
    private int _enabledModCount;

    public bool HasEnabledMods => EnabledModCount > 0;

    [ObservableProperty]
    private bool _isRunnerFound;

    [ObservableProperty]
    private string _runnerPathDisplay = "Not located";

    [ObservableProperty]
    private string _configPathDisplay = "Not located";

    [ObservableProperty]
    private string _runnerStatusSummary = "Checking engine binaries...";

    [ObservableProperty]
    private string _saveSummaryText = "No save file yet";

    [ObservableProperty]
    private string _runtimeSummaryText = "";

    public string UserStorageDir => RomPreparationService.GetUserStorageDir();

    partial void OnIsGameRunningChanged(bool value)
    {
        LogsSaves.IsGameRunning = value;
    }

    public void WireClipboard(Func<string, Task> copyHandler)
    {
        LogsSaves.CopyToClipboardAsync = copyHandler;
    }

    [RelayCommand]
    public void SelectTab(string tabName)
    {
        SelectedTabIndex = tabName.ToLowerInvariant() switch
        {
            "play" => 0,
            "mods" => 1,
            "settings" => 2,
            "logs" or "saves" or "logssaves" => 3,
            _ => 0,
        };
    }

    [RelayCommand]
    public void RefreshEnvironmentSummary()
    {
        if (RomPreparationService.TryLocateEngineBinaries(out var runner, out var config, out var err) &&
            runner != null && config != null)
        {
            IsRunnerFound = true;
            RunnerPathDisplay = runner;
            ConfigPathDisplay = config;
            RunnerStatusSummary = $"Engine Ready ({Path.GetFileName(runner)})";
        }
        else
        {
            IsRunnerFound = false;
            RunnerPathDisplay = "Missing";
            ConfigPathDisplay = "Missing";
            RunnerStatusSummary = err ?? "Engine binary not found";
        }

        try
        {
            var saveInfo = SaveManagementService.InspectSave();
            SaveSummaryText = !saveInfo.Exists
                ? "No pmdsky.sav yet (created on first save)"
                : saveInfo.IsValidEepromSize
                    ? $"128 KiB EEPROM OK • {saveInfo.SavestateCount} savestate(s)"
                    : $"Size mismatch ({saveInfo.SizeBytes} B) • {saveInfo.SavestateCount} savestate(s)";
        }
        catch (Exception ex)
        {
            SaveSummaryText = $"Save inspection unavailable ({ex.Message})";
        }

        RuntimeSummaryText = SkySettingsService.FormatSummary(Settings.BuildRuntimeSettings());
    }

    private async Task InitializeSavedRomAsync(string path)
    {
        await VerifyAndApplyRomAsync(path, "Verifying remembered ROM...", preservePreviousOnCancel: false);
    }

    private async Task VerifyAndApplyRomAsync(string chosenPath, string startLabel, bool preservePreviousOnCancel)
    {
        _verifyCts?.Cancel();
        _verifyCts?.Dispose();
        var cts = new CancellationTokenSource();
        _verifyCts = cts;

        IsProcessing = true;
        ProgressValue = 0;
        ProgressText = startLabel;

        var progress = new Progress<(double Progress, string Status)>(p =>
        {
            ProgressValue = p.Progress;
            ProgressText = p.Status;
        });

        try
        {
            var info = await RomPreparationService.VerifyRomAsync(chosenPath, progress, cts.Token);
            if (!cts.IsCancellationRequested)
                ApplyRomInfo(info);
        }
        catch (OperationCanceledException)
        {
            ProgressText = IsRomValid ? "Ready to Play" : "Verification Cancelled";
            ProgressValue = IsRomValid ? 100 : 0;
            if (!preservePreviousOnCancel)
                StatusMessage = "ROM verification cancelled.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Error verifying ROM: {ex.Message}";
            ProgressText = "Error";
            if (!preservePreviousOnCancel)
                IsRomValid = false;
        }
        finally
        {
            if (ReferenceEquals(_verifyCts, cts))
            {
                _verifyCts = null;
                IsProcessing = false;
            }
            cts.Dispose();
        }
    }

    private void ApplyRomInfo(RomInfo info)
    {
        RomPath = info.Path;
        RomRegion = info.Region;
        RomSha1 = info.Sha1;
        RomSizeText = info.SizeBytes > 0 ? $"{info.SizeBytes / (1024.0 * 1024.0):0.##} MiB ({info.SizeBytes:N0} B)" : "—";
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
        RefreshEnvironmentSummary();
    }

    private bool CanCancelVerification() => IsProcessing && _verifyCts != null;

    [RelayCommand(CanExecute = nameof(CanCancelVerification))]
    public void CancelVerification()
    {
        _verifyCts?.Cancel();
    }

    [RelayCommand]
    public async Task PickRomAsync(IStorageProvider? storageProvider)
    {
        if (storageProvider == null) return;

        var fileTypes = new FilePickerFileType[]
        {
            new("Nintendo DS ROM (*.nds)")
            {
                Patterns = new[] { "*.nds", "*.NDS" },
            },
            new("All Files (*.*)")
            {
                Patterns = new[] { "*.*" },
            },
        };

        var files = await storageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky ROM",
            AllowMultiple = false,
            FileTypeFilter = fileTypes,
        });

        if (files.Count > 0)
        {
            var chosenPath = files[0].Path.LocalPath;
            await VerifyAndApplyRomAsync(chosenPath, "Starting ROM verification...", preservePreviousOnCancel: IsRomValid);
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
            var proc = RomPreparationService.LaunchGame(RomPath, line => LogsSaves.AppendLogLine(line));
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
                        : $"nds_runner exited with code {exitCode}. Check Saves & Logs for details.";
                    LogsSaves.RefreshSaves();
                    RefreshEnvironmentSummary();
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

    private bool CanStopGame() => IsGameRunning && _activeProcess is { HasExited: false };

    [RelayCommand(CanExecute = nameof(CanStopGame))]
    public void StopGame()
    {
        var proc = _activeProcess;
        if (proc == null || proc.HasExited) return;

        try
        {
            proc.Kill(entireProcessTree: true);
            StatusMessage = "Stopped nds_runner process.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Unable to stop nds_runner: {ex.Message}";
        }
    }

    [RelayCommand]
    public void OpenSaveFolder()
    {
        try
        {
            RomPreparationService.OpenPathInOs(RomPreparationService.GetUserStorageDir(), createDirectory: true);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Unable to open data folder: {ex.Message}";
        }
    }

    [RelayCommand]
    public void ResetRom()
    {
        _verifyCts?.Cancel();
        RomPreparationService.ClearSavedRomPath();
        RomPath = null;
        RomRegion = "None";
        RomSha1 = "";
        RomSizeText = "—";
        IsRomLoaded = false;
        IsRomValid = false;
        ProgressValue = 0;
        ProgressText = "Ready";
        StatusMessage = "Select your Explorers of Sky Nintendo DS ROM (.nds) to begin.";
    }
}

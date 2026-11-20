using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SkyRecompiled.Launcher.Models;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public sealed partial class LogsSavesViewModel : ViewModelBase
{
    private const int MaxLogLines = 2000;
    private readonly string _savesDir;
    private readonly string _savestatesDir;
    private readonly string _logPath;
    private readonly Queue<string> _logLines = new();
    private readonly object _queueLock = new();
    private int _logFlushPending;

    public LogsSavesViewModel()
        : this(
            RomPreparationService.GetSavesDir(),
            RomPreparationService.GetSavestatesDir(),
            RomPreparationService.GetLogPath())
    {
    }

    public LogsSavesViewModel(string savesDir, string savestatesDir, string logPath)
    {
        _savesDir = savesDir;
        _savestatesDir = savestatesDir;
        _logPath = logPath;
        RefreshSaves();
        RefreshLog();
    }

    public Func<string, Task<bool>>? CopyToClipboardAsync { get; set; }
    public Action? OnSaveStateChanged { get; set; }

    public ObservableCollection<SaveBackupInfo> Backups { get; } = new();

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(RestoreSelectedBackupCommand))]
    private bool _isGameRunning;

    [ObservableProperty]
    private bool _hasSaveFile;

    [ObservableProperty]
    private bool _isSaveValidEeprom;

    [ObservableProperty]
    private string _saveBadgeText = "No save file yet";

    [ObservableProperty]
    private string _savePathDisplay = "";

    [ObservableProperty]
    private string _saveDetailsText = "";

    [ObservableProperty]
    private string _savestateCountText = "0 savestate(s)";

    [ObservableProperty]
    [NotifyCanExecuteChangedFor(nameof(RestoreSelectedBackupCommand))]
    private SaveBackupInfo? _selectedBackup;

    [ObservableProperty]
    private string _statusMessage = "128 KiB (131072-byte) EEPROM save integrity enforced.";

    [ObservableProperty]
    private string _logText = "No engine logs captured yet. Launch SkyRecompiled to inspect output.";

    public bool HasBackups => Backups.Count > 0;
    public bool HasNoBackups => Backups.Count == 0;

    partial void OnIsGameRunningChanged(bool value)
    {
        RestoreSelectedBackupCommand.NotifyCanExecuteChanged();
        CreateBackupCommand.NotifyCanExecuteChanged();
    }

    [RelayCommand]
    public void RefreshSaves()
    {
        try
        {
            var info = SaveManagementService.InspectSave(_savesDir, _savestatesDir);
            HasSaveFile = info.Exists;
            IsSaveValidEeprom = info.IsValidEepromSize;
            SavePathDisplay = info.Path;
            SavestateCountText = $"{info.SavestateCount} savestate file(s)";

            if (!info.Exists)
            {
                SaveBadgeText = "Not Created";
                SaveDetailsText = "Start the game once to create your 128 KiB EEPROM save (pmdsky.sav).";
            }
            else if (info.IsValidEepromSize)
            {
                SaveBadgeText = "Valid 128 KiB EEPROM";
                SaveDetailsText = $"Size: {info.SizeBytes:N0} bytes • SHA-256: {info.Sha256Short}… • Modified: {info.LastModifiedUtc:yyyy-MM-dd HH:mm:ss} UTC";
            }
            else
            {
                SaveBadgeText = $"Unexpected Size ({info.SizeBytes} B)";
                SaveDetailsText = $"Expected {SaveManagementService.ExpectedEepromSaveBytes:N0} bytes; found {info.SizeBytes:N0} bytes. Restore from a valid backup before playing.";
            }
        }
        catch (Exception ex)
        {
            HasSaveFile = false;
            IsSaveValidEeprom = false;
            SavePathDisplay = SaveManagementService.GetSaveFilePath(_savesDir);
            SavestateCountText = "Unavailable";
            SaveBadgeText = "Inspection Unavailable";
            SaveDetailsText = $"Unable to inspect save file: {ex.Message}";
        }

        var prevSelected = SelectedBackup?.FileName;
        Backups.Clear();
        try
        {
            foreach (var b in SaveManagementService.ListBackups(_savesDir))
                Backups.Add(b);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Unable to list save backups: {ex.Message}";
        }
        SelectedBackup = Backups.FirstOrDefault(b => b.FileName == prevSelected) ?? Backups.FirstOrDefault();
        OnPropertyChanged(nameof(HasBackups));
        OnPropertyChanged(nameof(HasNoBackups));
        CreateBackupCommand.NotifyCanExecuteChanged();
        RestoreSelectedBackupCommand.NotifyCanExecuteChanged();
    }

    private bool CanCreateBackup() => !IsGameRunning && HasSaveFile;

    [RelayCommand(CanExecute = nameof(CanCreateBackup))]
    public void CreateBackup()
    {
        if (IsGameRunning) return;
        try
        {
            var backup = SaveManagementService.CreateBackup(_savesDir);
            RefreshSaves();
            SelectedBackup = Backups.FirstOrDefault(b => b.FileName == backup.FileName) ?? SelectedBackup;
            StatusMessage = $"Created atomic save backup: {backup.FileName}";
            OnSaveStateChanged?.Invoke();
        }
        catch (Exception ex)
        {
            StatusMessage = $"Backup failed: {ex.Message}";
        }
    }

    private bool CanRestoreSelectedBackup() => !IsGameRunning && SelectedBackup != null;

    [RelayCommand(CanExecute = nameof(CanRestoreSelectedBackup))]
    public void RestoreSelectedBackup()
    {
        if (SelectedBackup == null || IsGameRunning) return;
        try
        {
            var name = SelectedBackup.FileName;
            SaveManagementService.RestoreBackup(SelectedBackup.FullPath, _savesDir);
            RefreshSaves();
            StatusMessage = $"Restored save from '{name}' (safety pre-restore backup saved).";
            OnSaveStateChanged?.Invoke();
        }
        catch (Exception ex)
        {
            StatusMessage = $"Restore failed: {ex.Message}";
        }
    }

    [RelayCommand]
    public void OpenSavesFolder()
    {
        try { RomPreparationService.OpenPathInOs(_savesDir, createDirectory: true); }
        catch (Exception ex) { StatusMessage = ex.Message; }
    }

    [RelayCommand]
    public void OpenBackupsFolder()
    {
        try { RomPreparationService.OpenPathInOs(SaveManagementService.GetBackupsDir(_savesDir), createDirectory: true); }
        catch (Exception ex) { StatusMessage = ex.Message; }
    }

    [RelayCommand]
    public void OpenSavestatesFolder()
    {
        try { RomPreparationService.OpenPathInOs(_savestatesDir, createDirectory: true); }
        catch (Exception ex) { StatusMessage = ex.Message; }
    }

    public void AppendLogLine(string line)
    {
        lock (_queueLock)
        {
            _logLines.Enqueue(line);
            while (_logLines.Count > MaxLogLines)
                _logLines.Dequeue();
        }

        if (Application.Current == null)
        {
            LogText = BuildLogSnapshot();
            return;
        }

        if (Interlocked.Exchange(ref _logFlushPending, 1) == 0)
        {
            Dispatcher.UIThread.Post(() =>
            {
                Interlocked.Exchange(ref _logFlushPending, 0);
                LogText = BuildLogSnapshot();
            });
        }
    }

    private string BuildLogSnapshot()
    {
        lock (_queueLock)
        {
            return string.Join(Environment.NewLine, _logLines);
        }
    }

    [RelayCommand]
    public void RefreshLog()
    {
        if (!File.Exists(_logPath))
        {
            LogText = $"No log file at {_logPath} yet.";
            return;
        }

        try
        {
            var allLines = File.ReadAllLines(_logPath, Encoding.UTF8);
            var tail = allLines.Length > MaxLogLines
                ? allLines[(allLines.Length - MaxLogLines)..]
                : allLines;

            lock (_queueLock)
            {
                _logLines.Clear();
                foreach (var ln in tail)
                    _logLines.Enqueue(ln);
                LogText = tail.Length == 0
                    ? "(nds_runner.log is currently empty)"
                    : string.Join(Environment.NewLine, _logLines);
            }
        }
        catch (Exception ex)
        {
            LogText = $"Unable to read log file: {ex.Message}";
        }
    }

    [RelayCommand]
    public void ClearLogView()
    {
        lock (_queueLock)
        {
            _logLines.Clear();
        }
        LogText = "(Log view cleared — file on disk preserved)";
    }

    [RelayCommand]
    public async Task CopyLogAsync()
    {
        if (CopyToClipboardAsync == null)
        {
            StatusMessage = "Clipboard is unavailable.";
            return;
        }

        try
        {
            var copied = await CopyToClipboardAsync(LogText);
            StatusMessage = copied
                ? "Copied current engine log output to clipboard."
                : "Clipboard is busy or unavailable. Please try copying again.";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Unable to copy log to clipboard ({ex.Message}). Please try again.";
        }
    }

    [RelayCommand]
    public void OpenLogFile()
    {
        try
        {
            var dir = Path.GetDirectoryName(_logPath) ?? RomPreparationService.GetUserStorageDir();
            RomPreparationService.OpenPathInOs(File.Exists(_logPath) ? _logPath : dir, createDirectory: !File.Exists(_logPath));
        }
        catch (Exception ex)
        {
            StatusMessage = ex.Message;
        }
    }
}

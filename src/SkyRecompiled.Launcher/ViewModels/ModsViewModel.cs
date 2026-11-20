using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SkyRecompiled.Launcher.Models;
using SkyRecompiled.Launcher.Services;

namespace SkyRecompiled.Launcher.ViewModels;

public sealed partial class ModItemViewModel : ViewModelBase
{
    private readonly Action<ModItemViewModel> _onChanged;
    private bool _suppressNotification;

    public ModItemViewModel(ModEntry entry, Action<ModItemViewModel> onChanged)
    {
        _onChanged = onChanged;
        Id = entry.Id;
        Name = entry.Name;
        Author = string.IsNullOrWhiteSpace(entry.Author) ? "Unknown author" : entry.Author;
        Version = entry.Version;
        Description = string.IsNullOrWhiteSpace(entry.Description)
            ? "NitroFS replacement layer."
            : entry.Description;
        FileCount = entry.FileCount;
        FolderPath = entry.FolderPath;

        _suppressNotification = true;
        _isEnabled = entry.Enabled;
        _priority = entry.Priority;
        _suppressNotification = false;
    }

    public string Id { get; }
    public string Name { get; }
    public string Author { get; }
    public string Version { get; }
    public string Description { get; }
    public int FileCount { get; }
    public string FolderPath { get; }
    public string Subtitle => $"v{Version} • {Author} • {FileCount} file(s)";

    [ObservableProperty]
    private bool _isEnabled;

    [ObservableProperty]
    private int _priority;

    partial void OnIsEnabledChanged(bool value)
    {
        if (!_suppressNotification)
            _onChanged(this);
    }

    partial void OnPriorityChanged(int value)
    {
        if (!_suppressNotification)
            _onChanged(this);
    }

    [RelayCommand]
    private void IncreasePriority() => Priority += 10;

    [RelayCommand]
    private void DecreasePriority() => Priority -= 10;
}

public sealed record ModConflictItem(string VirtualPath, string ChainDisplay, string WinnerModId);

public sealed partial class ModsViewModel : ViewModelBase
{
    private readonly string _modsDir;
    private readonly string _statePath;
    private readonly string _manifestPath;

    public ModsViewModel()
        : this(
            RomPreparationService.GetModsDir(),
            RomPreparationService.GetModStatePath(),
            RomPreparationService.GetActiveModsManifestPath())
    {
    }

    public ModsViewModel(string modsDir, string statePath, string manifestPath)
    {
        _modsDir = modsDir;
        _statePath = statePath;
        _manifestPath = manifestPath;
        RefreshMods();
    }

    public Action<int>? OnEnabledModCountChanged { get; set; }

    public ObservableCollection<ModItemViewModel> Mods { get; } = new();
    public ObservableCollection<ModConflictItem> Conflicts { get; } = new();
    public ObservableCollection<ModValidationIssue> ValidationIssues { get; } = new();

    [ObservableProperty]
    private int _enabledCount;

    [ObservableProperty]
    private int _totalOverrideCount;

    [ObservableProperty]
    private string _statusMessage = "Place mod folders with mod.toml + romfs/ inside the mods directory.";

    [ObservableProperty]
    private string _modsDirectoryDisplay = "";

    public bool HasMods => Mods.Count > 0;
    public bool HasNoMods => Mods.Count == 0;
    public bool HasConflicts => Conflicts.Count > 0;
    public bool HasNoConflicts => Conflicts.Count == 0;
    public bool HasValidationIssues => ValidationIssues.Count > 0;

    [RelayCommand]
    public void RefreshMods()
    {
        ModsDirectoryDisplay = _modsDir;
        var scan = ModManagerService.ScanMods(_modsDir, _statePath);
        ApplyScanResult(scan);
        try
        {
            ModManagerService.ExportActiveModsManifest(scan, _manifestPath);
        }
        catch
        {
            // Manifest export is best-effort during discovery
        }
    }

    private void ApplyScanResult(ModScanResult scan)
    {
        Mods.Clear();
        foreach (var entry in scan.Mods)
            Mods.Add(new ModItemViewModel(entry, OnItemModified));

        Conflicts.Clear();
        foreach (var conflict in scan.Conflicts)
        {
            Conflicts.Add(new ModConflictItem(
                conflict.VirtualPath,
                string.Join(" -> ", conflict.ModChain),
                conflict.WinnerModId));
        }

        ValidationIssues.Clear();
        foreach (var issue in scan.Issues)
            ValidationIssues.Add(issue);

        EnabledCount = scan.Mods.Count(m => m.Enabled);
        TotalOverrideCount = scan.Mods.Where(m => m.Enabled).Sum(m => m.FileCount);

        OnPropertyChanged(nameof(HasMods));
        OnPropertyChanged(nameof(HasNoMods));
        OnPropertyChanged(nameof(HasConflicts));
        OnPropertyChanged(nameof(HasNoConflicts));
        OnPropertyChanged(nameof(HasValidationIssues));

        if (scan.Issues.Count > 0)
            StatusMessage = $"Scanned {scan.Mods.Count} valid mod(s); {scan.Issues.Count} validation issue(s) skipped.";
        else if (scan.Mods.Count == 0)
            StatusMessage = "No NitroFS mod layers found yet. Import a mod folder or open the mods directory.";
        else
            StatusMessage = $"{EnabledCount} of {scan.Mods.Count} layer(s) enabled ({TotalOverrideCount} file override(s), {Conflicts.Count} conflict(s)).";

        OnEnabledModCountChanged?.Invoke(EnabledCount);
    }

    private void OnItemModified(ModItemViewModel _)
    {
        var overrides = new Dictionary<string, ModStateOverride>(StringComparer.Ordinal);
        foreach (var item in Mods)
            overrides[item.Id] = new ModStateOverride(item.IsEnabled, item.Priority);

        try
        {
            ModManagerService.SaveStateAtomic(overrides, _statePath);
            var rescan = ModManagerService.ScanMods(_modsDir, _statePath);
            ApplyScanResult(rescan);
            ModManagerService.ExportActiveModsManifest(rescan, _manifestPath);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Failed to save mod load order: {ex.Message}";
        }
    }

    public void ImportModFolder(string folderPath)
    {
        try
        {
            var imported = ModManagerService.ImportModFolder(folderPath, _modsDir, _statePath);
            RefreshMods();
            StatusMessage = $"Imported mod layer '{imported.Name}' ({imported.FileCount} file(s)).";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Import failed: {ex.Message}";
        }
    }

    [RelayCommand]
    public void OpenModsFolder()
    {
        try
        {
            RomPreparationService.OpenPathInOs(_modsDir, createDirectory: true);
        }
        catch (Exception ex)
        {
            StatusMessage = $"Could not open mods folder: {ex.Message}";
        }
    }
}

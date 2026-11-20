using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using SkyRecompiled.Launcher.Models;

namespace SkyRecompiled.Launcher.Services;

/// <summary>
/// Discovers, validates, orders, and persists layered NitroFS mods under <c>mods/</c>
/// with parity to <c>tools/sky_mod_packer.py</c>.
/// </summary>
public static partial class ModManagerService
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    [GeneratedRegex("^[A-Za-z0-9_-]+$", RegexOptions.CultureInvariant)]
    private static partial Regex ValidModIdRegex();

    public static bool IsSymlink(FileSystemInfo info)
        => info.LinkTarget != null || (info.Attributes & FileAttributes.ReparsePoint) != 0;

    public static void ValidateVirtualPath(string path)
    {
        if (string.IsNullOrEmpty(path) || path.StartsWith('/') || path.Contains('\\') || path.Contains(':'))
            throw new ArgumentException($"Invalid NitroFS path: '{path}'", nameof(path));

        var parts = path.Split('/');
        foreach (var part in parts)
        {
            if (part is "" or "." or "..")
                throw new ArgumentException($"Invalid NitroFS path: '{path}'", nameof(path));
            if (part.Length > 127)
                throw new ArgumentException($"Invalid NitroFS path component: '{part}'", nameof(path));
            foreach (var ch in part)
            {
                if (ch > 0xFF)
                    throw new ArgumentException($"NitroFS path is not representable in the FNT: '{path}'", nameof(path));
                if (ch < 32)
                    throw new ArgumentException($"Invalid NitroFS path component: '{part}'", nameof(path));
            }
        }
    }

    public static ModScanResult ScanMods(string? modsRoot = null, string? stateFilePath = null)
    {
        var root = modsRoot ?? RomPreparationService.GetModsDir();
        var statePath = stateFilePath ?? RomPreparationService.GetModStatePath();
        var issues = new List<ModValidationIssue>();

        if (!Directory.Exists(root))
        {
            return new ModScanResult(Array.Empty<ModEntry>(), Array.Empty<ModConflict>(), issues);
        }

        var overrides = LoadState(statePath);
        var discovered = new List<ModEntry>();
        var seenIds = new HashSet<string>(StringComparer.Ordinal);

        IEnumerable<string> entryPaths;
        try
        {
            entryPaths = Directory.GetFileSystemEntries(root).OrderBy(p => p, StringComparer.Ordinal);
        }
        catch (Exception ex)
        {
            issues.Add(new ModValidationIssue("(mods)", $"Failed to enumerate mods folder: {ex.Message}"));
            return new ModScanResult(Array.Empty<ModEntry>(), Array.Empty<ModConflict>(), issues);
        }

        foreach (var entryPath in entryPaths)
        {
            var dirInfo = new DirectoryInfo(entryPath);
            if (IsSymlink(dirInfo))
            {
                issues.Add(new ModValidationIssue(dirInfo.Name, "Symlinked mod entries are not allowed."));
                continue;
            }
            if (!dirInfo.Exists)
                continue; // Not a directory

            var romfsPath = Path.Combine(entryPath, "romfs");
            var romfsInfo = new DirectoryInfo(romfsPath);
            if (IsSymlink(romfsInfo))
            {
                issues.Add(new ModValidationIssue(dirInfo.Name, $"Mod romfs must not be a symlink: {romfsPath}"));
                continue;
            }
            if (!romfsInfo.Exists)
            {
                // Ignore directories without romfs/ before validating metadata (matches sky_mod_packer.py)
                continue;
            }

            ParsedModMetadata meta;
            try
            {
                meta = ParseModToml(Path.Combine(entryPath, "mod.toml"), dirInfo.Name);
            }
            catch (Exception ex)
            {
                issues.Add(new ModValidationIssue(dirInfo.Name, ex.Message));
                continue;
            }

            if (string.IsNullOrEmpty(meta.Id) || !ValidModIdRegex().IsMatch(meta.Id))
            {
                issues.Add(new ModValidationIssue(dirInfo.Name, $"Invalid mod ID: '{meta.Id}'"));
                continue;
            }
            if (!seenIds.Add(meta.Id))
            {
                issues.Add(new ModValidationIssue(dirInfo.Name, $"Duplicate mod ID: '{meta.Id}'"));
                continue;
            }

            List<string> virtualPaths;
            try
            {
                virtualPaths = CollectRomfsPaths(romfsPath);
            }
            catch (Exception ex)
            {
                issues.Add(new ModValidationIssue(meta.Id, ex.Message));
                continue;
            }

            bool effectiveEnabled = meta.Enabled;
            int effectivePriority = meta.Priority;
            if (overrides.TryGetValue(meta.Id, out var ov))
            {
                effectiveEnabled = ov.Enabled;
                effectivePriority = ov.Priority;
            }

            discovered.Add(new ModEntry(
                Id: meta.Id,
                Name: string.IsNullOrWhiteSpace(meta.Name) ? meta.Id : meta.Name,
                Author: meta.Author,
                Version: string.IsNullOrWhiteSpace(meta.Version) ? "1.0.0" : meta.Version,
                Priority: effectivePriority,
                DefaultPriority: meta.Priority,
                Description: meta.Description,
                Enabled: effectiveEnabled,
                DefaultEnabled: meta.Enabled,
                FolderPath: entryPath,
                FileCount: virtualPaths.Count,
                VirtualPaths: virtualPaths));
        }

        // Deterministic load order: priority ascending, then id ascending (higher priority applies last / wins)
        discovered.Sort((a, b) =>
        {
            var cmp = a.Priority.CompareTo(b.Priority);
            return cmp != 0 ? cmp : string.Compare(a.Id, b.Id, StringComparison.Ordinal);
        });

        var touchHistory = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (var mod in discovered)
        {
            if (!mod.Enabled) continue;
            foreach (var vpath in mod.VirtualPaths)
            {
                if (!touchHistory.TryGetValue(vpath, out var chain))
                {
                    chain = new List<string>();
                    touchHistory[vpath] = chain;
                }
                chain.Add(mod.Id);
            }
        }

        var conflicts = touchHistory
            .Where(kv => kv.Value.Count > 1)
            .OrderBy(kv => kv.Key, StringComparer.Ordinal)
            .Select(kv => new ModConflict(kv.Key, kv.Value.ToArray(), kv.Value[^1]))
            .ToList();

        return new ModScanResult(discovered, conflicts, issues);
    }

    private static List<string> CollectRomfsPaths(string romfsRoot)
    {
        var collected = new List<string>();
        var pending = new Stack<string>();
        pending.Push(romfsRoot);

        while (pending.Count > 0)
        {
            var cur = pending.Pop();
            foreach (var subDir in Directory.GetDirectories(cur).OrderBy(p => p, StringComparer.Ordinal))
            {
                var di = new DirectoryInfo(subDir);
                if (IsSymlink(di))
                    throw new InvalidOperationException($"Symlinks are not allowed in mod layers: {subDir}");
                pending.Push(subDir);
            }

            foreach (var file in Directory.GetFiles(cur).OrderBy(p => p, StringComparer.Ordinal))
            {
                var fi = new FileInfo(file);
                if (IsSymlink(fi))
                    throw new InvalidOperationException($"Symlinks are not allowed in mod layers: {file}");

                var rel = Path.GetRelativePath(romfsRoot, file).Replace('\\', '/');
                ValidateVirtualPath(rel);
                collected.Add(rel);
            }
        }

        collected.Sort(StringComparer.Ordinal);
        return collected;
    }

    public sealed record ParsedModMetadata(
        string Id,
        string Name,
        string Author,
        string Version,
        int Priority,
        string Description,
        bool Enabled);

    public static ParsedModMetadata ParseModToml(string tomlPath, string defaultId)
    {
        string id = defaultId;
        string name = "";
        string author = "";
        string version = "1.0.0";
        int priority = 100;
        string description = "";
        bool enabled = true;

        if (!File.Exists(tomlPath))
            return new ParsedModMetadata(id, name, author, version, priority, description, enabled);

        var text = File.ReadAllText(tomlPath, Encoding.UTF8);
        foreach (var rawLine in text.Split('\n'))
        {
            var stripped = StripTomlComment(rawLine).Trim();
            if (string.IsNullOrEmpty(stripped) || stripped.StartsWith('['))
                continue;

            var eq = stripped.IndexOf('=');
            if (eq < 0)
                throw new FormatException($"Invalid mod.toml line in {tomlPath}: '{stripped}'");

            var key = stripped[..eq].Trim();
            var rawVal = stripped[(eq + 1)..].Trim();

            switch (key)
            {
                case "id":
                    id = UnquoteTomlString(rawVal, tomlPath, key);
                    break;
                case "name":
                    name = UnquoteTomlString(rawVal, tomlPath, key);
                    break;
                case "author":
                    author = UnquoteTomlString(rawVal, tomlPath, key);
                    break;
                case "version":
                    version = UnquoteTomlString(rawVal, tomlPath, key);
                    break;
                case "description":
                    description = UnquoteTomlString(rawVal, tomlPath, key);
                    break;
                case "priority":
                    priority = ParseTomlInt(rawVal, tomlPath);
                    break;
                case "enabled":
                    enabled = ParseTomlBool(rawVal, tomlPath);
                    break;
            }
        }

        return new ParsedModMetadata(id, name, author, version, priority, description, enabled);
    }

    private static string StripTomlComment(string line)
    {
        bool inDouble = false;
        bool inSingle = false;
        for (int i = 0; i < line.Length; i++)
        {
            char c = line[i];
            if (c == '\\' && inDouble && i + 1 < line.Length)
            {
                i++;
                continue;
            }
            if (c == '"' && !inSingle)
                inDouble = !inDouble;
            else if (c == '\'' && !inDouble)
                inSingle = !inSingle;
            else if (c == '#' && !inDouble && !inSingle)
                return line[..i];
        }
        return line;
    }

    private static string UnquoteTomlString(string rawVal, string tomlPath, string key)
    {
        if (rawVal.Length >= 2 && rawVal[0] == '"' && rawVal[^1] == '"')
        {
            return rawVal[1..^1]
                .Replace("\\\"", "\"")
                .Replace("\\\\", "\\")
                .Replace("\\n", "\n")
                .Replace("\\t", "\t");
        }
        if (rawVal.Length >= 2 && rawVal[0] == '\'' && rawVal[^1] == '\'')
        {
            return rawVal[1..^1];
        }
        throw new FormatException($"Invalid string value for '{key}' in {tomlPath}: '{rawVal}'");
    }

    private static int ParseTomlInt(string rawVal, string tomlPath)
    {
        if (string.Equals(rawVal, "true", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(rawVal, "false", StringComparison.OrdinalIgnoreCase))
            throw new FormatException($"Invalid mod priority in {tomlPath}: {rawVal}");

        var unquoted = rawVal;
        if (rawVal.Length >= 2 && ((rawVal[0] == '"' && rawVal[^1] == '"') || (rawVal[0] == '\'' && rawVal[^1] == '\'')))
            unquoted = rawVal[1..^1].Trim();

        if (!int.TryParse(unquoted, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
            throw new FormatException($"Invalid mod priority in {tomlPath}: {rawVal}");
        return parsed;
    }

    private static bool ParseTomlBool(string rawVal, string tomlPath)
    {
        var unquoted = rawVal;
        if (rawVal.Length >= 2 && ((rawVal[0] == '"' && rawVal[^1] == '"') || (rawVal[0] == '\'' && rawVal[^1] == '\'')))
            unquoted = rawVal[1..^1].Trim();

        if (string.Equals(unquoted, "true", StringComparison.OrdinalIgnoreCase))
            return true;
        if (string.Equals(unquoted, "false", StringComparison.OrdinalIgnoreCase))
            return false;
        throw new FormatException($"Invalid mod enabled flag in {tomlPath}: {rawVal}");
    }

    public static Dictionary<string, ModStateOverride> LoadState(string? stateFilePath = null)
    {
        var path = stateFilePath ?? RomPreparationService.GetModStatePath();
        if (!File.Exists(path))
            return new Dictionary<string, ModStateOverride>(StringComparer.Ordinal);

        try
        {
            var json = File.ReadAllText(path, Encoding.UTF8);
            var parsed = JsonSerializer.Deserialize<Dictionary<string, ModStateOverride>>(json);
            return parsed != null
                ? new Dictionary<string, ModStateOverride>(parsed, StringComparer.Ordinal)
                : new Dictionary<string, ModStateOverride>(StringComparer.Ordinal);
        }
        catch
        {
            return new Dictionary<string, ModStateOverride>(StringComparer.Ordinal);
        }
    }

    public static void SaveStateAtomic(IDictionary<string, ModStateOverride> overrides, string? stateFilePath = null)
    {
        var path = stateFilePath ?? RomPreparationService.GetModStatePath();
        var sorted = new SortedDictionary<string, ModStateOverride>(overrides, StringComparer.Ordinal);
        var json = JsonSerializer.Serialize(sorted, JsonOptions) + "\n";
        AtomicWriteText(path, json);
    }

    public static void ExportActiveModsManifest(ModScanResult scan, string? manifestPath = null)
    {
        var path = manifestPath ?? RomPreparationService.GetActiveModsManifestPath();
        var payload = new
        {
            version = 1,
            updated_utc = DateTimeOffset.UtcNow.ToString("O"),
            enabled_mods = scan.Mods
                .Where(m => m.Enabled)
                .Select(m => new
                {
                    id = m.Id,
                    name = m.Name,
                    priority = m.Priority,
                    folder = m.FolderPath,
                    file_count = m.FileCount,
                })
                .ToArray(),
            conflicts = scan.Conflicts
                .Select(c => new
                {
                    path = c.VirtualPath,
                    chain = c.ModChain,
                    winner = c.WinnerModId,
                })
                .ToArray(),
        };
        var json = JsonSerializer.Serialize(payload, JsonOptions) + "\n";
        AtomicWriteText(path, json);
    }

    public static ModEntry ImportModFolder(string sourceDir, string? modsRoot = null, string? stateFilePath = null)
    {
        if (string.IsNullOrWhiteSpace(sourceDir))
            throw new ArgumentException("Source mod folder path is required.", nameof(sourceDir));

        var srcInfo = new DirectoryInfo(sourceDir);
        if (!srcInfo.Exists)
            throw new DirectoryNotFoundException($"Mod folder does not exist: {sourceDir}");
        if (IsSymlink(srcInfo))
            throw new InvalidOperationException("Cannot import a symlinked mod folder.");

        var srcRomfs = Path.Combine(sourceDir, "romfs");
        var srcRomfsInfo = new DirectoryInfo(srcRomfs);
        if (!srcRomfsInfo.Exists)
            throw new InvalidOperationException($"Selected folder '{srcInfo.Name}' does not contain a 'romfs' directory.");
        if (IsSymlink(srcRomfsInfo))
            throw new InvalidOperationException("Source mod 'romfs' directory must not be a symlink.");

        var meta = ParseModToml(Path.Combine(sourceDir, "mod.toml"), srcInfo.Name);
        if (string.IsNullOrEmpty(meta.Id) || !ValidModIdRegex().IsMatch(meta.Id))
            throw new InvalidOperationException($"Invalid mod ID '{meta.Id}'. Allowed characters: A-Z, a-z, 0-9, _, -.");

        _ = CollectRomfsPaths(srcRomfs);

        var root = modsRoot ?? RomPreparationService.GetModsDir();
        Directory.CreateDirectory(root);

        var existing = ScanMods(root, stateFilePath);
        if (existing.Mods.Any(m => string.Equals(m.Id, meta.Id, StringComparison.Ordinal)))
            throw new InvalidOperationException($"A mod with ID '{meta.Id}' is already installed.");

        var targetFolderName = ValidModIdRegex().IsMatch(srcInfo.Name) ? srcInfo.Name : meta.Id;
        var destDir = Path.Combine(root, targetFolderName);
        if (Directory.Exists(destDir))
            throw new InvalidOperationException($"Destination folder '{targetFolderName}' already exists in mods/.");

        CopyDirectoryClean(sourceDir, destDir);

        var refreshed = ScanMods(root, stateFilePath);
        return refreshed.Mods.First(m => string.Equals(m.Id, meta.Id, StringComparison.Ordinal));
    }

    private static void CopyDirectoryClean(string sourceDir, string destDir)
    {
        Directory.CreateDirectory(destDir);
        foreach (var file in Directory.GetFiles(sourceDir))
        {
            var fi = new FileInfo(file);
            if (IsSymlink(fi))
                throw new InvalidOperationException($"Refusing to copy symlinked file: {file}");
            File.Copy(file, Path.Combine(destDir, fi.Name), overwrite: false);
        }

        foreach (var subDir in Directory.GetDirectories(sourceDir))
        {
            var di = new DirectoryInfo(subDir);
            if (IsSymlink(di))
                throw new InvalidOperationException($"Refusing to copy symlinked directory: {subDir}");
            CopyDirectoryClean(subDir, Path.Combine(destDir, di.Name));
        }
    }

    private static void AtomicWriteText(string targetPath, string content)
    {
        var dir = Path.GetDirectoryName(targetPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        var tmpPath = Path.Combine(dir ?? ".", $".{Path.GetFileName(targetPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllText(tmpPath, content, new UTF8Encoding(false));
            File.Move(tmpPath, targetPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmpPath))
                File.Delete(tmpPath);
        }
    }
}

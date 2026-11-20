using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using SkyRecompiled.Launcher.Models;

namespace SkyRecompiled.Launcher.Services;

/// <summary>
/// Inspects the 128 KiB (131072-byte) EEPROM save file and provides atomic,
/// safety-backed backup and restore operations.
/// </summary>
public static class SaveManagementService
{
    public const long ExpectedEepromSaveBytes = 131072;

    public static string GetSaveFilePath(string? savesDir = null)
        => Path.Combine(savesDir ?? RomPreparationService.GetSavesDir(), "pmdsky.sav");

    public static string GetBackupsDir(string? savesDir = null)
        => Path.Combine(savesDir ?? RomPreparationService.GetSavesDir(), "backups");

    public static SaveFileInfo InspectSave(string? savesDir = null, string? savestatesDir = null)
    {
        var resolvedSavesDir = savesDir ?? RomPreparationService.GetSavesDir();
        var resolvedStatesDir = savestatesDir ?? RomPreparationService.GetSavestatesDir();
        var savePath = GetSaveFilePath(resolvedSavesDir);

        int savestateCount = 0;
        if (Directory.Exists(resolvedStatesDir))
        {
            savestateCount = Directory.EnumerateFiles(resolvedStatesDir, "*", SearchOption.TopDirectoryOnly)
                .Count(p => !Path.GetFileName(p).StartsWith('.'));
        }

        if (!File.Exists(savePath))
        {
            return new SaveFileInfo(
                savePath,
                Exists: false,
                SizeBytes: 0,
                IsValidEepromSize: false,
                LastModifiedUtc: null,
                Sha256Short: "",
                SavestateCount: savestateCount);
        }

        var fi = new FileInfo(savePath);
        string shaShort = "";
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(savePath))
        {
            var hash = sha.ComputeHash(stream);
            shaShort = Convert.ToHexString(hash).ToLowerInvariant()[..16];
        }

        return new SaveFileInfo(
            savePath,
            Exists: true,
            SizeBytes: fi.Length,
            IsValidEepromSize: fi.Length == ExpectedEepromSaveBytes,
            LastModifiedUtc: fi.LastWriteTimeUtc,
            Sha256Short: shaShort,
            SavestateCount: savestateCount);
    }

    public static IReadOnlyList<SaveBackupInfo> ListBackups(string? savesDir = null)
    {
        var backupsDir = GetBackupsDir(savesDir);
        if (!Directory.Exists(backupsDir))
            return Array.Empty<SaveBackupInfo>();

        return Directory.EnumerateFiles(backupsDir, "*.sav", SearchOption.TopDirectoryOnly)
            .Select(p => new FileInfo(p))
            .OrderByDescending(fi => fi.LastWriteTimeUtc)
            .ThenByDescending(fi => fi.Name, StringComparer.Ordinal)
            .Select(fi => new SaveBackupInfo(
                fi.Name,
                fi.FullName,
                fi.Length,
                fi.Length == ExpectedEepromSaveBytes,
                fi.LastWriteTimeUtc))
            .ToList();
    }

    public static SaveBackupInfo CreateBackup(string? savesDir = null, string prefix = "pmdsky")
    {
        var resolvedSavesDir = savesDir ?? RomPreparationService.GetSavesDir();
        var savePath = GetSaveFilePath(resolvedSavesDir);
        if (!File.Exists(savePath))
            throw new FileNotFoundException("No active save file exists to back up.", savePath);

        var backupsDir = GetBackupsDir(resolvedSavesDir);
        Directory.CreateDirectory(backupsDir);

        var safePrefix = string.IsNullOrWhiteSpace(prefix) ? "pmdsky" : prefix.Trim();
        var stamp = DateTimeOffset.UtcNow.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
        var candidate = Path.Combine(backupsDir, $"{safePrefix}-{stamp}.sav");
        int suffix = 1;
        while (File.Exists(candidate))
        {
            candidate = Path.Combine(backupsDir, $"{safePrefix}-{stamp}-{suffix}.sav");
            suffix++;
        }

        var data = File.ReadAllBytes(savePath);
        AtomicWriteBytes(candidate, data);

        var fi = new FileInfo(candidate);
        return new SaveBackupInfo(
            fi.Name,
            fi.FullName,
            fi.Length,
            fi.Length == ExpectedEepromSaveBytes,
            fi.LastWriteTimeUtc);
    }

    public static void RestoreBackup(string backupPath, string? savesDir = null)
    {
        if (string.IsNullOrWhiteSpace(backupPath) || !File.Exists(backupPath))
            throw new FileNotFoundException("Selected save backup does not exist.", backupPath);

        var backupBytes = File.ReadAllBytes(backupPath);
        if (backupBytes.LongLength != ExpectedEepromSaveBytes)
        {
            throw new InvalidDataException(
                $"Refusing to restore save backup '{Path.GetFileName(backupPath)}': expected exactly {ExpectedEepromSaveBytes} bytes (128 KiB EEPROM), got {backupBytes.LongLength} bytes.");
        }

        var resolvedSavesDir = savesDir ?? RomPreparationService.GetSavesDir();
        Directory.CreateDirectory(resolvedSavesDir);
        var savePath = GetSaveFilePath(resolvedSavesDir);

        if (File.Exists(savePath))
        {
            CreateBackup(resolvedSavesDir, "pre-restore");
        }

        AtomicWriteBytes(savePath, backupBytes);
    }

    private static void AtomicWriteBytes(string targetPath, byte[] data)
    {
        var dir = Path.GetDirectoryName(targetPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        var tmpPath = Path.Combine(dir ?? ".", $".{Path.GetFileName(targetPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllBytes(tmpPath, data);
            File.Move(tmpPath, targetPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmpPath))
                File.Delete(tmpPath);
        }
    }
}

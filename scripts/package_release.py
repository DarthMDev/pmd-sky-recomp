#!/usr/bin/env python3
"""
SkyRecompiled -- Multi-Platform Alpha Release Packager
=====================================================
Packages SkyRecompiled for distribution across macOS, Linux, and Windows.
Every package includes:
  - Canonical Avalonia desktop launcher (with script/PowerShell fallback if omitted)
  - First-run GUI ROM selector and setup UI (with clean SHA-1 validation)
  - Persistent save/settings redirection to standard OS directories
  - Complete control presets (modern gamepad & keyboard dungeon crawl)
  - Clean-room legal compliance (zero proprietary assets or ROMs bundled)
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
CONFIG_DIR = REPO_ROOT / "config"
DIST_DIR = BUILD_DIR / "dist"
LAUNCHER_CSPROJ = REPO_ROOT / "src" / "SkyRecompiled.Launcher" / "SkyRecompiled.Launcher.csproj"

VERSION = "0.2.3"

WINDOWS_BAT = """@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0SkyRecompiled.exe" (
    "%~dp0SkyRecompiled.exe" %*
    exit /b
)
powershell -ExecutionPolicy Bypass -NoProfile -File "%~dp0SkyRecompiled.ps1" %*
set "exit_code=%ERRORLEVEL%"
if %exit_code% NEQ 0 (
    pause
)
exit /b %exit_code%
"""

WINDOWS_PS1 = """# SkyRecompiled -- Windows Native Launcher & ROM Setup
param([switch]$CheckRunner)

$ErrorActionPreference = "Stop"

$RunnerExe = Join-Path $PSScriptRoot "bin\\nds_runner.exe"
if (-not (Test-Path -LiteralPath $RunnerExe -PathType Leaf)) {
    Write-Error "Missing bin\\nds_runner.exe. Extract the entire Windows ZIP before launching. If the ZIP has no runner, follow the Windows source build instructions in the project README."
}
$SdlDll = Join-Path $PSScriptRoot "bin\\SDL3.dll"
if (-not (Test-Path -LiteralPath $SdlDll -PathType Leaf)) {
    Write-Error "Missing bin\\SDL3.dll. Extract the entire Windows ZIP before launching."
}
$GameToml = Join-Path $PSScriptRoot "config\\game.toml"
if (-not (Test-Path -LiteralPath $GameToml -PathType Leaf)) {
    Write-Error "Missing config\\game.toml. Extract the entire Windows ZIP before launching."
}
$AvaloniaExe = Join-Path $PSScriptRoot "SkyRecompiled.exe"
if ($CheckRunner) {
    if (Test-Path -LiteralPath $AvaloniaExe -PathType Leaf) {
        & $AvaloniaExe --check-runner
        exit $LASTEXITCODE
    }
    & $RunnerExe --help
    exit $LASTEXITCODE
}

if (Test-Path -LiteralPath $AvaloniaExe -PathType Leaf) {
    & $AvaloniaExe @args
    exit $LASTEXITCODE
}

$CleanUsaSha1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
$CleanEuSha1  = "c838a5adf1ed32d2da8454976e5b1a1aa189c139"

$AppData = [Environment]::GetFolderPath("ApplicationData")
$UserDir = Join-Path $AppData "SkyRecompiled"
$SavesDir = Join-Path $UserDir "saves"
$ConsoleDir = Join-Path $UserDir "console"
$SettingsFile = Join-Path $UserDir "settings.toml"

New-Item -ItemType Directory -Force -Path $SavesDir | Out-Null
New-Item -ItemType Directory -Force -Path $ConsoleDir | Out-Null

function Test-RomSha1($path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA1).Hash.ToLower()
    return ($hash -eq $CleanUsaSha1 -or $hash -eq $CleanEuSha1)
}

function Show-Alert($title, $msg) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($msg, $title, [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error)
}

function Pick-Rom() {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky .nds ROM"
    $dialog.Filter = "Nintendo DS ROM (*.nds)|*.nds|All Files (*.*)|*.*"
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        return $dialog.FileName
    }
    return $null
}

function Save-RomPathSetting($settingsPath, $romPathValue) {
    $dir = Split-Path -Parent $settingsPath
    if (-not [string]::IsNullOrEmpty($dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $lines = @()
    if (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
        $lines = @(Get-Content -LiteralPath $settingsPath | Where-Object { $_ -notmatch '^\\s*rom_path\\s*=' })
    } else {
        $lines = @("# SkyRecompiled User Configuration")
    }
    $sanitized = $romPathValue.Replace("`r", "").Replace("`n", "")
    $lines += ("rom_path = `"$sanitized`"")
    $tmpPath = Join-Path $dir (".settings." + [guid]::NewGuid().ToString("N") + ".tmp")
    try {
        Set-Content -LiteralPath $tmpPath -Value $lines -Encoding UTF8
        Move-Item -LiteralPath $tmpPath -Destination $settingsPath -Force
    } finally {
        if (Test-Path -LiteralPath $tmpPath) {
            Remove-Item -LiteralPath $tmpPath -Force -ErrorAction SilentlyContinue
        }
    }
}

# Read settings if exists
$RomPath = $null
if (Test-Path -LiteralPath $SettingsFile) {
    Get-Content -LiteralPath $SettingsFile | ForEach-Object {
        if ($_ -match '^\\s*rom_path\\s*=\\s*"([^"]+)"') {
            $RomPath = $matches[1]
        }
    }
}

while (-not $RomPath -or -not (Test-RomSha1 $RomPath)) {
    if ($RomPath -and -not (Test-RomSha1 $RomPath)) {
        Show-Alert "Invalid ROM Selected" "The selected file does not match a verified clean Explorers of Sky ROM (USA or Europe SHA-1 mismatch).`n`nPlease select an authentic, clean .nds cartridge dump."
    }
    $chosen = Pick-Rom
    if (-not $chosen) {
        Write-Host "Setup cancelled by user."
        exit 0
    }
    if (Test-RomSha1 $chosen) {
        $RomPath = $chosen
        Save-RomPathSetting $SettingsFile $RomPath
    } else {
        $RomPath = $chosen
    }
}

$SavePath = Join-Path $SavesDir "pmdsky.sav"
$SavestateDir = Join-Path $UserDir "savestates"
New-Item -ItemType Directory -Force -Path $SavestateDir | Out-Null

Write-Host "Launching SkyRecompiled..." -ForegroundColor Green
& $RunnerExe $ConsoleDir --interactive --config $GameToml --rom $RomPath --save-path $SavePath --savestate-dir $SavestateDir --rtc-host
exit $LASTEXITCODE
"""

LINUX_LAUNCHER = """#!/usr/bin/env bash
# SkyRecompiled -- Linux Native Launcher & ROM Setup
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONPATH="${SCRIPT_DIR}:${SCRIPT_DIR}/scripts:${PYTHONPATH:-}"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/bin:${LD_LIBRARY_PATH:-}"

if [[ -x "${SCRIPT_DIR}/bin/SkyRecompiled.Launcher" ]]; then
    exec "${SCRIPT_DIR}/bin/SkyRecompiled.Launcher" "$@"
elif command -v python3 >/dev/null 2>&1; then
    exec python3 "${SCRIPT_DIR}/scripts/launcher.py" "$@"
else
    echo "ERROR: SkyRecompiled launcher binary and python3 are both unavailable." >&2
    exit 1
fi
"""

LINUX_DESKTOP = """[Desktop Entry]
Version=1.0
Type=Application
Name=SkyRecompiled
Comment=Pokémon Mystery Dungeon: Explorers of Sky -- Native Static Recompilation PC Port
Exec=SkyRecompiled
Icon=applications-games
Terminal=false
Categories=Game;Emulator;
"""

README_TEXT = f"""SkyRecompiled {VERSION}
========================
Native ahead-of-time (AOT) static recompilation port of
Pokémon Mystery Dungeon: Explorers of Sky.

QUICK START:
------------
1. Launch SkyRecompiled:
   - macOS:   Double-click SkyRecompiled.app
   - Windows: Double-click SkyRecompiled.exe (or SkyRecompiled.bat)
   - Linux:   Run ./SkyRecompiled (or click SkyRecompiled.desktop)
2. On first launch, choose your legally owned Explorers of Sky .nds ROM
   (clean USA or Europe) inside the SkyRecompiled Launcher.
3. Your ROM path, runtime settings, mods, and saves are persisted cleanly.

CONTROLS:
---------
Configured in config/controls_preset.toml (and in-game Settings via F1 / Guide).
- D-Pad / Left Stick / Arrows: Move
- A / Cross / Z: Confirm / Attack
- B / Circle / X: Cancel / Run
- X / Triangle / S: Main Menu
- Y / Square / A: Rotate / Target
- L / Shoulder / Q: Diagonal Movement Lock
- R / Shoulder / E: Quick Menu

CLEAN-ROOM & LEGAL:
-------------------
This software contains zero copyrighted Nintendo DS ROM code, textures,
audio, or assets. Players must provide their own legally owned cartridge dump.
"""


def _locate_dotnet() -> str | None:
    found = shutil.which("dotnet")
    if found:
        return found
    macos_default = Path("/usr/local/share/dotnet/dotnet")
    if macos_default.is_file():
        return str(macos_default)
    return None


def publish_avalonia_launcher(
    rid: str,
    dest_dir: Path,
    *,
    executable_name: str,
    required: bool = False,
) -> bool:
    """Publish the self-contained single-file Avalonia launcher for the target RID."""
    dotnet_bin = _locate_dotnet()
    if not dotnet_bin or not LAUNCHER_CSPROJ.is_file():
        msg = (
            f"Avalonia launcher publish skipped for {rid}: "
            f"dotnet={dotnet_bin!r}, project_exists={LAUNCHER_CSPROJ.is_file()}"
        )
        if required:
            raise SystemExit(f"Cannot package with --require-avalonia: {msg}")
        print(f"[package:avalonia] {msg}")
        return False

    dest_dir.mkdir(parents=True, exist_ok=True)
    src_exe_name = "SkyRecompiled.Launcher.exe" if rid.startswith("win-") else "SkyRecompiled.Launcher"

    with tempfile.TemporaryDirectory(prefix=f"sky-avalonia-{rid}-") as tmp_dir:
        out_dir = Path(tmp_dir)
        cmd = [
            dotnet_bin,
            "publish",
            str(LAUNCHER_CSPROJ),
            "-c",
            "Release",
            "-r",
            rid,
            "--self-contained",
            "true",
            "-p:PublishSingleFile=true",
            "-p:IncludeNativeLibrariesForSelfExtract=true",
            "-o",
            str(out_dir),
        ]
        try:
            print(f"\033[1;36m[package:avalonia]\033[0m Publishing {rid} launcher...")
            subprocess.run(cmd, check=True)
        except (subprocess.CalledProcessError, OSError) as exc:
            if required:
                raise SystemExit(f"Failed to publish Avalonia launcher for {rid}: {exc}") from exc
            print(f"[package:avalonia] Warning: publish failed ({exc}); falling back to script launcher.")
            return False

        published_exe = out_dir / src_exe_name
        if not published_exe.is_file():
            if required:
                raise SystemExit(f"Avalonia publish did not produce expected binary: {published_exe}")
            return False

        dest_exe = dest_dir / executable_name
        shutil.copy2(published_exe, dest_exe)
        if not rid.startswith("win-"):
            dest_exe.chmod(dest_exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        for item in sorted(out_dir.iterdir()):
            if not item.is_file() or item.name == src_exe_name:
                continue
            if item.suffix.lower() in (".pdb", ".dbg", ".xml"):
                continue
            if item.suffix.lower() in (".dll", ".dylib") or ".so" in item.name:
                target = dest_dir / item.name
                shutil.copy2(item, target)
                if not rid.startswith("win-"):
                    target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    return True


def package_macos(*, without_game_banks: bool = False, require_avalonia: bool = False) -> Path:
    """Package the macOS application bundle into a distribution ZIP."""
    print(f"\033[1;36m[package:macos]\033[0m Packaging macOS Application Bundle...")
    package_script = REPO_ROOT / "scripts" / "package_macos_app.py"
    package_cmd = [sys.executable, str(package_script)]
    if without_game_banks:
        package_cmd.append("--without-game-banks")
    if require_avalonia:
        package_cmd.append("--require-avalonia")
    subprocess.run(package_cmd, check=True)

    app_path = BUILD_DIR / "SkyRecompiled.app"
    runner_path = app_path / "Contents" / "MacOS" / "nds_runner"
    if not runner_path.is_file():
        raise SystemExit(
            f"Cannot package macOS: missing {runner_path}. "
            "Build the runner with scripts/build.py before packaging."
        )
    zip_path = DIST_DIR / f"SkyRecompiled-v{VERSION}-macos-arm64.zip"
    DIST_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Creating {zip_path}...")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(app_path):
            for file in files:
                p = Path(root) / file
                arcname = p.relative_to(BUILD_DIR)
                zf.write(p, arcname)

    print(f"\033[1;32m[package:macos:done]\033[0m {zip_path} ({zip_path.stat().st_size / 1024 / 1024:.1f} MB)")
    return zip_path


def verify_linux_elf_x86_64(binary_path: Path) -> None:
    """Verify that the binary is a 64-bit little-endian x86_64 ELF executable."""
    with binary_path.open("rb") as f:
        header = f.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        raise SystemExit(
            f"Cannot package Linux: {binary_path} is not a Linux ELF binary "
            "(looks like a host binary from another OS such as macOS Mach-O). "
            "Build the Linux package on a Linux x86_64 host."
        )
    ei_class = header[4]
    ei_data = header[5]
    e_type, e_machine = struct.unpack("<HH", header[16:20])
    if ei_class != 2 or ei_data != 1 or e_type not in (2, 3) or e_machine != 0x3E:
        raise SystemExit(
            f"Cannot package Linux: {binary_path} is not a 64-bit x86_64 ELF executable "
            f"(EI_CLASS={ei_class}, EI_DATA={ei_data}, e_type={e_type}, e_machine=0x{e_machine:x})."
        )


def package_linux(
    *,
    without_game_banks: bool = False,
    include_avalonia: bool | None = None,
    require_avalonia: bool = False,
) -> Path:
    """Package the Linux standalone distribution into a tar.gz archive."""
    runner_dir = BUILD_DIR / ("runner-fallback" if without_game_banks else "runner")
    runner_src = runner_dir / "nds_runner"
    if not runner_src.is_file():
        raise SystemExit(
            f"Cannot package Linux: missing {runner_src}. "
            "Build on Linux with scripts/build.py before packaging."
        )
    verify_linux_elf_x86_64(runner_src)

    should_publish_avalonia = (
        include_avalonia
        if include_avalonia is not None
        else (require_avalonia or platform.system() == "Linux")
    )

    print(f"\033[1;36m[package:linux]\033[0m Packaging Linux Standalone Distribution...")
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    pkg_dir = BUILD_DIR / f"SkyRecompiled-v{VERSION}-linux-x86_64"
    if pkg_dir.exists():
        shutil.rmtree(pkg_dir)

    bin_dir = pkg_dir / "bin"
    cfg_dir = pkg_dir / "config"
    scripts_dir = pkg_dir / "scripts"
    bin_dir.mkdir(parents=True, exist_ok=True)
    cfg_dir.mkdir(parents=True, exist_ok=True)
    scripts_dir.mkdir(parents=True, exist_ok=True)

    # 1. Launcher & Desktop file
    launcher_file = pkg_dir / "SkyRecompiled"
    launcher_file.write_text(LINUX_LAUNCHER, encoding="utf-8")
    launcher_file.chmod(launcher_file.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    desktop_file = pkg_dir / "SkyRecompiled.desktop"
    desktop_file.write_text(LINUX_DESKTOP, encoding="utf-8")

    # 2. Python launcher fallback
    shutil.copy2(REPO_ROOT / "scripts" / "launcher.py", scripts_dir / "launcher.py")

    # 3. Canonical Avalonia desktop launcher
    if should_publish_avalonia:
        if publish_avalonia_launcher(
            "linux-x64",
            bin_dir,
            executable_name="SkyRecompiled.Launcher",
            required=require_avalonia,
        ):
            verify_linux_elf_x86_64(bin_dir / "SkyRecompiled.Launcher")

    # 4. Config files
    shutil.copy2(CONFIG_DIR / "game.toml", cfg_dir / "game.toml")
    shutil.copy2(CONFIG_DIR / "controls_preset.toml", cfg_dir / "controls_preset.toml")

    # 5. Validated Linux x86_64 binary and optional bundled SDL3 shared libraries
    dest = bin_dir / "nds_runner"
    shutil.copy2(runner_src, dest)
    dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    for extra in sorted(runner_dir.glob("libSDL3.so*")):
        if extra.is_file() and not extra.is_symlink():
            shutil.copy2(extra, bin_dir / extra.name)
            if not (bin_dir / "libSDL3.so.0").exists():
                shutil.copy2(extra, bin_dir / "libSDL3.so.0")
    sdl_license = runner_dir / "SDL3-LICENSE.txt"
    if sdl_license.is_file():
        shutil.copy2(sdl_license, bin_dir / "SDL3-LICENSE.txt")

    readme = README_TEXT
    if without_game_banks:
        readme += (
            "\nThis Linux build runs cartridge code at runtime. It does not include\n"
            "statically recompiled game code. See the project README for a native\n"
            "build using your own ROM.\n"
        )
    (pkg_dir / "README.txt").write_text(readme, encoding="utf-8")

    tar_path = DIST_DIR / f"SkyRecompiled-v{VERSION}-linux-x86_64.tar.gz"
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(pkg_dir, arcname=pkg_dir.name)

    print(f"\033[1;32m[package:linux:done]\033[0m {tar_path} ({tar_path.stat().st_size / 1024 / 1024:.1f} MB)")
    return tar_path


def package_windows(
    *,
    without_game_banks: bool = False,
    include_avalonia: bool | None = None,
    require_avalonia: bool = False,
) -> Path:
    """Package the standalone Windows x64 ZIP distribution with runner, launcher, and SDL3 DLL."""
    runner_dir = BUILD_DIR / ("runner-fallback" if without_game_banks else "runner")
    required_files = ("nds_runner.exe", "SDL3.dll", "SDL3-LICENSE.txt")
    for name in required_files:
        if not (runner_dir / name).is_file():
            raise SystemExit(
                f"Cannot package Windows: missing {runner_dir / name}. "
                "Build on Windows with scripts/build.py and --sdl3-sdk before packaging."
            )

    should_publish_avalonia = (
        include_avalonia
        if include_avalonia is not None
        else (require_avalonia or platform.system() == "Windows")
    )

    print(f"\033[1;36m[package:windows]\033[0m Packaging Windows x64 Distribution...")
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    pkg_dir = BUILD_DIR / f"SkyRecompiled-v{VERSION}-windows-x64"
    if pkg_dir.exists():
        shutil.rmtree(pkg_dir)

    bin_dir = pkg_dir / "bin"
    cfg_dir = pkg_dir / "config"
    scripts_dir = pkg_dir / "scripts"
    bin_dir.mkdir(parents=True, exist_ok=True)
    cfg_dir.mkdir(parents=True, exist_ok=True)
    scripts_dir.mkdir(parents=True, exist_ok=True)

    # 1. Native Windows Launchers
    (pkg_dir / "SkyRecompiled.bat").write_text(WINDOWS_BAT, encoding="utf-8")
    (pkg_dir / "SkyRecompiled.ps1").write_text(WINDOWS_PS1, encoding="utf-8-sig")

    # 2. Canonical Avalonia desktop launcher
    if should_publish_avalonia:
        publish_avalonia_launcher(
            "win-x64",
            pkg_dir,
            executable_name="SkyRecompiled.exe",
            required=require_avalonia,
        )

    # 3. Python launcher fallback
    shutil.copy2(REPO_ROOT / "scripts" / "launcher.py", scripts_dir / "launcher.py")

    # 4. Config files
    shutil.copy2(CONFIG_DIR / "game.toml", cfg_dir / "game.toml")
    shutil.copy2(CONFIG_DIR / "controls_preset.toml", cfg_dir / "controls_preset.toml")

    for name in required_files:
        shutil.copy2(runner_dir / name, bin_dir / name)

    readme = README_TEXT
    if without_game_banks:
        readme += (
            "\nThis Windows build runs cartridge code at runtime. It does not include\n"
            "statically recompiled game code. See the project README for a native\n"
            "build using your own ROM.\n"
        )
    (pkg_dir / "README.txt").write_text(readme, encoding="utf-8")

    zip_path = DIST_DIR / f"SkyRecompiled-v{VERSION}-windows-x64.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(pkg_dir):
            for file in files:
                p = Path(root) / file
                arcname = p.relative_to(pkg_dir.parent)
                zf.write(p, arcname)

    print(f"\033[1;32m[package:windows:done]\033[0m {zip_path} ({zip_path.stat().st_size / 1024 / 1024:.1f} MB)")
    return zip_path


def package_ios() -> Path:
    """Package the ROM-free iOS alpha IPA for distribution."""
    print(f"\033[1;36m[package:ios]\033[0m Packaging iOS Alpha IPA Distribution...")
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    ipa_source = BUILD_DIR / "ios" / "SkyRecompiled-iOS-alpha.ipa"
    if not ipa_source.is_file():
        build_ios_script = REPO_ROOT / "scripts" / "build_ios.py"
        subprocess.run([sys.executable, str(build_ios_script)], check=True)
    if not ipa_source.is_file():
        raise SystemExit(f"Cannot package iOS: missing {ipa_source}")

    ipa_dist = DIST_DIR / f"SkyRecompiled-v{VERSION}-ios.ipa"
    shutil.copy2(ipa_source, ipa_dist)
    print(f"\033[1;32m[package:ios:done]\033[0m {ipa_dist} ({ipa_dist.stat().st_size / 1024 / 1024:.1f} MB)")
    return ipa_dist


def main() -> None:
    """Parse arguments and create distribution packages for specified platforms."""
    parser = argparse.ArgumentParser(description="SkyRecompiled Release Packager")
    parser.add_argument("--platform", choices=["macos", "linux", "windows", "ios", "all"], default="all")
    parser.add_argument(
        "--without-game-banks",
        action="store_true",
        help="macOS/Linux/Windows: package the build from build/runner-fallback",
    )
    parser.add_argument(
        "--require-avalonia",
        action="store_true",
        help="Fail instead of falling back to script launchers if the Avalonia launcher cannot be published",
    )
    parser.add_argument(
        "--skip-avalonia",
        action="store_true",
        help="Linux/Windows: skip publishing the Avalonia desktop launcher",
    )
    args = parser.parse_args()
    if args.without_game_banks and args.platform == "ios":
        parser.error("--without-game-banks is not supported for --platform ios")
    if args.require_avalonia and args.skip_avalonia:
        parser.error("--require-avalonia and --skip-avalonia cannot be used together")

    include_avalonia = False if args.skip_avalonia else None

    DIST_DIR.mkdir(parents=True, exist_ok=True)
    host_system = platform.system()

    if args.platform == "macos" or (args.platform == "all" and host_system == "Darwin"):
        package_macos(
            without_game_banks=args.without_game_banks,
            require_avalonia=args.require_avalonia,
        )
    if args.platform == "linux" or (args.platform == "all" and host_system == "Linux"):
        package_linux(
            without_game_banks=args.without_game_banks,
            include_avalonia=include_avalonia,
            require_avalonia=args.require_avalonia,
        )
    if args.platform == "windows" or (args.platform == "all" and host_system == "Windows"):
        package_windows(
            without_game_banks=args.without_game_banks,
            include_avalonia=include_avalonia,
            require_avalonia=args.require_avalonia,
        )
    if args.platform == "ios" or (args.platform == "all" and host_system == "Darwin"):
        package_ios()

    print("\n\033[1;32mAll release packages generated in build/dist/\033[0m")
    for f in DIST_DIR.iterdir():
        print(f"  - {f.name} ({f.stat().st_size / 1024 / 1024:.2f} MB)")


if __name__ == "__main__":
    main()

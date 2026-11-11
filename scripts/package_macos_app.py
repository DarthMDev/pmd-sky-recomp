#!/usr/bin/env python3
"""
SkyRecompiled -- macOS Application Packager
==========================================
Packages SkyRecompiled into a native macOS Application Bundle (`SkyRecompiled.app`):
  - Contents/Info.plist (Retina-ready, category: games)
  - Contents/MacOS/SkyRecompiled (Native launcher with first-run ROM picker & settings persistence)
  - Contents/MacOS/nds_runner (Native arm64 compiled static recompilation executable)
  - Contents/Resources/game.toml (Cartridge configuration)

Settings and save data are persisted in:
  ~/Library/Application Support/SkyRecompiled/
    ├── settings.toml
    ├── saves/pmdsky.sav
    └── console/
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
RUNNER_BIN = BUILD_DIR / "runner" / "nds_runner"
RUNNER_FALLBACK_BIN = BUILD_DIR / "runner-fallback" / "nds_runner"
CONFIG_TOML = REPO_ROOT / "config" / "game.toml"
DEFAULT_APP_DIR = BUILD_DIR / "SkyRecompiled.app"

INFO_PLIST_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleDisplayName</key>
    <string>SkyRecompiled</string>
    <key>CFBundleExecutable</key>
    <string>SkyRecompiled</string>
    <key>CFBundleIdentifier</key>
    <string>dev.skyrecompiled.app</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>SkyRecompiled</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
"""

LAUNCHER_SCRIPT = """#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

CLEAN_USA_SHA1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
CLEAN_EU_SHA1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139"
VALID_ROM_SHA1S = {CLEAN_USA_SHA1, CLEAN_EU_SHA1}
BUNDLE_CONTENTS = Path(__file__).resolve().parent.parent
NDS_RUNNER = BUNDLE_CONTENTS / "MacOS" / "nds_runner"
GAME_TOML = BUNDLE_CONTENTS / "Resources" / "game.toml"

APP_SUPPORT = Path.home() / "Library" / "Application Support" / "SkyRecompiled"
SETTINGS_FILE = APP_SUPPORT / "settings.toml"
SKY_SETTINGS_FILE = APP_SUPPORT / "sky_settings.toml"
SAVES_DIR = APP_SUPPORT / "saves"
CONSOLE_DIR = APP_SUPPORT / "console"


def alert(title: str, message: str) -> None:
    try:
        safe_title = title.replace('"', '\\\\"')
        safe_msg = message.replace('"', '\\\\"')
        apple_script = f'display alert "{safe_title}" message "{safe_msg}" as critical'
        res = subprocess.run(["osascript", "-e", apple_script], capture_output=True)
        if res.returncode == 0:
            return
    except Exception:
        pass

    try:
        import tkinter as tk
        from tkinter import messagebox
        root = tk.Tk()
        root.withdraw()
        root.update()
        root.lift()
        root.attributes("-topmost", True)
        messagebox.showerror(title, message)
        root.destroy()
        return
    except Exception:
        pass


def pick_rom() -> Optional[str]:
    prompt = "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky (USA or Europe) .nds ROM"
    # 1. Native macOS AppleScript (displays prompt instructions at top of file dialog)
    try:
        apple_script = (
            'try\\n'
            f'  set f to choose file with prompt "{prompt}" of type {{"nds"}}\\n'
            '  return POSIX path of f\\n'
            'on error number -128\\n'
            '  return "USER_CANCELLED"\\n'
            'on error\\n'
            '  return ""\\n'
            'end try'
        )
        res = subprocess.run(["osascript", "-e", apple_script], capture_output=True, text=True)
        if res.returncode == 0:
            out = res.stdout.strip()
            if out == "USER_CANCELLED":
                return None
            if out:
                return out
    except Exception:
        pass

    # 2. Tkinter fallback
    try:
        import tkinter as tk
        from tkinter import filedialog
        root = tk.Tk()
        root.withdraw()
        root.update()
        root.lift()
        root.attributes("-topmost", True)
        path = filedialog.askopenfilename(
            title=prompt,
            filetypes=[
                ("Nintendo DS ROM", "*.nds"),
                ("All Files", "*.*"),
            ],
        )
        root.destroy()
        return path if path else None
    except Exception:
        pass

    return None


class ProgressDialog:
    def __init__(self, title: str = "SkyRecompiled", message: str = "Starting SkyRecompiled..."):
        self.root = None
        try:
            import tkinter as tk
            from tkinter import ttk
            self.tk = tk
            self.ttk = ttk
            self.root = tk.Tk()
            self.root.title(title)
            self.root.resizable(False, False)

            w, h = 420, 115
            ws = self.root.winfo_screenwidth()
            hs = self.root.winfo_screenheight()
            x = max(0, (ws // 2) - (w // 2))
            y = max(0, (hs // 2) - (h // 2))
            self.root.geometry(f"{w}x{h}+{x}+{y}")
            self.root.attributes("-topmost", True)
            self.root.lift()

            frame = ttk.Frame(self.root, padding="18 16 18 16")
            frame.pack(fill=tk.BOTH, expand=True)

            self.label = ttk.Label(frame, text=message, font=("Helvetica", 12))
            self.label.pack(anchor=tk.W, pady=(0, 10))

            self.pb = ttk.Progressbar(frame, orient="horizontal", length=380, mode="determinate")
            self.pb.pack(fill=tk.X, expand=True)
            self.pb["maximum"] = 100
            self.pb["value"] = 0

            self.root.update()
        except Exception:
            self.root = None

    def update(self, value: float, text: Optional[str] = None) -> None:
        if not self.root:
            return
        try:
            self.pb["value"] = value
            if text:
                self.label.config(text=text)
            self.root.update()
        except Exception:
            pass

    def close(self) -> None:
        if not self.root:
            return
        try:
            self.root.destroy()
            self.root = None
        except Exception:
            pass


def load_settings() -> dict:
    if not SETTINGS_FILE.exists():
        return {}
    settings = {}
    for line in SETTINGS_FILE.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.strip().startswith("#"):
            k, v = line.split("=", 1)
            settings[k.strip()] = v.strip().strip('"').strip("'")
    return settings


def save_settings(settings: dict) -> None:
    APP_SUPPORT.mkdir(parents=True, exist_ok=True)
    lines = [f'{k} = "{v}"' for k, v in settings.items()]
    SETTINGS_FILE.write_text("\\n".join(lines) + "\\n", encoding="utf-8")


def verify_sha1(path_str: str, progress_cb=None) -> bool:
    p = Path(path_str)
    if not p.is_file():
        return False
    size = p.stat().st_size
    if size == 0:
        return False
    h = hashlib.sha1()
    read_bytes = 0
    with p.open("rb") as f:
        while chunk := f.read(1048576):  # 1MB blocks
            h.update(chunk)
            read_bytes += len(chunk)
            if progress_cb:
                pct = (read_bytes / size) * 60.0
                progress_cb(pct, f"Verifying ROM integrity... {int((read_bytes / size) * 100)}%")
    return h.hexdigest().lower() in VALID_ROM_SHA1S


def main():
    os.environ["TK_SILENCE_DEPRECATION"] = "1"
    APP_SUPPORT.mkdir(parents=True, exist_ok=True)
    SAVES_DIR.mkdir(parents=True, exist_ok=True)
    CONSOLE_DIR.mkdir(parents=True, exist_ok=True)

    settings = load_settings()
    rom_path = settings.get("rom_path")

    progress = None
    while not rom_path or not verify_sha1(rom_path):
        if rom_path and not verify_sha1(rom_path):
            alert("Invalid ROM Selected", "The selected file does not match a verified clean Explorers of Sky ROM (USA or Europe SHA-1 mismatch).")
        chosen = pick_rom()
        if not chosen:
            # User canceled
            sys.exit(0)

        progress = ProgressDialog("SkyRecompiled", "Verifying cartridge ROM...")
        if verify_sha1(chosen, progress.update):
            rom_path = chosen
            settings["rom_path"] = rom_path
            save_settings(settings)
            progress.update(75, "Configuring game environment & saves...")
            time.sleep(0.3)
        else:
            progress.close()
            progress = None
            rom_path = None

    if not progress:
        progress = ProgressDialog("SkyRecompiled", "Starting SkyRecompiled...")
        progress.update(40, "Loading static recompilation banks...")
        time.sleep(0.3)

    progress.update(90, "Launching game engine...")

    save_path = SAVES_DIR / "pmdsky.sav"
    cmd = [
        str(NDS_RUNNER),
        str(CONSOLE_DIR),
        "--interactive",
        "--config", str(GAME_TOML),
        "--rom", str(rom_path),
        "--save-path", str(save_path),
        "--runtime-settings-path", str(SKY_SETTINGS_FILE),
    ]

    # Hand off to nds_runner
    if not NDS_RUNNER.exists():
        progress.close()
        alert(
            "Engine Not Compiled",
            "Could not locate nds_runner inside the application bundle.\\n\\n"
            "If this bundle came from an uncompiled CI skeleton, please build SkyRecompiled with 'scripts/build.py' or download the release build with pre-compiled game banks.",
        )
        sys.exit(1)

    proc = subprocess.Popen(cmd)
    time.sleep(0.8)
    progress.close()
    proc.wait()
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
"""


def package_app(
    out_app: Path, *, without_game_banks: bool = False, allow_missing_runner: bool = False
) -> None:
    print(f"\033[1;36m[SkyRecompiled:package]\033[0m Packaging {out_app}...")
    runner_bin = RUNNER_FALLBACK_BIN if without_game_banks else RUNNER_BIN
    if not CONFIG_TOML.exists():
        print(f"\033[1;31m[ERROR]\033[0m Cartridge config not found at {CONFIG_TOML}.")
        sys.exit(1)

    contents_dir = out_app / "Contents"
    macos_dir = contents_dir / "MacOS"
    resources_dir = contents_dir / "Resources"

    if out_app.exists():
        shutil.rmtree(out_app)

    macos_dir.mkdir(parents=True, exist_ok=True)
    resources_dir.mkdir(parents=True, exist_ok=True)

    # 1. Info.plist
    (contents_dir / "Info.plist").write_text(INFO_PLIST_TEMPLATE, encoding="utf-8")
    (contents_dir / "PkgInfo").write_bytes(b"APPL????")

    # 2. Config TOML
    shutil.copy2(CONFIG_TOML, resources_dir / "game.toml")

    # 3. Executables
    if runner_bin.exists():
        dest_runner = macos_dir / "nds_runner"
        shutil.copy2(runner_bin, dest_runner)
        dest_runner.chmod(dest_runner.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    elif allow_missing_runner:
        print("[SkyRecompiled:package] Note: nds_runner not yet compiled; packaging application wrapper skeleton.")
    else:
        raise SystemExit(
            f"Cannot package macOS: missing {runner_bin}. "
            "Run scripts/build.py before packaging, or pass --allow-missing-runner "
            "to create a launcher-only skeleton."
        )

    # Build and copy Avalonia C# launcher
    launcher_proj = REPO_ROOT / "src" / "SkyRecompiled.Launcher" / "SkyRecompiled.Launcher.csproj"
    publish_dir = REPO_ROOT / "src" / "SkyRecompiled.Launcher" / "bin" / "Release" / "net8.0" / "osx-arm64" / "publish"
    dotnet_bin = shutil.which("dotnet") or "/usr/local/share/dotnet/dotnet"

    if Path(dotnet_bin).exists() and launcher_proj.exists():
        print(f"\033[1;36m[SkyRecompiled:package]\033[0m Building native Avalonia C# launcher with {dotnet_bin}...")
        env = dict(os.environ)
        env["PATH"] = f"/usr/local/share/dotnet:{env.get('PATH', '')}"
        subprocess.run([
            dotnet_bin, "publish", str(launcher_proj),
            "-c", "Release",
            "-r", "osx-arm64",
            "--self-contained", "true",
            "-p:PublishSingleFile=true"
        ], check=True, env=env)

        for item in publish_dir.iterdir():
            if item.is_file() and not item.name.endswith(".pdb"):
                target_name = "SkyRecompiled" if item.name == "SkyRecompiled.Launcher" else item.name
                dest = macos_dir / target_name
                shutil.copy2(item, dest)
                dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        # Copy required native dynamic libraries (SkiaSharp, HarfBuzzSharp, AvaloniaNative)
        release_bin_dir = launcher_proj.parent / "bin" / "Release" / "net8.0" / "osx-arm64"
        for dylib_name in ["libSkiaSharp.dylib", "libHarfBuzzSharp.dylib", "libAvaloniaNative.dylib"]:
            src_dylib = release_bin_dir / dylib_name
            if src_dylib.exists():
                dest_dylib = macos_dir / dylib_name
                shutil.copy2(src_dylib, dest_dylib)
                dest_dylib.chmod(dest_dylib.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        print("\033[1;32m[SkyRecompiled:package]\033[0m Integrated Avalonia C# native launcher and graphics dylibs into bundle.")
    else:
        print("[SkyRecompiled:package] Fallback: using Python wrapper launcher.")
        script_launcher = macos_dir / "SkyRecompiledLauncher.py"
        script_launcher.write_text(LAUNCHER_SCRIPT, encoding="utf-8")
        script_launcher.chmod(script_launcher.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        launcher_src = REPO_ROOT / "scripts" / "macos_launcher.c"
        dest_launcher = macos_dir / "SkyRecompiled"
        compile_cmd = [
            "clang",
            "-O2",
            "-arch", "arm64",
            "-arch", "x86_64",
            str(launcher_src),
            "-o", str(dest_launcher),
        ]
        subprocess.run(compile_cmd, check=True)
        dest_launcher.chmod(dest_launcher.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    # 4. Code sign and remove quarantine so LaunchServices double-click works seamlessly
    try:
        subprocess.run(["xattr", "-cr", str(out_app)], capture_output=True)
        subprocess.run(["codesign", "--force", "--deep", "-s", "-", str(out_app)], capture_output=True)
    except Exception:
        pass

    print(f"\033[1;32m[SUCCESS]\033[0m Successfully created native macOS bundle: {out_app}")


def main():
    parser = argparse.ArgumentParser(description="Package SkyRecompiled.app macOS bundle")
    parser.add_argument("--out", type=str, default=str(DEFAULT_APP_DIR), help="Output .app bundle path")
    parser.add_argument(
        "--without-game-banks",
        action="store_true",
        help="Bundle build/runner-fallback/nds_runner (cartridge runtime execution)",
    )
    parser.add_argument(
        "--allow-missing-runner",
        action="store_true",
        help="Package without nds_runner instead of failing (creates a non-launchable skeleton)",
    )
    args = parser.parse_args()

    if platform.system() != "Darwin":
        print("[WARNING] App bundling is specific to macOS. Creating bundle structure anyway...")

    package_app(
        Path(args.out).resolve(),
        without_game_banks=args.without_game_banks,
        allow_missing_runner=args.allow_missing_runner,
    )


if __name__ == "__main__":
    main()

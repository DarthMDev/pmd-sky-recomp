#!/usr/bin/env python3
"""
SkyRecompiled — macOS Application Packager
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
import hashlib
import os
import subprocess
import sys
from pathlib import Path

CLEAN_USA_SHA1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
BUNDLE_CONTENTS = Path(__file__).resolve().parent.parent
NDS_RUNNER = BUNDLE_CONTENTS / "MacOS" / "nds_runner"
GAME_TOML = BUNDLE_CONTENTS / "Resources" / "game.toml"

APP_SUPPORT = Path.home() / "Library" / "Application Support" / "SkyRecompiled"
SETTINGS_FILE = APP_SUPPORT / "settings.toml"
SAVES_DIR = APP_SUPPORT / "saves"
CONSOLE_DIR = APP_SUPPORT / "console"


def alert(title: str, message: str) -> None:
    safe_title = title.replace('"', '\\\\"')
    safe_msg = message.replace('"', '\\\\"')
    apple_script = f'display alert "{safe_title}" message "{safe_msg}" as critical'
    subprocess.run(["osascript", "-e", apple_script])


def pick_rom() -> str | None:
    apple_script = (
        'try\\n'
        '  set f to choose file with prompt "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky (USA) .nds ROM" of type {"nds"}\\n'
        '  return POSIX path of f\\n'
        'on error\\n'
        '  return ""\\n'
        'end try'
    )
    res = subprocess.run(["osascript", "-e", apple_script], capture_output=True, text=True)
    out = res.stdout.strip()
    return out if out else None


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


def verify_sha1(path_str: str) -> bool:
    p = Path(path_str)
    if not p.is_file():
        return False
    h = hashlib.sha1()
    with p.open("rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest() == CLEAN_USA_SHA1


def main():
    APP_SUPPORT.mkdir(parents=True, exist_ok=True)
    SAVES_DIR.mkdir(parents=True, exist_ok=True)
    CONSOLE_DIR.mkdir(parents=True, exist_ok=True)

    settings = load_settings()
    rom_path = settings.get("rom_path")

    while not rom_path or not verify_sha1(rom_path):
        if rom_path and not verify_sha1(rom_path):
            alert("Invalid ROM Selected", "The selected file does not match the clean Explorers of Sky (USA) ROM (SHA-1 mismatch).")
        chosen = pick_rom()
        if not chosen:
            # User canceled
            sys.exit(0)
        if verify_sha1(chosen):
            rom_path = chosen
            settings["rom_path"] = rom_path
            save_settings(settings)
        else:
            rom_path = None

    save_path = SAVES_DIR / "pmdsky.sav"
    cmd = [
        str(NDS_RUNNER),
        str(CONSOLE_DIR),
        "--interactive",
        "--config", str(GAME_TOML),
        "--rom", str(rom_path),
        "--save-path", str(save_path),
    ]

    # Hand off to nds_runner
    os.execv(str(NDS_RUNNER), cmd)


if __name__ == "__main__":
    main()
"""


def package_app(out_app: Path) -> None:
    print(f"\033[1;36m[SkyRecompiled:package]\033[0m Packaging {out_app}...")
    if not RUNNER_BIN.exists():
        print(f"\033[1;31m[ERROR]\033[0m Runner binary not found at {RUNNER_BIN}. Run scripts/build.py first.")
        sys.exit(1)

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
    # Copy nds_runner
    dest_runner = macos_dir / "nds_runner"
    shutil.copy2(RUNNER_BIN, dest_runner)
    dest_runner.chmod(dest_runner.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    # Write launcher wrapper
    dest_launcher = macos_dir / "SkyRecompiled"
    dest_launcher.write_text(LAUNCHER_SCRIPT, encoding="utf-8")
    dest_launcher.chmod(dest_launcher.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    print(f"\033[1;32m[SUCCESS]\033[0m Successfully created native macOS bundle: {out_app}")


def main():
    parser = argparse.ArgumentParser(description="Package SkyRecompiled.app macOS bundle")
    parser.add_argument("--out", type=str, default=str(DEFAULT_APP_DIR), help="Output .app bundle path")
    args = parser.parse_args()

    if platform.system() != "Darwin":
        print("[WARNING] App bundling is specific to macOS. Creating bundle structure anyway...")

    package_app(Path(args.out).resolve())


if __name__ == "__main__":
    main()

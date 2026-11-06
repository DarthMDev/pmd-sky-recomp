#!/usr/bin/env python3
"""
SkyRecompiled -- Cross-Platform Native Launcher & ROM Setup
===========================================================
Universal GUI launcher and first-run ROM selector for:
  - macOS (Apple Silicon & Intel)
  - Windows (x64)
  - Linux (x86_64 & arm64)

Features:
  - Native OS GUI file picker on first launch (osascript / PowerShell / Zenity / KDialog / Tkinter)
  - Clean SHA-1 verification for both USA (5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152)
    and Europe (c838a5adf1ed32d2da8454976e5b1a1aa189c139) ROMs
  - Persistent user settings across launches
  - Standard OS user directories:
      macOS:   ~/Library/Application Support/SkyRecompiled/
      Windows: %APPDATA%/SkyRecompiled/
      Linux:   ~/.local/share/SkyRecompiled/
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

CLEAN_USA_SHA1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
CLEAN_EU_SHA1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139"
VALID_ROM_SHA1S = {CLEAN_USA_SHA1, CLEAN_EU_SHA1}


def get_user_data_dir() -> Path:
    sys_name = platform.system()
    if sys_name == "Darwin":
        return Path.home() / "Library" / "Application Support" / "SkyRecompiled"
    elif sys_name == "Windows":
        appdata = os.environ.get("APPDATA")
        base = Path(appdata) if appdata else Path.home() / "AppData" / "Roaming"
        return base / "SkyRecompiled"
    else:  # Linux / BSD
        xdg_data = os.environ.get("XDG_DATA_HOME")
        base = Path(xdg_data) if xdg_data else Path.home() / ".local" / "share"
        return base / "SkyRecompiled"


def find_binary_and_config() -> tuple[Path, Path]:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    # Candidate runner paths
    exe_name = "nds_runner.exe" if platform.system() == "Windows" else "nds_runner"
    runner_candidates = [
        # Inside macOS .app bundle
        script_dir / exe_name,
        script_dir.parent / "MacOS" / exe_name,
        # Package layout
        script_dir / "bin" / exe_name,
        script_dir / exe_name,
        # Repository layout
        repo_root / "build" / "runner" / exe_name,
        repo_root / "build" / exe_name,
    ]
    runner_bin = next((p for p in runner_candidates if p.is_file()), None)

    # Candidate config paths
    config_candidates = [
        # Inside macOS .app bundle
        script_dir.parent / "Resources" / "game.toml",
        script_dir / "game.toml",
        script_dir / "config" / "game.toml",
        repo_root / "config" / "game.toml",
        repo_root / "game.toml",
    ]
    config_file = next((p for p in config_candidates if p.is_file()), None)

    if not runner_bin:
        raise FileNotFoundError(f"Could not locate {exe_name}. Please build SkyRecompiled first.")
    if not config_file:
        raise FileNotFoundError("Could not locate game.toml cartridge configuration.")

    return runner_bin, config_file


def alert(title: str, message: str) -> None:
    sys_name = platform.system()
    if sys_name == "Darwin":
        try:
            safe_title = title.replace('"', '\\"')
            safe_msg = message.replace('"', '\\"')
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
    elif sys_name == "Windows":
        try:
            safe_title = title.replace("'", "''")
            safe_msg = message.replace("'", "''")
            ps_script = (
                f"[System.Windows.Forms.MessageBox]::Show('{safe_msg}', '{safe_title}', "
                "[System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error)"
            )
            subprocess.run(["powershell", "-WindowStyle", "Hidden", "-Command", ps_script])
            return
        except Exception:
            pass
        try:
            import tkinter as tk
            from tkinter import messagebox
            root = tk.Tk()
            root.withdraw()
            messagebox.showerror(title, message)
            root.destroy()
            return
        except Exception:
            pass
    else:  # Linux
        try:
            subprocess.run(["zenity", "--error", f"--title={title}", f"--text={message}"])
            return
        except Exception:
            pass
        try:
            subprocess.run(["kdialog", "--error", message, "--title", title])
            return
        except Exception:
            pass
        try:
            import tkinter as tk
            from tkinter import messagebox
            root = tk.Tk()
            root.withdraw()
            messagebox.showerror(title, message)
            root.destroy()
            return
        except Exception:
            pass

    print(f"\n[ERROR: {title}] {message}\n", file=sys.stderr)


def pick_rom() -> Optional[str]:
    sys_name = platform.system()
    prompt = "Select your legally owned Pokémon Mystery Dungeon: Explorers of Sky (USA or Europe) .nds ROM"

    if sys_name == "Darwin":
        # 1. Native macOS AppleScript (displays prompt instructions at top of file dialog)
        try:
            apple_script = (
                'try\n'
                f'  set f to choose file with prompt "{prompt}" of type {{"nds"}}\n'
                '  return POSIX path of f\n'
                'on error number -128\n'
                '  return "USER_CANCELLED"\n'
                'on error\n'
                '  return ""\n'
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

    elif sys_name == "Windows":
        # 1. Native PowerShell OpenFileDialog (works on every modern Windows installation out of the box)
        ps_script = (
            "Add-Type -AssemblyName System.Windows.Forms; "
            "$f = New-Object System.Windows.Forms.OpenFileDialog; "
            "$f.Filter = 'Nintendo DS ROM (*.nds)|*.nds|All Files (*.*)|*.*'; "
            f"$f.Title = '{prompt}'; "
            "if ($f.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $f.FileName }"
        )
        try:
            res = subprocess.run(
                ["powershell", "-WindowStyle", "Hidden", "-Command", ps_script],
                capture_output=True,
                text=True,
            )
            out = res.stdout.strip()
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
            root.attributes("-topmost", True)
            path = filedialog.askopenfilename(
                title=prompt,
                filetypes=[("Nintendo DS ROM", "*.nds"), ("All Files", "*.*")],
            )
            root.destroy()
            if path:
                return path
        except Exception:
            pass

    else:  # Linux / BSD
        # 1. Zenity (GNOME / Ubuntu / Fedora standard)
        try:
            res = subprocess.run(
                [
                    "zenity",
                    "--file-selection",
                    f"--title={prompt}",
                    "--file-filter=Nintendo DS ROM (*.nds) | *.nds *.NDS",
                    "--file-filter=All Files | *",
                ],
                capture_output=True,
                text=True,
            )
            out = res.stdout.strip()
            if out:
                return out
        except Exception:
            pass

        # 2. KDialog (KDE Plasma standard)
        try:
            res = subprocess.run(
                [
                    "kdialog",
                    "--getopenfilename",
                    str(Path.home()),
                    "*.nds *.NDS|Nintendo DS ROM (*.nds)",
                    "--title",
                    prompt,
                ],
                capture_output=True,
                text=True,
            )
            out = res.stdout.strip()
            if out:
                return out
        except Exception:
            pass

        # 3. Tkinter
        try:
            import tkinter as tk
            from tkinter import filedialog
            root = tk.Tk()
            root.withdraw()
            root.attributes("-topmost", True)
            path = filedialog.askopenfilename(
                title=prompt,
                filetypes=[("Nintendo DS ROM", "*.nds"), ("All Files", "*.*")],
            )
            root.destroy()
            if path:
                return path
        except Exception:
            pass

    # Terminal / headless fallback
    print("\n" + "=" * 60)
    print("  SkyRecompiled -- Initial Setup")
    print("=" * 60)
    print(f"{prompt}\n")
    try:
        val = input("Enter full path to .nds ROM (or Enter to cancel): ").strip().strip('"').strip("'")
        return val if val else None
    except (EOFError, KeyboardInterrupt):
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
        while chunk := f.read(1048576):  # 1MB chunks
            h.update(chunk)
            read_bytes += len(chunk)
            if progress_cb:
                pct = (read_bytes / size) * 60.0
                progress_cb(pct, f"Verifying ROM integrity... {int((read_bytes / size) * 100)}%")
    return h.hexdigest().lower() in VALID_ROM_SHA1S


def load_settings(settings_file: Path) -> dict[str, str]:
    if not settings_file.exists():
        return {}
    settings: dict[str, str] = {}
    for line in settings_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            settings[k.strip()] = v.strip().strip('"').strip("'")
    return settings


def save_settings(settings_file: Path, settings: dict[str, str]) -> None:
    settings_file.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# SkyRecompiled User Configuration",
        *[f'{k} = "{v}"' for k, v in sorted(settings.items())],
    ]
    settings_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="SkyRecompiled Launcher")
    parser.add_argument("--rom", type=str, default=None, help="Explicit ROM path override")
    parser.add_argument("--reset-rom", action="store_true", help="Prompt to select a new ROM")
    args = parser.parse_args()

    user_dir = get_user_data_dir()
    user_dir.mkdir(parents=True, exist_ok=True)
    saves_dir = user_dir / "saves"
    saves_dir.mkdir(parents=True, exist_ok=True)
    console_dir = user_dir / "console"
    console_dir.mkdir(parents=True, exist_ok=True)
    settings_file = user_dir / "settings.toml"

    os.environ["TK_SILENCE_DEPRECATION"] = "1"
    settings = load_settings(settings_file)
    rom_path = args.rom or settings.get("rom_path")

    if args.reset_rom:
        rom_path = None

    progress = None
    while not rom_path or not verify_sha1(rom_path):
        if rom_path and not verify_sha1(rom_path):
            alert(
                "Invalid ROM Selected",
                "The selected file does not match a verified clean Explorers of Sky ROM (USA or Europe SHA-1 mismatch).\n\n"
                "Please select an authentic, unmodified Pokémon Mystery Dungeon: Explorers of Sky .nds cartridge dump.",
            )
        chosen = pick_rom()
        if not chosen:
            print("[SkyRecompiled] ROM selection cancelled by user.")
            sys.exit(0)

        progress = ProgressDialog("SkyRecompiled", "Verifying cartridge ROM...")
        if verify_sha1(chosen, progress.update):
            rom_path = chosen
            settings["rom_path"] = rom_path
            save_settings(settings_file, settings)
            progress.update(75, "Configuring game environment & saves...")
            time.sleep(0.3)
        else:
            progress.close()
            progress = None
            rom_path = chosen  # will loop and show alert

    if not progress:
        progress = ProgressDialog("SkyRecompiled", "Starting SkyRecompiled...")
        progress.update(40, "Loading static recompilation banks...")
        time.sleep(0.3)

    progress.update(90, "Launching game engine...")

    runner_bin, config_file = find_binary_and_config()
    save_path = saves_dir / "pmdsky.sav"
    savestate_dir = user_dir / "savestates"
    savestate_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(runner_bin),
        str(console_dir),
        "--interactive",
        "--config", str(config_file),
        "--rom", str(rom_path),
        "--save-path", str(save_path),
        "--savestate-dir", str(savestate_dir),
        "--rtc-host",
    ]

    print(f"\033[1;32m[SkyRecompiled]\033[0m Launching: {runner_bin}")
    print(f"  ROM:       {rom_path}")
    print(f"  Save file: {save_path}")

    proc = subprocess.Popen(cmd)
    time.sleep(0.8)
    progress.close()
    proc.wait()
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Unit tests for scripts/launcher.py settings persistence, reset, and ROM verification."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

import scripts.launcher as launcher


class TestLauncher(unittest.TestCase):
    def test_verify_sha1_missing_empty_and_progress(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            missing = base / "missing.nds"
            self.assertFalse(launcher.verify_sha1(str(missing)))

            empty = base / "empty.nds"
            empty.write_bytes(b"")
            self.assertFalse(launcher.verify_sha1(str(empty)))

            sample = base / "Pokémon Mystery Dungeon (Spaces & Unicode é).nds"
            payload = b"SKY_RECOMPILED_TEST_ROM" * 1024
            sample.write_bytes(payload)
            sample_sha1 = hashlib.sha1(payload).hexdigest()

            self.assertFalse(launcher.verify_sha1(str(sample)))
            seen_progress: list[tuple[float, str]] = []
            with mock.patch.object(launcher, "VALID_ROM_SHA1S", {sample_sha1}):
                self.assertTrue(
                    launcher.verify_sha1(
                        str(sample),
                        progress_cb=lambda pct, msg: seen_progress.append((pct, msg)),
                    )
                )
            self.assertTrue(seen_progress)
            self.assertAlmostEqual(seen_progress[-1][0], 60.0)
            self.assertIn("100%", seen_progress[-1][1])

    def test_save_settings_preserves_unrelated_keys_and_unicode_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            settings_file = Path(td) / "nested" / "settings.toml"
            settings_file.parent.mkdir(parents=True, exist_ok=True)
            settings_file.write_text(
                '# Existing\nrescue_endpoint = "https://example.invalid"\nui_theme = "dark"\n',
                encoding="utf-8",
            )

            rom_path = "/Users/Player One/ROMs/Pokémon Explorers of Sky (USA).nds"
            launcher.save_settings(settings_file, {"rom_path": rom_path})

            loaded = launcher.load_settings(settings_file)
            self.assertEqual(loaded["rom_path"], rom_path)
            self.assertEqual(loaded["rescue_endpoint"], "https://example.invalid")
            self.assertEqual(loaded["ui_theme"], "dark")
            self.assertEqual(list(settings_file.parent.iterdir()), [settings_file])

    def test_clear_saved_rom_path_preserves_other_settings(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            settings_file = Path(td) / "settings.toml"
            launcher.save_settings(
                settings_file,
                {"rom_path": "/tmp/sky.nds", "custom_flag": "enabled"},
                merge=False,
            )
            launcher.clear_saved_rom_path(settings_file)
            loaded = launcher.load_settings(settings_file)
            self.assertNotIn("rom_path", loaded)
            self.assertEqual(loaded["custom_flag"], "enabled")
            self.assertEqual(list(settings_file.parent.iterdir()), [settings_file])

            # Clearing again when rom_path is absent is a clean no-op
            launcher.clear_saved_rom_path(settings_file)
            self.assertEqual(launcher.load_settings(settings_file), {"custom_flag": "enabled"})

    def test_csharp_launcher_suite(self) -> None:
        dotnet = shutil.which("dotnet") or (
            "/usr/local/share/dotnet/dotnet" if Path("/usr/local/share/dotnet/dotnet").is_file() else None
        )
        if not dotnet:
            self.skipTest("dotnet SDK not installed")
        sdk_check = subprocess.run([dotnet, "--list-sdks"], capture_output=True, text=True, check=False)
        if sdk_check.returncode != 0 or not sdk_check.stdout.strip():
            self.skipTest("dotnet SDK not installed")

        csproj = REPO_ROOT / "tests" / "SkyRecompiled.Launcher.Tests" / "SkyRecompiled.Launcher.Tests.csproj"
        run_res = subprocess.run(
            [dotnet, "run", "--project", str(csproj)],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            run_res.returncode,
            0,
            msg=f"C# launcher tests failed:\nSTDOUT:\n{run_res.stdout}\nSTDERR:\n{run_res.stderr}",
        )


if __name__ == "__main__":
    unittest.main()

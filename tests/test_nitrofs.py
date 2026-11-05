#!/usr/bin/env python3
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.sky_mod_packer import NitroFS, ModMetadata
ROM_PATH = REPO_ROOT / "roms" / "pmdsky.us.nds"


class TestNitroFS(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ROM_PATH.is_file():
            raise unittest.SkipTest(f"ROM not found at {ROM_PATH}")
        cls.rom_bytes = ROM_PATH.read_bytes()

    def test_parse_rom_nitrofs(self):
        fs = NitroFS.from_rom(self.rom_bytes)
        self.assertGreater(len(fs.files), 100, "Expected at least 100 files in EoS ROM")
        # Check some known PMDSky asset paths
        known_samples = [f for f in fs.files.keys() if f.endswith(".bin") or f.endswith(".smd")]
        self.assertTrue(len(known_samples) > 0, "Expected .bin or .smd files in NitroFS")

    def test_layered_mod_priority(self):
        fs = NitroFS()
        fs.files["data/balance.bin"] = b"BASE_BALANCE_DATA"
        fs.files["data/portrait.bin"] = b"BASE_PORTRAIT_DATA"

        with tempfile.TemporaryDirectory() as td:
            mods_root = Path(td)

            # Mod 1: Low priority (priority = 50)
            mod1 = mods_root / "mod_low"
            (mod1 / "romfs" / "data").mkdir(parents=True)
            (mod1 / "mod.toml").write_text('id = "mod_low"\npriority = 50\n', encoding="utf-8")
            (mod1 / "romfs" / "data" / "balance.bin").write_bytes(b"LOW_PRIORITY_OVERRIDE")

            # Mod 2: High priority (priority = 100)
            mod2 = mods_root / "mod_high"
            (mod2 / "romfs" / "data").mkdir(parents=True)
            (mod2 / "mod.toml").write_text('id = "mod_high"\npriority = 100\n', encoding="utf-8")
            (mod2 / "romfs" / "data" / "balance.bin").write_bytes(b"HIGH_PRIORITY_WINNER")

            applied = fs.apply_layered_mods(mods_root)
            self.assertEqual(len(applied), 2)

            # High priority mod must win
            self.assertEqual(fs.files["data/balance.bin"], b"HIGH_PRIORITY_WINNER")
            # Unmodified file remains untouched
            self.assertEqual(fs.files["data/portrait.bin"], b"BASE_PORTRAIT_DATA")

    def test_fnt_fat_rebuild(self):
        fs = NitroFS()
        fs.files["data/test1.bin"] = b"HELLO_WORLD_TEST_1"
        fs.files["data/sub/test2.bin"] = b"HELLO_WORLD_TEST_2_LONGER"

        fnt, fat, payload = fs.build_fnt_and_fat(base_file_offset=0x1000)
        self.assertGreater(len(fnt), 0)
        self.assertGreater(len(fat), 0)
        self.assertGreater(len(payload), 0)
        self.assertEqual(len(fat), 2 * 8, "Expected 2 FAT entries")


if __name__ == "__main__":
    unittest.main()

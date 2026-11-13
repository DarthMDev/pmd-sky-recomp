#!/usr/bin/env python3
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.sky_mod_packer import NitroFS

ROM_PATH = REPO_ROOT / "roms" / "pmdsky.us.nds"


def synthetic_rom(files):
    fs = NitroFS()
    fs.files = files
    fnt, _fat, _payload = fs.build_fnt_and_fat()
    fnt_off = 0x200
    fat_off = fnt_off + len(fnt)
    payload_off = fat_off + len(files) * 8
    fnt, fat, payload = fs.build_fnt_and_fat(base_file_offset=payload_off)
    rom = bytearray(payload_off + len(payload))
    struct.pack_into("<IIII", rom, 0x40, fnt_off, len(fnt), fat_off, len(fat))
    rom[fnt_off : fnt_off + len(fnt)] = fnt
    rom[fat_off : fat_off + len(fat)] = fat
    rom[payload_off : payload_off + len(payload)] = payload
    return bytes(rom)


class TestNitroFSSynthetic(unittest.TestCase):
    def test_fnt_fat_round_trip_nested_and_empty_files(self):
        files = {
            "root.bin": b"root",
            "data/a.bin": b"short",
            "data/nested/b.bin": b"a longer payload",
            "empty.dat": b"",
        }
        self.assertEqual(NitroFS.from_rom(synthetic_rom(files)).files, files)

    def test_rebuild_preserves_file_count_and_variable_size_payloads(self):
        fs = NitroFS()
        fs.files = {"one": b"x", "nested/two": b"1234567"}
        self.assertEqual(NitroFS.from_rom(synthetic_rom(fs.files)).files, fs.files)

    def test_rejects_out_of_bounds_header_regions(self):
        rom = bytearray(0x200)
        struct.pack_into("<IIII", rom, 0x40, 0x1ff, 8, 0, 0)
        with self.assertRaisesRegex(ValueError, "FNT lies outside"):
            NitroFS.from_rom(bytes(rom))

    def test_rejects_invalid_fat_ranges(self):
        rom = bytearray(synthetic_rom({"file": b"abc"}))
        _fnt_off, _fnt_sz, fat_off, _fat_sz = struct.unpack_from("<IIII", rom, 0x40)
        struct.pack_into("<II", rom, fat_off, len(rom) + 1, len(rom) + 2)
        with self.assertRaisesRegex(ValueError, "Invalid FAT range"):
            NitroFS.from_rom(bytes(rom))

    def test_layered_mod_priority_and_tie_order_are_deterministic(self):
        fs = NitroFS()
        fs.files["data/file.bin"] = b"base"
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            for folder, mod_id, data in (("z_mod", "z_mod", b"z"), ("a_mod", "a_mod", b"a")):
                layer = mods / folder
                (layer / "romfs/data").mkdir(parents=True)
                (layer / "mod.toml").write_text(f'id = "{mod_id}"\npriority = 10\n')
                (layer / "romfs/data/file.bin").write_bytes(data)
            applied = fs.apply_layered_mods(mods)
        self.assertEqual([mod_id for mod_id, _ in applied], ["a_mod", "z_mod"])
        self.assertEqual(fs.files["data/file.bin"], b"z")

    def test_rejects_duplicate_ids_invalid_paths_and_symlinks(self):
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            for folder in ("first", "second"):
                (mods / folder / "romfs").mkdir(parents=True)
                (mods / folder / "mod.toml").write_text('id = "same"\n')
            with self.assertRaisesRegex(ValueError, "Duplicate mod ID"):
                NitroFS().apply_layered_mods(mods)

        with self.assertRaisesRegex(ValueError, "Invalid NitroFS path"):
            NitroFS._validate_virtual_path("../escape")
        with self.assertRaisesRegex(ValueError, "Invalid NitroFS path"):
            NitroFS._validate_virtual_path("C:/absolute")

        with tempfile.TemporaryDirectory() as td, tempfile.TemporaryDirectory() as outside:
            mods = Path(td)
            layer = mods / "mod" / "romfs"
            layer.mkdir(parents=True)
            (mods / "mod" / "mod.toml").write_text('id = "mod"\n')
            outside_file = Path(outside) / "escape.bin"
            outside_file.write_bytes(b"outside")
            (layer / "escape.bin").symlink_to(outside_file)
            with self.assertRaisesRegex(ValueError, "Symlinks are not allowed"):
                NitroFS().apply_layered_mods(mods)


class TestNitroFSWithLocalROM(unittest.TestCase):
    @unittest.skipUnless(ROM_PATH.is_file(), f"ROM not found at {ROM_PATH}")
    def test_parse_local_rom_nitrofs(self):
        fs = NitroFS.from_rom(ROM_PATH.read_bytes())
        self.assertGreater(len(fs.files), 100, "Expected at least 100 files in EoS ROM")
        self.assertTrue(any(f.endswith((".bin", ".smd")) for f in fs.files))


if __name__ == "__main__":
    unittest.main()

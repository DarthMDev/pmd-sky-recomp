#!/usr/bin/env python3
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.sky_mod_packer import NitroFS, atomic_write_bytes, _nds_crc16

ROM_PATH = REPO_ROOT / "roms" / "pmdsky.us.nds"


def synthetic_rom(files, overlays=None):
    fs = NitroFS()
    fs.files = dict(files)
    if overlays:
        for fid, blob in overlays.items():
            fs.unmapped_fat_entries[fid] = blob
    fnt, _fat, _payload = fs.build_fnt_and_fat()
    fnt_off = 0x200
    if overlays:
        # Place unmapped overlay data before FNT/FAT (matching retail NDS ROM layout)
        ov_area = bytearray()
        ov_ranges = {}
        cur = 0x200
        for fid in sorted(overlays.keys()):
            blob = overlays[fid]
            ov_ranges[fid] = (cur, cur + len(blob))
            ov_area.extend(blob)
            cur += len(blob)
            while len(ov_area) % 4 != 0:
                ov_area.append(0x00)
                cur += 1
        fnt_off = 0x200 + len(ov_area)
    else:
        ov_area = bytearray()
        ov_ranges = {}

    total_fat_entries = len(files) + (max(overlays.keys()) + 1 if overlays else 0)
    fat_off = fnt_off + len(fnt)
    payload_off = fat_off + total_fat_entries * 8

    # Build named FAT & payload starting at payload_off, then patch overlay FAT entries if present
    fs_no_ov = NitroFS()
    fs_no_ov.files = dict(files)
    if overlays:
        # Use unmapped prefix count for FNT first_fid, but place overlay payloads before FNT
        fnt, fat, payload = fs.build_fnt_and_fat(base_file_offset=payload_off, preserve_layout=False)
        # Recompute named payload offsets tightly from payload_off while keeping overlay ranges in [0x200, fnt_off)
        fat_ba = bytearray(fat)
        named_count = len(files)
        unmapped_count = max(overlays.keys()) + 1
        named_payload = bytearray()
        cur_p = payload_off
        for fid in range(unmapped_count):
            s, e = ov_ranges.get(fid, (0, 0))
            struct.pack_into("<II", fat_ba, fid * 8, s, e)
        for fid in range(unmapped_count, total_fat_entries):
            orig_s, orig_e = struct.unpack_from("<II", fat_ba, fid * 8)
            blob = payload[orig_s - payload_off : orig_e - payload_off]
            struct.pack_into("<II", fat_ba, fid * 8, cur_p, cur_p + len(blob))
            named_payload.extend(blob)
            cur_p += len(blob)
            while len(named_payload) % 4 != 0:
                named_payload.append(0x00)
                cur_p += 1
        fat = bytes(fat_ba)
        payload = bytes(named_payload)
    else:
        fnt, fat, payload = fs.build_fnt_and_fat(base_file_offset=payload_off)

    rom = bytearray(payload_off + len(payload))
    struct.pack_into("<IIII", rom, 0x40, fnt_off, len(fnt), fat_off, len(fat))
    if overlays:
        rom[0x200 : 0x200 + len(ov_area)] = ov_area
    rom[fnt_off : fnt_off + len(fnt)] = fnt
    rom[fat_off : fat_off + len(fat)] = fat
    rom[payload_off : payload_off + len(payload)] = payload
    struct.pack_into("<I", rom, 0x80, len(rom))
    struct.pack_into("<H", rom, 0x15E, _nds_crc16(bytes(rom[:0x15E])))
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

    def test_rebuild_rejects_file_directory_path_collision(self):
        fs = NitroFS()
        fs.files = {"data": b"file", "data/child.bin": b"child"}
        with self.assertRaisesRegex(ValueError, "both a file and directory"):
            fs.build_fnt_and_fat()

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

    def test_ignores_directories_without_romfs_before_metadata_validation(self):
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            unrelated = mods / "not-a-mod"
            unrelated.mkdir()
            (unrelated / "mod.toml").write_text('id = "invalid id"\n')
            self.assertEqual(NitroFS().apply_layered_mods(mods), [])

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

    def test_preserves_file_ids_and_unmapped_overlay_fat_entries(self):
        overlays = {0: b"OV00_CODE", 1: b"OV01_CODE_EXTRA"}
        files = {
            "root.bin": b"ROOT_ORIGINAL",
            "data/short.bin": b"SHORT",
            "data/long.bin": b"LONG_ORIGINAL_PAYLOAD",
        }
        base_rom = synthetic_rom(files, overlays=overlays)
        fs = NitroFS.from_rom(base_rom)
        orig_ids = dict(fs.file_ids)
        self.assertEqual(fs.unmapped_fat_entries, overlays)
        self.assertEqual(sorted(orig_ids.values()), [2, 3, 4])

        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            mod_dir = mods / "asset_mod"
            (mod_dir / "romfs" / "data").mkdir(parents=True)
            (mod_dir / "mod.toml").write_text('id = "asset_mod"\npriority = 50\n')
            # Shorter replacement and longer replacement
            (mod_dir / "romfs" / "data" / "long.bin").write_bytes(b"TINY")
            (mod_dir / "romfs" / "data" / "short.bin").write_bytes(b"MUCH_LONGER_REPLACEMENT_DATA")
            applied = fs.apply_layered_mods(mods, strict_existing_only=True)
            self.assertEqual(applied, [("asset_mod", 2)])

        patched_rom = fs.build_patched_rom(base_rom)
        reparsed = NitroFS.from_rom(patched_rom)
        self.assertEqual(reparsed.file_ids, orig_ids)
        self.assertEqual(reparsed.unmapped_fat_entries, overlays)
        self.assertEqual(reparsed.files["root.bin"], b"ROOT_ORIGINAL")
        self.assertEqual(reparsed.files["data/long.bin"], b"TINY")
        self.assertEqual(reparsed.files["data/short.bin"], b"MUCH_LONGER_REPLACEMENT_DATA")
        used_sz = struct.unpack_from("<I", patched_rom, 0x80)[0]
        self.assertEqual(used_sz, len(patched_rom))
        hdr_crc = struct.unpack_from("<H", patched_rom, 0x15E)[0]
        self.assertEqual(hdr_crc, _nds_crc16(patched_rom[:0x15E]))

    def test_strict_existing_only_rejects_added_files(self):
        fs = NitroFS.from_rom(synthetic_rom({"data/existing.bin": b"ok"}))
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            layer = mods / "adder" / "romfs" / "data"
            layer.mkdir(parents=True)
            (mods / "adder" / "mod.toml").write_text('id = "adder"\n')
            (layer / "new_file.bin").write_bytes(b"boom")
            with self.assertRaisesRegex(ValueError, "cannot add new NitroFS path"):
                fs.apply_layered_mods(mods, strict_existing_only=True)

    def test_disabled_mods_and_conflict_reporting(self):
        fs = NitroFS()
        fs.files["data/file.bin"] = b"base"
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            for mod_id, prio, enabled, payload in (
                ("a_mod", 10, "true", b"a"),
                ("z_mod", 20, "true", b"z"),
                ("off_mod", 99, "false", b"disabled"),
            ):
                layer = mods / mod_id
                (layer / "romfs" / "data").mkdir(parents=True)
                (layer / "mod.toml").write_text(
                    f'id = "{mod_id}"\npriority = {prio}\nenabled = {enabled}\n'
                )
                (layer / "romfs" / "data" / "file.bin").write_bytes(payload)
            applied = fs.apply_layered_mods(mods)
        self.assertEqual([m for m, _ in applied], ["a_mod", "z_mod"])
        self.assertEqual(fs.files["data/file.bin"], b"z")
        self.assertEqual(fs.last_conflicts, {"data/file.bin": ["a_mod", "z_mod"]})

    def test_rejects_unterminated_fnt_and_invalid_mod_metadata(self):
        rom = bytearray(synthetic_rom({"file.bin": b"abc"}))
        fnt_off, _fnt_sz, _fat_off, _fat_sz = struct.unpack_from("<IIII", rom, 0x40)
        sub_off = struct.unpack_from("<I", rom, fnt_off)[0]
        # Set FNT size to end immediately after the single file entry, omitting the 0x00 terminator
        struct.pack_into("<I", rom, 0x44, sub_off + 1 + len("file.bin"))
        with self.assertRaisesRegex(ValueError, "Unterminated FNT directory entry"):
            NitroFS.from_rom(bytes(rom))

        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            layer = mods / "bad_prio"
            (layer / "romfs").mkdir(parents=True)
            (layer / "mod.toml").write_text('id = "bad_prio"\npriority = "not_an_int"\n')
            with self.assertRaisesRegex(ValueError, "Invalid mod priority"):
                NitroFS().apply_layered_mods(mods)

    def test_atomic_write_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "nested" / "out.bin"
            atomic_write_bytes(target, b"first")
            self.assertEqual(target.read_bytes(), b"first")
            atomic_write_bytes(target, b"second_payload")
            self.assertEqual(target.read_bytes(), b"second_payload")
            self.assertEqual(list(target.parent.iterdir()), [target])

    def test_toml_trailing_comments_in_mod_metadata(self):
        fs = NitroFS()
        fs.files["data/a.bin"] = b"base"
        with tempfile.TemporaryDirectory() as td:
            mods = Path(td)
            mod_a = mods / "mod_a"
            (mod_a / "romfs" / "data").mkdir(parents=True)
            (mod_a / "mod.toml").write_text(
                'id = "mod_a" # identifier\npriority = 25 # higher wins\nenabled = true # active\n'
            )
            (mod_a / "romfs" / "data" / "a.bin").write_bytes(b"active")

            mod_b = mods / "mod_b"
            (mod_b / "romfs" / "data").mkdir(parents=True)
            (mod_b / "mod.toml").write_text(
                'id = "mod_b"\npriority = 99 # would win if enabled\nenabled = false # disabled\n'
            )
            (mod_b / "romfs" / "data" / "a.bin").write_bytes(b"inactive")

            applied = fs.apply_layered_mods(mods)
        self.assertEqual(applied, [("mod_a", 1)])
        self.assertEqual(fs.files["data/a.bin"], b"active")

    def test_build_patched_rom_preserves_arm7_after_first_named_payload(self):
        base = bytearray(synthetic_rom({"data/a.bin": b"OLD_DATA"}))
        # Append an ARM7 section after the named payload and point header 0x30 to it
        arm7_bytes = b"ARM7_EXEC_CODE_SECTION"
        arm7_off = len(base)
        base.extend(arm7_bytes)
        struct.pack_into("<IIII", base, 0x30, arm7_off, 0x02380000, 0x02380000, len(arm7_bytes))

        fs = NitroFS.from_rom(bytes(base))
        fs.files["data/a.bin"] = b"NEW_REPLACEMENT_DATA"
        patched = fs.build_patched_rom(bytes(base))
        self.assertEqual(patched[arm7_off : arm7_off + len(arm7_bytes)], arm7_bytes)
        self.assertEqual(NitroFS.from_rom(patched).files["data/a.bin"], b"NEW_REPLACEMENT_DATA")


class TestNitroFSWithLocalROM(unittest.TestCase):
    @unittest.skipUnless(ROM_PATH.is_file(), f"ROM not found at {ROM_PATH}")
    def test_parse_local_rom_nitrofs(self):
        fs = NitroFS.from_rom(ROM_PATH.read_bytes())
        self.assertGreater(len(fs.files), 100, "Expected at least 100 files in EoS ROM")
        self.assertTrue(any(f.endswith((".bin", ".smd")) for f in fs.files))


if __name__ == "__main__":
    unittest.main()

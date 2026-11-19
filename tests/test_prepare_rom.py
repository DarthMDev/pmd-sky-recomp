#!/usr/bin/env python3
"""Unit tests for translator/prepare_rom.py custom-ROM isolation, bounds validation, and overlay 36."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

import translator.prepare_rom as prepare_rom_mod


def _arm_bl(from_addr: int, to_addr: int) -> int:
    delta = (to_addr - (from_addr + 8)) >> 2
    return 0xEB000000 | (delta & 0x00FFFFFF)


def _blz_compress_nops_64() -> tuple[bytes, bytes]:
    """Return (compressed_blz, expected_64B_decompressed) for an active overlay."""
    prefix = struct.pack("<III", 0xE12FFF1E, 0xE1A00000, 0xE1A00000)  # 12 bytes
    nop = b"\x00\x00\xA0\xE1"
    expected = prefix + nop * 13  # 12 + 52 = 64 bytes

    # Backward stream ending at len(buf)-8:
    # - 4 compressed tokens (disp=4, len=12): each (lo=0x01, hi=0x90) -> 48 bytes
    # - 4 literal bytes (written in reverse at dst=63..60) -> 0xE1, 0xA0, 0x00, 0x00 in reverse = nop!
    # Wait: literals read backwards from src: dst=63 gets out[--src], so stream has [0x00, 0x00, 0xA0, 0xE1]
    stream = (b"\x01\x90" * 4) + nop + bytes([0x0F])
    comp_size = len(stream) + 8
    top_and_bottom = (8 << 24) | comp_size
    add_size = len(expected) - (len(prefix) + comp_size)
    compressed = prefix + stream + struct.pack("<II", top_and_bottom, add_size)
    return compressed, expected


def build_synthetic_nds_rom(
    *,
    num_overlays: int = 37,
    include_ov35_blz: bool = True,
    include_ov36: bool = True,
) -> tuple[bytes, bytes]:
    """Construct a minimal valid NDS image with ARM9, ITCM autoload, ARM7, and overlays 0..36."""
    arm9_ram = 0x02000000
    arm9_entry = 0x02000800
    arm9_size = 0x1000
    arm9 = bytearray(arm9_size)

    # Place ModuleParams at +0x100 and NITRO_MAGIC at +0x11C
    mp_off = 0x100
    al_list = arm9_ram + 0x200
    al_list_end = al_list + 12
    al_start = arm9_ram + 0x240
    bss_start = arm9_ram + 0xC00
    bss_end = arm9_ram + 0xD00
    struct.pack_into(
        "<IIIIIII",
        arm9,
        mp_off,
        al_list,
        al_list_end,
        al_start,
        bss_start,
        bss_end,
        0,
        0x04020000,
    )
    arm9[mp_off + 28 : mp_off + 36] = prepare_rom_mod.NITRO_MAGIC

    # 1 ITCM autoload entry (0x01FF8000, size 0x40)
    struct.pack_into("<III", arm9, 0x200, 0x01FF8000, 0x40, 0)
    struct.pack_into("<I", arm9, 0x240, 0xE12FFF1E)  # BX lr in ITCM

    # Entry code at 0x02000800 calling helper at 0x02000814
    struct.pack_into("<II", arm9, 0x800, _arm_bl(0x02000800, 0x02000814), 0xE12FFF1E)
    struct.pack_into("<II", arm9, 0x814, 0xE92D4000, 0xE12FFF1E)  # STMFD sp!, {lr}; BX lr

    # ARM7 binary
    arm7_ram = 0x02380000
    arm7_entry = 0x02380200
    arm7_size = 0x400
    arm7 = bytearray(arm7_size)
    a7_al_list = arm7_ram + 0x1E8
    a7_al_list_end = a7_al_list + 12
    a7_al_start = arm7_ram + 0x300
    a7_bss_s = arm7_ram + 0x380
    struct.pack_into("<IIII", arm7, 0x1D0, a7_al_list, a7_al_list_end, a7_al_start, a7_bss_s)
    struct.pack_into("<III", arm7, 0x1E8, 0x037F8000, 0x20, 0)
    struct.pack_into("<I", arm7, 0x200, 0xE12FFF1E)
    struct.pack_into("<I", arm7, 0x300, 0xE12FFF1E)

    # Overlay payloads & FAT entries
    ov_payloads: list[tuple[int, int, int, int, int, int, bytes]] = []
    ov35_expected = b""
    for oid in range(num_overlays):
        if oid == 35 and include_ov35_blz:
            comp, ov35_expected = _blz_compress_nops_64()
            ov_payloads.append((oid, 0x02300000, len(ov35_expected), 0, 0, 0x01000000, comp))
        elif oid == 36 and include_ov36:
            ov36 = bytearray(0x40)
            struct.pack_into("<I", ov36, 0x00, _arm_bl(0x023A0000, 0x023A0008))
            struct.pack_into("<I", ov36, 0x04, _arm_bl(0x023A0004, 0x02000820))  # cross-call into ARM9
            struct.pack_into("<I", ov36, 0x08, 0xE12FFF1E)
            struct.pack_into("<I", ov36, 0x10, 0xE12FFF1E)
            sinit_s = 0x023A0020
            sinit_e = 0x023A0024
            struct.pack_into("<I", ov36, 0x20, 0x023A0010)
            ov_payloads.append((oid, 0x023A0000, len(ov36), sinit_s, sinit_e, 0, bytes(ov36)))
        else:
            ov_payloads.append((oid, 0x022DC000, 0x10, 0, 0, 0, b"\x00" * 0x10))

    # Layout in ROM: header (0x200), arm9, ov9, arm7, fnt, fat, overlay payloads
    arm9_off = 0x200
    ov9_off = arm9_off + len(arm9)
    ov9_size = num_overlays * 32
    arm7_off = ov9_off + ov9_size
    fnt_off = arm7_off + len(arm7)
    fnt = b"\x08\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00"  # empty root dir FNT
    fat_off = fnt_off + len(fnt)
    fat_size = num_overlays * 8
    data_off = fat_off + fat_size

    fat = bytearray(fat_size)
    ov9 = bytearray(ov9_size)
    payload_blob = bytearray()
    cur = data_off
    for idx, (oid, ram_addr, ram_size, sinit_s, sinit_e, comp_flag, blob) in enumerate(ov_payloads):
        start = cur
        end = start + len(blob)
        struct.pack_into("<II", fat, idx * 8, start, end)
        struct.pack_into(
            "<IIIIIIII",
            ov9,
            idx * 32,
            oid,
            ram_addr,
            ram_size,
            0,
            sinit_s,
            sinit_e,
            idx,
            comp_flag,
        )
        payload_blob.extend(blob)
        cur = end
        while len(payload_blob) % 4 != 0:
            payload_blob.append(0)
            cur += 1

    rom = bytearray(data_off + len(payload_blob))
    struct.pack_into("<IIII", rom, 0x20, arm9_off, arm9_entry, arm9_ram, len(arm9))
    struct.pack_into("<IIII", rom, 0x30, arm7_off, arm7_entry, arm7_ram, len(arm7))
    struct.pack_into("<IIII", rom, 0x40, fnt_off, len(fnt), fat_off, fat_size)
    struct.pack_into("<IIII", rom, 0x50, ov9_off, ov9_size, 0, 0)
    rom[arm9_off : arm9_off + len(arm9)] = arm9
    rom[ov9_off : ov9_off + ov9_size] = ov9
    rom[arm7_off : arm7_off + len(arm7)] = arm7
    rom[fnt_off : fnt_off + len(fnt)] = fnt
    rom[fat_off : fat_off + fat_size] = fat
    rom[data_off : data_off + len(payload_blob)] = payload_blob
    return bytes(rom), ov35_expected


def write_minimal_symbols(symbols_dir: Path) -> None:
    symbols_dir.mkdir(parents=True, exist_ok=True)
    (symbols_dir / "arm9.yml").write_text(
        "arm9:\n  address:\n    NA: 0x02000000\n  functions: []\n  data: []\n",
        encoding="utf-8",
    )
    (symbols_dir / "arm7.yml").write_text(
        "arm7:\n  address:\n    NA: 0x02380000\n  functions: []\n  data: []\n",
        encoding="utf-8",
    )


class TestPrepareRom(unittest.TestCase):
    def test_blz_decompression_roundtrip_helper(self) -> None:
        comp, expected = _blz_compress_nops_64()
        self.assertEqual(prepare_rom_mod.blz_decompress_backward(comp), expected)

    def test_custom_rom_overlay36_and_blz_overlay_without_yaml(self) -> None:
        rom_bytes, ov35_expected = build_synthetic_nds_rom()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            rom_path = root / "custom_hack.nds"
            rom_path.write_bytes(rom_bytes)
            symbols_dir = root / "symbols"
            write_minimal_symbols(symbols_dir)
            out_dir = root / "extracted"

            sha1, region, banks = prepare_rom_mod.prepare_rom(
                rom_path,
                symbols_dir,
                out_dir,
                allow_custom_rom=True,
                forced_region="NA",
            )
            self.assertEqual(sha1, hashlib.sha1(rom_bytes).hexdigest())
            self.assertEqual(region, "NA")

            bank_map = {b.bank_name: b for b in banks}
            self.assertIn("sky_arm9_main_na", bank_map)
            self.assertIn("sky_arm7_main_na", bank_map)
            self.assertIn("sky_arm9_ov35_na", bank_map)
            self.assertIn("sky_arm9_ov36_na", bank_map)

            # Verify BLZ-compressed overlay 35 decompressed to expected bytes
            self.assertEqual(bank_map["sky_arm9_ov35_na"].bin_path.read_bytes(), ov35_expected)

            # Verify overlay 36 TOML seeded entry, sinit, and BL target without overlay36.yml
            ov36_toml = bank_map["sky_arm9_ov36_na"].toml_path.read_text(encoding="utf-8")
            self.assertIn("addr = 0x023A0000", ov36_toml)
            self.assertIn("addr = 0x023A0008", ov36_toml)
            self.assertIn("addr = 0x023A0010", ov36_toml)

            # Verify cross-overlay BL into ARM9 seeded 0x02000820
            arm9_toml = bank_map["sky_arm9_main_na"].toml_path.read_text(encoding="utf-8")
            self.assertIn("addr = 0x02000820", arm9_toml)

            manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertTrue(manifest["is_custom_rom"])
            self.assertEqual(manifest["bank_count"], len(banks))

    def test_custom_rom_cache_isolation(self) -> None:
        repo = Path("/tmp/fake-repo")
        clean_sha1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
        custom_sha1 = "deadbeef0123456789abcdef0123456789abcdef"

        c_out, c_gen = prepare_rom_mod.resolve_output_dirs(
            repo, clean_sha1, "NA", repo / "x", repo / "y", True, True
        )
        self.assertEqual(c_out, repo / "build" / "extracted_sky_us")
        self.assertEqual(c_gen, repo / "build" / "generated_sky_us")

        h_out, h_gen = prepare_rom_mod.resolve_output_dirs(
            repo, custom_sha1, "NA", repo / "x", repo / "y", True, True
        )
        self.assertEqual(h_out, repo / "build" / "extracted_sky_custom_us_deadbeef0123")
        self.assertEqual(h_gen, repo / "build" / "generated_sky_custom_us_deadbeef0123")
        self.assertNotEqual(h_out, c_out)
        self.assertNotEqual(h_gen, c_gen)

    def test_rejects_out_of_bounds_header_and_overlay_fat(self) -> None:
        rom_bytes, _ = build_synthetic_nds_rom(num_overlays=2, include_ov35_blz=False, include_ov36=False)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            symbols_dir = root / "symbols"
            write_minimal_symbols(symbols_dir)

            tiny = root / "tiny.nds"
            tiny.write_bytes(b"\x00" * 0x100)
            with self.assertRaisesRegex(ValueError, "too small"):
                prepare_rom_mod.prepare_rom(
                    tiny, symbols_dir, root / "out1", allow_custom_rom=True, forced_region="NA"
                )

            bad_sec = bytearray(rom_bytes)
            struct.pack_into("<II", bad_sec, 0x28, 0x02000000, len(bad_sec) + 0x100)
            bad_sec_path = root / "bad_sec.nds"
            bad_sec_path.write_bytes(bad_sec)
            with self.assertRaisesRegex(ValueError, "ARM9 lies outside"):
                prepare_rom_mod.prepare_rom(
                    bad_sec_path, symbols_dir, root / "out2", allow_custom_rom=True, forced_region="NA"
                )

            bad_ov = bytearray(rom_bytes)
            ov9_off = struct.unpack_from("<I", bad_ov, 0x50)[0]
            struct.pack_into("<I", bad_ov, ov9_off + 24, 999)  # out-of-bounds FAT index
            bad_ov_path = root / "bad_ov.nds"
            bad_ov_path.write_bytes(bad_ov)
            with self.assertRaisesRegex(ValueError, "out-of-bounds FAT index"):
                prepare_rom_mod.prepare_rom(
                    bad_ov_path, symbols_dir, root / "out3", allow_custom_rom=True, forced_region="NA"
                )

    def test_main_preserves_explicit_us_default_dirs_and_uses_single_rom_read(self) -> None:
        rom_bytes, _ = build_synthetic_nds_rom(num_overlays=2, include_ov35_blz=False, include_ov36=False)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            rom_path = root / "custom.nds"
            rom_path.write_bytes(rom_bytes)
            explicit_out = prepare_rom_mod.Path(prepare_rom_mod.__file__).resolve().parent.parent / "build" / "extracted_sky_us"

            with mock.patch.object(prepare_rom_mod, "prepare_rom", return_value=("sha", "NA", [])) as mock_prep, \
                 mock.patch.object(
                     sys,
                     "argv",
                     [
                         "prepare_rom.py",
                         "--rom",
                         str(rom_path),
                         "--allow-custom-rom",
                         "--region",
                         "NA",
                         "--out-dir",
                         str(explicit_out),
                     ],
                 ):
                rc = prepare_rom_mod.main()

            self.assertEqual(rc, 0)
            self.assertEqual(mock_prep.call_args.args[2], explicit_out)
            self.assertEqual(mock_prep.call_args.kwargs["rom_bytes"], rom_bytes)


if __name__ == "__main__":
    unittest.main()

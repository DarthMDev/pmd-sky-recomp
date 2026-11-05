#!/usr/bin/env python3
"""
SkyRecompiled — ROM Extractor, Decompressor & Static Recompiler Pipeline
========================================================================
Clean-room offline preparation tool for Pokémon Mystery Dungeon: Explorers of Sky.

Responsibilities:
  1. Verify input Nintendo DS ROM SHA-1 against known No-Intro hashes
     (Clean USA 'NA' first; Clean Europe 'EU' & custom SkyTemple hacks modular).
  2. Parse NDS header, NitroSDK _start_ModuleParams, ARM9/ARM7 autoload blocks,
     FAT, and ARM9 overlay table (y9.bin).
  3. Perform exact NitroSDK backward LZ77 (MIi_UncompressBackward / BLZ)
     decompression whenever arm9.bin or any overlay is compressed.
  4. Parse pmdsky-debug resymgen YAML symbol tables (zero external Python deps!),
     preferring runtime relocated addresses (ITCM, ARM7 WRAM, ARM7 RAM), and
     classify every function entry point accurately as ARM vs Thumb.
  5. Emit extracted binaries, per-bank nds_recompile TOML configs, manifest.json,
     and optionally invoke nds_recompile in parallel across all 34 active banks.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from typing import Dict, List, Optional, Set, Tuple

KNOWN_ROMS: Dict[str, Tuple[str, str]] = {
    "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152": (
        "NA",
        "Pokemon Mystery Dungeon: Explorers of Sky (USA)",
    ),
    "c838a5adf1ed32d2da8454976e5b1a1aa189c139": (
        "EU",
        "Pokemon Mystery Dungeon: Explorers of Sky (Europe)",
    ),
}

NITRO_MAGIC = struct.pack("<II", 0xDEC00621, 0x2106C0DE)


# ─────────────────────────────────────────────────────────────────────────────
# NitroSDK MIi_UncompressBackward (BLZ) Decompressor
# ─────────────────────────────────────────────────────────────────────────────
def blz_decompress_backward(buf: bytes) -> bytes:
    """Exact implementation of NitroSDK MIi_UncompressBackward."""
    if len(buf) < 8:
        raise ValueError("BLZ buffer shorter than 8-byte trailer")
    top_and_bottom, add_size = struct.unpack_from("<II", buf, len(buf) - 8)
    if top_and_bottom == 0 and add_size == 0:
        return buf
    hdr_size = (top_and_bottom >> 24) & 0xFF
    comp_size = top_and_bottom & 0x00FFFFFF
    if hdr_size < 8 or comp_size > len(buf):
        raise ValueError(
            f"Invalid BLZ trailer: hdr_size=0x{hdr_size:X} comp_size=0x{comp_size:X} len=0x{len(buf):X}"
        )
    orig_size = len(buf) + add_size
    out = bytearray(orig_size)
    out[: len(buf)] = buf

    src = len(buf) - hdr_size
    src_stop = len(buf) - comp_size
    dst = orig_size

    while src > src_stop:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= src_stop:
                break
            if (flags & 0x80) == 0:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            else:
                src -= 2
                hi = out[src + 1]
                lo = out[src]
                length = (hi >> 4) + 3
                disp = (((hi & 0x0F) << 8) | lo) + 3
                for _ in range(length):
                    b = out[dst + disp - 1]
                    dst -= 1
                    out[dst] = b
            flags = (flags << 1) & 0xFF

    if dst != src_stop:
        raise ValueError(f"BLZ decompression boundary mismatch: dst=0x{dst:X} != 0x{src_stop:X}")
    return bytes(out)


# ─────────────────────────────────────────────────────────────────────────────
# Zero-Dependency Parser for pmdsky-debug resymgen YAML Symbol Tables
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class PmdSymbol:
    name: str
    addresses: Dict[str, List[int]] = field(default_factory=dict)
    lengths: Dict[str, int] = field(default_factory=dict)


@dataclass
class PmdBlock:
    name: str
    addresses: Dict[str, int] = field(default_factory=dict)
    lengths: Dict[str, int] = field(default_factory=dict)
    subregions: List[str] = field(default_factory=list)
    functions: List[PmdSymbol] = field(default_factory=list)
    data: List[PmdSymbol] = field(default_factory=list)


def _parse_int(val: str) -> int:
    val = val.strip()
    if "#" in val:
        val = val.split("#", 1)[0].strip()
    return int(val, 0)


def parse_resymgen_yaml(path: Path) -> Dict[str, PmdBlock]:
    """Parse pmdsky-debug YAML files without requiring PyYAML."""
    blocks: Dict[str, PmdBlock] = {}
    cur_block: Optional[PmdBlock] = None
    sec_indent2: Optional[str] = None
    cur_sym: Optional[PmdSymbol] = None
    sym_field: Optional[str] = None
    addr_ver_list_key: Optional[str] = None
    skip_multiline_indent: Optional[int] = None

    with path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line_stripped = raw_line.rstrip("\r\n")
            if not line_stripped.strip() or line_stripped.lstrip().startswith("#"):
                continue
            indent = len(line_stripped) - len(line_stripped.lstrip(" "))
            if skip_multiline_indent is not None:
                if indent > skip_multiline_indent:
                    continue
                skip_multiline_indent = None

            content = line_stripped.strip()
            if content.endswith(("|", "|-", "|+", ">", ">-", ">+")):
                skip_multiline_indent = indent
                # Still check if it reset a state at indent 2 or 6
                if indent == 2:
                    sec_indent2 = None
                    cur_sym = None
                elif indent == 6:
                    sym_field = None
                    addr_ver_list_key = None
                continue

            if indent == 0 and content.endswith(":"):
                bname = content[:-1].strip()
                cur_block = PmdBlock(name=bname)
                blocks[bname] = cur_block
                sec_indent2 = None
                cur_sym = None
                sym_field = None
                addr_ver_list_key = None
            elif indent == 2 and cur_block is not None:
                cur_sym = None
                sym_field = None
                addr_ver_list_key = None
                if content.endswith("[]"):
                    sec_indent2 = None
                elif content.endswith(":"):
                    sec_indent2 = content[:-1].strip()
                else:
                    sec_indent2 = None
            elif indent == 4 and cur_block is not None:
                addr_ver_list_key = None
                if sec_indent2 == "address" and ":" in content:
                    k, v = content.split(":", 1)
                    if v.strip():
                        cur_block.addresses[k.strip()] = _parse_int(v)
                elif sec_indent2 == "length" and ":" in content:
                    k, v = content.split(":", 1)
                    if v.strip():
                        cur_block.lengths[k.strip()] = _parse_int(v)
                elif sec_indent2 == "subregions" and content.startswith("- "):
                    cur_block.subregions.append(content[2:].strip())
                elif sec_indent2 in ("functions", "data") and content.startswith("- name:"):
                    sname = content.split(":", 1)[1].strip().strip("\"'")
                    cur_sym = PmdSymbol(name=sname)
                    if sec_indent2 == "functions":
                        cur_block.functions.append(cur_sym)
                    else:
                        cur_block.data.append(cur_sym)
                    sym_field = None
            elif indent == 6 and cur_sym is not None:
                addr_ver_list_key = None
                if content.endswith(":"):
                    sym_field = content[:-1].strip()
                else:
                    sym_field = None
            elif indent == 8 and cur_sym is not None:
                if sym_field == "address":
                    if content.endswith(":"):
                        addr_ver_list_key = content[:-1].strip()
                        cur_sym.addresses.setdefault(addr_ver_list_key, [])
                    elif ":" in content:
                        addr_ver_list_key = None
                        k, v = content.split(":", 1)
                        if v.strip():
                            cur_sym.addresses[k.strip()] = [_parse_int(v)]
                elif sym_field == "length" and ":" in content:
                    addr_ver_list_key = None
                    k, v = content.split(":", 1)
                    if v.strip():
                        cur_sym.lengths[k.strip()] = _parse_int(v)
            elif indent == 10 and cur_sym is not None and sym_field == "address":
                if addr_ver_list_key and content.startswith("- "):
                    cur_sym.addresses[addr_ver_list_key].append(_parse_int(content[2:]))

    return blocks


def load_pmd_block_with_subregions(yml_path: Path) -> PmdBlock:
    top_blocks = parse_resymgen_yaml(yml_path)
    if not top_blocks:
        raise ValueError(f"No YAML blocks found in {yml_path}")
    main_block = next(iter(top_blocks.values()))
    subdir = yml_path.with_suffix("")
    for sub_name in main_block.subregions:
        sub_path = subdir / sub_name
        if sub_path.exists():
            sub_blocks = parse_resymgen_yaml(sub_path)
            for sb in sub_blocks.values():
                main_block.functions.extend(sb.functions)
                main_block.data.extend(sb.data)
    return main_block


# ─────────────────────────────────────────────────────────────────────────────
# ARM vs Thumb Function Mode Classification
# ─────────────────────────────────────────────────────────────────────────────
def collect_call_mode_hints(
    buf: bytes,
    base_addr: int,
    code_copies: List[Tuple[int, int, int]],
) -> Tuple[Set[int], Set[int]]:
    """
    Scan binary for unambiguous interworking call/pointer mode hints:
      - ARM BL   -> target is ARM
      - ARM BLX  -> target is Thumb
      - Thumb BL -> target is Thumb
      - Thumb BLX-> target is ARM
    Returns (arm_targets, thumb_targets).
    """
    arm_targets: Set[int] = set()
    thumb_targets: Set[int] = set()

    def runtime_base_for_offset(off: int) -> int:
        src_addr = base_addr + off
        for rt_start, src_start, sz in code_copies:
            if src_start <= src_addr < src_start + sz:
                return rt_start + (src_addr - src_start)
        return src_addr

    n = len(buf)
    for off in range(0, n - 3, 4):
        w = struct.unpack_from("<I", buf, off)[0]
        pc_base = runtime_base_for_offset(off)
        # ARM unconditional/conditional BL (cond 0xE = AL, 0x0 = EQ, 0x1 = NE)
        top = w >> 28
        if top in (0xE, 0x0, 0x1, 0x2, 0x3, 0xA, 0xB, 0xC, 0xD) and (w & 0x0F000000) == 0x0B000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            target = (pc_base + 8 + (imm24 << 2)) & 0xFFFFFFFF
            arm_targets.add(target)
        elif (w & 0xFE000000) == 0xFA000000:
            # ARM BLX immediate -> Thumb
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            h_bit = (w >> 23) & 0x2
            target = (pc_base + 8 + (imm24 << 2) + h_bit) & 0xFFFFFFFF
            thumb_targets.add(target & ~1)

    for off in range(0, n - 3, 2):
        hw0, hw1 = struct.unpack_from("<HH", buf, off)
        if (hw0 & 0xF800) == 0xF000:
            pc_base = runtime_base_for_offset(off)
            hi11 = hw0 & 0x07FF
            if hi11 & 0x0400:
                hi11 -= 0x0800
            lo11 = hw1 & 0x07FF
            if (hw1 & 0xF800) == 0xF800:
                # Thumb BL -> Thumb
                target = (pc_base + 4 + (hi11 << 12) + (lo11 << 1)) & 0xFFFFFFFF
                thumb_targets.add(target & ~1)
            elif (hw1 & 0xF800) == 0xE800 and (hw1 & 1) == 0:
                # Thumb BLX -> ARM (aligned PC)
                aligned_pc = (pc_base + 4) & ~3
                target = (aligned_pc + (hi11 << 12) + (lo11 << 1)) & 0xFFFFFFFF
                arm_targets.add(target & ~3)

    return arm_targets, thumb_targets


def classify_func_mode(
    raw_addr: int,
    buf: bytes,
    base_addr: int,
    code_copies: List[Tuple[int, int, int]],
    arm_hints: Set[int],
    thumb_hints: Set[int],
) -> Tuple[int, str]:
    """Return (clean_aligned_addr, 'arm' | 'thumb')."""
    if raw_addr & 1:
        return (raw_addr & ~1), "thumb"
    if raw_addr & 2:
        return raw_addr, "thumb"

    # Map runtime addr -> buffer offset
    src_addr = raw_addr
    for rt_start, src_start, sz in code_copies:
        if rt_start <= raw_addr < rt_start + sz:
            src_addr = src_start + (raw_addr - rt_start)
            break
    off = src_addr - base_addr
    if off < 0 or off + 4 > len(buf):
        return raw_addr, "arm"

    w0 = struct.unpack_from("<I", buf, off)[0]
    hw0, hw1 = w0 & 0xFFFF, (w0 >> 16) & 0xFFFF

    # Check unambiguous Thumb prologues FIRST (even before noisy cross-scan hints):
    #   - PUSH {..., lr} (0xB5xx) or PUSH {...} (0xB4xx)
    #   - SWI #imm; BX lr (0xDFxx followed by 0x4770 -> 0x4770DFxx)
    #   - BX lr (0x4770) or BX pc (0x4778) Thumb veneer
    #   - SUB sp, #imm (0xB080..0xB0FF)
    if (hw0 & 0xFE00) == 0xB400:
        return raw_addr, "thumb"
    if (hw0 & 0xFF00) == 0xDF00 and hw1 == 0x4770:
        return raw_addr, "thumb"
    if hw0 in (0x4770, 0x4778):
        return raw_addr, "thumb"
    if (hw0 & 0xFF80) == 0xB080 and (w0 >> 28) != 0xE:
        return raw_addr, "thumb"

    # Unambiguous ARM first instruction (condition AL = 0xE):
    if (w0 >> 28) == 0xE:
        return raw_addr, "arm"

    # Consult caller BL/BLX hints when top nibble != 0xE:
    in_arm = raw_addr in arm_hints
    in_thumb = raw_addr in thumb_hints
    if in_thumb and not in_arm:
        return raw_addr, "thumb"
    if in_arm and not in_thumb:
        return raw_addr, "arm"

    # Fallback inspection of second word: if w0 has cond != 0xE and looks like
    # two valid 16-bit Thumb instructions (e.g., MOV/CMP/LDR/ADD/BX), choose thumb.
    if (hw1 == 0x4770) or ((hw0 & 0xF800) == 0x4800) or ((hw0 & 0xE000) == 0x2000):
        return raw_addr, "thumb"

    return raw_addr, "arm"


_IDENT_RE = re.compile(r"[^A-Za-z0-9_]")


def sanitize_c_ident(name: str, addr: int) -> str:
    cleaned = _IDENT_RE.sub("_", name).strip("_")
    if not cleaned or cleaned[0].isdigit():
        cleaned = f"sym_{cleaned}" if cleaned else f"fn_{addr:08X}"
    if cleaned.startswith(("afunc_", "tfunc_")):
        cleaned = f"sym_{cleaned}"
    return cleaned


def pick_symbol_addrs(sym: PmdSymbol, region: str, prefer_tags: List[str]) -> List[int]:
    for tag in prefer_tags:
        key = f"{region}-{tag}"
        if key in sym.addresses and sym.addresses[key]:
            return sym.addresses[key]
    return sym.addresses.get(region, [])


def pick_symbol_len(sym: PmdSymbol, region: str, prefer_tags: List[str]) -> Optional[int]:
    for tag in prefer_tags:
        key = f"{region}-{tag}"
        if key in sym.lengths:
            return sym.lengths[key]
    return sym.lengths.get(region)


def choose_shard_count(size_bytes: int) -> int:
    """Note: runner/CMakeLists.txt requires shards >= 2 (*_[0-9][0-9].c)."""
    if size_bytes >= 0x60000:
        return 12
    if size_bytes >= 0x40000:
        return 8
    if size_bytes >= 0x18000:
        return 4
    return 2


# ─────────────────────────────────────────────────────────────────────────────
# Bank Builder & TOML Emitter
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class BankSpec:
    bank_name: str
    cpu: str
    bin_path: Path
    toml_path: Path
    load_address: int
    size: int
    entry_pc: int
    sha1: str
    shards: int
    entry_count: int
    data_range_count: int
    code_copy_count: int


def write_bank_toml(
    toml_path: Path,
    prog_name: str,
    prog_id: str,
    load_address: int,
    size: int,
    entry_pc: int,
    sha1_hex: str,
    md5_hex: str,
    entries: List[Tuple[int, str, str]],  # (addr, mode, unique_c_name)
    data_ranges: List[Tuple[int, int, str]],  # (start, end, note)
    code_copies: List[Tuple[int, int, int, str]],  # (rt_start, src_start, sz, name)
) -> None:
    lines: List[str] = [
        f"# AUTO-GENERATED by SkyRecompiled tools/prepare_rom.py — DO NOT EDIT.",
        "[program]",
        f'name = "{prog_name}"',
        f'id = "{prog_id}"',
        f"load_address = 0x{load_address:08X}",
        f"size = 0x{size:X}",
        f"entry_pc = 0x{entry_pc:08X}",
        "",
        "[identity]",
        f'sha1 = "{sha1_hex}"',
        f'md5 = "{md5_hex}"',
        "",
    ]

    for rt_start, src_start, sz, cc_name in code_copies:
        lines.extend([
            "[[code_copy]]",
            f'name = "{cc_name}"',
            f"runtime_start = 0x{rt_start:08X}",
            f"source_start = 0x{src_start:08X}",
            f"size = 0x{sz:X}",
            "",
        ])

    for start, end, note in data_ranges:
        safe_note = note.replace("\\", "\\\\").replace('"', '\\"')
        lines.extend([
            "[[data_range]]",
            f"start = 0x{start:08X}",
            f"end = 0x{end:08X}",
            f'note = "{safe_note}"',
            "",
        ])

    for addr, mode, cname in entries:
        lines.extend([
            "[[entry_point]]",
            f"addr = 0x{addr:08X}",
            f'mode = "{mode}"',
            f'name = "{cname}"',
            "",
        ])

    toml_path.write_text("\n".join(lines), encoding="utf-8")


def deduplicate_and_filter(
    raw_entries: List[Tuple[int, str, str]],
    raw_data_ranges: List[Tuple[int, int, str]],
    valid_ranges: List[Tuple[int, int]],
) -> Tuple[List[Tuple[int, str, str]], List[Tuple[int, int, str]]]:
    """Ensure all entry points lie within valid runtime ranges, have unique C names,
    and never collide with [[data_range]] intervals."""

    def in_valid(a: int) -> bool:
        return any(lo <= a < hi for lo, hi in valid_ranges)

    by_addr: Dict[int, Tuple[str, str]] = {}
    for addr, mode, raw_name in raw_entries:
        align = 2 if mode == "thumb" else 4
        if addr % align != 0 or not in_valid(addr):
            continue
        if addr not in by_addr:
            by_addr[addr] = (mode, sanitize_c_ident(raw_name, addr))

    sorted_addrs = sorted(by_addr.keys())
    name_counts: Dict[str, int] = {}
    for addr in sorted_addrs:
        _, nm = by_addr[addr]
        name_counts[nm] = name_counts.get(nm, 0) + 1

    used_names: Set[str] = set()
    final_entries: List[Tuple[int, str, str]] = []
    entry_addr_set = set(sorted_addrs)

    for addr in sorted_addrs:
        mode, nm = by_addr[addr]
        if name_counts[nm] > 1 or nm in used_names:
            nm = f"{nm}_{addr:08X}"
        while nm in used_names:
            nm = f"{nm}_dup"
        used_names.add(nm)
        final_entries.append((addr, mode, nm))

    # Filter & merge non-colliding data_ranges
    clean_dr: List[Tuple[int, int, str]] = []
    for start, end, note in sorted(raw_data_ranges, key=lambda x: (x[0], x[1])):
        if start >= end:
            continue
        # Never emit a data_range that contains a declared entry_point
        if any(start <= ea < end for ea in entry_addr_set):
            continue
        clean_dr.append((start, end, note))

    return final_entries, clean_dr


def scan_arm_code_seeds(
    data: bytes,
    base_addr: int,
    limit: int,
    code_copies: List[Tuple[int, int, int]],
    prefix: str,
) -> List[Tuple[int, str, str]]:
    """Conservative ARM function discovery (BL targets + STMFD {..,lr} prologues)."""
    seeds: List[Tuple[int, str, str]] = []

    def to_runtime(src_addr: int) -> int:
        for rt_s, src_s, sz in code_copies:
            if src_s <= src_addr < src_s + sz:
                return rt_s + (src_addr - src_s)
        return src_addr

    end_off = min(len(data), max(0, limit))
    for off in range(0, end_off - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        rt_pc = to_runtime(base_addr + off)
        # Unconditional ARM BL target
        if (w & 0xFF000000) == 0xEB000000:
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            target = (rt_pc + 8 + (imm24 << 2)) & 0xFFFFFFFF
            seeds.append((target, "arm", f"{prefix}_bl_{target:08X}"))
        # Standard MWCC ARM prologue STMFD sp!, {..., lr} + unconditional ARM op
        if (w & 0xFFFF4000) == 0xE92D4000 and off + 8 <= end_off:
            w1 = struct.unpack_from("<I", data, off + 4)[0]
            if (w1 >> 28) == 0xE:
                seeds.append((rt_pc, "arm", f"{prefix}_fn_{rt_pc:08X}"))
    return seeds


# ─────────────────────────────────────────────────────────────────────────────
# Full ROM Extraction & Bank Preparation Pipeline
# ─────────────────────────────────────────────────────────────────────────────
def prepare_rom(
    rom_path: Path,
    symbols_dir: Path,
    out_dir: Path,
    allow_custom_rom: bool = False,
    forced_region: Optional[str] = None,
) -> Tuple[str, str, List[BankSpec]]:
    rom = rom_path.read_bytes()
    rom_sha1 = hashlib.sha1(rom).hexdigest()

    if rom_sha1 in KNOWN_ROMS:
        region, rom_title_desc = KNOWN_ROMS[rom_sha1]
    elif allow_custom_rom and forced_region in ("NA", "EU"):
        region = forced_region
        rom_title_desc = f"Custom/Modified Explorers of Sky ROM ({region})"
    else:
        raise SystemExit(
            f"[prepare_rom] ERROR: Unrecognized ROM SHA-1: {rom_sha1}\n"
            f"Expected Clean USA SHA-1:    5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152\n"
            f"Expected Clean Europe SHA-1: c838a5adf1ed32d2da8454976e5b1a1aa189c139\n"
            f"(Pass --allow-custom-rom --region NA|EU for SkyTemple ROM hacks.)"
        )

    if forced_region and forced_region != region:
        raise SystemExit(f"[prepare_rom] Region mismatch: ROM is {region}, requested {forced_region}")

    arm9_off, arm9_entry, arm9_ram, arm9_size = struct.unpack_from("<IIII", rom, 0x20)
    arm7_off, arm7_entry, arm7_ram, arm7_size = struct.unpack_from("<IIII", rom, 0x30)
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from("<IIII", rom, 0x40)
    ov9_off, ov9_size, ov7_off, ov7_size = struct.unpack_from("<IIII", rom, 0x50)

    print(f"[prepare_rom] Verified {rom_title_desc}")
    print(f"[prepare_rom]   SHA-1: {rom_sha1} (Region: {region})")

    bins_dir = out_dir / "bins"
    cfgs_dir = out_dir / "configs"
    bins_dir.mkdir(parents=True, exist_ok=True)
    cfgs_dir.mkdir(parents=True, exist_ok=True)

    banks: List[BankSpec] = []

    # ── 1. ARM9 Main + ITCM Autoload ─────────────────────────────────────────
    arm9_raw = bytearray(rom[arm9_off : arm9_off + arm9_size])
    magic_idx = arm9_raw.find(NITRO_MAGIC)
    if magic_idx < 28:
        raise RuntimeError("Could not locate NitroSDK _start_ModuleParams magic in arm9.bin")
    mp_off = magic_idx - 28
    (
        al_list,
        al_list_end,
        al_start,
        bss_start,
        bss_end,
        comp_static_end,
        sdk_ver,
    ) = struct.unpack_from("<IIIIIII", arm9_raw, mp_off)

    if comp_static_end != 0:
        comp_len = comp_static_end - arm9_ram
        print(f"[prepare_rom] Decompressing BLZ arm9.bin (0x{comp_len:X} B)...")
        arm9_img = bytearray(blz_decompress_backward(bytes(arm9_raw[:comp_len])))
        # NitroSDK crt0 clears compressed_static_end to 0 after decompression
        struct.pack_into("<I", arm9_img, mp_off + 20, 0)
    else:
        arm9_img = arm9_raw

    arm9_bytes = bytes(arm9_img)
    arm9_code_copies: List[Tuple[int, int, int, str]] = []
    arm9_cc_tuples: List[Tuple[int, int, int]] = []
    cur_al_src = al_start
    num_al = (al_list_end - al_list) // 12
    for idx in range(num_al):
        adst, asize, _abss = struct.unpack_from("<III", arm9_bytes, (al_list - arm9_ram) + idx * 12)
        if adst == 0x01FF8000 and asize > 0:
            arm9_code_copies.append((adst, cur_al_src, asize, "itcm"))
            arm9_cc_tuples.append((adst, cur_al_src, asize))
        cur_al_src += asize

    arm9_block = load_pmd_block_with_subregions(symbols_dir / "arm9.yml")
    arm9_hints, arm9_th_hints = collect_call_mode_hints(arm9_bytes, arm9_ram, arm9_cc_tuples)

    arm9_raw_entries: List[Tuple[int, str, str]] = [(arm9_entry, "arm", "_start")]
    for sym in arm9_block.functions:
        for raw_a in pick_symbol_addrs(sym, region, ["ITCM"]):
            ca, mode = classify_func_mode(
                raw_a, arm9_bytes, arm9_ram, arm9_cc_tuples, arm9_hints, arm9_th_hints
            )
            arm9_raw_entries.append((ca, mode, sym.name))

    # Pre-scan all active overlays so cross-overlay BL calls into ARM9/ITCM
    # (e.g. unnamed helpers in arm9 called by ov34/ov11/ov29) are seeded too!
    decoded_overlays: List[Tuple[int, int, int, int, int, bytes]] = []
    cross_arm9_seeds: List[Tuple[int, str, str]] = []
    num_overlays = ov9_size // 32
    for idx in range(num_overlays):
        (
            oid,
            ram_addr,
            ram_size,
            _bss_size,
            sinit_s,
            sinit_e,
            fid,
            comp_flag,
        ) = struct.unpack_from("<IIIIIIII", rom, ov9_off + idx * 32)
        f_start, f_end = struct.unpack_from("<II", rom, fat_off + fid * 8)
        raw_ov = rom[f_start:f_end]
        ov_bytes = (
            blz_decompress_backward(raw_ov)
            if (comp_flag & 0x01000000) != 0
            else raw_ov
        )
        if len(ov_bytes) != ram_size:
            raise ValueError(
                f"Overlay {oid:02d} size mismatch: got 0x{len(ov_bytes):X}, header expects 0x{ram_size:X}"
            )
        if ram_size <= 0x20 and all(b == 0 for b in ov_bytes):
            continue
        decoded_overlays.append((oid, ram_addr, ram_size, sinit_s, sinit_e, ov_bytes))
        text_lim = (sinit_s - ram_addr) if (ram_addr < sinit_s <= ram_addr + ram_size) else ram_size
        for t, m, nm in scan_arm_code_seeds(ov_bytes, ram_addr, text_lim, [], f"ov{oid:02d}"):
            if (arm9_ram <= t < bss_start) or (0x01FF8000 <= t < 0x01FFC000):
                cross_arm9_seeds.append((t, m, nm))

    arm9_raw_dr: List[Tuple[int, int, str]] = [
        (arm9_ram + mp_off, arm9_ram + mp_off + 36, "_start_ModuleParams"),
        (bss_start, arm9_ram + len(arm9_bytes), "autoload_source_overwritten_by_bss"),
    ]
    for dsym in arm9_block.data:
        dlen = pick_symbol_len(dsym, region, ["ITCM"])
        if dlen and dlen > 0:
            for da in pick_symbol_addrs(dsym, region, ["ITCM"]):
                if (arm9_ram <= da and da + dlen <= bss_start) or any(
                    rt <= da and da + dlen <= rt + sz for rt, _src, sz in arm9_cc_tuples
                ):
                    arm9_raw_dr.append((da, da + dlen, dsym.name))

    def not_in_drs(addr: int, drs: List[Tuple[int, int, str]]) -> bool:
        return not any(s <= addr < e for s, e, _ in drs)

    # Dynamic .text upper bound (avoids scanning .rodata/.data on both NA and EU)
    arm9_main_fn_addrs = [a for a, _, _ in arm9_raw_entries if arm9_ram <= a < bss_start]
    arm9_text_limit = (
        min(bss_start - arm9_ram, (max(arm9_main_fn_addrs) - arm9_ram) + 0x1000)
        if arm9_main_fn_addrs
        else (bss_start - arm9_ram)
    )

    # Heuristic ARM9 + ITCM function seeds (never overriding explicit data ranges)
    itcm_seeds: List[Tuple[int, str, str]] = []
    for rt_s, src_s, cc_sz in arm9_cc_tuples:
        src_off = src_s - arm9_ram
        itcm_seeds.extend(
            scan_arm_code_seeds(
                arm9_bytes[src_off : src_off + cc_sz], rt_s, cc_sz, [], "itcm"
            )
        )

    for t, m, nm in (
        scan_arm_code_seeds(arm9_bytes, arm9_ram, arm9_text_limit, arm9_cc_tuples, "arm9")
        + itcm_seeds
        + cross_arm9_seeds
    ):
        if (
            (arm9_ram <= t < arm9_ram + arm9_text_limit)
            or any(rt <= t < rt + sz for rt, _src, sz in arm9_cc_tuples)
        ) and not_in_drs(t, arm9_raw_dr):
            arm9_raw_entries.append((t, m, nm))

    arm9_valid_ranges = [(arm9_ram, bss_start)] + [
        (rt, rt + sz) for rt, _src, sz in arm9_cc_tuples
    ]
    arm9_entries, arm9_drs = deduplicate_and_filter(
        arm9_raw_entries, arm9_raw_dr, arm9_valid_ranges
    )

    arm9_bin_path = bins_dir / "sky_arm9_main.bin"
    arm9_toml_path = cfgs_dir / "sky_arm9_main.toml"
    arm9_bin_path.write_bytes(arm9_bytes)
    arm9_sha1 = hashlib.sha1(arm9_bytes).hexdigest()
    arm9_md5 = hashlib.md5(arm9_bytes).hexdigest()
    write_bank_toml(
        arm9_toml_path,
        f"PMD Explorers of Sky ({region}) — ARM9 Main + ITCM",
        f"C2S_{region}_ARM9",
        arm9_ram,
        len(arm9_bytes),
        arm9_entry,
        arm9_sha1,
        arm9_md5,
        arm9_entries,
        arm9_drs,
        arm9_code_copies,
    )
    banks.append(
        BankSpec(
            bank_name="sky_arm9_main",
            cpu="arm9",
            bin_path=arm9_bin_path,
            toml_path=arm9_toml_path,
            load_address=arm9_ram,
            size=len(arm9_bytes),
            entry_pc=arm9_entry,
            sha1=arm9_sha1,
            shards=16,
            entry_count=len(arm9_entries),
            data_range_count=len(arm9_drs),
            code_copy_count=len(arm9_code_copies),
        )
    )

    # ── 2. ARM7 Main + WRAM (0x037F8000) + RAM (0x027E0000) ──────────────────
    arm7_bytes = rom[arm7_off : arm7_off + arm7_size]
    # Locate ARM7 autoload descriptors at end of arm7.bin (0x23801D0 params)
    a7_al_list, a7_al_list_end, a7_al_start, a7_bss_s = struct.unpack_from(
        "<IIII", arm7_bytes, 0x1D0
    )
    arm7_code_copies: List[Tuple[int, int, int, str]] = []
    arm7_cc_tuples: List[Tuple[int, int, int]] = []
    cur_a7_src = a7_al_start
    for idx in range((a7_al_list_end - a7_al_list) // 12):
        adst, asize, _abss = struct.unpack_from(
            "<III", arm7_bytes, (a7_al_list - arm7_ram) + idx * 12
        )
        label = "arm7_wram" if adst >= 0x03000000 else f"arm7_ram_{idx}"
        arm7_code_copies.append((adst, cur_a7_src, asize, label))
        arm7_cc_tuples.append((adst, cur_a7_src, asize))
        cur_a7_src += asize

    arm7_block = load_pmd_block_with_subregions(symbols_dir / "arm7.yml")
    a7_hints, a7_th_hints = collect_call_mode_hints(arm7_bytes, arm7_ram, arm7_cc_tuples)

    arm7_raw_entries: List[Tuple[int, str, str]] = [(arm7_entry, "arm", "_start_arm7")]
    for sym in arm7_block.functions:
        for raw_a in pick_symbol_addrs(sym, region, ["WRAM", "RAM"]):
            ca, mode = classify_func_mode(
                raw_a, arm7_bytes, arm7_ram, arm7_cc_tuples, a7_hints, a7_th_hints
            )
            arm7_raw_entries.append((ca, mode, sym.name))

    arm7_raw_dr: List[Tuple[int, int, str]] = [
        (arm7_ram + 0x1D0, arm7_ram + 0x1E8, "arm7_module_params"),
        (a7_al_start, arm7_ram + len(arm7_bytes), "arm7_autoload_sources"),
    ]
    for dsym in arm7_block.data:
        dlen = pick_symbol_len(dsym, region, ["WRAM", "RAM"])
        if dlen and dlen > 0:
            for da in pick_symbol_addrs(dsym, region, ["WRAM", "RAM"]):
                arm7_raw_dr.append((da, da + dlen, dsym.name))

    for t, m, nm in scan_arm_code_seeds(
        arm7_bytes, arm7_ram, len(arm7_bytes), arm7_cc_tuples, "arm7"
    ):
        if not_in_drs(t, arm7_raw_dr):
            arm7_raw_entries.append((t, m, nm))

    arm7_valid_ranges = [(arm7_ram, a7_al_start)] + [
        (rt, rt + sz) for rt, _src, sz in arm7_cc_tuples
    ]
    arm7_entries, arm7_drs = deduplicate_and_filter(
        arm7_raw_entries, arm7_raw_dr, arm7_valid_ranges
    )

    arm7_bin_path = bins_dir / "sky_arm7_main.bin"
    arm7_toml_path = cfgs_dir / "sky_arm7_main.toml"
    arm7_bin_path.write_bytes(arm7_bytes)
    arm7_sha1 = hashlib.sha1(arm7_bytes).hexdigest()
    arm7_md5 = hashlib.md5(arm7_bytes).hexdigest()
    write_bank_toml(
        arm7_toml_path,
        f"PMD Explorers of Sky ({region}) — ARM7 Driver",
        f"C2S_{region}_ARM7",
        arm7_ram,
        len(arm7_bytes),
        arm7_entry,
        arm7_sha1,
        arm7_md5,
        arm7_entries,
        arm7_drs,
        arm7_code_copies,
    )
    banks.append(
        BankSpec(
            bank_name="sky_arm7_main",
            cpu="arm7",
            bin_path=arm7_bin_path,
            toml_path=arm7_toml_path,
            load_address=arm7_ram,
            size=len(arm7_bytes),
            entry_pc=arm7_entry,
            sha1=arm7_sha1,
            shards=4,
            entry_count=len(arm7_entries),
            data_range_count=len(arm7_drs),
            code_copy_count=len(arm7_code_copies),
        )
    )

    # ── 3. ARM9 Overlays (ov00 .. ov35) ──────────────────────────────────────
    for oid, ram_addr, ram_size, sinit_s, sinit_e, ov_bytes in decoded_overlays:
        ov_yml = symbols_dir / f"overlay{oid:02d}.yml"
        ov_block = load_pmd_block_with_subregions(ov_yml) if ov_yml.exists() else PmdBlock(f"overlay{oid}")

        ov_arm_hints, ov_th_hints = collect_call_mode_hints(ov_bytes, ram_addr, [])
        # Merge cross-calls from arm9 into non-overlapping/exact hints
        ov_arm_hints |= arm9_hints
        ov_th_hints |= arm9_th_hints

        ov_raw_entries: List[Tuple[int, str, str]] = []
        # Offset 0 of every active NitroSDK overlay in EoS is an entry function
        c0, m0 = classify_func_mode(
            ram_addr, ov_bytes, ram_addr, [], ov_arm_hints, ov_th_hints
        )
        ov_raw_entries.append((c0, m0, f"ov{oid:02d}_entry"))

        # Static initializers [sinit_s, sinit_e)
        ov_raw_dr: List[Tuple[int, int, str]] = []
        if ram_addr <= sinit_s < sinit_e <= ram_addr + ram_size:
            ov_raw_dr.append((sinit_s, sinit_e, f"ov{oid:02d}_sinit_table"))
            for ptr_addr in range(sinit_s, sinit_e, 4):
                fn_ptr = struct.unpack_from("<I", ov_bytes, ptr_addr - ram_addr)[0]
                if fn_ptr != 0 and ram_addr <= (fn_ptr & ~1) < ram_addr + ram_size:
                    ca, mode = classify_func_mode(
                        fn_ptr, ov_bytes, ram_addr, [], ov_arm_hints, ov_th_hints
                    )
                    ov_raw_entries.append((ca, mode, f"ov{oid:02d}_sinit_{ca:08X}"))

        # pmdsky-debug named functions
        for sym in ov_block.functions:
            for raw_a in pick_symbol_addrs(sym, region, []):
                ca, mode = classify_func_mode(
                    raw_a, ov_bytes, ram_addr, [], ov_arm_hints, ov_th_hints
                )
                ov_raw_entries.append((ca, mode, sym.name))

        # pmdsky-debug data ranges
        for dsym in ov_block.data:
            dlen = pick_symbol_len(dsym, region, [])
            if dlen and dlen > 0:
                for da in pick_symbol_addrs(dsym, region, []):
                    if ram_addr <= da and da + dlen <= ram_addr + ram_size:
                        ov_raw_dr.append((da, da + dlen, dsym.name))

        # Heuristic prologue + internal BL discovery across all overlays
        text_lim = (sinit_s - ram_addr) if (ram_addr < sinit_s <= ram_addr + ram_size) else ram_size
        for t, m, nm in scan_arm_code_seeds(ov_bytes, ram_addr, text_lim, [], f"ov{oid:02d}"):
            if ram_addr <= t < ram_addr + text_lim and not_in_drs(t, ov_raw_dr):
                ov_raw_entries.append((t, m, nm))

        ov_entries, ov_drs = deduplicate_and_filter(
            ov_raw_entries, ov_raw_dr, [(ram_addr, ram_addr + ram_size)]
        )
        first_entry_pc = ov_entries[0][0]

        bank_name = f"sky_arm9_ov{oid:02d}"
        ov_bin_path = bins_dir / f"{bank_name}.bin"
        ov_toml_path = cfgs_dir / f"{bank_name}.toml"
        ov_bin_path.write_bytes(ov_bytes)
        ov_sha1 = hashlib.sha1(ov_bytes).hexdigest()
        ov_md5 = hashlib.md5(ov_bytes).hexdigest()
        write_bank_toml(
            ov_toml_path,
            f"PMD Explorers of Sky ({region}) — Overlay {oid:02d}",
            f"C2S_{region}_OV{oid:02d}",
            ram_addr,
            ram_size,
            first_entry_pc,
            ov_sha1,
            ov_md5,
            ov_entries,
            ov_drs,
            [],
        )
        banks.append(
            BankSpec(
                bank_name=bank_name,
                cpu="arm9",
                bin_path=ov_bin_path,
                toml_path=ov_toml_path,
                load_address=ram_addr,
                size=ram_size,
                entry_pc=first_entry_pc,
                sha1=ov_sha1,
                shards=choose_shard_count(ram_size),
                entry_count=len(ov_entries),
                data_range_count=len(ov_drs),
                code_copy_count=0,
            )
        )

    manifest = {
        "rom_sha1": rom_sha1,
        "region": region,
        "title": rom_title_desc,
        "bank_count": len(banks),
        "banks": [
            {
                "bank": b.bank_name,
                "cpu": b.cpu,
                "bin": str(b.bin_path.resolve()),
                "toml": str(b.toml_path.resolve()),
                "load_address": f"0x{b.load_address:08X}",
                "size": b.size,
                "entry_pc": f"0x{b.entry_pc:08X}",
                "sha1": b.sha1,
                "shards": b.shards,
                "entry_points": b.entry_count,
                "data_ranges": b.data_range_count,
                "code_copies": b.code_copy_count,
            }
            for b in banks
        ],
    }
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    total_entries = sum(b.entry_count for b in banks)
    total_drs = sum(b.data_range_count for b in banks)
    print(
        f"[prepare_rom] Generated {len(banks)} banks ({total_entries} seeded entry points, "
        f"{total_drs} data ranges) -> {out_dir}"
    )
    return rom_sha1, region, banks


def run_recompiler_for_bank(
    recompiler: Path,
    bank: BankSpec,
    gen_dir: Path,
    audit_only: bool,
    max_function_bytes: int,
) -> Tuple[str, int, str]:
    cmd = [
        str(recompiler),
        "--config",
        str(bank.toml_path),
        "--bin",
        str(bank.bin_path),
        "--bank",
        bank.bank_name,
    ]
    if audit_only:
        cmd.append("--audit")
    else:
        cmd.extend([
            "--out",
            str(gen_dir),
            "--validate-live-bytes",
            "--coalesce-fallthroughs",
            "--max-function-bytes",
            str(max_function_bytes),
            "--shards",
            str(bank.shards),
            "--stable-address-shards",
        ])
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    return bank.bank_name, proc.returncode, out


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Prepare & statically recompile PMD: Explorers of Sky ROM")
    parser.add_argument("--rom", required=True, type=Path, help="Path to legally owned EoS .nds ROM")
    parser.add_argument(
        "--symbols-dir",
        type=Path,
        default=repo_root / "third_party" / "pmdsky-debug" / "symbols",
        help="Path to pmdsky-debug/symbols directory",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=repo_root / "build" / "extracted_sky_us",
        help="Directory for extracted binaries and generated TOML configs",
    )
    parser.add_argument("--allow-custom-rom", action="store_true", help="Allow modified/SkyTemple ROMs")
    parser.add_argument("--region", choices=["NA", "EU"], help="Explicit region override for custom ROMs")
    parser.add_argument("--audit", action="store_true", help="Run nds_recompile --audit across all banks")
    parser.add_argument("--compile", action="store_true", help="Emit C static banks via nds_recompile")
    parser.add_argument(
        "--recompiler",
        type=Path,
        default=repo_root / "build" / "recompiler" / "nds_recompile",
        help="Path to built nds_recompile binary",
    )
    parser.add_argument(
        "--gen-dir",
        type=Path,
        default=repo_root / "build" / "generated_sky_us",
        help="Output directory for generated C bank shards",
    )
    parser.add_argument("--max-function-bytes", type=int, default=8192, help="Max function chunk bytes (<=8192)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 8, help="Parallel recompiler workers")

    args = parser.parse_args()
    if not args.rom.is_file():
        print(f"[prepare_rom] ERROR: ROM file not found: {args.rom}", file=sys.stderr)
        return 1

    _sha1, _region, banks = prepare_rom(
        args.rom,
        args.symbols_dir,
        args.out_dir,
        allow_custom_rom=args.allow_custom_rom,
        forced_region=args.region,
    )

    if not args.audit and not args.compile:
        return 0

    if not args.recompiler.is_file():
        print(f"[prepare_rom] ERROR: nds_recompile not found at {args.recompiler}", file=sys.stderr)
        return 1

    if args.compile:
        args.gen_dir.mkdir(parents=True, exist_ok=True)

    mode_label = "Auditing" if args.audit else f"Recompiling -> {args.gen_dir}"
    print(f"[prepare_rom] {mode_label} {len(banks)} banks using {args.jobs} threads...")

    failures = 0
    results: Dict[str, str] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futs = {
            pool.submit(
                run_recompiler_for_bank,
                args.recompiler,
                b,
                args.gen_dir,
                args.audit,
                args.max_function_bytes,
            ): b
            for b in banks
        }
        for fut in as_completed(futs):
            bname, rc, log = fut.result()
            results[bname] = log
            if rc != 0:
                failures += 1
                print(f"\n[FAIL] {bname} (exit {rc}):\n{log}", file=sys.stderr)

    for b in banks:
        log = results[b.bank_name]
        finder_line = next((ln for ln in log.splitlines() if ln.startswith("[finder] functions=")), "")
        emit_line = next((ln for ln in log.splitlines() if ln.startswith("[emit] bank ")), "")
        not_impl = ""
        if args.audit and "NOT-IMPLEMENTED ops" in log:
            after = log.split("NOT-IMPLEMENTED ops", 1)[1].splitlines()[1:3]
            not_impl = " | not_impl=" + ",".join(s.strip() for s in after if s.strip())
        summary = emit_line or (finder_line + not_impl)
        print(f"  {b.bank_name:15s}: {summary}")

    if failures:
        print(f"[prepare_rom] ERROR: {failures} bank(s) failed.", file=sys.stderr)
        return 1

    print(f"[prepare_rom] SUCCESS: All {len(banks)} banks processed cleanly!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

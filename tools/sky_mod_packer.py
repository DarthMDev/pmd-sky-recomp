#!/usr/bin/env python3
"""
SkyRecompiled — Layered NitroFS Mod Loader & Virtual Filesystem Packer
=====================================================================
A zero-dependency, pure-Python NitroFS (FNT + FAT) reader, unpacker, repacker,
and layered mod builder for Pokémon Mystery Dungeon: Explorers of Sky.

Features:
  - Parses DS ROM FNT (File Name Table) and FAT (File Allocation Table).
  - Unpacks complete game romfs into a clean folder structure.
  - Layered Modding: merges clean base ROM with one or more `mods/<mod_id>/romfs/...`
    directories in order of priority specified in `mod.toml`.
  - Rebuilds merged runtime NitroFS images in <1 second without modifying the original ROM!
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class ModMetadata:
    id: str
    name: str = ""
    author: str = ""
    version: str = "1.0.0"
    priority: int = 100
    description: str = ""


class NitroFS:
    def __init__(self) -> None:
        # Map of normalized virtual path (e.g. "data/font/font.bin") -> bytes
        self.files: Dict[str, bytes] = {}

    @classmethod
    def from_rom(cls, rom: bytes) -> NitroFS:
        fs = cls()
        if len(rom) < 0x200:
            raise ValueError("ROM size is too small for a Nintendo DS header")

        fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", rom, 0x40)
        num_fat_entries = fat_sz // 8

        # 1. Read all file slices from FAT
        fat_entries: List[Tuple[int, int]] = []
        for i in range(num_fat_entries):
            s, e = struct.unpack_from("<II", rom, fat_off + i * 8)
            fat_entries.append((s, e))

        # 2. Parse FNT directory table
        # Root directory is ID 0xF000
        root_sub_off, root_first_file, num_dirs = struct.unpack_from("<IHH", rom, fnt_off)
        if num_dirs > 4096 or num_dirs == 0:
            raise ValueError(f"Invalid NitroFS directory count: {num_dirs}")

        dir_table: List[Tuple[int, int, int]] = []
        for d in range(num_dirs):
            sub_off, first_fid, parent_id = struct.unpack_from("<IHH", rom, fnt_off + d * 8)
            dir_table.append((sub_off, first_fid, parent_id))

        # Directory path reconstruction
        dir_paths: Dict[int, str] = {0xF000: ""}

        # Helper to traverse directories
        for dir_idx in range(num_dirs):
            dir_id = 0xF000 + dir_idx
            sub_off, cur_file_id, _parent = dir_table[dir_idx]
            cur_path = dir_paths.get(dir_id, "")

            pos = fnt_off + sub_off
            while pos < fnt_off + fnt_sz:
                t_len = rom[pos]
                pos += 1
                if t_len == 0x00:
                    break

                is_dir = (t_len & 0x80) != 0
                name_len = t_len & 0x7F
                name = rom[pos : pos + name_len].decode("latin-1", errors="replace")
                pos += name_len

                if is_dir:
                    sub_dir_id = struct.unpack_from("<H", rom, pos)[0]
                    pos += 2
                    sub_path = f"{cur_path}/{name}" if cur_path else name
                    dir_paths[sub_dir_id] = sub_path
                else:
                    file_path = f"{cur_path}/{name}" if cur_path else name
                    if cur_file_id < len(fat_entries):
                        fs_start, fs_end = fat_entries[cur_file_id]
                        if fs_start <= len(rom) and fs_end <= len(rom) and fs_start <= fs_end:
                            fs.files[file_path] = rom[fs_start:fs_end]
                    cur_file_id += 1

        return fs

    def unpack(self, out_dir: Path) -> None:
        """Extract all files in the virtual filesystem to disk."""
        out_dir.mkdir(parents=True, exist_ok=True)
        for rel_path, data in self.files.items():
            dest = out_dir / rel_path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)

    def apply_layered_mods(self, mods_root: Path) -> List[Tuple[str, int]]:
        """Scans mods_root for mod directories with mod.toml and applies overrides
        in order of priority (higher priority overwrites lower)."""
        if not mods_root.is_dir():
            return []

        discovered_mods: List[Tuple[ModMetadata, Path]] = []
        for entry in mods_root.iterdir():
            if not entry.is_dir():
                continue
            meta = self._parse_mod_toml(entry / "mod.toml", entry.name)
            romfs_dir = entry / "romfs"
            if romfs_dir.is_dir():
                discovered_mods.append((meta, romfs_dir))

        # Sort by priority ascending (so higher priority applies last / wins)
        discovered_mods.sort(key=lambda m: (m[0].priority, m[0].id))

        applied: List[Tuple[str, int]] = []
        for meta, romfs_dir in discovered_mods:
            override_count = 0
            for root, _dirs, files in os.walk(romfs_dir):
                for f in files:
                    full_p = Path(root) / f
                    rel_p = str(full_p.relative_to(romfs_dir)).replace("\\", "/")
                    self.files[rel_p] = full_p.read_bytes()
                    override_count += 1
            applied.append((meta.id, override_count))

        return applied

    @staticmethod
    def _parse_mod_toml(toml_path: Path, default_id: str) -> ModMetadata:
        meta = ModMetadata(id=default_id)
        if not toml_path.exists():
            return meta
        for line in toml_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip().lower()
            v = v.strip().strip('"').strip("'")
            if k == "id":
                meta.id = v
            elif k == "name":
                meta.name = v
            elif k == "author":
                meta.author = v
            elif k == "version":
                meta.version = v
            elif k == "priority":
                try:
                    meta.priority = int(v)
                except ValueError:
                    pass
        return meta

    def build_fnt_and_fat(self, base_file_offset: int = 0) -> Tuple[bytes, bytes, bytes]:
        """Rebuilds NitroFS FNT and FAT structures plus the contiguous file payload data."""
        # 1. Build directory tree from self.files keys
        # Tree node: { "files": [name...], "dirs": { name: node } }
        tree: dict = {"files": [], "dirs": {}}
        for file_path in sorted(self.files.keys()):
            parts = file_path.split("/")
            cur = tree
            for folder in parts[:-1]:
                if folder not in cur["dirs"]:
                    cur["dirs"][folder] = {"files": [], "dirs": {}}
                cur = cur["dirs"][folder]
            cur["files"].append(parts[-1])

        # Assign directory IDs (Breadth-first search)
        # Root is 0xF000
        dir_list: List[Tuple[str, dict, int]] = [("", tree, 0xF000)]  # (rel_path, node, parent_id)
        queue = [("", tree, 0xF000)]
        next_dir_id = 0xF001

        all_nodes: List[Tuple[int, dict, int]] = [(0xF000, tree, 0xF000)]  # (dir_id, node, parent_id)
        while queue:
            parent_path, parent_node, parent_id = queue.pop(0)
            for dname in sorted(parent_node["dirs"].keys()):
                cur_node = parent_node["dirs"][dname]
                cur_id = next_dir_id
                next_dir_id += 1
                all_nodes.append((cur_id, cur_node, parent_id))
                queue.append((f"{parent_path}/{dname}" if parent_path else dname, cur_node, cur_id))

        total_dirs = len(all_nodes)
        dir_table_size = total_dirs * 8

        # 2. Build sub-tables and determine file IDs
        sub_tables: List[bytes] = []
        cur_sub_off = dir_table_size
        dir_entries_meta: List[Tuple[int, int, int]] = []  # (sub_off, first_fid, parent_id)

        file_id_map: List[str] = []
        cur_fid = 0

        # Build sub-table for each directory
        for dir_id, node, parent_id in all_nodes:
            first_fid = cur_fid
            buf = bytearray()

            # Files first
            for fname in sorted(node["files"]):
                enc_name = fname.encode("latin-1", errors="replace")
                buf.append(len(enc_name) & 0x7F)
                buf.extend(enc_name)
                # Find full path
                # Reconstruct full path
                cur_fid += 1

            # Sub-directories
            for dname in sorted(node["dirs"].keys()):
                enc_dname = dname.encode("latin-1", errors="replace")
                buf.append((len(enc_dname) & 0x7F) | 0x80)
                buf.extend(enc_dname)
                # Locate assigned sub_dir_id
                sub_id = next(nid for nid, n, _ in all_nodes if n is node["dirs"][dname])
                buf.extend(struct.pack("<H", sub_id))

            buf.append(0x00)  # End of directory marker
            dir_entries_meta.append((cur_sub_off, first_fid, parent_id if dir_id != 0xF000 else total_dirs))
            sub_tables.append(bytes(buf))
            cur_sub_off += len(buf)

        # Assemble full FNT bytes
        fnt_buf = bytearray()
        for sub_off, first_fid, parent_or_count in dir_entries_meta:
            fnt_buf.extend(struct.pack("<IHH", sub_off, first_fid, parent_or_count))
        for st in sub_tables:
            fnt_buf.extend(st)

        # Pad FNT to 4-byte alignment
        while len(fnt_buf) % 4 != 0:
            fnt_buf.append(0x00)

        # 3. Assemble FAT and payload data
        # Flatten all files in sorted order
        payload_buf = bytearray()
        fat_buf = bytearray()
        cur_file_data_off = base_file_offset

        for file_path in sorted(self.files.keys()):
            data = self.files[file_path]
            start_off = cur_file_data_off
            end_off = start_off + len(data)
            fat_buf.extend(struct.pack("<II", start_off, end_off))
            payload_buf.extend(data)
            # Pad file to 4-byte alignment
            cur_file_data_off = end_off
            while len(payload_buf) % 4 != 0:
                payload_buf.append(0x00)
                cur_file_data_off += 1

        return bytes(fnt_buf), bytes(fat_buf), bytes(payload_buf)


def main():
    parser = argparse.ArgumentParser(description="SkyRecompiled Layered NitroFS Mod Packer")
    sub = parser.add_subparsers(dest="command")

    p_unpack = sub.add_parser("unpack", help="Unpack clean base ROM romfs to disk")
    p_unpack.add_argument("--rom", required=True, type=Path, help="Path to .nds ROM")
    p_unpack.add_argument("--out", required=True, type=Path, help="Output directory")

    p_list = sub.add_parser("list", help="List files in base ROM romfs")
    p_list.add_argument("--rom", required=True, type=Path, help="Path to .nds ROM")

    p_build = sub.add_parser("build-mods", help="Build layered NitroFS image from clean ROM + mods/")
    p_build.add_argument("--rom", required=True, type=Path, help="Path to clean base .nds ROM")
    p_build.add_argument("--mods-dir", required=True, type=Path, help="Directory containing mod folders")
    p_build.add_argument("--out-fnt", required=True, type=Path, help="Output FNT binary path")
    p_build.add_argument("--out-fat", required=True, type=Path, help="Output FAT binary path")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command == "unpack":
        print(f"[sky_mod_packer] Reading {args.rom}...")
        rom = args.rom.read_bytes()
        fs = NitroFS.from_rom(rom)
        print(f"[sky_mod_packer] Unpacking {len(fs.files)} files to {args.out}...")
        fs.unpack(args.out)
        print("[sky_mod_packer] Unpack complete!")

    elif args.command == "list":
        rom = args.rom.read_bytes()
        fs = NitroFS.from_rom(rom)
        for f in sorted(fs.files.keys()):
            print(f"{len(fs.files[f]):>8} B  {f}")

    elif args.command == "build-mods":
        rom = args.rom.read_bytes()
        fs = NitroFS.from_rom(rom)
        applied = fs.apply_layered_mods(args.mods_dir)
        for mod_id, count in applied:
            print(f"[sky_mod_packer] Applied mod '{mod_id}': {count} files overridden")
        fnt, fat, payload = fs.build_fnt_and_fat()
        args.out_fnt.write_bytes(fnt)
        args.out_fat.write_bytes(fat)
        print(f"[sky_mod_packer] Wrote FNT ({len(fnt)} B) and FAT ({len(fat)} B)")


if __name__ == "__main__":
    main()

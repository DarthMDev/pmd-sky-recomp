#!/usr/bin/env python3
"""
SkyRecompiled -- Layered NitroFS Mod Loader & Virtual Filesystem Packer
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
import tempfile
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


@dataclass
class ModMetadata:
    id: str
    name: str = ""
    author: str = ""
    version: str = "1.0.0"
    priority: int = 100
    description: str = ""
    enabled: bool = True


def _nds_crc16(data: bytes) -> int:
    """Compute Nintendo DS header CRC-16 (Modbus polynomial 0xA001, seed 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def atomic_write_bytes(path: Path, data: bytes) -> None:
    """Atomically write bytes to path using a temporary file in the same directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as tf:
            tmp_path = Path(tf.name)
            tf.write(data)
            tf.flush()
            os.fsync(tf.fileno())
        os.replace(tmp_path, path)
    except Exception:
        if tmp_path is not None and tmp_path.exists():
            tmp_path.unlink(missing_ok=True)
        raise


class NitroFS:
    def __init__(self) -> None:
        # Map of normalized virtual path (e.g. "data/font/font.bin") -> bytes
        self.files: Dict[str, bytes] = {}
        self.file_ids: Dict[str, int] = {}
        self.unmapped_fat_entries: Dict[int, bytes] = {}
        self.raw_fnt: Optional[bytes] = None
        self.total_fat_entries: int = 0
        self.last_conflicts: Dict[str, List[str]] = {}

    @classmethod
    def from_rom(cls, rom: bytes) -> NitroFS:
        fs = cls()
        if len(rom) < 0x200:
            raise ValueError("ROM size is too small for a Nintendo DS header")

        fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", rom, 0x40)

        def check_region(offset: int, size: int, label: str) -> None:
            if offset < 0 or size < 0 or offset > len(rom) or size > len(rom) - offset:
                raise ValueError(f"{label} lies outside the ROM")

        check_region(fnt_off, fnt_sz, "FNT")
        check_region(fat_off, fat_sz, "FAT")
        if fnt_sz < 8 or fat_sz % 8:
            raise ValueError("Invalid NitroFS FNT/FAT size")
        num_fat_entries = fat_sz // 8

        # 1. Read all file slices from FAT
        fat_entries: List[Tuple[int, int]] = []
        for i in range(num_fat_entries):
            s, e = struct.unpack_from("<II", rom, fat_off + i * 8)
            if s > e or e > len(rom):
                raise ValueError(f"Invalid FAT range for file ID {i}")
            fat_entries.append((s, e))

        # 2. Parse FNT directory table
        # Root directory is ID 0xF000
        root_sub_off, root_first_file, num_dirs = struct.unpack_from("<IHH", rom, fnt_off)
        if num_dirs == 0 or num_dirs > 4096 or num_dirs * 8 > fnt_sz:
            raise ValueError(f"Invalid NitroFS directory count: {num_dirs}")

        dir_table: List[Tuple[int, int, int]] = []
        for d in range(num_dirs):
            sub_off, first_fid, parent_id = struct.unpack_from("<IHH", rom, fnt_off + d * 8)
            if sub_off < num_dirs * 8 or sub_off >= fnt_sz or first_fid > num_fat_entries:
                raise ValueError(f"Invalid FNT directory entry {d}")
            dir_table.append((sub_off, first_fid, parent_id))

        # Directory path reconstruction
        dir_paths: Dict[int, str] = {0xF000: ""}
        claimed_fids: Dict[int, str] = {}

        # Helper to traverse directories
        for dir_idx in range(num_dirs):
            dir_id = 0xF000 + dir_idx
            sub_off, cur_file_id, _parent = dir_table[dir_idx]
            cur_path = dir_paths.get(dir_id, "")

            pos = fnt_off + sub_off
            fnt_end = fnt_off + fnt_sz
            terminated = False
            while pos < fnt_end:
                t_len = rom[pos]
                pos += 1
                if t_len == 0x00:
                    terminated = True
                    break

                is_dir = (t_len & 0x80) != 0
                name_len = t_len & 0x7F
                if name_len == 0 or pos + name_len + (2 if is_dir else 0) > fnt_end:
                    raise ValueError(f"Truncated FNT name in directory {dir_idx}")
                name = rom[pos : pos + name_len].decode("latin-1", errors="replace")
                pos += name_len

                if is_dir:
                    sub_dir_id = struct.unpack_from("<H", rom, pos)[0]
                    pos += 2
                    if sub_dir_id < 0xF000 or sub_dir_id >= 0xF000 + num_dirs:
                        raise ValueError(f"Invalid FNT subdirectory ID: {sub_dir_id:#x}")
                    cls._validate_virtual_path(name)
                    sub_path = f"{cur_path}/{name}" if cur_path else name
                    dir_paths[sub_dir_id] = sub_path
                else:
                    file_path = f"{cur_path}/{name}" if cur_path else name
                    cls._validate_virtual_path(file_path)
                    if cur_file_id >= len(fat_entries):
                        raise ValueError(f"FNT references missing file ID {cur_file_id}")
                    fs_start, fs_end = fat_entries[cur_file_id]
                    if file_path in fs.files or cur_file_id in claimed_fids:
                        raise ValueError(
                            f"Duplicate NitroFS file entry: {file_path!r} (fid {cur_file_id})"
                        )
                    fs.files[file_path] = rom[fs_start:fs_end]
                    fs.file_ids[file_path] = cur_file_id
                    claimed_fids[cur_file_id] = file_path
                    cur_file_id += 1
            if not terminated:
                raise ValueError(f"Unterminated FNT directory entry {dir_idx}")

        fs.raw_fnt = bytes(rom[fnt_off : fnt_off + fnt_sz])
        fs.total_fat_entries = num_fat_entries
        for fid in range(num_fat_entries):
            if fid not in claimed_fids:
                s, e = fat_entries[fid]
                fs.unmapped_fat_entries[fid] = bytes(rom[s:e])

        return fs

    def unpack(self, out_dir: Path) -> None:
        """Extract all files in the virtual filesystem to disk safely."""
        out_resolved = out_dir.resolve()
        out_resolved.mkdir(parents=True, exist_ok=True)
        for rel_path, data in self.files.items():
            # Prevent absolute paths or directory traversal (..)
            norm_rel = os.path.normpath(rel_path).replace("\\", "/")
            if norm_rel.startswith("/") or norm_rel.startswith("../") or "/../" in norm_rel or norm_rel == "..":
                continue
            dest = (out_resolved / norm_rel).resolve()
            if not str(dest).startswith(str(out_resolved) + os.sep) and dest != out_resolved:
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)

    def apply_layered_mods(
        self,
        mods_root: Path,
        *,
        strict_existing_only: bool = False,
        enabled_mods: Optional[Set[str]] = None,
    ) -> List[Tuple[str, int]]:
        """Scans mods_root for mod directories with mod.toml and applies overrides
        in order of priority (higher priority overwrites lower)."""
        self.last_conflicts = {}
        if not mods_root.is_dir():
            return []

        discovered_mods: List[Tuple[ModMetadata, Path]] = []
        seen_ids: Set[str] = set()
        for entry in sorted(mods_root.iterdir()):
            if entry.is_symlink() or not entry.is_dir():
                continue
            romfs_dir = entry / "romfs"
            if romfs_dir.is_symlink():
                raise ValueError(f"Mod romfs must not be a symlink: {romfs_dir}")
            if not romfs_dir.is_dir():
                continue
            meta = self._parse_mod_toml(entry / "mod.toml", entry.name)
            if not meta.id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in meta.id):
                raise ValueError(f"Invalid mod ID: {meta.id!r}")
            if meta.id in seen_ids:
                raise ValueError(f"Duplicate mod ID: {meta.id}")
            seen_ids.add(meta.id)
            discovered_mods.append((meta, romfs_dir))

        active_mods = [
            (meta, romfs_dir)
            for meta, romfs_dir in discovered_mods
            if meta.enabled and (enabled_mods is None or meta.id in enabled_mods)
        ]

        # Sort by priority ascending (so higher priority applies last / wins)
        active_mods.sort(key=lambda m: (m[0].priority, m[0].id))

        touch_history: Dict[str, List[str]] = {}
        applied: List[Tuple[str, int]] = []
        for meta, romfs_dir in active_mods:
            override_count = 0
            for root, dirs, files in os.walk(romfs_dir, followlinks=False):
                root_path = Path(root)
                for name in dirs + files:
                    if (root_path / name).is_symlink():
                        raise ValueError(f"Symlinks are not allowed in mod layers: {root_path / name}")
                for f in sorted(files):
                    full_p = root_path / f
                    rel_p = full_p.relative_to(romfs_dir).as_posix()
                    self._validate_virtual_path(rel_p)
                    if strict_existing_only and rel_p not in self.files:
                        raise ValueError(
                            f"Mod {meta.id!r} cannot add new NitroFS path {rel_p!r} in strict replacement mode"
                        )
                    if len(self.files) >= 65535 and rel_p not in self.files:
                        raise ValueError("NitroFS file count exceeds the 16-bit file ID limit")
                    self.files[rel_p] = full_p.read_bytes()
                    touch_history.setdefault(rel_p, []).append(meta.id)
                    override_count += 1
            applied.append((meta.id, override_count))

        self.last_conflicts = {
            path: mods for path, mods in sorted(touch_history.items()) if len(mods) > 1
        }
        return applied

    @staticmethod
    def _validate_virtual_path(path: str) -> None:
        parts = path.split("/")
        if (not path or path.startswith("/") or "\\" in path or ":" in path
                or any(part in ("", ".", "..") for part in parts)):
            raise ValueError(f"Invalid NitroFS path: {path!r}")
        for part in parts:
            try:
                encoded = part.encode("latin-1")
            except UnicodeEncodeError as exc:
                raise ValueError(f"NitroFS path is not representable in the FNT: {path!r}") from exc
            if len(encoded) > 127 or any(ord(ch) < 32 for ch in part):
                raise ValueError(f"Invalid NitroFS path component: {part!r}")

    @staticmethod
    def _parse_mod_toml(toml_path: Path, default_id: str) -> ModMetadata:
        meta = ModMetadata(id=default_id)
        if not toml_path.exists():
            return meta
        try:
            data = tomllib.loads(toml_path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as exc:
            raise ValueError(f"Invalid mod.toml in {toml_path}: {exc}") from exc

        if "id" in data:
            meta.id = str(data["id"])
        if "name" in data:
            meta.name = str(data["name"])
        if "author" in data:
            meta.author = str(data["author"])
        if "version" in data:
            meta.version = str(data["version"])
        if "description" in data:
            meta.description = str(data["description"])
        if "priority" in data:
            raw_prio = data["priority"]
            if isinstance(raw_prio, bool):
                raise ValueError(f"Invalid mod priority in {toml_path}: {raw_prio!r}")
            if isinstance(raw_prio, int):
                meta.priority = raw_prio
            elif isinstance(raw_prio, str):
                try:
                    meta.priority = int(raw_prio.strip())
                except ValueError as exc:
                    raise ValueError(f"Invalid mod priority in {toml_path}: {raw_prio!r}") from exc
            else:
                raise ValueError(f"Invalid mod priority in {toml_path}: {raw_prio!r}")
        if "enabled" in data:
            raw_enabled = data["enabled"]
            if isinstance(raw_enabled, bool):
                meta.enabled = raw_enabled
            elif isinstance(raw_enabled, str):
                low = raw_enabled.strip().lower()
                if low == "true":
                    meta.enabled = True
                elif low == "false":
                    meta.enabled = False
                else:
                    raise ValueError(f"Invalid mod enabled flag in {toml_path}: {raw_enabled!r}")
            else:
                raise ValueError(f"Invalid mod enabled flag in {toml_path}: {raw_enabled!r}")
        return meta

    def build_fnt_and_fat(
        self,
        base_file_offset: int = 0,
        *,
        preserve_layout: bool = True,
    ) -> Tuple[bytes, bytes, bytes]:
        """Rebuilds NitroFS FNT and FAT structures plus the contiguous file payload data."""
        can_preserve = (
            preserve_layout
            and self.raw_fnt is not None
            and set(self.files.keys()) == set(self.file_ids.keys())
            and (len(self.files) + len(self.unmapped_fat_entries) == self.total_fat_entries)
            and (set(self.file_ids.values()) | set(self.unmapped_fat_entries.keys()))
            == set(range(self.total_fat_entries))
        )
        if can_preserve:
            assert self.raw_fnt is not None
            fnt_buf = bytearray(self.raw_fnt)
            while len(fnt_buf) % 4 != 0:
                fnt_buf.append(0x00)
            fid_to_path = {fid: path for path, fid in self.file_ids.items()}
            payload_buf = bytearray()
            fat_buf = bytearray()
            cur_file_data_off = base_file_offset
            for fid in range(self.total_fat_entries):
                if fid in self.unmapped_fat_entries:
                    data = self.unmapped_fat_entries[fid]
                else:
                    data = self.files[fid_to_path[fid]]
                start_off = cur_file_data_off
                end_off = start_off + len(data)
                fat_buf.extend(struct.pack("<II", start_off, end_off))
                payload_buf.extend(data)
                cur_file_data_off = end_off
                while len(payload_buf) % 4 != 0:
                    payload_buf.append(0x00)
                    cur_file_data_off += 1
            return bytes(fnt_buf), bytes(fat_buf), bytes(payload_buf)

        # 1. Build directory tree from self.files keys
        # Tree node: { "files": [name...], "dirs": { name: node } }
        unmapped_prefix_count = (
            max(self.unmapped_fat_entries.keys()) + 1 if self.unmapped_fat_entries else 0
        )
        tree: dict = {"files": [], "dirs": {}}
        if unmapped_prefix_count + len(self.files) > 65535:
            raise ValueError("NitroFS file count exceeds the 16-bit file ID limit")
        for file_path in sorted(self.files.keys()):
            self._validate_virtual_path(file_path)
            parts = file_path.split("/")
            cur = tree
            for folder in parts[:-1]:
                if folder in cur["files"]:
                    raise ValueError(f"NitroFS path is both a file and directory: {file_path!r}")
                if folder not in cur["dirs"]:
                    cur["dirs"][folder] = {"files": [], "dirs": {}}
                cur = cur["dirs"][folder]
            if parts[-1] in cur["dirs"]:
                raise ValueError(f"NitroFS path is both a file and directory: {file_path!r}")
            cur["files"].append(parts[-1])

        # Assign directory IDs (Breadth-first search)
        # Root is 0xF000
        queue = [("", tree, 0xF000)]
        next_dir_id = 0xF001

        all_nodes: List[Tuple[int, dict, int]] = [(0xF000, tree, 0xF000)]  # (dir_id, node, parent_id)
        node_path_map: dict[int, str] = {id(tree): ""}
        while queue:
            parent_path, parent_node, parent_id = queue.pop(0)
            for dname in sorted(parent_node["dirs"].keys()):
                cur_node = parent_node["dirs"][dname]
                cur_id = next_dir_id
                next_dir_id += 1
                cur_path = f"{parent_path}/{dname}" if parent_path else dname
                node_path_map[id(cur_node)] = cur_path
                all_nodes.append((cur_id, cur_node, parent_id))
                queue.append((cur_path, cur_node, cur_id))

        total_dirs = len(all_nodes)
        if total_dirs > 4096:
            raise ValueError("NitroFS directory count exceeds the supported limit")
        dir_table_size = total_dirs * 8

        # 2. Build sub-tables and determine file IDs
        sub_tables: List[bytes] = []
        cur_sub_off = dir_table_size
        dir_entries_meta: List[Tuple[int, int, int]] = []  # (sub_off, first_fid, parent_id)

        ordered_files: List[str] = []
        cur_fid = unmapped_prefix_count

        # Build sub-table for each directory
        for dir_id, node, parent_id in all_nodes:
            first_fid = cur_fid
            buf = bytearray()
            dpath = node_path_map.get(id(node), "")

            # Files first
            for fname in sorted(node["files"]):
                enc_name = fname.encode("latin-1")
                buf.append(len(enc_name))
                buf.extend(enc_name)
                full_path = f"{dpath}/{fname}" if dpath else fname
                ordered_files.append(full_path)
                cur_fid += 1

            # Sub-directories
            for dname in sorted(node["dirs"].keys()):
                enc_dname = dname.encode("latin-1")
                buf.append(len(enc_dname) | 0x80)
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

        # 3. Assemble FAT and payload data in the EXACT same order as FNT IDs
        payload_buf = bytearray()
        fat_buf = bytearray()
        cur_file_data_off = base_file_offset

        for fid in range(unmapped_prefix_count):
            data = self.unmapped_fat_entries.get(fid, b"")
            start_off = cur_file_data_off
            end_off = start_off + len(data)
            fat_buf.extend(struct.pack("<II", start_off, end_off))
            payload_buf.extend(data)
            cur_file_data_off = end_off
            while len(payload_buf) % 4 != 0:
                payload_buf.append(0x00)
                cur_file_data_off += 1

        for file_path in ordered_files:
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

    def build_patched_rom(self, base_rom: bytes) -> bytes:
        """Build a patched .nds ROM preserving executable sections, overlay FAT entries,
        and original FNT file IDs."""
        if len(base_rom) < 0x200:
            raise ValueError("ROM size is too small for a Nintendo DS header")
        if (
            self.raw_fnt is None
            or set(self.files.keys()) != set(self.file_ids.keys())
            or len(self.files) + len(self.unmapped_fat_entries) != self.total_fat_entries
            or (set(self.file_ids.values()) | set(self.unmapped_fat_entries.keys()))
            != set(range(self.total_fat_entries))
        ):
            raise ValueError("build_patched_rom requires preserved base ROM file layout")

        fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", base_rom, 0x40)
        if fat_sz != self.total_fat_entries * 8 or fat_off + fat_sz > len(base_rom) or fnt_off + fnt_sz > len(base_rom):
            raise ValueError("Base ROM FNT/FAT layout does not match NitroFS state")

        orig_fat = [
            struct.unpack_from("<II", base_rom, fat_off + i * 8)
            for i in range(self.total_fat_entries)
        ]
        named_fids = set(self.file_ids.values())
        fid_to_path = {fid: path for path, fid in self.file_ids.items()}

        raw_payload_start = min(orig_fat[fid][0] for fid in named_fids) if named_fids else len(base_rom)
        arm9_off, _, _, arm9_sz = struct.unpack_from("<IIII", base_rom, 0x20)
        arm7_off, _, _, arm7_sz = struct.unpack_from("<IIII", base_rom, 0x30)
        ov9_off, ov9_sz, ov7_off, ov7_sz = struct.unpack_from("<IIII", base_rom, 0x50)
        icon_off = struct.unpack_from("<I", base_rom, 0x68)[0]
        header_section_ends = [
            fnt_off + fnt_sz,
            fat_off + fat_sz,
            *(
                off + sz
                for off, sz in (
                    (arm9_off, arm9_sz),
                    (arm7_off, arm7_sz),
                    (ov9_off, ov9_sz),
                    (ov7_off, ov7_sz),
                )
                if sz > 0 and off + sz <= len(base_rom)
            ),
        ]
        if 0 < icon_off and icon_off + 2 <= len(base_rom):
            banner_ver = struct.unpack_from("<H", base_rom, icon_off)[0]
            banner_sizes = {0x0001: 0x840, 0x0002: 0x940, 0x0003: 0xA40, 0x0103: 0x23C0}
            banner_sz = banner_sizes.get(banner_ver, 0x840)
            if icon_off + banner_sz <= len(base_rom):
                header_section_ends.append(icon_off + banner_sz)
        preserve_end = min(len(base_rom), max(raw_payload_start, *header_section_ends))
        out_rom = bytearray(base_rom[:preserve_end])
        while len(out_rom) < preserve_end:
            out_rom.append(0x00)
        while len(out_rom) % 4 != 0:
            out_rom.append(0x00)

        for fid in range(self.total_fat_entries):
            if fid not in named_fids:
                start_off, end_off = orig_fat[fid]
                if end_off > preserve_end:
                    data = self.unmapped_fat_entries.get(fid, base_rom[start_off:end_off])
                    while len(out_rom) % 4 != 0:
                        out_rom.append(0x00)
                    start_off = len(out_rom)
                    out_rom.extend(data)
                    end_off = len(out_rom)
                struct.pack_into("<II", out_rom, fat_off + fid * 8, start_off, end_off)
            else:
                data = self.files[fid_to_path[fid]]
                while len(out_rom) % 4 != 0:
                    out_rom.append(0x00)
                start_off = len(out_rom)
                out_rom.extend(data)
                end_off = len(out_rom)
                struct.pack_into("<II", out_rom, fat_off + fid * 8, start_off, end_off)

        if len(out_rom) >= 0x160:
            struct.pack_into("<I", out_rom, 0x80, len(out_rom))
            struct.pack_into("<H", out_rom, 0x15E, _nds_crc16(bytes(out_rom[:0x15E])))

        return bytes(out_rom)


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
    p_build.add_argument("--out-payload", type=Path, default=None, help="Output merged payload binary path (defaults to <out_fat_dir>/payload.bin)")
    p_build.add_argument("--strict-existing-only", action="store_true", help="Reject mods that add new NitroFS paths")

    p_build_rom = sub.add_parser("build-rom", help="Build patched .nds ROM preserving executable banks and file IDs (replacement-only)")
    p_build_rom.add_argument("--rom", required=True, type=Path, help="Path to clean base .nds ROM")
    p_build_rom.add_argument("--mods-dir", required=True, type=Path, help="Directory containing mod folders")
    p_build_rom.add_argument("--out-rom", required=True, type=Path, help="Output patched .nds ROM path")
    p_build_rom.add_argument("--strict-existing-only", action="store_true", help="Reject mods that add new NitroFS paths (always enforced for build-rom)")

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
        applied = fs.apply_layered_mods(
            args.mods_dir,
            strict_existing_only=args.strict_existing_only,
        )
        for mod_id, count in applied:
            print(f"[sky_mod_packer] Applied mod '{mod_id}': {count} files overridden")
        for path, mods in fs.last_conflicts.items():
            print(f"[sky_mod_packer] Conflict on '{path}': {' -> '.join(mods)} (winner: {mods[-1]})")
        fnt, fat, payload = fs.build_fnt_and_fat()
        atomic_write_bytes(args.out_fnt, fnt)
        atomic_write_bytes(args.out_fat, fat)
        payload_dest = args.out_payload or (args.out_fat.parent / "payload.bin")
        atomic_write_bytes(payload_dest, payload)
        print(f"[sky_mod_packer] Wrote FNT ({len(fnt)} B), FAT ({len(fat)} B), and Payload ({len(payload)} B) to {payload_dest}")

    elif args.command == "build-rom":
        rom = args.rom.read_bytes()
        fs = NitroFS.from_rom(rom)
        applied = fs.apply_layered_mods(
            args.mods_dir,
            strict_existing_only=True,
        )
        for mod_id, count in applied:
            print(f"[sky_mod_packer] Applied mod '{mod_id}': {count} files overridden")
        for path, mods in fs.last_conflicts.items():
            print(f"[sky_mod_packer] Conflict on '{path}': {' -> '.join(mods)} (winner: {mods[-1]})")
        patched = fs.build_patched_rom(rom)
        atomic_write_bytes(args.out_rom, patched)
        print(f"[sky_mod_packer] Wrote patched ROM ({len(patched)} B) to {args.out_rom}")


if __name__ == "__main__":
    main()

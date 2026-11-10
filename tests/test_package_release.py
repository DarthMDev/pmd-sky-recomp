#!/usr/bin/env python3
"""Unit tests for release packaging and Linux ELF binary validation."""

from __future__ import annotations

import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import scripts.launcher as launcher
import scripts.package_release as package_release


def make_elf64_x86_64_header() -> bytes:
    """Construct a minimal valid 64-bit little-endian x86_64 ELF header."""
    e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
    # e_type=ET_EXEC (2), e_machine=EM_X86_64 (0x3E), e_version=1
    return e_ident + struct.pack("<HHI", 2, 0x3E, 1) + b"\x00" * 40


def make_macho_arm64_header() -> bytes:
    """Construct a minimal 64-bit arm64 Mach-O magic header."""
    # MH_MAGIC_64 (0xFEEDFACF), CPU_TYPE_ARM64 (0x0100000C)
    return struct.pack("<II", 0xFEEDFACF, 0x0100000C) + b"\x00" * 24


class TestPackageRelease(unittest.TestCase):
    """Verify that Linux release packaging validates ELF binaries and rejects foreign binaries."""

    def test_verify_linux_elf_accepts_x86_64_elf(self) -> None:
        """Accept a valid 64-bit little-endian x86_64 ELF binary."""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "nds_runner"
            binary.write_bytes(make_elf64_x86_64_header())
            package_release.verify_linux_elf_x86_64(binary)

    def test_verify_linux_elf_rejects_macos_macho(self) -> None:
        """Reject a macOS arm64 Mach-O binary when packaging for Linux."""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "nds_runner"
            binary.write_bytes(make_macho_arm64_header())
            with self.assertRaises(SystemExit) as ctx:
                package_release.verify_linux_elf_x86_64(binary)
            self.assertIn("not a Linux ELF binary", str(ctx.exception))

    def test_verify_linux_elf_rejects_wrong_arch(self) -> None:
        """Reject an ELF binary targeting a non-x86_64 architecture (e.g. EM_AARCH64=0xB7)."""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "nds_runner"
            e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
            binary.write_bytes(e_ident + struct.pack("<HHI", 2, 0xB7, 1))
            with self.assertRaises(SystemExit) as ctx:
                package_release.verify_linux_elf_x86_64(binary)
            self.assertIn("not a 64-bit x86_64 ELF executable", str(ctx.exception))

    def test_verify_linux_elf_rejects_non_executable_type(self) -> None:
        """Reject a relocatable ELF object file (ET_REL=1) even if x86_64."""
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "nds_runner"
            e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
            binary.write_bytes(e_ident + struct.pack("<HHI", 1, 0x3E, 1))
            with self.assertRaises(SystemExit) as ctx:
                package_release.verify_linux_elf_x86_64(binary)
            self.assertIn("e_type=1", str(ctx.exception))

    def test_launcher_discovers_packaged_linux_layout(self) -> None:
        """Verify find_binary_and_config locates bin/nds_runner and config/game.toml from scripts/launcher.py."""
        with tempfile.TemporaryDirectory() as tmp:
            pkg_root = Path(tmp) / "SkyRecompiled-v0.1.0-alpha-linux-x86_64"
            (pkg_root / "bin").mkdir(parents=True)
            (pkg_root / "config").mkdir(parents=True)
            (pkg_root / "scripts").mkdir(parents=True)
            runner_file = pkg_root / "bin" / "nds_runner"
            config_file = pkg_root / "config" / "game.toml"
            fake_launcher = pkg_root / "scripts" / "launcher.py"
            runner_file.write_bytes(make_elf64_x86_64_header())
            config_file.write_text("[game]\n", encoding="utf-8")
            fake_launcher.write_text("# stub\n", encoding="utf-8")

            with mock.patch.object(launcher, "__file__", str(fake_launcher)), \
                 mock.patch.object(launcher.platform, "system", return_value="Linux"):
                found_runner, found_config = launcher.find_binary_and_config()
                self.assertEqual(found_runner.resolve(), runner_file.resolve())
                self.assertEqual(found_config.resolve(), config_file.resolve())

    def test_package_linux_requires_runner_binary(self) -> None:
        """Fail when build/runner/nds_runner is missing instead of creating an empty archive."""
        with tempfile.TemporaryDirectory() as tmp:
            fake_build = Path(tmp) / "build"
            fake_dist = fake_build / "dist"
            with mock.patch.object(package_release, "BUILD_DIR", fake_build), \
                 mock.patch.object(package_release, "DIST_DIR", fake_dist):
                with self.assertRaises(SystemExit) as ctx:
                    package_release.package_linux(without_game_banks=True)
                self.assertIn("Cannot package Linux: missing", str(ctx.exception))

    def test_package_linux_bundles_verified_elf_and_sdl3(self) -> None:
        """Package a verified Linux x86_64 ELF binary and bundled libSDL3.so.0."""
        with tempfile.TemporaryDirectory() as tmp:
            fake_build = Path(tmp) / "build"
            fake_dist = fake_build / "dist"
            runner_dir = fake_build / "runner-fallback"
            runner_dir.mkdir(parents=True)
            (runner_dir / "nds_runner").write_bytes(make_elf64_x86_64_header())
            (runner_dir / "libSDL3.so.0").write_bytes(b"FAKE_SDL3_SO")
            (runner_dir / "SDL3-LICENSE.txt").write_text("Zlib License\n", encoding="utf-8")

            with mock.patch.object(package_release, "BUILD_DIR", fake_build), \
                 mock.patch.object(package_release, "DIST_DIR", fake_dist):
                tar_path = package_release.package_linux(without_game_banks=True)
                self.assertTrue(tar_path.is_file())
                with tarfile.open(tar_path, "r:gz") as tar:
                    names = set(tar.getnames())
                prefix = f"SkyRecompiled-v{package_release.VERSION}-linux-x86_64"
                self.assertIn(f"{prefix}/bin/nds_runner", names)
                self.assertIn(f"{prefix}/bin/libSDL3.so.0", names)
                self.assertIn(f"{prefix}/bin/SDL3-LICENSE.txt", names)
                self.assertIn(f"{prefix}/SkyRecompiled", names)
                self.assertIn(f"{prefix}/scripts/launcher.py", names)


if __name__ == "__main__":
    unittest.main()

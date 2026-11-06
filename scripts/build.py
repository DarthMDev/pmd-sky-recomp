#!/usr/bin/env python3
"""
SkyRecompiled — Top-Level Build Orchestrator
===========================================
Automates the full build pipeline for Pokémon Mystery Dungeon: Explorers of Sky
Static Recompilation:
  1. Verifies prerequisites and initializes third_party dependencies if needed.
  2. Compiles the static recompiler (nds_recompile).
  3. Generates clean-room FreeBIOS C dispatch banks.
  4. Extracts and recompiles all 34 game modules (prepare_rom.py).
  5. Configures and builds the native runner (nds_runner).
  6. Optionally packages SkyRecompiled.app on macOS.

Usage:
  python3 scripts/build.py [--rom <path>] [--package] [--clean] [--jobs N]
"""

from __future__ import annotations

import argparse
import hashlib
import multiprocessing
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
RECOMPILER_BUILD_DIR = BUILD_DIR / "recompiler"
RUNNER_BUILD_DIR = BUILD_DIR / "runner"
GENERATED_BIOS_DIR = BUILD_DIR / "generated_bios"
GENERATED_BANKS_DIR = BUILD_DIR / "generated_sky_us"
DEFAULT_ROM_PATH = REPO_ROOT / "roms" / "pmdsky.us.nds"
CLEAN_USA_SHA1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
CLEAN_EU_SHA1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139"

# Ensure Homebrew and standard tool paths are accessible
for extra_path in ["/opt/homebrew/bin", "/usr/local/bin"]:
    if Path(extra_path).exists() and extra_path not in os.environ.get("PATH", ""):
        os.environ["PATH"] = f"{extra_path}:{os.environ.get('PATH', '')}"


def log(msg: str) -> None:
    print(f"\033[1;36m[SkyRecompiled:build]\033[0m {msg}", flush=True)


def log_err(msg: str) -> None:
    print(f"\033[1;31m[SkyRecompiled:build ERROR]\033[0m {msg}", file=sys.stderr, flush=True)


def run_cmd(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    display_cmd = " ".join(cmd)
    log(f"Running: {display_cmd}")
    res = subprocess.run(cmd, cwd=cwd or REPO_ROOT, env=env)
    if res.returncode != 0:
        log_err(f"Command failed with exit code {res.returncode}: {display_cmd}")
        sys.exit(res.returncode)


def check_prerequisites() -> None:
    log("Checking prerequisites...")
    required_tools = ["cmake", "ninja"]
    for tool in required_tools:
        if not shutil.which(tool):
            log_err(f"Missing required tool: '{tool}'. Please install it (e.g. brew install cmake ninja).")
            sys.exit(1)


def ensure_third_party() -> None:
    ndsrecomp_dir = REPO_ROOT / "third_party" / "ndsrecomp"
    pmdsky_debug_dir = REPO_ROOT / "third_party" / "pmdsky-debug"
    recomp_ui_dir = REPO_ROOT / "third_party" / "recomp-ui"

    if not (ndsrecomp_dir.exists() and pmdsky_debug_dir.exists() and recomp_ui_dir.exists()):
        log("Third-party dependencies missing. Running scripts/setup_third_party.sh...")
        setup_script = REPO_ROOT / "scripts" / "setup_third_party.sh"
        if not setup_script.exists():
            log_err(f"Cannot find {setup_script}")
            sys.exit(1)
        run_cmd(["bash", str(setup_script)])
    else:
        log("Third-party dependencies verified.")


def build_recompiler(jobs: int, build_type: str) -> Path:
    log(f"Building nds_recompile ({build_type})...")
    RECOMPILER_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    exe_name = "nds_recompile.exe" if sys.platform == "win32" else "nds_recompile"
    recompiler_bin = RECOMPILER_BUILD_DIR / exe_name

    cmake_args = [
        "cmake",
        "-S", str(REPO_ROOT / "third_party" / "ndsrecomp" / "recompiler"),
        "-B", str(RECOMPILER_BUILD_DIR),
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]
    run_cmd(cmake_args)
    run_cmd(["cmake", "--build", str(RECOMPILER_BUILD_DIR), "--target", "nds_recompile", f"-j{jobs}"])

    if not recompiler_bin.exists():
        log_err(f"Expected recompiler binary not found at {recompiler_bin}")
        sys.exit(1)
    log(f"Static recompiler ready: {recompiler_bin}")
    return recompiler_bin


def generate_freebios(recompiler_bin: Path) -> None:
    GENERATED_BIOS_DIR.mkdir(parents=True, exist_ok=True)
    freebios_files = [
        GENERATED_BIOS_DIR / "freebios_arm9.c",
        GENERATED_BIOS_DIR / "freebios_arm9_dispatch.c",
        GENERATED_BIOS_DIR / "freebios_arm7.c",
        GENERATED_BIOS_DIR / "freebios_arm7_dispatch.c",
        GENERATED_BIOS_DIR / "arm9_bios.c",
        GENERATED_BIOS_DIR / "arm7_bios.c",
    ]
    if all(p.exists() for p in freebios_files):
        log("FreeBIOS C dispatch banks already generated.")
        return

    log("Generating FreeBIOS C dispatch banks...")
    ndsrecomp_root = REPO_ROOT / "third_party" / "ndsrecomp"
    bios9_toml = ndsrecomp_root / "bios" / "freebios9.toml"
    bios7_toml = ndsrecomp_root / "bios" / "freebios7.toml"
    freebios9_bin = ndsrecomp_root / "third_party" / "freebios" / "drastic_bios_arm9.bin"
    freebios7_bin = ndsrecomp_root / "third_party" / "freebios" / "drastic_bios_arm7.bin"

    # ARM9 FreeBIOS (single unsharded translation unit expected by runner CMake)
    run_cmd([
        str(recompiler_bin),
        "--config", str(bios9_toml),
        "--bin", str(freebios9_bin),
        "--out", str(GENERATED_BIOS_DIR),
        "--bank", "freebios_arm9",
        "--shards", "1",
    ])
    # ARM7 FreeBIOS (single unsharded translation unit expected by runner CMake)
    run_cmd([
        str(recompiler_bin),
        "--config", str(bios7_toml),
        "--bin", str(freebios7_bin),
        "--out", str(GENERATED_BIOS_DIR),
        "--bank", "freebios_arm7",
        "--shards", "1",
    ])

    # Stub native BIOS dispatch files required by CMakeLists
    for name in ["arm9_bios", "arm7_bios"]:
        c_file = GENERATED_BIOS_DIR / f"{name}.c"
        disp_file = GENERATED_BIOS_DIR / f"{name}_dispatch.c"
        if not c_file.exists():
            c_file.write_text(f"// Clean-room stub for {name}\n#include <stddef.h>\n", encoding="utf-8")
        if not disp_file.exists():
            disp_file.write_text(
                f'#include <stddef.h>\n#include "runtime_arm.h"\n'
                f'const NdsDispatchEntry g_dispatch_{name}[] = {{}};\n'
                f'const unsigned g_dispatch_{name}_len = 0;\n',
                encoding="utf-8",
            )
    log("FreeBIOS generation complete.")


def find_rom(rom_arg: str | None) -> Path:
    if rom_arg:
        p = Path(rom_arg).expanduser().resolve()
        if p.exists():
            return p
        log_err(f"Specified ROM path does not exist: {p}")
        sys.exit(1)

    if DEFAULT_ROM_PATH.exists():
        return DEFAULT_ROM_PATH

    log_err(
        f"No ROM specified and default ROM not found at {DEFAULT_ROM_PATH}.\n"
        f"Please provide your legally owned Explorers of Sky ROM with --rom <path>"
    )
    sys.exit(1)


def compile_game_banks(rom_path: Path, forced_region: str | None = None) -> None:
    log(f"Verifying and extracting ROM: {rom_path}")
    rom_bytes = rom_path.read_bytes()
    sha1 = hashlib.sha1(rom_bytes).hexdigest()
    if sha1 == CLEAN_USA_SHA1:
        region = "NA"
    elif sha1 == CLEAN_EU_SHA1:
        region = "EU"
    elif forced_region:
        region = forced_region
    else:
        log_err(f"ROM SHA-1 mismatch! Not clean USA ({CLEAN_USA_SHA1}) or Europe ({CLEAN_EU_SHA1})")
        sys.exit(1)

    log(f"ROM SHA-1 verified: {sha1} (Region: {region})")
    prepare_script = REPO_ROOT / "tools" / "prepare_rom.py"
    run_cmd([sys.executable, str(prepare_script), "--rom", str(rom_path), "--compile"])


def build_runner(jobs: int, build_type: str, region_mode: str = "auto") -> Path:
    log(f"Building nds_runner ({build_type}, region={region_mode})...")
    RUNNER_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    runner_src_dir = REPO_ROOT / "third_party" / "ndsrecomp" / "runner"

    us_banks = BUILD_DIR / "generated_sky_us"
    if not us_banks.exists() and (BUILD_DIR / "generated_sky_na").exists():
        us_banks = BUILD_DIR / "generated_sky_na"
    eu_banks = BUILD_DIR / "generated_sky_eu"

    def has_banks(bank_dir: Path) -> bool:
        return bank_dir.exists() and any(bank_dir.glob("*_dispatch.c"))

    is_apple = platform.system() == "Darwin"
    cmake_args = [
        "cmake",
        "-S", str(runner_src_dir),
        "-B", str(RUNNER_BUILD_DIR),
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DNDS_BOOTSTRAP_FIRMWARE=OFF",
        f"-DNDS_ENABLE_COMPUTE_RENDERER={'OFF' if is_apple else 'ON'}",
        f"-DNDS_GENERATED_DIR={GENERATED_BIOS_DIR}",
    ]

    if region_mode == "dual" or (region_mode == "auto" and has_banks(us_banks) and has_banks(eu_banks)):
        log("Configuring dual-region runner (USA + Europe)...")
        cmake_args.extend([
            f"-DNDS_TITLE_BANK_DIRS={us_banks};{eu_banks}",
            f"-DNDS_TITLE_ROM_SHA1S={CLEAN_USA_SHA1};{CLEAN_EU_SHA1}",
        ])
    elif region_mode == "EU" or (not has_banks(us_banks) and has_banks(eu_banks)):
        log("Configuring Europe (EU) runner...")
        cmake_args.extend([
            f"-DNDS_TITLE_BANK_DIR={eu_banks}",
            f"-DNDS_TITLE_ROM_SHA1={CLEAN_EU_SHA1}",
        ])
    else:
        log("Configuring USA (NA) runner...")
        cmake_args.extend([
            f"-DNDS_TITLE_BANK_DIR={us_banks}",
            f"-DNDS_TITLE_ROM_SHA1={CLEAN_USA_SHA1}",
        ])
    if is_apple and Path("/opt/homebrew").exists():
        cmake_args.append("-DCMAKE_PREFIX_PATH=/opt/homebrew")

    run_cmd(cmake_args)
    run_cmd(["cmake", "--build", str(RUNNER_BUILD_DIR), "--target", "nds_runner", f"-j{jobs}"])

    exe_name = "nds_runner.exe" if sys.platform == "win32" else "nds_runner"
    runner_bin = RUNNER_BUILD_DIR / exe_name
    if not runner_bin.exists():
        log_err(f"Runner binary not found at {runner_bin}")
        sys.exit(1)

    log(f"Runner build SUCCESS: {runner_bin}")
    return runner_bin


def main() -> None:
    parser = argparse.ArgumentParser(description="SkyRecompiled Top-Level Build Orchestrator")
    parser.add_argument("--rom", type=str, default=None, help="Path to Explorers of Sky (USA) .nds ROM")
    parser.add_argument("--clean", action="store_true", help="Clean build directory before building")
    parser.add_argument("--build-type", choices=["Release", "Debug", "RelWithDebInfo"], default="Release")
    parser.add_argument("--jobs", "-j", type=int, default=multiprocessing.cpu_count(), help="Parallel build jobs")
    parser.add_argument("--region", choices=["NA", "EU", "dual", "auto"], default="auto", help="Cartridge region target")
    parser.add_argument("--skip-recompile", action="store_true", help="Skip prepare_rom.py if banks exist")
    parser.add_argument("--package", action="store_true", help="Package macOS SkyRecompiled.app bundle")
    args = parser.parse_args()

    if args.clean:
        log("Cleaning build directory...")
        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        log("Clean complete.")

    check_prerequisites()
    ensure_third_party()

    recompiler_bin = build_recompiler(args.jobs, args.build_type)
    generate_freebios(recompiler_bin)

    if not args.skip_recompile or not any(GENERATED_BANKS_DIR.glob("*_dispatch.c")):
        rom_path = find_rom(args.rom)
        compile_game_banks(rom_path, forced_region=args.region if args.region in ("NA", "EU") else None)
    else:
        log("Skipping ROM extraction (game banks already present).")

    runner_bin = build_runner(args.jobs, args.build_type, region_mode=args.region)

    if args.package or platform.system() == "Darwin":
        log("Packaging macOS application bundle...")
        package_script = REPO_ROOT / "scripts" / "package_macos_app.py"
        if package_script.exists():
            run_cmd([sys.executable, str(package_script)])
        else:
            log("Packaging script not yet created; skipping.")

    log("\033[1;32mSkyRecompiled build finished successfully!\033[0m")
    log(f"Executable: {runner_bin}")
    log("Interactive launch: ./scripts/run_sky.sh")


if __name__ == "__main__":
    main()

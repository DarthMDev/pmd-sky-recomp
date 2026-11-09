#!/usr/bin/env python3
"""
SkyRecompiled -- Top-Level Build Orchestrator
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
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
CMAKE_BUILD_DIR = BUILD_DIR / "cmake"
RECOMPILER_BUILD_DIR = BUILD_DIR / "recompiler"
RUNNER_BUILD_DIR = BUILD_DIR / "runner"
GENERATED_BIOS_DIR = BUILD_DIR / "generated_bios"
GENERATED_BANKS_DIR = BUILD_DIR / "generated_sky_us"
DEFAULT_ROM_PATH = REPO_ROOT / "roms" / "pmdsky.us.nds"
SDL3_WINDOWS_DLL = Path("x86_64-w64-mingw32/bin/SDL3.dll")
CLEAN_USA_SHA1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152"
CLEAN_EU_SHA1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139"

# Ensure Homebrew and standard tool paths are accessible
for extra_path in ["/opt/homebrew/bin", "/usr/local/bin"]:
    if Path(extra_path).exists() and extra_path not in os.environ.get("PATH", ""):
        os.environ["PATH"] = f"{extra_path}{os.pathsep}{os.environ.get('PATH', '')}"


def log(msg: str) -> None:
    """Format and print an informational build status message."""
    print(f"\033[1;36m[SkyRecompiled:build]\033[0m {msg}", flush=True)


def log_err(msg: str) -> None:
    """Format and print an error message to standard error."""
    print(f"\033[1;31m[SkyRecompiled:build ERROR]\033[0m {msg}", file=sys.stderr, flush=True)


def run_cmd(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    """Execute a subprocess command, exiting immediately on failure."""
    display_cmd = " ".join(cmd)
    log(f"Running: {display_cmd}")
    res = subprocess.run(cmd, cwd=cwd or REPO_ROOT, env=env)
    if res.returncode != 0:
        log_err(f"Command failed with exit code {res.returncode}: {display_cmd}")
        sys.exit(res.returncode)


def check_prerequisites() -> None:
    """Verify that required build tools are installed and available on PATH."""
    log("Checking prerequisites...")
    required_tools = ["cmake", "ninja"]
    if sys.platform == "win32":
        required_tools += ["gcc", "g++"]
    for tool in required_tools:
        if not shutil.which(tool):
            log_err(f"Missing required tool on PATH: '{tool}'. See CONTRIBUTING.md.")
            sys.exit(1)


def windows_compiler_args() -> list[str]:
    """Return CMake compiler and linker flags for MinGW-w64 on Windows."""
    if sys.platform != "win32":
        return []
    return [
        "-DCMAKE_C_COMPILER=gcc",
        "-DCMAKE_CXX_COMPILER=g++",
        # Bundle the compiler runtime in the executable; SDL3 is staged separately.
        "-DCMAKE_EXE_LINKER_FLAGS=-static -Wl,--stack,67108864",
    ]


def ensure_third_party() -> None:
    """Verify and update pinned dependencies before building."""
    bash = shutil.which("bash")
    if sys.platform == "win32":
        git_bash = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
        if git_bash.is_file():
            bash = str(git_bash)
    if not bash:
        log_err("Bash is required to verify third-party dependencies. See README.md.")
        sys.exit(1)
    env = {**os.environ, "SKY_THIRD_PARTY_DIR": str(REPO_ROOT / "third_party")}
    run_cmd([bash, str(REPO_ROOT / "scripts" / "setup_third_party.sh")], env=env)


def build_recompiler(jobs: int, build_type: str) -> Path:
    """Build the offline nds_recompile static recompiler executable."""
    log(f"Building nds_recompile ({build_type})...")
    RECOMPILER_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    exe_name = "nds_recompile.exe" if sys.platform == "win32" else "nds_recompile"
    recompiler_bin = RECOMPILER_BUILD_DIR / exe_name

    cmake_args = [
        "cmake",
        "-S", str(REPO_ROOT / "translator"),
        "-B", str(CMAKE_BUILD_DIR / "recompiler"),
        f"-DSKY_RECOMPILER_OUTPUT_DIR={RECOMPILER_BUILD_DIR}",
        "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        *windows_compiler_args(),
    ]
    run_cmd(cmake_args)
    run_cmd(["cmake", "--build", str(CMAKE_BUILD_DIR / "recompiler"), "--target", "nds_recompile", f"-j{jobs}"])

    if not recompiler_bin.exists():
        log_err(f"Expected recompiler binary not found at {recompiler_bin}")
        sys.exit(1)
    log(f"Static recompiler ready: {recompiler_bin}")
    return recompiler_bin


def generate_freebios(
    recompiler_bin: Path,
    ndsrecomp_root: Path = REPO_ROOT / "third_party" / "ndsrecomp",
    output_dir: Path = GENERATED_BIOS_DIR,
) -> None:
    """Generate FreeBIOS dispatch banks when their tool or inputs change."""
    output_dir.mkdir(parents=True, exist_ok=True)
    cpus = (9, 7)
    configs = [ndsrecomp_root / "bios" / f"freebios{cpu}.toml" for cpu in cpus]
    binaries = [
        ndsrecomp_root / "third_party" / "freebios" / f"drastic_bios_arm{cpu}.bin"
        for cpu in cpus
    ]
    generated = [
        f"freebios_arm{cpu}{suffix}"
        for cpu in cpus
        for suffix in (".c", ".h", "_dispatch.c")
    ]
    fingerprint = "\n".join(
        hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (recompiler_bin, *configs, *binaries)
    )
    stamp = output_dir / ".freebios-inputs"
    if (not stamp.is_file() or stamp.read_text() != fingerprint
            or not all((output_dir / name).is_file() for name in generated)):
        log("Generating FreeBIOS C dispatch banks...")
        with tempfile.TemporaryDirectory(dir=output_dir) as scratch:
            staging = Path(scratch)
            for cpu, config, binary in zip(cpus, configs, binaries):
                # Runner CMake expects a single unsharded translation unit per CPU.
                run_cmd([
                    str(recompiler_bin),
                    "--config", str(config),
                    "--bin", str(binary),
                    "--out", scratch,
                    "--bank", f"freebios_arm{cpu}",
                    "--shards", "1",
                ])
            stamp.unlink(missing_ok=True)
            for name in generated:
                (staging / name).replace(output_dir / name)
            (staging / stamp.name).write_text(fingerprint)
            (staging / stamp.name).replace(stamp)
        log("FreeBIOS generation complete.")
    else:
        log("FreeBIOS C dispatch banks already generated.")

    # Stub native BIOS dispatch files required by CMakeLists
    for name in ["arm9_bios", "arm7_bios"]:
        c_file = output_dir / f"{name}.c"
        disp_file = output_dir / f"{name}_dispatch.c"
        if not c_file.exists():
            c_file.write_text(f"// Clean-room stub for {name}\n#include <stddef.h>\n", encoding="utf-8")
        if not disp_file.exists():
            disp_file.write_text(
                f'#include <stddef.h>\n#include "runtime_arm.h"\n'
                f'const NdsDispatchEntry g_dispatch_{name}[1] = {{0}};\n'
                f'const unsigned g_dispatch_{name}_len = 0;\n',
                encoding="utf-8",
            )


def find_rom(rom_arg: str | None) -> Path:
    """Locate and return the path to the Explorers of Sky ROM image."""
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


def compile_game_banks(rom_path: Path, recompiler_bin: Path, forced_region: str | None = None) -> None:
    """Verify ROM checksum and trigger AOT recompilation of cartridge banks."""
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
    prepare_script = REPO_ROOT / "translator" / "prepare_rom.py"
    run_cmd([
        sys.executable, str(prepare_script), "--rom", str(rom_path),
        "--compile", "--recompiler", str(recompiler_bin),
    ])


def build_runner(
    jobs: int, build_type: str, region_mode: str = "auto",
    *, sdl3_sdk: Path | None = None, without_game_banks: bool = False,
) -> Path:
    """Configure and build the native nds_runner executable."""
    log(f"Building nds_runner ({build_type}, region={region_mode})...")
    runner_build_dir = BUILD_DIR / "runner-fallback" if without_game_banks else RUNNER_BUILD_DIR
    runner_cmake_dir = CMAKE_BUILD_DIR / runner_build_dir.name

    us_banks = BUILD_DIR / "generated_sky_us"
    if not us_banks.exists() and (BUILD_DIR / "generated_sky_na").exists():
        us_banks = BUILD_DIR / "generated_sky_na"
    eu_banks = BUILD_DIR / "generated_sky_eu"

    def has_banks(bank_dir: Path) -> bool:
        """Check whether the given directory contains generated dispatch banks."""
        return bank_dir.exists() and any(bank_dir.glob("*_dispatch.c"))

    is_apple = platform.system() == "Darwin"
    cmake_args = [
        "cmake",
        "-S", str(REPO_ROOT),
        "-B", str(runner_cmake_dir),
        f"-DSKY_RUNNER_OUTPUT_DIR={runner_build_dir}",
        "-G", "Ninja",
        # Clear stale bank settings before selecting the requested region.
        "-U", "NDS_TITLE_BANK_DIR*", "-U", "NDS_TITLE_ROM_SHA1*",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DNDS_BOOTSTRAP_FIRMWARE=ON",
        f"-DNDS_ENABLE_COMPUTE_RENDERER={'OFF' if is_apple or sys.platform == 'win32' else 'ON'}",
        f"-DNDS_GENERATED_DIR={GENERATED_BIOS_DIR}",
        f"-DNDS_RECOMP_UI_ROOT={REPO_ROOT / 'third_party' / 'recomp-ui'}",
        *windows_compiler_args(),
    ]

    if without_game_banks:
        log("Building a runner that uses runtime execution for the cartridge.")
    elif region_mode == "dual" or (region_mode == "auto" and has_banks(us_banks) and has_banks(eu_banks)):
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
    if sdl3_sdk:
        cmake_args.append(f"-DSDL3_DIR={sdl3_sdk / 'cmake'}")

    run_cmd(cmake_args)
    run_cmd(["cmake", "--build", str(runner_cmake_dir), "--target", "nds_runner", f"-j{jobs}"])

    exe_name = "nds_runner.exe" if sys.platform == "win32" else "nds_runner"
    runner_bin = runner_build_dir / exe_name
    if not runner_bin.exists():
        log_err(f"Runner binary not found at {runner_bin}")
        sys.exit(1)

    if sdl3_sdk:
        shutil.copy2(sdl3_sdk / SDL3_WINDOWS_DLL, runner_build_dir)
        shutil.copy2(sdl3_sdk / "LICENSE.txt", runner_build_dir / "SDL3-LICENSE.txt")
    log(f"Runner build SUCCESS: {runner_bin}")
    return runner_bin


def main() -> None:
    """Parse command-line arguments and run the SkyRecompiled build orchestration pipeline."""
    parser = argparse.ArgumentParser(description="SkyRecompiled Top-Level Build Orchestrator")
    parser.add_argument("--rom", type=str, default=None, help="Path to Explorers of Sky (USA) .nds ROM")
    parser.add_argument("--clean", action="store_true", help="Clean build directory before building")
    parser.add_argument("--build-type", choices=["Release", "Debug", "RelWithDebInfo"], default="Release")
    parser.add_argument("--jobs", "-j", type=int, default=multiprocessing.cpu_count(), help="Parallel build jobs")
    parser.add_argument("--region", choices=["NA", "EU", "dual", "auto"], default="auto", help="Cartridge region target")
    parser.add_argument("--skip-recompile", action="store_true", help="Skip prepare_rom.py if banks exist")
    parser.add_argument("--sdl3-sdk", type=Path, help="Windows: extracted SDL3 MinGW development SDK folder")
    parser.add_argument("--without-game-banks", action="store_true", help="Build in build/runner-fallback without a ROM or statically compiled game banks")
    parser.add_argument("--package", action="store_true", help="Package macOS SkyRecompiled.app bundle")
    args = parser.parse_args()
    if sys.platform == "win32":
        if args.sdl3_sdk is None:
            parser.error("Windows builds require --sdl3-sdk pointing to the extracted SDL3 MinGW SDK.")
        args.sdl3_sdk = args.sdl3_sdk.resolve()
        for relative in ("cmake/SDL3Config.cmake", SDL3_WINDOWS_DLL, "LICENSE.txt"):
            if not (args.sdl3_sdk / relative).is_file():
                parser.error(f"SDL3 MinGW SDK file missing: {args.sdl3_sdk / relative}")
    elif args.sdl3_sdk is not None:
        parser.error("--sdl3-sdk is only used on Windows.")

    if args.clean:
        log("Cleaning build directory...")
        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        log("Clean complete.")

    check_prerequisites()
    ensure_third_party()

    recompiler_bin = build_recompiler(args.jobs, args.build_type)
    generate_freebios(recompiler_bin)

    if not args.without_game_banks:
        if args.skip_recompile and any(GENERATED_BANKS_DIR.glob("*_dispatch.c")):
            log("Skipping ROM extraction (game banks already present).")
        else:
            rom_path = find_rom(args.rom)
            compile_game_banks(rom_path, recompiler_bin, forced_region=args.region if args.region in ("NA", "EU") else None)

    runner_bin = build_runner(
        args.jobs, args.build_type, region_mode=args.region,
        sdl3_sdk=args.sdl3_sdk, without_game_banks=args.without_game_banks,
    )

    if not args.without_game_banks and (args.package or platform.system() == "Darwin"):
        log("Packaging macOS application bundle...")
        package_script = REPO_ROOT / "scripts" / "package_macos_app.py"
        if package_script.exists():
            run_cmd([sys.executable, str(package_script)])
        else:
            log("Packaging script not yet created; skipping.")

    log("\033[1;32mSkyRecompiled build finished successfully!\033[0m")
    log(f"Executable: {runner_bin}")
    if args.without_game_banks:
        log(f'Launch check: "{runner_bin}" --help')
    elif sys.platform == "win32":
        log("Interactive launch from PowerShell: python scripts/launcher.py")
    else:
        log("Interactive launch: ./scripts/run_sky.sh")


if __name__ == "__main__":
    main()

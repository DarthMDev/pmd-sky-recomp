#!/usr/bin/env python3
"""Build the ROM-free iOS alpha with Xcode's iPhone SDK."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from build import BUILD_DIR, REPO_ROOT, generate_freebios, log, run_cmd

IOS_DIR = BUILD_DIR / "ios"
SOURCE_DIR = REPO_ROOT / "src" / "ios"
SDL_VERSION = "3.4.14"
SDL_SHA256 = "9d57b178fb297e121ef2605275937b7afaa7cd24d99ce1f95953e69e7a2535d6"
SDL_URL = f"https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-{SDL_VERSION}"
IPA = IOS_DIR / "SkyRecompiled-iOS-alpha.ipa"


def setup_dependencies(deps: Path) -> tuple[Path, Path, Path]:
    ndsrecomp = deps / "ndsrecomp"
    ui = deps / "recomp-ui"
    if not (deps / ".bootstrapped").exists():
        env = {**os.environ, "SKY_THIRD_PARTY_DIR": str(deps)}
        run_cmd(["bash", str(REPO_ROOT / "scripts" / "setup_third_party.sh")], env=env)
        (deps / ".bootstrapped").touch()

    apply = ["git", "-C", str(ndsrecomp), "apply"]
    for name in ("ndsrecomp-ios.patch", "ndsrecomp-ios-touch.patch"):
        patch = REPO_ROOT / "patches" / name
        if subprocess.run([*apply, "--reverse", "--check", str(patch)],
                          capture_output=True).returncode != 0:
            run_cmd([*apply, str(patch)])

    sdl = deps / f"SDL-release-{SDL_VERSION}"
    if not (sdl / "CMakeLists.txt").is_file():
        with tempfile.TemporaryDirectory(dir=deps) as scratch:
            staging = Path(scratch)
            archive = staging / "sdl.tar.gz"
            run_cmd(["curl", "-fsSL", "--retry", "3", SDL_URL, "-o", str(archive)])
            if hashlib.sha256(archive.read_bytes()).hexdigest() != SDL_SHA256:
                raise SystemExit("SDL archive SHA-256 mismatch")
            run_cmd(["tar", "-xzf", str(archive), "-C", scratch])
            (staging / sdl.name).rename(sdl)
    return ndsrecomp, ui, sdl


def stage_resources(app: Path, ndsrecomp: Path, ui: Path, sdl: Path) -> None:
    shutil.copy2(REPO_ROOT / "config" / "game.toml", app / "game.toml")
    licenses = app / "Licenses"
    licenses.mkdir(exist_ok=True)
    for name, source in {
        "SkyRecompiled.txt": REPO_ROOT / "LICENSE",
        "ndsrecomp.txt": ndsrecomp / "LICENSE",
        "ndsrecomp-GPL.txt": ndsrecomp / "LICENSE-GPL",
        "ndsrecomp-MIT.txt": ndsrecomp / "LICENSE-MIT",
        "ndsrecomp-attribution.txt": ndsrecomp / "THIRD_PARTY_ATTRIBUTION.md",
        "arm-recomp-core.txt": ndsrecomp / "external" / "arm-recomp-core" / "LICENSE",
        "FreeBIOS.txt": ndsrecomp / "third_party" / "freebios" / "LICENSE.txt",
        "libslirp.txt": ndsrecomp / "runner" / "vendor" / "melonds" / "net" / "libslirp" / "COPYRIGHT",
        "recomp-ui.txt": ui / "LICENSE",
        "SDL.txt": sdl / "LICENSE.txt",
    }.items():
        shutil.copy2(source, licenses / name)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", action="store_true", help="Build an arm64 simulator .app")
    args = parser.parse_args()
    if os.uname().sysname != "Darwin":
        parser.error("macOS with Xcode is required")

    sdk = "iphonesimulator" if args.simulator else "iphoneos"
    run_cmd(["xcrun", "--sdk", sdk, "--show-sdk-path"])
    IOS_DIR.mkdir(parents=True, exist_ok=True)
    ndsrecomp, ui, sdl = setup_dependencies(IOS_DIR / "deps")

    host = IOS_DIR / "host"
    run_cmd([
        "cmake", "-S", str(ndsrecomp / "recompiler"), "-B", str(host), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_SYSROOT=macosx",
    ])
    run_cmd(["cmake", "--build", str(host), "--target", "nds_recompile"])
    test = host / "ios_rom_compiler_test"
    run_cmd([
        "xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
        "-I", str(SOURCE_DIR),
        "-I", str(ndsrecomp / "external" / "arm-recomp-core" / "common"),
        str(REPO_ROOT / "tests" / "ios_rom_compiler_test.cpp"),
        str(SOURCE_DIR / "RomCompiler.cpp"), str(host / "libnds_armv4t.a"),
        "-o", str(test),
    ])
    run_cmd([str(test)])
    touch_test = host / "ios_touch_input_test"
    run_cmd([
        "xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
        "-I", str(SOURCE_DIR), str(REPO_ROOT / "tests" / "ios_touch_input_test.cpp"),
        "-o", str(touch_test),
    ])
    run_cmd([str(touch_test)])
    bios = IOS_DIR / "freebios"
    generate_freebios(host / "nds_recompile", ndsrecomp, bios)

    target = IOS_DIR / ("simulator" if args.simulator else "device")
    run_cmd([
        "cmake", "-S", str(SOURCE_DIR), "-B", str(target), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=iOS",
        f"-DCMAKE_OSX_SYSROOT={sdk}", "-DCMAKE_OSX_ARCHITECTURES=arm64",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0",
        f"-DSKY_NDSRECOMP_DIR={ndsrecomp}", f"-DSKY_SDL_DIR={sdl}",
        f"-DNDS_RECOMP_UI_ROOT={ui}", f"-DNDS_GENERATED_DIR={bios}",
    ])
    run_cmd(["cmake", "--build", str(target), "--target", "nds_runner"])
    app = target / "SkyRecompiled.app"
    if args.simulator:
        stage_resources(app, ndsrecomp, ui, sdl)
        log(f"Simulator app: {app}")
        return

    # Package only the executable and known resources from this build.
    with tempfile.TemporaryDirectory(dir=IOS_DIR) as scratch:
        staged = Path(scratch) / "Payload" / app.name
        staged.mkdir(parents=True)
        for name in ("SkyRecompiled", "Info.plist"):
            shutil.copy2(app / name, staged / name)
        stage_resources(staged, ndsrecomp, ui, sdl)
        archive = Path(scratch) / IPA.name
        run_cmd(["ditto", "-c", "-k", "--norsrc", "--keepParent", str(staged.parent), str(archive)])
        run_cmd(["unzip", "-tq", str(archive)])
        archive.replace(IPA)
    log(f"IPA for re-signing: {IPA}")


if __name__ == "__main__":
    main()

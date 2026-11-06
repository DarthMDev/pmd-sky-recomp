# SkyRecompiled

[![Latest Release](https://img.shields.io/github/v/release/DarthMDev/pmd-sky-recomp?label=Latest%20Release&color=blue)](https://github.com/DarthMDev/pmd-sky-recomp/releases/latest)
[![macOS Download](https://img.shields.io/badge/Download-macOS%20Apple%20Silicon-brightgreen?logo=apple)](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-macos-arm64.zip)
[![Windows Download](https://img.shields.io/badge/Download-Windows%20x64%20(Alpha)-blue?logo=windows)](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-windows-x64.zip)
[![Linux Download](https://img.shields.io/badge/Download-Linux%20x86__64%20(Alpha)-orange?logo=linux)](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-linux-x86_64.tar.gz)
[![CI Status](https://img.shields.io/github/actions/workflow/status/DarthMDev/pmd-sky-recomp/ci.yml?branch=main&label=CI)](https://github.com/DarthMDev/pmd-sky-recomp/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?logo=discord&logoColor=white)](https://discord.gg/neWFpX9ps7)

**SkyRecompiled** is a native ahead-of-time (AOT) static recompilation PC port of *Pokémon Mystery Dungeon: Explorers of Sky*, built on [`ndsrecomp`](https://github.com/RetroPortingToolKit/ndsrecomp) and [`pmdsky-debug`](https://github.com/UsernameFodder/pmdsky-debug).

### Downloads (v0.1.0 Alpha)

| Platform | Build Status | Package |
| :--- | :--- | :--- |
| **macOS Apple Silicon (`arm64`)** | Verified Working | [SkyRecompiled-v0.1.0-alpha-macos-arm64.zip](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-macos-arm64.zip) |
| **Windows (`x86_64`)** | Untested Alpha (Testers Needed!) | [SkyRecompiled-v0.1.0-alpha-windows-x64.zip](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-windows-x64.zip) |
| **Linux (`x86_64`)** | Untested Alpha (Testers Needed!) | [SkyRecompiled-v0.1.0-alpha-linux-x86_64.tar.gz](https://github.com/DarthMDev/pmd-sky-recomp/releases/download/v0.1.0-alpha/SkyRecompiled-v0.1.0-alpha-linux-x86_64.tar.gz) |

> **Clean-Room & Legal Notice:** This repository contains **zero** copyrighted Nintendo DS ROMs, extracted assets, BIOS/firmware dumps, or ROM-derived C code. Players must supply their own legally dumped *Pokémon Mystery Dungeon: Explorers of Sky* `.nds` cartridge image. No Nintendo DS BIOS or firmware files are required (`FreeBIOS` + synthesized NVRAM direct boot).

---

## Features & Roadmap

- **Milestone 1 (Pre-Alpha -- Active):** Native **macOS Apple Silicon (`arm64`)** static recompilation of all **34 executable modules** (`arm9`, `itcm`, `arm7` + WRAM/RAM drivers, and all 32 active overlays -- `91,872` recompiled functions), BIOS-free direct boot, 128 KiB FRAM/EEPROM saves, savestates, audio, and controller support.
- **Milestone 2:** Native **Windows (`x86_64`)** and **Linux (`x86_64` / `arm64`)** builds & packaging.
- **Milestone 3:** **Beyond-Emulator Graphics & Smoothness** -- True `16:9` / `21:9` native 2D widescreen (`overlay29` & `overlay11` camera/tile hooks + anchored HUD), **120Hz / 144Hz / 240Hz** sub-pixel camera & sprite interpolation, and custom Apple Metal / `SDL_GPU` pixel-art & weather shaders.
- **Milestone 4:** **Launcher, SkyTemple Mod Loader & Online Rescue Board** -- Zero-recompile layered NitroFS mod overrides, SkyTemple custom ASM hack recompilation, and an asynchronous *Rescue Team DX*-style Online Rescue Bulletin Board backed by vanilla **Wonder Mail S / SOS / A-OK Mail** structs (with 54-character passwords preserved forever as an offline fallback).

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full technical specification.

---

## Quickstart (macOS Apple Silicon)

### Prerequisites
```bash
brew install cmake ninja sdl3 python
```

### 1. Bootstrap Third-Party Dependencies
```bash
./scripts/setup_third_party.sh
```

### 2. Build the Static Recompiler & FreeBIOS Banks
```bash
cmake -S third_party/ndsrecomp/recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler
```

### 3. Extract & Recompile Your Legally Owned USA ROM
Verify your clean No-Intro USA ROM (`SHA-1: 5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152`):
```bash
python3 tools/prepare_rom.py --rom "/path/to/Pokemon Mystery Dungeon - Explorers of Sky (USA).nds" --compile
```

### 4. Build & Launch SkyRecompiled
```bash
cmake -S third_party/ndsrecomp/runner -B build/runner -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DNDS_ENABLE_COMPUTE_RENDERER=OFF \
  -DNDS_GENERATED_DIR="$PWD/build/generated_bios" \
  -DNDS_TITLE_BANK_DIR="$PWD/build/generated_sky_us" \
  -DNDS_TITLE_ROM_SHA1=5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152
cmake --build build/runner --target nds_runner

./build/runner/nds_runner build/console_state --interactive \
  --config config/game.toml \
  --rom "/path/to/Pokemon Mystery Dungeon - Explorers of Sky (USA).nds"
```

---

## iPhone and iPad alpha

Build on a Mac with Xcode, CMake, Ninja, and Python 3:

```bash
python3 scripts/build_ios.py
```

The build writes `build/ios/SkyRecompiled-iOS-alpha.ipa`. The **iOS alpha IPA**
GitHub Actions workflow builds the same artifact without signing secrets or a ROM.
Download and extract the workflow artifact to get the IPA.

1. Set up AltStore Classic on your iPhone or iPad and enable Developer Mode on
   iOS or iPadOS 16 and later. [AltStore setup for macOS](https://faq.altstore.io/altstore-classic/how-to-install-altstore-macos)
2. Save the IPA to Files. In AltStore, open **My Apps**, tap **+**, and select
   the IPA. AltStore signs it for your device. Refresh it before the signing
   expires. [AltStore signing and refresh](https://faq.altstore.io/altstore-classic/your-altstore)
3. Open SkyRecompiled, choose **Import ROM from Files**, and select your own
   clean USA or Europe Explorers of Sky `.nds` dump. Tap **Compile and Play**.
   Use the on-screen controls or connect a controller. Tap **Launcher** to exit
   the game and flush pending saves.
4. In **Files → On My iPhone/iPad → SkyRecompiled**, find `Logs/latest.log`,
   `Saves/pmdsky.sav`, and `settings.plist`.

The iOS app reuses the desktop runner and compiles ROM instructions into portable
intermediate code on the device. It can run without JIT signing privileges.
This alpha uses the runner's interpreter for that code, so performance may
differ from the native desktop build.

For Apple silicon Mac testing, build with `python3 scripts/build_ios.py --simulator`
and install `build/ios/simulator/SkyRecompiled.app` on a booted iOS Simulator.
Running the iOS build directly on an Apple silicon Mac requires Apple development
or distribution signing. [Apple silicon iOS app testing](https://developer.apple.com/documentation/apple-silicon/running-your-ios-apps-in-macos)

---

## AI Usage & Verification Disclaimer

AI coding tools were used during the development of this project. All translated and recompiled output is rigorously verified against real Nintendo DS hardware and reference emulator behavior:
- **100% Executable Coverage:** All 34 executable modules (`arm9`, `itcm`, `arm7` + WRAM/RAM drivers, and all 32 active overlays -- 91,872 functions) compile natively with zero unimplemented instructions.
- **Hardware-Synced Logic:** Over 99.9% of guest instructions execute through native static C code with zero RAM code rejects. Game logic, turn execution, deterministic RNG, and cartridge FRAM/EEPROM saves are verified against hardware behavior and melonDS references via automated test suites.

---

## Credits & Acknowledgments

- [`RetroPortingToolKit/ndsrecomp`](https://github.com/RetroPortingToolKit/ndsrecomp) & [`recomp-ui`](https://github.com/RetroPortingToolKit/recomp-ui) -- Nintendo DS static recompilation framework & runtime
- [`UsernameFodder/pmdsky-debug`](https://github.com/UsernameFodder/pmdsky-debug) -- *Explorers of Sky* symbol tables & C headers
- [`pret/pmd-sky`](https://github.com/pret/pmd-sky) -- Decompilation reference for game engine structures
- `FreeBIOS` (`BSD-2-Clause`) & `melonDS` (`GPL-3.0-or-later`)

---

## License

SkyRecompiled is licensed under the **GNU General Public License v3.0 or later** ([`LICENSE`](LICENSE)). Integrated third-party components are acknowledged under their respective licenses (`FreeBIOS` under BSD-2-Clause, `melonDS` under GPL-3.0-or-later, `pmdsky-debug` under MIT/GPL-3.0, and `ndsrecomp` under PolyForm Noncommercial 1.0.0).

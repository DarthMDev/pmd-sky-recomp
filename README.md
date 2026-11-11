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

### Opening and using in-game settings

Press **F1** during gameplay, or press **Guide** on a controller. You can also
open settings with **Select+Start**. On iPhone or iPad, tap the **⚙** button in
the on-screen controls. The settings open on the category list:
**Video**, **Controller**, **Keyboard**, and **System**. Use the arrow keys or
D-pad to choose a category and press **Enter** or **A** to open it. Press
**Escape** or **B/Back** to return to the category list; press it again to close
settings.

In **Controller** or **Keyboard**, select an action such as **A** or **Up** and
press **Enter** or **A**. Then press the controller button, move the stick, or
press the keyboard key you want to assign. **Backspace** clears a binding;
**Escape** or **Guide** cancels assignment. Each page has a **Restore defaults**
action. On iOS, a connected keyboard or controller can assign inputs; the touch
controls remain available on screen.

### Can I play a European ROM in another language?

The European cartridge includes English, French, German, Italian, and Spanish.
SkyRecompiled's generated firmware currently selects English, so the European
game starts in English. A **Game language** setting is planned for Explorers
of Sky’s own options menu. It will show the languages available on your
cartridge and apply your choice on the next launch. The USA release contains
English game text.

---

## Quickstart (macOS Apple Silicon)

### Prerequisites
```bash
brew install cmake ninja sdl3 python
```

### Build and launch

Supply a clean USA or Europe ROM. Bootstrap the pinned tools, compile the game,
and package the macOS app:

```bash
./scripts/setup_third_party.sh
python3 scripts/build.py --rom "/path/to/Explorers of Sky.nds" --package
open build/SkyRecompiled.app
```

For the offline translator and runtime build layout, see [Architecture](ARCHITECTURE.md#build-layout).

---

## Quickstart (Windows x64)

You can download the pre-packaged Windows release above (which runs cartridge code at runtime) or build the runner locally with your own clean *Explorers of Sky* USA or Europe ROM to include statically recompiled game code.

Install Python 3.12+, Git for Windows (with Git Bash available on PATH), and [MSYS2](https://www.msys2.org/). In the **MSYS2 MINGW64** shell, install the build tools:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
```

Extract **`SDL3-devel-3.4.14-mingw.zip`** from the [SDL3 release page](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.14). In PowerShell, run these commands from the repository root. Adjust the paths to match your installation and ROM.

```powershell
& "${env:ProgramFiles}\Git\bin\bash.exe" scripts/setup_third_party.sh
$env:Path = "C:\msys64\mingw64\bin;$env:Path"
python scripts/build.py --sdl3-sdk "C:\path\to\SDL3-3.4.14" --rom "C:\path\to\Explorers of Sky.nds"
python scripts/launcher.py --rom "C:\path\to\Explorers of Sky.nds"
```

After the build succeeds, `& .\build\runner\nds_runner.exe --help` checks that the runner starts. To create a local ZIP, run `python scripts/package_release.py --platform windows`. Extract the whole ZIP before opening `SkyRecompiled.bat`.

---

## iPhone and iPad alpha

Download the pre-built `SkyRecompiled-v0.1.0-alpha-ios.ipa` from the [latest GitHub Releases](https://github.com/DarthMDev/pmd-sky-recomp/releases/tag/v0.1.0-alpha), or build from source on a Mac with Xcode, CMake, Ninja, and Python 3:

```bash
python3 scripts/build_ios.py
```

The build writes `build/ios/SkyRecompiled-iOS-alpha.ipa` (or package for distribution with `python3 scripts/package_release.py --platform ios`). The **iOS alpha IPA**
GitHub Actions workflow builds and verifies this same artifact on push to `main` and pull requests.

1. SkyRecompiled requires iOS or iPadOS 16.0 or later. Set up AltStore Classic
   on your iPhone or iPad and enable Developer Mode. [AltStore setup for macOS](https://faq.altstore.io/altstore-classic/how-to-install-altstore-macos)
2. Save the IPA to Files. In AltStore, open **My Apps**, tap **+**, and select
   the IPA. AltStore signs it for your device. Refresh it before the signing
   expires. [AltStore signing and refresh](https://faq.altstore.io/altstore-classic/your-altstore)
3. Open SkyRecompiled, choose **Import ROM from Files**, and select your own
   clean USA or Europe Explorers of Sky `.nds` dump. Tap **Compile and Play**.
   Use the on-screen controls or connect a controller. Tap **Launcher** to exit
   the game and flush pending saves.
4. In **Files → On My iPhone/iPad → SkyRecompiled**, find `Logs/latest.log`
   after launching the app and `Saves/pmdsky.sav` after the game creates a save.
   The iOS software renderer uses the native 1× resolution.

The iOS app reuses the desktop runner and compiles ROM instructions into portable
intermediate code on the device. It can run without JIT signing privileges.
This alpha uses the runner's interpreter for that code, so performance may
differ from the native desktop build.

For Apple silicon Mac testing, boot an iPhone or iPad in Simulator, then run
these commands from the repository root. Simulator sets up the runtime the
app needs at launch.

```bash
python3 scripts/build_ios.py --simulator
xcrun simctl install booted build/ios/simulator/SkyRecompiled.app
xcrun simctl launch booted dev.skyrecompiled.ios
```

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
- [`Asphaltian/pmd-space`](https://github.com/Asphaltian/pmd-space) (*Explorers of Space*, `GPL-3.0-only`) -- Source for adapted PMD Sky widescreen code (3D dungeon darkness bands, full-screen fade rectangles, margin sprite culling reaches, and off-screen cutscene actor handling)
- `FreeBIOS` (`BSD-2-Clause`) & `melonDS` (`GPL-3.0-or-later`)

---

## License

The combined SkyRecompiled work, including its linked `nds_runner`, is distributed under the **GNU General Public License v3.0** ([`LICENSE`](LICENSE)). Code adapted from `Asphaltian/pmd-space` is available under its upstream GPL-3.0-only grant. Independently licensed code retains its original terms, including the GPL-3.0-or-later grant for independently licensed SkyRecompiled code. Integrated third-party components retain their respective licenses (`FreeBIOS` under BSD-2-Clause, `melonDS` under GPL-3.0-or-later, `pmdsky-debug` under MIT/GPL-3.0).

At the pinned `ndsrecomp` revision, PolyForm Noncommercial 1.0.0 is the default for upstream project code. The upstream license grants MIT terms for the original runtime code linked into `nds_runner` and GPL-3.0-or-later terms for its melonDS-derived code. Separately licensed dependencies retain their own terms. See the pinned [upstream licensing notice](https://github.com/RetroPortingToolKit/ndsrecomp/blob/54967a4e26bcd6ad46445d51bab3214decc3fd59/README.md#copyright-and-licensing).

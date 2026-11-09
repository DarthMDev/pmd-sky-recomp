# SkyRecompiled -- Architecture & Engineering Specification

**SkyRecompiled** is an ahead-of-time (AOT) static recompilation PC port of *Pokémon Mystery Dungeon: Explorers of Sky* (`C2SE` / `C2SP`), inspired by *Mario Kart WiiCompiled* and *Zelda 64: Recompiled*.

Players supply **only** their legally owned *Explorers of Sky* `.nds` ROM--no proprietary Nintendo DS BIOS or firmware dumps are required, and zero Nintendo copyrighted assets or ROM-derived C sources are ever stored in Git.

---

## 1. Why Static Recompilation (`ndsrecomp` + `pmdsky-debug`) Instead of Decomp Source Port?

We evaluated [`pret/pmd-sky`](https://github.com/pret/pmd-sky) alongside [`RetroPortingToolKit/ndsrecomp`](https://github.com/RetroPortingToolKit/ndsrecomp) and [`UsernameFodder/pmdsky-debug`](https://github.com/UsernameFodder/pmdsky-debug):

- `pret/pmd-sky` is a matching 32-bit Metrowerks CodeWarrior (`mwccarm`) decompilation targeting byte-identical DS ROM output, with substantial portions of `arm9` and the 32 active overlays still in raw ARM assembly or pointer-size-dependent 32-bit structs. Converting 75,000+ functions across 34 overlapping 32-bit modules to pointer-safe 64-bit host C would take years before booting.
- **Static Recompilation** (`ndsrecomp` + `pmdsky-debug` symbols) translates 100% of `arm9`, `arm7`, `ITCM`, `ARM7 WRAM`, and all **32 active overlays** (`91,872` functions, `0` unimplemented instructions) directly into native `arm64`/`x86_64` C shards while keeping an authoritative 32-bit guest address space (`0x02000000..0x023FFFFF`) where **every `pmdsky-debug` symbol address, struct offset, and SkyTemple mod works natively**.
- We still use `pmdsky-debug` C headers (`#define PMDSKY_UNSIZED_HEADERS`) and `pret/pmd-sky` decompiled C logic as exact behavioral specifications for native C++ hooks (widescreen camera/culling, 120Hz+ sub-pixel interpolation, and Wonder Mail S online rescue).

---

## 2. Supported Base ROMs & Clean-Room Verification

| Region | File / No-Intro Name | Size (Bytes) | SHA-1 | SHA-256 | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **USA (`NA`)** | `Pokemon Mystery Dungeon - Explorers of Sky (USA).nds` | `134,217,728` | `5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152` | `91161cb227c44a3e79fa2a622060385815f565647357062d7887f13f49d591e2` | **Primary Alpha Target** |
| **Europe (`EU`)** | `Pokemon Mystery Dungeon - Explorers of Sky (Europe) (En,Fr,De,Es,It).nds` | `134,217,728` | `c838a5adf1ed32d2da8454976e5b1a1aa189c139` | `1fa39d35873b58e02f3623438414c334ad93b840651a8a9ac13ee3c789f170c1` | **Modular Multi-Region Ready** |

> **Warning:** Scene-modified dumps such as `4273 - ... (US)(XenoPhobia).nds` (`SHA-1: 7efb50f925f0b483a66f2a266524173a8a25c4f0`) have altered headers/intro patches and are intentionally rejected by default (`translator/prepare_rom.py`). Custom SkyTemple ROM hacks are supported via `--allow-custom-rom --region NA|EU`.

---

## 3. Binary Memory Map & Recompilation Invariants

### 3.1 Static Modules & Runtime Relocations (`NA`)

| Bank Name | CPU | Source Image | Runtime Execution Range(s) | Functions | Shards | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `sky_arm9_main` | ARM9 | `0x02000000` (`0xB73F8` B) | `0x02000000..0x020B3380` + ITCM `0x01FF8000..0x01FFC000` | `22,949` | `16` | `do_autoload` copies `0x020B3380..0x020B7380` to ITCM `0x01FF8000`, then clears `0x020B3380..0x022BCA80` as BSS |
| `sky_arm7_main` | ARM7 | `0x02380000` (`0x27080` B) | `0x02380000..0x023801E8` + WRAM `0x037F8000` (`0xF608` B) + RAM `0x027E0000` (`0x17878` B) | `5,875` | `4` | `do_autoload_arm7` relocates sound/SPI/Wi-Fi drivers to WRAM & Main RAM mirror |
| `sky_arm9_ov00` | ARM9 | `0x022BCA80` (`0x609A0` B) | `0x022BCA80..0x0231D420` | `16,500` | `12` | Top menu, script engine primitives, DSE/UI helpers |
| `sky_arm9_ov01` | ARM9 | `0x02329520` (`0x12D20` B) | `0x02329520..0x0233C240` | `1,934` | `2` | Top menu main loop & Wonder Mail S menus |
| `sky_arm9_ov02` | ARM9 | `0x02329520` (`0x2AFA0` B) | `0x02329520..0x023544C0` | `8,032` | `4` | Nintendo WFC / Wi-Fi stack (ARM + Thumb) |
| `sky_arm9_ov03..09` | ARM9 | `0x0233CA80` (`0x2200..0xA160` B) | `0x0233CA80..` | `252..1,668` | `2` ea | Friend Rescue, Trade Items/Team, Sky Jukebox, Wonder Mail submenus |
| `sky_arm9_ov10` | ARM9 | `0x022BCA80` (`0x1F7A0` B) | `0x022BCA80..0x022DC220` | `1,197` | `4` | Ground/Dungeon shared UI & portrait rendering |
| `sky_arm9_ov11` | ARM9 | `0x022DC240` (`0x48C40` B) | `0x022DC240..0x02324E80` | `6,405` | `8` | **Ground Engine** (overworld, cutscenes, Treasure Town, guild) |
| `sky_arm9_ov13..28` | ARM9 | `0x0238A140` (`0xC60..0x4B40` B) | `0x0238A140..` | `31..391` | `2` ea | Personality Quiz (`ov13`), Sentry Duty (`ov14`), Bank/Shops/Cafe/Storage/Credits (`ov15..28`) |
| `sky_arm9_ov29` | ARM9 | `0x022DC240` (`0x77620` B) | `0x022DC240..0x02353860` | `20,476` | `12` | **Dungeon Engine** (floor gen, turn loop, AI, move/item effects, camera/tiles) |
| `sky_arm9_ov30..31` | ARM9 | `0x02382820` (`0x38A0..0x7A80` B) | `0x02382820..` | `784..1,438` | `2` ea | Dungeon quicksave & dungeon pause/inventory menus |
| `sky_arm9_ov34` | ARM9 | `0x022DC240` (`0x00E60` B) | `0x022DC240..0x022DD0A0` | `204` | `2` | Initial boot / game-state router overlay |

*(Overlays `12`, `32`, `33`, and `35` are unused 32-byte zero stubs in vanilla EoS and are automatically skipped.)*

### 3.2 Critical Invariants (Do Not Violate!)

1. **Cartridge Backup Save Type (`eeprom`, `131072` bytes):**
   - *Explorers of Sky* uses a **128 KiB FRAM/EEPROM** backup chip speaking 3-byte SPI `0x02`/`0x03`.
   - In `config/game.toml`, **always** keep `save_type = "eeprom"` and `save_size = 131072`. Never set `save_type = "flash"` (`cart_backup.cpp` writes `0` on SPI `0x02` for Flash, causing `CardBackupError` at `0x02003D94`).
2. **4-Page (`16 KiB`) Live Validation Cap (`runtime_arm.cpp:449`):**
   - `CachedStaticLookup` tracks at most `4` 4 KiB pages per static function. Any function spanning $\ge 5$ pages is rejected at runtime and falls back to the Tier-3 interpreter.
   - **Always** recompile banks with `--validate-live-bytes --max-function-bytes 8192 --coalesce-fallthroughs --stable-address-shards` and `--shards >= 2`. **Never** pass `--validated-live-direct-calls` on multi-page AOT banks.
3. **Bank Naming Convention (`runner/CMakeLists.txt`):**
   - Every bank name MUST contain `_arm9_` or `_arm7_` (`sky_arm9_*` / `sky_arm7_*`) so `runner/CMakeLists.txt` assigns `NDS_STATIC_CPU=0` (ARM9) or `NDS_STATIC_CPU=1` (ARM7), and `--shards` MUST be $\ge 2$ so `*_[0-9][0-9].c` globs match.
4. **BIOS-Free Direct Boot:**
   - Enabled via `boot = "direct"`, `freebios = true`, `generated_firmware = true` in `config/game.toml`. Uses BSD-2-Clause `FreeBIOS` + synthesized NVRAM firmware settings.

---

## 4. Roadmap & Subsystem Architecture

### Milestone 1: macOS Apple Silicon (`arm64`) Core Port (First Alpha)
- Full 34-bank static recompilation (`translator/prepare_rom.py`), macOS `arm64` native runner (`SDL3` Metal presentation), BIOS-free direct boot, 128 KiB EEPROM saves, savestates, audio, gamepad/keyboard bindings, and `.app` bundle (`SkyRecompiled.app`).

### Milestone 2: Cross-Platform Windows & Linux Builds
- Top-level `CMakeLists.txt` + `CMakePresets.json` orchestrating `nds_recompile`, `generated_bios`, `prepare_rom.py`, and `SkyRecompiled` across:
  - **macOS (`arm64` / Universal)** -- AppleClang + Ninja + SDL3 Metal
  - **Windows (`x86_64`)** -- MSVC / MinGW-w64 + Ninja + SDL3 Direct3D12/Vulkan/OpenGL4.3
  - **Linux (`x86_64` / `aarch64`)** -- Clang/GCC + Ninja + SDL3 Vulkan/OpenGL4.3

### Build layout

- `translator/` owns offline ROM preparation and symbol binding generation.
  Its CMake entry point builds the pinned `nds_recompile` backend.
- `runtime/` owns integration of the native runner. The root CMake project
  builds the pinned `nds_runner` backend through this directory. The iOS app
  and its ROM compiler integration live in `runtime/ios/`.
- `scripts/build.py` coordinates the two build trees under `build/cmake/`.
  The executables still land in `build/recompiler/` and `build/runner/` (or
  `build/runner-fallback/` for cartridge runtime execution).

### Milestone 3: Performance & Graphics Beyond Emulators
- **Note on Frame Rate:** Vanilla *Explorers of Sky* already runs natively at **60 FPS** (`59.8261 Hz`), NOT 30 FPS! Therefore, instead of a "30->60 FPS patch", our standout smoothness upgrade is **High-Refresh-Rate (`120Hz` / `144Hz` / `240Hz`) Sub-Pixel Camera & Entity Interpolation**--reading camera/entity coordinates from `DUNGEON_PTR_MASTER` (`overlay29`) and `GROUND_STATE_PTRS` (`overlay11`) at $t-1$ and $t$ to render silky-smooth intermediate frames on ProMotion / high-Hz monitors with zero added input latency.
- **True Native Widescreen (`16:9` `342x192`, `16:10` `308x192`, `21:9` `448x192`):**
  - Extend `gpu2d.cpp` scanline/BG/OAM rasterizer from `256` px to `W` px (`512x256` virtual BG tilemaps already exist in VRAM!).
  - Hook `overlay29` tile-column upload & entity visibility culling (`pmdsky-debug` camera symbols) to stream extra tile columns on the left/right margins.
  - Anchor 2D OAM/Window HUD elements (HP, Belly, Floor, Message Log) to the true `16:9` screen edges + optional **Single-Screen Modern Layout** (full `16:9` gameplay with togglable translucent mini-map/team overlay).
- **Custom Apple Metal / `SDL_GPU` HD Shader Pipeline:**
  - Crisp pixel-art AA (`pixellate` / `sharp-bilinear`), optional CRT/LCD subpixel grid, and per-weather dungeon ambient bloom/color grading driven by `GetWeatherColorTable` (`0x022DE620` NA).

- **In-game language (planned):** Add **Game language** to Explorers of Sky’s
  own options menu for every supported cartridge. Show the languages
  available for the active ROM. The USA release contains English, and the
  European release contains English, French, German, Italian, and Spanish.
  Pass the confirmed choice from the game menu to the runtime and persist it
  in host settings. Apply it on the next launch so
  the game loads the matching resources during startup. If the active ROM
  lacks the preferred language, use English for that session and retain the
  saved preference.
  After loading any firmware state and before direct boot, write the
  effective DS language ID (1 through 5 in the order above) into bits 0–2 of
  the flags word at offset `0x64` in both generated firmware user settings blocks.
  Preserve the other flags and recalculate each block's checksum. Direct boot
  mirrors the effective settings into guest RAM at `0x027FFC80`.

### Milestone 4: Launcher, SkyTemple Mod Loader & Online Rescue Overhaul
- **Mod Support (Two-Tier):**
  1. **Zero-Recompile Layered NitroFS Virtual Filesystem (`mods/<mod_name>/romfs/...`):** Intercept NitroFS `FAT`/`FNT` file reads (portraits, sprites, music `.smd`/`.swd`, scripts `.ssb`, text `message.bin`, dungeon balance `.bin`) on the fly without patching the `.nds` ROM.
  2. **SkyTemple ROM Hack AOT Support:** `translator/prepare_rom.py --allow-custom-rom` automatically BLZ-decompresses modified `arm9`/overlays and recompiles custom ASM hacks (`extra_space` overlay `36` if present) into native code.
- **Online Rescue Overhaul (*Rescue Team DX* Style on Near-\$0 Hosting):**
  - Read/inject vanilla **Wonder Mail S / SOS Mail / A-OK Mail / Thank-You Mail** structs directly in guest RAM (with classic 54-char Wonder Mail S passwords preserved forever as an offline/serverless fallback!).
  - Lightweight open-source REST/WebSocket rescue bulletin board targetable to **Cloudflare Workers + Cloudflare D1 (SQLite)** free tier (\$0/month for tens of thousands of daily rescues). Live production deployment: `https://sky-rescue-worker.darthanonymous1.workers.dev/`.

---

## 5. Licensing Notice

- `SkyRecompiled` glue scripts, tools, and clean-room configs are original code; `pmdsky-debug` symbols/headers are dual-licensed `MIT OR GPL-3.0-or-later`; `FreeBIOS` is `BSD-2-Clause`.
- `third_party/ndsrecomp/runner` links vendored `melonDS` components (`GPU3D`, `Wifi`) under **GPL-3.0-or-later**. Accordingly, any distributed `SkyRecompiled` binary builds must comply with **GPL-3.0-or-later** (`LICENSE-GPL`).

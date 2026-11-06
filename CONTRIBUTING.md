# Contributing to SkyRecompiled

Thank you for your interest in contributing to **SkyRecompiled**, the ahead-of-time (AOT) static recompilation project for *Pokémon Mystery Dungeon: Explorers of Sky* (`C2SE`).

To ensure the long-term legal safety, stability, and code quality of the project, all contributors must adhere to the guidelines below.

---

## 1. Clean-Room Discipline & Legal Invariants (Non-Negotiable)

SkyRecompiled is developed under strict clean-room reverse engineering principles:

- **Zero Copyrighted Assets:** Under NO circumstances may copyrighted Nintendo, The Pokémon Company, or Spike Chunsoft assets be committed to this repository. This includes ROM dumps (`.nds`), BIOS/firmware dumps, extracted sprite archives, music/soundfonts (`.sdat`), or proprietary script dumps.
- **ROM-Free CI & Tests:** All CI jobs, unit test suites, and mock fixtures must run without requiring proprietary assets. End-users supply their own legally dumped cartridges locally.
- **Original Code & Licensing:** All code must be original clean-room implementations or derived from permissively licensed open-source projects under **GPL-3.0-or-later**. Do not paste leaked source code or decompiled disassembly verbatim.

---

## 2. Core Architectural Invariants

Any contribution touching the core runner, recompiler, or save system must preserve these technical invariants:

- **Save Format & Size:** Fixed `131072` bytes (128 KB) EEPROM format (`save_type = "eeprom"`).
- **Validation Cap:** Non-volatile EEPROM state integrity must maintain the 4-page live validation cap.
- **Direct Boot:** The engine must maintain direct, BIOS-free boot into the ARM9 entry point without requiring external Nintendo firmware files.
- **Modern C++20:** All native runner code must strictly follow standard C++20 with zero undefined behavior, zero compiler warnings, and cross-platform compatibility across macOS (Apple Silicon `arm64`), Linux (`x86_64`), and Windows (`x86_64` MSVC).
- **Hot-Path Performance:** Frame render loops and game tick dispatches must have zero dynamic heap allocations to preserve 60–120fps+ frame budgets.

---

## 3. Building & Testing Locally

### Prerequisites
- **macOS:** Xcode command line tools, `cmake`, `ninja`, `sdl3` (`brew install cmake ninja sdl3`)
- **Linux:** GCC/Clang, `cmake`, `ninja-build`, `libgl1-mesa-dev`, `libasound2-dev`
- **Windows:** See the [Windows quickstart](README.md#quickstart-windows-x64) for prerequisites and build commands.

### Quick Setup & Build
```bash
# 1. Bootstrap pinned dependencies
./scripts/setup_third_party.sh

# 2. Build via the unified build orchestrator
python3 scripts/build.py --help
# macOS and Linux; on Windows, use the Windows quickstart linked above.
python3 scripts/build.py --rom "/path/to/Explorers of Sky.nds"
```

### Running Test Suites
Before submitting a Pull Request, ensure all tests pass:
```bash
# Run all unit tests
python3 -m unittest discover tests/

# Run the headless determinism smoke test
python3 scripts/smoke_test.py --cycles 250000000
```

---

## 4. Pull Request Guidelines

1. **Keep PRs Focused:** Submit small, focused pull requests dedicated to a single feature, bugfix, or subsystem. Avoid mega-PRs mixing unrelated changes.
2. **Conventional Commit Messages:** Use conventional commit syntax (`feat:`, `fix:`, `ci:`, `docs:`, `refactor:`, `perf:`).
3. **Automated Review & Code Quality:** Pull requests undergo automated security, memory safety, and anti-slop audits. Avoid dead code branches, hallucinated APIs, or placeholder `// TODO: implement later` stubs.
4. **Accompanying Tests:** New tools, codecs, or core runtime logic must include accompanying unit tests under `tests/`.

---

## 5. Community & Conduct

Be respectful, constructive, and helpful to fellow explorers and contributors. We build SkyRecompiled for the community to preserve and celebrate *Explorers of Sky* on modern hardware with native enhancements!

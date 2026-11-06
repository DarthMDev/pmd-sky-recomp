# SkyRecompiled Code Review & Invariant Guidelines

These review rules apply to all automated and human reviews across changes, commits, and trails in `SkyRecompiled`.

## 1. Clean-Room Reverse Engineering Discipline (P0 - Critical)
- **Zero Proprietary Assets**: Under NO circumstances may Nintendo, Pokémon Company, or Spike Chunsoft copyrighted binaries, ROM dumps (`.nds`), extracted sprites, proprietary soundfonts, audio streams (`.sdat`), or script dumps be committed to this repository.
- **ROM-Free CI & Tools**: All test runners, unit tests, and CI workflows must execute purely against generated synthetic mock ROMs or clean-room synthetic fixtures. Never reference proprietary data paths.

## 2. Save Format & State Integrity (P0 - Critical)
- **Save Format**: `save_type = "eeprom"`, fixed save size of exactly `131072` bytes (128 KB).
- **Validation Cap**: Maximum 4-page live validation cap for non-volatile EEPROM state integrity.
- **Save Safety**: Never bypass validation checksums or write unvalidated sectors directly to non-volatile storage.

## 3. Architecture & Standards
- **C++ Standard**: Modern C++20 across all native runner code (`nds_runner`).
- **Memory Safety**: No undefined behavior, dangling pointers, unchecked buffer indexing, or data races.
- **Direct Boot**: Must maintain direct BIOS-free boot into ARM9 entry point without requiring proprietary NDS ARM7/ARM9 BIOS or firmware files.
- **Hot-Path Performance**: Zero dynamic heap allocations (`new`, `malloc`, `std::vector::push_back` reallocation) in per-frame tick loops; deterministic integer math for frame budgets (60–120fps+).

## 4. Security & Deserialization Safety
- **Mod Loaders & File Packers**: Strictly validate all file paths during extraction/repacking to prevent directory traversal attacks (e.g., `../` path escaping).
- **Bitstream Codecs**: In Wonder Mail S, SOS, and A-OK password decoders, all incoming byte streams and strings must validate charsets (`&67NPR89F0+#STXY45MCHJ-K12=%3Q@W`) and Fletcher checksums before parsing payload bodies.
- **Network Services**: Cloudflare Workers / APIs must sanitize all user inputs and enforce rate limits.

## 5. Anti-Slop & Code Quality Discipline
- **No Hallucinated APIs**: Reject unverified synthetic APIs or non-existent standard library functions.
- **No Dead Boilerplate**: Flag redundant wrappers, useless type conversions, copy-pasted dead logic branches, or lazy placeholder comments (e.g., `// TODO: implement later`).
- **Test Coverage**: Any new tool, bitstream codec, or core sub-system must include accompanying automated unit tests.

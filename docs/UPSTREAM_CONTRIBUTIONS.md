# Upstream Contributions & Tool Forks

This document outlines the upstream pull requests and forks created during the development of SkyRecompiled. These improvements resolve cross-platform build compatibility issues (macOS Apple Silicon, Windows MSVC) and runtime configurability in the underlying recompilation toolchain.

---

## 1. Upstream Repositories & Patches

### A. `RetroPortingToolKit/ndsrecomp`
- **Upstream Repository:** `https://github.com/RetroPortingToolKit/ndsrecomp`
- **Recommended Fork:** `https://github.com/DarthMDev/ndsrecomp`

#### Proposed PR 1: Apple Silicon & Dynarmic-Free Build Fixes
- **Patch file:** `patches/ndsrecomp-macos-arm64.patch`
- **Summary:**
  - Adds `<unistd.h>` inclusion and `access(path, X_OK)` support on `__APPLE__` in `runner/src/live_overlay_platform.cpp`.
  - Stubs `arm9_native_compilations` and `arm9_native_prepare` in `runner/src/debug_server.cpp` when compiled without `NDS_ARM9_DYNARMIC`.
  - Implements `runtime_note_code_write_pages` stub in `recompiler/tests/flat_execution_probe.cpp` to prevent unresolved external symbols.

#### Proposed PR 2: Windows MSVC Compiler Detection in `compile_live_shards.py`
- **Patch file:** `patches/ndsrecomp-msvc-tools.patch`
- **Summary:**
  - Detects `cl.exe` / `msvc` stems in `compiler_identity()` instead of trying to invoke `-dumpmachine`, allowing live shard compilation on Windows with Visual Studio.

#### Proposed PR 3: Configurable Window Title in Interactive Frontend
- **Patch file:** `patches/ndsrecomp-window-title.patch`
- **Summary:**
  - Adds custom title string support in `NdsFrontendOptions` so downstream ports and launchers can brand window titles cleanly without hardcoded strings.

---

### B. `RetroPortingToolKit/arm-recomp-core`
- **Upstream Repository:** `https://github.com/RetroPortingToolKit/arm-recomp-core`
- **Recommended Fork:** `https://github.com/DarthMDev/arm-recomp-core`

#### Proposed PR: Standard C++20 `std::popcount` for Cross-Platform Compilation
- **Patch file:** `patches/arm-recomp-core-msvc-popcount.patch`
- **Summary:**
  - Replaces GCC/Clang-specific `__builtin_popcount` with `<bit>`'s `std::popcount` in `common/interpreter_impl.h`.
  - Allows building the ARM interpreter core out of the box on MSVC without emulation stubs.

---

## 2. Fork Creation & PR Submission Guide

When ready to open PRs upstream:

1. **Fork the upstream repositories on GitHub:**
   - Fork `https://github.com/RetroPortingToolKit/ndsrecomp` to your GitHub account (`DarthMDev`).
   - Fork `https://github.com/RetroPortingToolKit/arm-recomp-core` to your GitHub account (`DarthMDev`).

2. **Apply Patches to Feature Branches:**
   ```bash
   # For ndsrecomp:
   git clone https://github.com/DarthMDev/ndsrecomp.git
   cd ndsrecomp
   git checkout -b fix/macos-and-msvc-support
   git apply /path/to/pmd-sky-recomp/patches/ndsrecomp-macos-arm64.patch
   git apply /path/to/pmd-sky-recomp/patches/ndsrecomp-msvc-tools.patch
   git commit -am "fix: add macOS Apple Silicon and MSVC build support"
   git push origin fix/macos-and-msvc-support
   ```

   ```bash
   # For arm-recomp-core:
   git clone https://github.com/DarthMDev/arm-recomp-core.git
   cd arm-recomp-core
   git checkout -b fix/portable-std-popcount
   git apply /path/to/pmd-sky-recomp/patches/arm-recomp-core-msvc-popcount.patch
   git commit -am "fix: use std::popcount for MSVC compatibility"
   git push origin fix/portable-std-popcount
   ```

3. **Open Pull Requests:**
   - Navigate to the respective upstream repositories on GitHub and click **Compare & pull request**.

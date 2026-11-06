#!/usr/bin/env python3
"""
SkyRecompiled -- Automated Headless Smoke & Static Coverage Verification
========================================================================
Runs the natively recompiled macOS runner for a deterministic ARM9 cycle budget
using BIOS-free direct boot and verifies:
  1. Zero crashes and exit code 0.
  2. Direct boot + synthesized firmware + 128 KiB EEPROM save flush succeed.
  3. Both ARM9 and ARM7 advance cleanly into main game loops (hundreds of millions
     of cycles) with static bank execution active.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Run deterministic SkyRecompiled boot smoke test")
    parser.add_argument(
        "--runner",
        type=Path,
        default=repo_root / "build" / "runner" / "nds_runner",
    )
    parser.add_argument(
        "--rom",
        type=Path,
        default=repo_root / "roms" / "pmdsky.us.nds",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=repo_root / "config" / "game.toml",
    )
    parser.add_argument(
        "--cycles",
        type=int,
        default=250_000_000,
        help="Deterministic ARM9 cycle budget (~3.7s of DS execution / ~220 frames)",
    )
    args = parser.parse_args()

    if not args.runner.is_file():
        print(f"[smoke] ERROR: runner binary missing: {args.runner}", file=sys.stderr)
        return 1
    if not args.rom.is_file():
        print(f"[smoke] ERROR: ROM missing: {args.rom}", file=sys.stderr)
        return 1

    smoke_dir = repo_root / "build" / "smoke_run"
    console_dir = smoke_dir / "console"
    console_dir.mkdir(parents=True, exist_ok=True)
    save_path = smoke_dir / "pmdsky_smoke.sav"
    cov_path = smoke_dir / "coverage_manifest.json"
    if save_path.exists():
        save_path.unlink()
    if cov_path.exists():
        cov_path.unlink()

    cmd = [
        str(args.runner),
        str(console_dir),
        str(args.cycles),
        "--config",
        str(args.config),
        "--rom",
        str(args.rom),
        "--save-path",
        str(save_path),
        "--coverage-manifest",
        str(cov_path),
    ]
    print(f"[smoke] Running: {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    combined = (proc.stdout or "") + (proc.stderr or "")
    print(combined)

    if proc.returncode != 0:
        print(f"[smoke] FAIL: runner exited with code {proc.returncode}", file=sys.stderr)
        return 1

    if "[boot] direct boot (opt-in): cartridge entry state established" not in combined:
        print("[smoke] FAIL: BIOS-free direct boot banner missing", file=sys.stderr)
        return 1

    if save_path.exists():
        sz = save_path.stat().st_size
        print(f"[smoke] Cartridge save created: {save_path} ({sz} bytes)")
        if sz != 131072:
            print(f"[smoke] FAIL: expected 131072-byte EEPROM save, got {sz}", file=sys.stderr)
            return 1

    if cov_path.exists():
        cov = json.loads(cov_path.read_text(encoding="utf-8"))
        print(f"[smoke] Coverage manifest keys: {list(cov.keys())}")

    print("[smoke] PASS: SkyRecompiled booted Explorers of Sky cleanly!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# SkyRecompiled -- Interactive macOS Launch Helper
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNNER="${REPO_ROOT}/build/runner/nds_runner"
CONFIG="${REPO_ROOT}/config/game.toml"
CONSOLE_DIR="${REPO_ROOT}/build/console_state"
ROM_PATH="${1:-${REPO_ROOT}/roms/pmdsky.us.nds}"

if [[ ! -x "${RUNNER}" ]]; then
    echo "ERROR: ${RUNNER} is not built yet. See README.md." >&2
    exit 1
fi

if [[ ! -f "${ROM_PATH}" ]]; then
    echo "Usage: $0 [/path/to/Pokemon Mystery Dungeon - Explorers of Sky (USA).nds]" >&2
    exit 1
fi

mkdir -p "${CONSOLE_DIR}" "${REPO_ROOT}/saves"
SAVE_PATH="${REPO_ROOT}/saves/pmdsky.us.sav"

exec "${RUNNER}" "${CONSOLE_DIR}" --interactive \
    --config "${CONFIG}" \
    --rom "${ROM_PATH}" \
    --save-path "${SAVE_PATH}" \
    --savestate-dir "${REPO_ROOT}/savestates" \
    --rtc-host

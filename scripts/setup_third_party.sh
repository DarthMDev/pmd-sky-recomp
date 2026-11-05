#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# SkyRecompiled — Deterministic Third-Party Bootstrap Script
# Clones & pins exact commit SHAs for ndsrecomp (+ submodules), pmdsky-debug,
# and recomp-ui, stages toml++ v3.4.0, and applies macOS arm64 fixes.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_DIR="${REPO_ROOT}/third_party"
PATCH_FILE="${REPO_ROOT}/patches/ndsrecomp-macos-arm64.patch"

NDSRECOMP_URL="https://github.com/RetroPortingToolKit/ndsrecomp.git"
NDSRECOMP_SHA="54967a4e26bcd6ad46445d51bab3214decc3fd59"

PMDSKY_DEBUG_URL="https://github.com/UsernameFodder/pmdsky-debug.git"
PMDSKY_DEBUG_SHA="64979c3df12d59dfc6788140fc1eca4c36ef2a5c"

RECOMP_UI_URL="https://github.com/RetroPortingToolKit/recomp-ui.git"
RECOMP_UI_SHA="839a6f98c3014e7586ac942a66395ddaa6fc5bc5"

TOMLPP_URL="https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp"
TOMLPP_SHA256="6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e"

mkdir -p "${TP_DIR}"

clone_and_pin() {
    local name="$1"
    local url="$2"
    local sha="$3"
    local dest="${TP_DIR}/${name}"

    if [[ ! -d "${dest}/.git" ]]; then
        echo "[setup] Cloning ${name}..."
        git clone "${url}" "${dest}"
    fi

    echo "[setup] Pinning ${name} to ${sha:0:10}..."
    git -C "${dest}" fetch --quiet origin "${sha}" 2>/dev/null || true
    git -C "${dest}" checkout --quiet "${sha}"
}

clone_and_pin "ndsrecomp" "${NDSRECOMP_URL}" "${NDSRECOMP_SHA}"
git -C "${TP_DIR}/ndsrecomp" submodule update --init --recursive

clone_and_pin "pmdsky-debug" "${PMDSKY_DEBUG_URL}" "${PMDSKY_DEBUG_SHA}"
clone_and_pin "recomp-ui" "${RECOMP_UI_URL}" "${RECOMP_UI_SHA}"

TOMLPP_DEST="${TP_DIR}/ndsrecomp/recompiler/third_party/toml.hpp"
mkdir -p "$(dirname "${TOMLPP_DEST}")"
if [[ ! -s "${TOMLPP_DEST}" ]]; then
    echo "[setup] Fetching toml++ v3.4.0 single header..."
    curl -fsSL "${TOMLPP_URL}" -o "${TOMLPP_DEST}"
fi
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL_SHA256="$(sha256sum "${TOMLPP_DEST}" | awk '{print $1}')"
else
    ACTUAL_SHA256="$(shasum -a 256 "${TOMLPP_DEST}" | awk '{print $1}')"
fi
if [[ "${ACTUAL_SHA256}" != "${TOMLPP_SHA256}" ]]; then
    rm -f "${TOMLPP_DEST}"
    echo "[setup] ERROR: toml.hpp SHA-256 mismatch (${ACTUAL_SHA256} != ${TOMLPP_SHA256})" >&2
    exit 1
fi

if [[ -f "${PATCH_FILE}" ]]; then
    if git -C "${TP_DIR}/ndsrecomp" apply --reverse --check "${PATCH_FILE}" 2>/dev/null; then
        echo "[setup] macOS arm64 patch already applied to ndsrecomp."
    else
        echo "[setup] Applying patches/ndsrecomp-macos-arm64.patch..."
        git -C "${TP_DIR}/ndsrecomp" apply "${PATCH_FILE}"
    fi
fi

CORE_PATCH="${REPO_ROOT}/patches/arm-recomp-core-msvc-popcount.patch"
if [[ -f "${CORE_PATCH}" ]]; then
    if git -C "${TP_DIR}/ndsrecomp/external/arm-recomp-core" apply --reverse --check "${CORE_PATCH}" 2>/dev/null; then
        echo "[setup] MSVC popcount patch already applied to arm-recomp-core."
    else
        echo "[setup] Applying patches/arm-recomp-core-msvc-popcount.patch..."
        git -C "${TP_DIR}/ndsrecomp/external/arm-recomp-core" apply "${CORE_PATCH}"
    fi
fi

echo "[setup] All third_party dependencies verified and ready."

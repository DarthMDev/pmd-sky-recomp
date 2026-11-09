#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# SkyRecompiled -- Deterministic Third-Party Bootstrap Script
# Clones & pins exact commit SHAs for ndsrecomp (+ submodules), pmdsky-debug,
# and recomp-ui, stages toml++ v3.4.0, and applies macOS arm64 fixes.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP_DIR="${SKY_THIRD_PARTY_DIR:-${REPO_ROOT}/third_party}"

NDSRECOMP_URL="https://github.com/RetroPortingToolKit/ndsrecomp.git"
NDSRECOMP_SHA="54967a4e26bcd6ad46445d51bab3214decc3fd59"
# ndsrecomp's gitlink predates mstan/arm-recomp-core#8 (MSVC std::popcount).
ARM_CORE_SUBMODULE="external/arm-recomp-core"
ARM_CORE_SHA="a52db07db5c0d920a34de7148874a3176195ee69"

PMDSKY_DEBUG_URL="https://github.com/UsernameFodder/pmdsky-debug.git"
PMDSKY_DEBUG_SHA="64979c3df12d59dfc6788140fc1eca4c36ef2a5c"

RECOMP_UI_URL="https://github.com/RetroPortingToolKit/recomp-ui.git"
RECOMP_UI_SHA="839a6f98c3014e7586ac942a66395ddaa6fc5bc5"

TOMLPP_URL="https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp"
TOMLPP_SHA256="6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e"
TOMLPP_DEST="${TP_DIR}/ndsrecomp/recompiler/third_party/toml.hpp"

tomlpp_valid() {
    local digest
    [[ -s "$1" ]] || return 1
    # Hash stdin so checksum tools cannot prefix the digest when escaping a path.
    if command -v sha256sum >/dev/null 2>&1; then
        digest="$(sha256sum < "$1")" || return 1
    else
        digest="$(shasum -a 256 < "$1")" || return 1
    fi
    [[ "${digest%% *}" == "${TOMLPP_SHA256}" ]]
}

pinned_checkout() {
    local dest="${TP_DIR}/$1"
    local head deleted
    [[ -e "${dest}/.git" ]] || return 1
    head="$(git -C "${dest}" rev-parse HEAD)" || return 1
    deleted="$(git -C "${dest}" ls-files --deleted)" || return 1
    [[ "${head}" == "$2" && -z "${deleted}" ]]
}

cache_valid() {
    local submodules
    pinned_checkout ndsrecomp "${NDSRECOMP_SHA}" || return 1
    pinned_checkout "ndsrecomp/${ARM_CORE_SUBMODULE}" "${ARM_CORE_SHA}" || return 1
    pinned_checkout pmdsky-debug "${PMDSKY_DEBUG_SHA}" || return 1
    pinned_checkout recomp-ui "${RECOMP_UI_SHA}" || return 1
    submodules="$(git -C "${TP_DIR}/ndsrecomp" submodule status --recursive -- third_party/freebios)" || return 1
    if [[ "${submodules}" =~ (^|$'\n')[-+U] ]]; then
        return 1
    fi
    git -C "${TP_DIR}/ndsrecomp" submodule foreach --quiet --recursive \
        'deleted=$(git ls-files --deleted) && test -z "$deleted"' || return 1
    tomlpp_valid "${TOMLPP_DEST}"
}

apply_patch() {
    local dest="$1" patch="${REPO_ROOT}/patches/$2" required="${3:-optional}"
    local files added removed path
    local -a applied=()
    if [[ ! -f "${patch}" ]]; then
        if [[ "${required}" == "required" ]]; then
            echo "[setup] ERROR: Required patch ${patch} is missing" >&2
            exit 1
        fi
        return 0
    fi
    if git -C "${dest}" apply --reverse --check "${patch}" 2>/dev/null; then
        return 0
    fi

    # Restoring a missing file can leave the other files already patched.
    files="$(git -C "${dest}" apply --numstat "${patch}")"
    while IFS=$'\t' read -r added removed path; do
        if git -C "${dest}" apply --reverse --check --include="${path}" "${patch}" 2>/dev/null; then
            applied+=("--exclude=${path}")
        fi
    done <<< "${files}"
    echo "[setup] Applying $2..."
    git -C "${dest}" apply "${applied[@]:+"${applied[@]}"}" "${patch}"
}

apply_patches() {
    local ndsrecomp="${TP_DIR}/ndsrecomp"
    apply_patch "${ndsrecomp}" ndsrecomp-macos-arm64.patch
    apply_patch "${ndsrecomp}" ndsrecomp-msvc-tools.patch
    apply_patch "${ndsrecomp}" ndsrecomp-window-title.patch
    if [[ "${1:-}" == "--ios" ]]; then
        apply_patch "${ndsrecomp}" ndsrecomp-ios.patch required
        apply_patch "${ndsrecomp}" ndsrecomp-ios-touch.patch required
    fi
}

if cache_valid; then
    apply_patches "$@"
    echo "[setup] All third_party dependencies verified and ready."
    exit
fi

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
    if ! git -C "${dest}" cat-file -e "${sha}^{commit}"; then
        git -C "${dest}" fetch --quiet origin "${sha}"
    fi
    git -C "${dest}" checkout --quiet "${sha}"
    git -C "${dest}" ls-files --deleted -z | git -C "${dest}" checkout-index -z --stdin
}

pin_arm_core() {
    local ndsrecomp="$1" core="$1/${ARM_CORE_SUBMODULE}"
    local interpreter=common/interpreter_impl.h
    if [[ ! -e "${core}/.git" ]]; then
        git -C "${ndsrecomp}" submodule update --init -- "${ARM_CORE_SUBMODULE}"
    fi
    if ! git -C "${core}" cat-file -e "${ARM_CORE_SHA}^{commit}"; then
        git -C "${core}" fetch --quiet origin "${ARM_CORE_SHA}"
    fi

    # Older setups patched this header in place. Clear that edit only when
    # the file already matches the upstream version and has no staged edits.
    if git -C "${core}" diff --cached --quiet -- "${interpreter}" &&
        git -C "${core}" diff --quiet "${ARM_CORE_SHA}" -- "${interpreter}" &&
        ! git -C "${core}" diff --quiet -- "${interpreter}"; then
        git -C "${core}" restore -- "${interpreter}"
    fi
    echo "[setup] Pinning arm-recomp-core to ${ARM_CORE_SHA:0:10}..."
    git -C "${core}" checkout --quiet "${ARM_CORE_SHA}"
}

clone_and_pin "ndsrecomp" "${NDSRECOMP_URL}" "${NDSRECOMP_SHA}"
git -C "${TP_DIR}/ndsrecomp" submodule update --init --recursive -- third_party/freebios
pin_arm_core "${TP_DIR}/ndsrecomp"
git -C "${TP_DIR}/ndsrecomp" submodule foreach --quiet --recursive \
    'git ls-files --deleted -z | git checkout-index -z --stdin'

clone_and_pin "pmdsky-debug" "${PMDSKY_DEBUG_URL}" "${PMDSKY_DEBUG_SHA}"
clone_and_pin "recomp-ui" "${RECOMP_UI_URL}" "${RECOMP_UI_SHA}"

mkdir -p "$(dirname "${TOMLPP_DEST}")"
if ! tomlpp_valid "${TOMLPP_DEST}"; then
    echo "[setup] Fetching toml++ v3.4.0 single header..."
    staged="$(mktemp "${TOMLPP_DEST}.XXXXXX")"
    trap 'rm -f "${staged}"' EXIT
    curl -fsSL "${TOMLPP_URL}" -o "${staged}"
    if ! tomlpp_valid "${staged}"; then
        echo "[setup] ERROR: toml.hpp SHA-256 mismatch" >&2
        exit 1
    fi
    mv "${staged}" "${TOMLPP_DEST}"
fi

apply_patches "$@"
echo "[setup] All third_party dependencies verified and ready."

#!/usr/bin/env bash
#
# Build and sign inside the build container (docker/Dockerfile). Used by
# build.ps1, the devcontainer and CI, so all three run the same steps.
#
#   scripts/build.sh                 build and sign (sets up the workspace on first run)
#   scripts/build.sh --pristine      same, from a clean build directory
#   scripts/build.sh --update        west update after editing config/west.yml
#   scripts/build.sh --menuconfig    open menuconfig for the last build
#
# Env:
#   SHIELD   default rethink_bare_tester
#   BOARD    default framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk
#   SNIPPET  optional, e.g. rethink-usb-log or framework-cb-uart-log
#   ARTIFACT optional output name, default <shield>[-<snippet>]
#   ZMK_WS   west workspace, default /zmk-ws (a Docker volume)
#   CMAKE_ARGS extra arguments passed to CMake
#
# Outputs out/<ARTIFACT>.signed.bin in the repository.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="${ZMK_WS:-/zmk-ws}"
BOARD="${BOARD:-framework_wireless_tp_kb/nrf54lm20a/cpuapp/zmk}"
SHIELD="${SHIELD:-rethink_bare_tester}"
SNIPPET="${SNIPPET:-}"
ARTIFACT="${ARTIFACT:-${SHIELD}${SNIPPET:+-$SNIPPET}}"
BUILD_DIR="$WS/build/$ARTIFACT"

# west projects that get patches, and where the patch series live (in order)
PATCHED=(zmk zephyr)
PATCH_SOURCES=("$WS/modules/framework-cb/patches" "$REPO/patches")

die() { echo "error: $*" >&2; exit 1; }

reset_checkout() {
    git -C "$1" checkout -q -- . && git -C "$1" clean -fdq
}

patch_project() {
    local dir="$WS/$1" src patches=()
    for src in "${PATCH_SOURCES[@]}"; do
        [ -d "$src/$1" ] || continue
        patches+=("$src/$1"/*.patch)
    done
    [ -e "${patches[0]:-}" ] || return 0
    # Already applied
    cat "${patches[@]}" | git -C "$dir" apply --reverse --check 2>/dev/null && return 0
    reset_checkout "$dir"
    cat "${patches[@]}" | git -C "$dir" apply ||
        die "patches for $1 do not apply to the $1 revision in config/west.yml"
}

patch_all() {
    local p
    for p in "${PATCHED[@]}"; do patch_project "$p"; done
}

unpatch_all() {
    local p
    for p in "${PATCHED[@]}"; do
        [ -d "$WS/$p/.git" ] || continue
        reset_checkout "$WS/$p"
    done
}

update() {
    mkdir -p "$WS/config"
    cp "$REPO/config/west.yml" "$WS/config/west.yml"
    [ -d "$WS/.west" ] || (cd "$WS" && west init -l config)
    unpatch_all
    (cd "$WS" && west update --narrow --fetch-opt=--depth=1)
    patch_all
}

build() {
    if [ ! -d "$WS/zephyr" ] || ! cmp -s "$REPO/config/west.yml" "$WS/config/west.yml"; then
        echo "Workspace missing or config/west.yml changed: updating $WS"
        update
    fi
    patch_all

    local snippet_args=()
    [ -n "$SNIPPET" ] && snippet_args=(-S "$SNIPPET")

    # Zephyr_DIR instead of `west zephyr-export`, whose registry in ~/.cmake
    # does not survive a throwaway container
    # shellcheck disable=SC2086
    (cd "$WS" && west build -s zmk/app -d "$BUILD_DIR" -b "$BOARD" "${snippet_args[@]}" -- \
        -DZephyr_DIR="$WS/zephyr/share/zephyr-package/cmake" \
        -DSHIELD="$SHIELD" \
        -DZMK_CONFIG="$REPO/config" \
        -DZMK_EXTRA_MODULES="$REPO" \
        ${CMAKE_ARGS:-})

    local image="$BUILD_DIR/zephyr/zmk.signed.bin"
    [ -f "$image" ] || die "build finished but $image is missing"
    mkdir -p "$REPO/out"
    cp "$image" "$REPO/out/$ARTIFACT.signed.bin"
    echo
    echo "Signed image: out/$ARTIFACT.signed.bin"
}

case "${1:-}" in
    "")           build ;;
    --pristine)   rm -rf "$BUILD_DIR"; build ;;
    --update)     update ;;
    --menuconfig) (cd "$WS" && west build -d "$BUILD_DIR" -t menuconfig) ;;
    -h|--help)    sed -n '3,/^$/s/^# \{0,1\}//p' "$0" ;;
    *)            die "unknown option: $1" ;;
esac

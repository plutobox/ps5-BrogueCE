#!/usr/bin/env bash
# Pinned native PS5 dependencies. Run once before Makefile.native/package-native.sh.
set -euo pipefail
ps5dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$ps5dir/.." && pwd)
deps="$ps5dir/.deps"
template="$ps5dir/toolchain/boilerplate"
[[ -f "$template/tools/setup-native-dependencies.sh" ]] || {
    echo 'Initialize the toolchain: git submodule update --init ps5/toolchain/boilerplate' >&2
    exit 1
}
mkdir -p "$deps"
export CCACHE_DIR="${CCACHE_DIR:-$deps/ccache}"
export PS5_NATIVE_TEMPLATE="$template"
export PS5_NATIVE_SDK="$template/.deps/native/ps5-payload-sdk"
export PS5_PACBREW_PREFIX="$template/.deps/pacbrew/v0.40.2/sysroot/user/homebrew"
export PS5_OPENGL_REPO="$deps/ps5-opengl"
export PS5_OPENGL_PREFIX="$deps/opengl-sdk/ps5-opengl-sdk-1.0.2/sdk"
export PS5_SDL2_PREFIX="$PS5_OPENGL_REPO/build/sdl-native/sdk"
export PS5_PLATFORM_LIB="$deps/ps5-platform/platform/build/ps5/libps5platform.a"

fetch_rev() {
    local repo=$1 rev=$2 dest=$3
    if [[ ! -d "$dest/.git" ]]; then git init -q "$dest"; fi
    if [[ $(git -C "$dest" rev-parse HEAD 2>/dev/null || true) != "$rev" ]]; then
        git -C "$dest" fetch -q --depth 1 "https://github.com/$repo" "$rev"
        git -C "$dest" checkout -q --detach FETCH_HEAD
    fi
}
# Download retries are bounded; bootstrap scripts verify their own SDK/PacBrew pins.
retry() {
    local attempt
    for attempt in 1 2 3; do
        if "$@"; then return 0; fi
        echo "Dependency setup attempt $attempt failed: $*" >&2
        if [[ $attempt != 3 ]]; then sleep 5; fi
    done
    return 1
}
fetch_file() {
    local url=$1 hash=$2 dest=$3
    mkdir -p "$(dirname "$dest")"
    if [[ ! -f "$dest" ]] || ! printf '%s  %s\n' "$hash" "$dest" | sha256sum --check --strict >/dev/null 2>&1; then
        curl --fail --location --retry 3 --output "$dest.download" "$url"
        printf '%s  %s\n' "$hash" "$dest.download" | sha256sum --check --strict
        mv "$dest.download" "$dest"
    fi
}
zlib="$template/.deps/native/zlib/zlib-1.3.2.tar.gz"
fetch_file https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz \
    bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16 "$zlib"
retry bash "$template/tools/setup-native-dependencies.sh"
retry bash "$template/tools/setup-pacbrew-dependencies.sh" --all
fetch_rev mihawk-99/PS5_PayloadSDK e105cf6f25801d9e9981812a89a3b74021d2d2a2 "$deps/ps5-platform"
fetch_rev darkxex/ps5-opengl 6b6bb2039f741287ed7f1bc6bc4b2c589709a0ba "$PS5_OPENGL_REPO"
fetch_rev ps5-payload-dev/SDL 8c56053f13ca13a0c050de613706ff69eb615836 "$deps/SDL"
archive="$deps/downloads/ps5-opengl-sdk-1.0.2.tar.gz"
fetch_file https://github.com/darkxex/ps5-opengl/releases/download/v1.0.2/ps5-opengl-sdk-1.0.2.tar.gz \
    30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4 "$archive"
if [[ ! -f "$PS5_OPENGL_PREFIX/manifest.sha256" ]]; then
    mkdir -p "$deps/opengl-sdk"
    tar -xzf "$archive" -C "$deps/opengl-sdk"
fi
(cd "$PS5_OPENGL_PREFIX" && sha256sum --check --strict manifest.sha256 >/dev/null)
# SDL invokes its compiler directly; use a wrapper outside the read-only submodule.
printf '#!/bin/sh\nexec sh "%s/tooling/prospero-clang18" "$@"\n' "$template" > "$deps/clang"
chmod +x "$deps/clang"
if [[ ! -f "$PS5_SDL2_PREFIX/lib/libSDL2.a" ]]; then
    python3 "$PS5_OPENGL_REPO/integration/SDL2/build.py" native \
        --sdl-source "$deps/SDL" --sdk-prefix "$PS5_OPENGL_PREFIX" \
        --out "$PS5_OPENGL_REPO/build/sdl-native" \
        --payload-sdk "$PS5_NATIVE_SDK" --compiler-wrapper "$deps/clang"
fi
make -C "$deps/ps5-platform/platform" -j"$(nproc)" LLVM_CONFIG=llvm-config-18
# Reusable by a local shell as well as GitHub Actions.
: > "$deps/environment.sh"
for name in CCACHE_DIR PS5_NATIVE_TEMPLATE PS5_NATIVE_SDK PS5_PACBREW_PREFIX PS5_OPENGL_REPO PS5_OPENGL_PREFIX PS5_SDL2_PREFIX PS5_PLATFORM_LIB; do
    printf 'export %s=%q\n' "$name" "${!name}" >> "$deps/environment.sh"
    if [[ -n ${GITHUB_ENV:-} ]]; then printf '%s=%s\n' "$name" "${!name}" >> "$GITHUB_ENV"; fi
done
echo "Native toolchain ready. For local builds: source $deps/environment.sh"

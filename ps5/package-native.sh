#!/usr/bin/env bash
# Package Brogue CE as a native PS5 title (a folder app with eboot.bin) for
# ShadowMountPlus and the PS5 homebrew catalog.
#
# Run from ps5/ after the native objects are built:
#
#   make -f Makefile.native
#   bash package-native.sh [--build]
#
# It copies ps5-native-app-boilerplate into build-ps5/app with ps5-opengl's
# native runtime (app heap, linker script), adds the game, its libraries and
# its assets, and writes sce_sys. With --build it runs the boilerplate's
# build, which links eboot.bin and writes build-ps5/app/dist/$TITLE_ID{,.zip}.
#
# Inputs (defaults match Makefile.native):
#   PS5_NATIVE_TEMPLATE  ps5-native-app-boilerplate checkout with .deps set up
#   PS5_OPENGL_REPO      ps5-opengl checkout (for native-app/)
#   PS5_OPENGL_PREFIX    installed ps5-opengl SDK (include/, lib/)
#   PS5_SDL2_PREFIX      SDL2 built by ps5-opengl's integration/SDL2/build.py
#   PS5_PACBREW_PREFIX   the boilerplate's PacBrew ports (user/homebrew)
#   PS5_PLATFORM_LIB     libps5platform.a from mihawk-99/PS5_PayloadSDK
#   TITLE_ID, TITLE_NAME, CONTENT_VERSION (default: ../sce_sys/param.json)
set -euo pipefail

ps5dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
brogue=$(cd "$ps5dir/.." && pwd)
template=${PS5_NATIVE_TEMPLATE:-$HOME/ps5-native-app-boilerplate}
opengl_repo=${PS5_OPENGL_REPO:-$HOME/ps5-opengl}
opengl=${PS5_OPENGL_PREFIX:-$HOME/ps5-opengl-sdk/ps5-opengl-sdk-1.0.2/sdk}
sdl2=${PS5_SDL2_PREFIX:-$HOME/ps5-opengl/build/sdl-native/sdk}
pacbrew=${PS5_PACBREW_PREFIX:-$template/.deps/pacbrew/v0.40.2/sysroot/user/homebrew}
platform=${PS5_PLATFORM_LIB:-$HOME/ps5-platform-sdk/platform/build/ps5/libps5platform.a}
param_in=$brogue/sce_sys/param.json
out="$ps5dir/build-ps5/app"

die() { echo "package-native: $*" >&2; exit 1; }

# The title's identity comes from the committed sce_sys/param.json, which the
# homebrew catalog reads too; the environment can override it.
read_param() {
    python3 -c 'import json, sys; p = json.load(open(sys.argv[1]))
print(p["titleId"], p["contentVersion"])' "$param_in"
}
if [[ -f $param_in ]]; then
    read -r param_title param_version < <(read_param)
fi
title_id=${TITLE_ID:-${param_title:-PPSA92043}}
title_name=${TITLE_NAME:-Brogue CE}
content_version=${CONTENT_VERSION:-${param_version:-01.000.000}}

[[ $title_id =~ ^[A-Z]{4}[0-9]{5}$ ]] || die "bad TITLE_ID: $title_id"
[[ $content_version =~ ^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$ ]] ||
    die "bad CONTENT_VERSION: $content_version"
for path in "$template/tools/build.sh" "$template/.deps/native/ps5-payload-sdk" \
            "$opengl_repo/native-app/runtime_shims.c" \
            "$opengl/lib/libPS5OpenGLCore33.a" "$sdl2/lib/libSDL2.a" \
            "$pacbrew/lib/libSDL2_image.a" "$platform" \
            "$brogue/bin/assets/tiles.png" "$brogue/sce_sys/icon0.png" \
            "$ps5dir/brogue-native.objs"; do
    [[ -e $path ]] || die "missing $path"
done

rm -rf "$out"
mkdir -p "$out"

# The boilerplate project, with ps5-opengl's native runtime.
for directory in runtime sce_sys tooling tools .deps/native; do
    mkdir -p "$out/$(dirname "$directory")"
    cp -a "$template/$directory" "$out/$directory"
done
sdk="$out/.deps/native/ps5-payload-sdk"
mkdir -p "$out/src" "$out/vendor" "$out/build/native-imports"
cp "$opengl_repo/native-app/runtime_shims.c" "$opengl_repo/native-app/app_heap.c" \
   "$opengl_repo/native-app/cts_runtime_shims.c" "$ps5dir/native_shims.c" \
   "$out/src/"
cp "$out/tooling/native/ps5-pie.ld" "$out/tooling/native/ps5-pie-base.ld"
cp "$opengl_repo/native-app/ps5-pie.ld" "$opengl_repo/native-app/app-symbols.map" \
   "$out/tooling/native/"
cp "$opengl/lib/libSceAgc.so" "$opengl/lib/libSceAgcDriver.so" "$sdk/target/lib/"

replace_once() {
    local file=$1 old=$2 new=$3
    python3 - "$file" "$old" "$new" <<'EOF'
import sys
path, old, new = sys.argv[1:]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"package-native: template changed, cannot patch {path}: {old}")
open(path, "w").write(text.replace(old, new))
EOF
}

# The link inputs: every game object, then the libraries in one group.
compiler_rt="$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
libraries=("$pacbrew/lib/libSDL2_image.a" "$pacbrew/lib/libwebpdemux.a"
           "$pacbrew/lib/libwebp.a" "$pacbrew/lib/libsharpyuv.a"
           "$pacbrew/lib/libpng16.a" "$pacbrew/lib/libz.a" "$platform"
           "$sdl2/lib/libSDL2.a" "$opengl/lib/libPS5OpenGLCore33.a"
           "$sdk/target/lib/libunwind.a" "$sdk/target/lib/libc++abi.a"
           "$sdk/target/lib/libc++.a" "$compiler_rt")
for path in "${libraries[@]}"; do
    [[ -f $path ]] || die "missing link input: $path"
done
mapfile -t objects < "$ps5dir/brogue-native.objs"
for object in "${objects[@]}"; do
    [[ -f $object ]] || die "missing object: $object"
done

# The C library functions a title lacks (no system module it loads exports
# them), is refused (the sandbox fails them, or a system call raises SIGSYS)
# or gets wrong are bound to the platform layer's ps5_ versions: every one
# the game and its libraries use. A title must not define libc's own names,
# so they are bound at link time with --defsym. The heap stays ps5-opengl's.
mapfile -t provided < <(llvm-nm-18 --defined-only "$platform" 2>/dev/null |
    awk '$2 == "T" && $3 ~ /^ps5_/ { sub(/^ps5_/, "", $3); print $3 }' | sort -u)
mapfile -t used < <(llvm-nm-18 "${objects[@]}" "${libraries[@]}" 2>/dev/null |
    awk 'NF == 2 && ($1 == "U" || $1 == "w") { print $2 }' | sort -u)
mapfile -t bound < <(comm -12 <(printf '%s\n' "${provided[@]}") \
    <(printf '%s\n' "${used[@]}") |
    grep -v -x -E 'malloc|calloc|realloc|free|posix_memalign|aligned_alloc|malloc_usable_size|memalign|sysconf|pthread_exit')
# Not sysconf or pthread_exit: ps5_sysconf and ps5_pthread_exit call the
# originals, which a binding would turn into calls to themselves.
# mkstemp is imported although no input's symbol table names it; bind it
# regardless, as an unbound import resolves to nothing on the console.
for function in mkstemp; do
    [[ " ${provided[*]} " == *" $function "* && " ${bound[*]} " != *" $function "* ]] &&
        bound+=("$function")
done
defsyms=()
for function in "${bound[@]}"; do
    defsyms+=("--defsym=$function=ps5_$function")
done
# What neither the console nor the platform layer provides: native_shims.c.
for function in lstat; do
    if [[ " ${used[*]} " == *" $function "* ]]; then
        defsyms+=("--defsym=$function=brogue_ps5_$function")
    fi
done
echo "Bound to the platform layer: ${bound[*]}"

# ps5main.c gives Brogue's tile renderer the OpenGL window, and a software
# renderer drawing into an off-screen surface that it presents with OpenGL
# (ps5-opengl's SDL2 has no window surface and no render backend but the
# software one), and keeps the title alive in place of exit().
wraps="--wrap=SDL_CreateWindow --wrap=SDL_CreateRenderer --wrap=SDL_GetRenderer"
wraps+=" --wrap=SDL_SetWindowFullscreen --wrap=SDL_GetWindowFlags"
wraps+=" --wrap=SDL_SetWindowSize --wrap=SDL_RestoreWindow --wrap=SDL_SetWindowIcon"
wraps+=" --wrap=SDL_RenderClear --wrap=SDL_RenderFillRect --wrap=SDL_RenderCopy"
wraps+=" --wrap=SDL_RenderPresent --wrap=SDL_PollEvent --wrap=exit"

# As in ps5-opengl's folder.py: a bounded heap, prebuilt dependencies, and
# the app heap's allocator wrappers.
replace_once "$out/tooling/native/sce_module_writer.cpp" \
    'write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());' \
    'write_u64(result.data, result.heap_size, 0x10000000ULL);'
replace_once "$out/tools/build.sh" \
    'bash "$root/tools/setup-native-dependencies.sh" >/dev/null' \
    'test -x "$root/.deps/native/ps5-payload-sdk/bin/prospero-lld"'
# Brogue's assets sit at the title root (/app0, its DATADIR): the
# boilerplate's build copies only assets/, so it copies root/ too.
replace_once "$out/tools/build.sh" \
    '[[ ! -d $root/assets ]] || cp -a "$root/assets" "$app/assets"' \
    '[[ ! -d $root/assets ]] || cp -a "$root/assets" "$app/assets"
[[ ! -d $root/root ]] || cp -a "$root/root/." "$app/"'
replace_once "$out/tools/build.sh" \
    '--eh-frame-hdr \' \
    "--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size --wrap=fopen $wraps ${defsyms[*]} \\"

{
    printf 'SEARCH_DIR("%s")\nSEARCH_DIR("%s")\n' "$sdk/target/lib" "$opengl/lib"
    printf 'INPUT(\n'
    printf '  "%s"\n' "${objects[@]}"
    printf ')\nGROUP(\n'
    printf '  "%s"\n' "${libraries[@]}"
    printf ')\n'
} > "$out/vendor/libbrogue.a"

# Game data, read-only at the title root, /app0 (Brogue's DATADIR).
mkdir -p "$out/root/assets"
cp "$brogue/bin/assets/"* "$out/root/assets/"

# Title metadata and art.
rm -rf "$out/sce_sys"
mkdir -p "$out/sce_sys"
cp "$brogue/sce_sys/icon0.png" "$out/sce_sys/icon0.png"
python3 - "$opengl_repo/native-app/param.json" "$out/sce_sys/param.json" \
    "$title_id" "$title_name" "$content_version" <<'EOF'
import json, sys
template, output, title_id, name, version = sys.argv[1:]
param = json.load(open(template))
param["titleId"] = title_id
param["conceptId"] = title_id[4:]
param["contentId"] = f"IV9999-{title_id}_00-BROGUECEPS500000"
param["contentVersion"] = version
param["localizedParameters"]["en-US"]["titleName"] = name
json.dump(param, open(output, "w"), indent=2)
open(output, "a").write("\n")
EOF

echo "Prepared $out ($title_id)"
if [[ ${1:-} == --build ]]; then
    (cd "$out" &&
     env APP_STATIC_ARCHIVES=vendor/libbrogue.a APP_DEFINITIONS= APP_INCLUDE_PATHS= \
        PACBREW_PACKAGES= PACBREW_INCLUDE_PATHS= PACBREW_STATIC_ARCHIVES= \
        APP_RUNTIME_MODULES= PS5_PAYLOAD_SDK="$sdk" \
        bash "$out/tools/build.sh" Folder)
    echo "Title folder: $out/dist/$title_id"
fi

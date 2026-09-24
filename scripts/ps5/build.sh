#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${1:-"$root/build-ps5"}
case "$build_dir" in
    /*) ;;
    *) build_dir="$root/$build_dir" ;;
esac

sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
sdl_revision=37acbcac579944f196c1620c44b108cf52985749
sdl_source="$build_dir/ps5-sdl-src"
sdl_build="$build_dir/ps5-sdl-build"
sdl_prefix="$build_dir/ps5-sdl-prefix"
package_dir="$build_dir/ps5"
external_resources=${WILIWILI_PS5_EXTERNAL_RESOURCES:-0}
case "$external_resources" in
    1|ON|on|true|TRUE) external_resources=ON ;;
    0|OFF|off|false|FALSE) external_resources=OFF ;;
    *)
        echo "WILIWILI_PS5_EXTERNAL_RESOURCES must be 0/1 or ON/OFF" >&2
        exit 2
        ;;
esac

patch="$root/scripts/ps5/sdl.patch"
borealis_source="$root/library/borealis"
borealis_patch="$root/scripts/ps5/borealis.patch"
cpr_source="$root/library/cpr"
cpr_patch="$root/scripts/ps5/cpr.patch"
generator="$root/library/borealis/libromfs-generator"
sdl_state_file="$sdl_build/.wiliwili-sdl-state"
sdl_patch_hash=$(sha256sum "$patch")
sdl_state="${sdl_revision}:${sdl_patch_hash%% *}"

apply_submodule_patch() {
    local source=$1
    local patch_file=$2
    if git -C "$source" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
        return
    fi
    git -C "$source" apply --check "$patch_file"
    git -C "$source" apply "$patch_file"
}

if [[ ! -f "$sdk/toolchain/prospero.cmake" ]]; then
    echo "PS5_PAYLOAD_SDK must point to ps5-payload-sdk" >&2
    exit 1
fi
if [[ ! -x "$generator" ]]; then
    bash "$root/library/borealis/build_libromfs_generator.sh"
fi
apply_submodule_patch "$borealis_source" "$borealis_patch"
apply_submodule_patch "$cpr_source" "$cpr_patch"

if [[ ! -d "$sdl_source/.git" ]]; then
    git clone https://github.com/ps5-payload-dev/SDL.git "$sdl_source"
fi

if ! git -C "$sdl_source" cat-file -e "${sdl_revision}^{commit}" 2>/dev/null; then
    git -C "$sdl_source" fetch --depth 1 origin "$sdl_revision"
fi
git -C "$sdl_source" checkout --detach "$sdl_revision"
if git -C "$sdl_source" apply --reverse --check "$patch" >/dev/null 2>&1; then
    :
else
    git -C "$sdl_source" apply --check "$patch"
    git -C "$sdl_source" apply "$patch"
fi
if [[ ! -f "$sdl_state_file" ]] || [[ "$(<"$sdl_state_file")" != "$sdl_state" ]]; then
    rm -rf "$sdl_build" "$sdl_prefix"
fi

cmake -S "$sdl_source" -B "$sdl_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$sdl_prefix" \
    -DSDL_OPENGL=ON \
    -DSDL_LOADSO=ON \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON \
    -DSDL_TEST=OFF \
    -DSDL_TEST_LIBRARY=OFF \
    -DSDL_INSTALL_TESTS=OFF
cmake --build "$sdl_build" --target install
printf '%s\n' "$sdl_state" > "$sdl_state_file"


cmake -S "$root" -B "$build_dir" -G Ninja \
    -DPLATFORM_PS5=ON \
    -DPS5_PAYLOAD_SDK="$sdk" \
    -DPS5_SDL2_PREFIX="$sdl_prefix" \
    -DLIBROMFS_PREBUILT_GENERATOR="$generator" \
    -DPS5_EXTERNAL_RESOURCES="$external_resources" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target wiliwili.ps5

rm -rf "$package_dir/resources"
if [[ "$external_resources" == ON ]]; then
    cp -a "$root/resources" "$package_dir/resources"
fi

"$sdk/bin/prospero-clang++" -O2 -DNDEBUG \
    "$root/scripts/ps5/osmesa_installer.cpp" \
    $("$sdk/bin/prospero-pkg-config" --static --cflags --libs libcurl) \
    -o "$package_dir/osmesa-installer.elf"
cp "$sdk/target/user/homebrew/lib/libOSMesa.so.8.0.0" "$package_dir/libOSMesa.so.8"
cp "$sdk/target/user/homebrew/etc/ca-bundle.crt" "$package_dir/ca-bundle.crt"
cp "$root/scripts/ps5/homebrew.js" "$package_dir/homebrew.js"
cp "$root/resources/icon/icon.png" "$package_dir/icon0.png"

echo "PS5 bundle: $package_dir"

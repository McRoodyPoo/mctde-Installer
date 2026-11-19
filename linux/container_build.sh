#!/usr/bin/env bash
# Runs inside Valve's Steam Runtime "sniper" SDK container (Debian 11 base,
# glibc 2.31) started by build_in_container.sh: builds SDL3, fetches linuxdeploy,
# appimagetool and the AppImage runtime, builds the AppImage, and checks nothing
# in it needs a glibc newer than 2.31. All of it is cached in build/portable/.
#
# SDL3 is built here rather than taken from the runtime: the runtime's links
# X11, Wayland, ALSA, PulseAudio, fribidi, ... directly (the Steam Runtime
# always ships them), which would make all of those hard requirements. SDL's
# own defaults load them at run time only if present, so the GUI works on X11
# or Wayland with whatever audio stack, and the CLI needs no display at all.
set -euo pipefail
cd /src

MAX_GLIBC=2.31
SDL_VERSION=3.4.18
export OUT=build/portable      # own objects: never reuse ones built on the host
TOOLS=$OUT/tools
mkdir -p "$TOOLS"

fixperms() { [[ -n ${HOST_UID:-} ]] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" build || true; }
trap fixperms EXIT

fetch() {   # name url
    [[ -s $TOOLS/$1 ]] || { echo "== Fetching $1"; curl -fsSL -o "$TOOLS/$1" "$2"; chmod +x "$TOOLS/$1"; }
}
fetch linuxdeploy-x86_64.AppImage  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch appimagetool-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
fetch runtime-x86_64               https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
fetch SDL3-$SDL_VERSION.tar.gz      https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz

SDL_PREFIX=$PWD/$OUT/sdl3-$SDL_VERSION
if [[ ! -f $SDL_PREFIX/lib/pkgconfig/sdl3.pc ]]; then
    echo "== Building SDL $SDL_VERSION"
    src=$(mktemp -d)
    tar -xzf "$TOOLS/SDL3-$SDL_VERSION.tar.gz" -C "$src"
    cmake -S "$src/SDL3-$SDL_VERSION" -B "$src/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_DEPS_SHARED=ON >/dev/null
    cmake --build "$src/build" -j"$(nproc)" >/dev/null
    cmake --install "$src/build" >/dev/null
    rm -rf "$src"
fi
export PKG_CONFIG_PATH=$SDL_PREFIX/lib/pkgconfig
export LD_LIBRARY_PATH=$SDL_PREFIX/lib   # so linuxdeploy finds and bundles it

export APPIMAGE_RUNTIME=$PWD/$TOOLS/runtime-x86_64
export LINUXDEPLOY=$PWD/$TOOLS/linuxdeploy-x86_64.AppImage
export APPIMAGETOOL=$PWD/$TOOLS/appimagetool-x86_64.AppImage

echo "== Building the AppImage (SDL3 $(pkg-config --modversion sdl3), libcurl $(pkg-config --modversion libcurl))"
./make_appimage.sh

echo "== Checking glibc requirements (max allowed: $MAX_GLIBC)"
need=$(find "$OUT/AppDir" -type f \( -name '*.so*' -o -perm -u+x \) -exec sh -c \
        'objdump -T "$1" 2>/dev/null' _ {} \; \
      | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
echo "Highest glibc version needed: $need"
if [[ -z $need || $(printf '%s\n%s\n' "$need" "$MAX_GLIBC" | sort -V | tail -1) != "$MAX_GLIBC" ]]; then
    echo "ERROR: the AppImage needs glibc ${need:-?}, newer than $MAX_GLIBC"; exit 1
fi

mkdir -p build/linux
cp "$OUT/mctde-Installer-x86_64.AppImage" build/linux/
echo "Built build/linux/mctde-Installer-x86_64.AppImage (needs glibc $MAX_GLIBC+: SteamOS, Ubuntu 20.04+, Debian 11+, Fedora 32+, Mint 20+, Arch, ...)"

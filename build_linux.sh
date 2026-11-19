#!/usr/bin/env bash
# Native Linux build: the command-line tool (mctde-installer) and the GUI
# (mctde-installer-gui). Needs g++ (C++17), libcurl and SDL3 >= 3.2 dev files.
#   Arch:   sudo pacman -S --needed base-devel curl sdl3
#   Debian: sudo apt install build-essential libcurl4-openssl-dev libsdl3-dev
# Output goes to build/linux/. Run from anywhere.
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-build/linux}
mkdir -p "$OUT/obj"

CXX=${CXX:-g++}
CC=${CC:-gcc}
CXXFLAGS=${CXXFLAGS:-"-O2"}
CXXFLAGS="$CXXFLAGS -std=c++17 -Isrc -Ithird_party -Ithird_party/imgui"

CORE="Bhd5 Bnd3 C4110Header Detect Dcx DisplayConfig Download Extract ExePatch ExePatchTable
      Installer LaunchLinux LinuxLauncher NameHash Release SteamShortcut NamelistData Sha256 Unpacker Update VanillaManifest"
IMGUI="imgui imgui_draw imgui_tables imgui_widgets backends/imgui_impl_sdl3 backends/imgui_impl_sdlrenderer3"
SDL_CFLAGS=$(pkg-config --cflags sdl3)
SDL_LIBS=$(pkg-config --libs sdl3)
CURL_LIBS=$(pkg-config --libs libcurl)

compile() {   # src obj
    local src=$1 obj=$2
    if [[ ! -f $obj || $src -nt $obj || src/Detect.h -nt $obj ]]; then
        echo "  CXX $src"
        $CXX $CXXFLAGS $SDL_CFLAGS -c "$src" -o "$obj"
    fi
}

echo "Compiling..."
pids=()
for f in $CORE main GuiLinux; do compile "src/$f.cpp" "$OUT/obj/$f.o" & pids+=($!); done
for f in $IMGUI; do compile "third_party/imgui/$f.cpp" "$OUT/obj/$(basename $f).o" & pids+=($!); done
for p in "${pids[@]}"; do wait "$p"; done   # fails the script (set -e) if any compile failed

[[ $OUT/obj/miniz.o -nt third_party/miniz.c ]] || $CC -O2 -w -c third_party/miniz.c -o $OUT/obj/miniz.o
$CC -c src/linux_assets.S -o $OUT/obj/linux_assets.o

COREOBJ=$(for f in $CORE; do printf "%s " "$OUT/obj/$f.o"; done)
IMGUIOBJ=$(for f in $IMGUI; do printf "%s " "$OUT/obj/$(basename $f).o"; done)

echo "Linking..."
$CXX -static-libstdc++ -static-libgcc -o $OUT/mctde-installer $OUT/obj/main.o $COREOBJ $OUT/obj/miniz.o $CURL_LIBS -ldl -pthread
$CXX -static-libstdc++ -static-libgcc -o $OUT/mctde-installer-gui $OUT/obj/GuiLinux.o $COREOBJ $IMGUIOBJ $OUT/obj/miniz.o \
     $OUT/obj/linux_assets.o $SDL_LIBS $CURL_LIBS -ldl -pthread
echo "Built $OUT/mctde-installer and $OUT/mctde-installer-gui"

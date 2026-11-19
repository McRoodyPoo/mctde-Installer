#!/usr/bin/env bash
# Package the native Linux build as build/linux/mctde-Installer-x86_64.AppImage.
#
# Needs linuxdeploy and appimagetool (both AppImages from their GitHub
# releases). Put them on PATH, or point LINUXDEPLOY / APPIMAGETOOL at them:
#   LINUXDEPLOY=~/tools/linuxdeploy-x86_64.AppImage \
#   APPIMAGETOOL=~/tools/appimagetool-x86_64.AppImage ./make_appimage.sh
#
# An AppImage only runs where glibc is at least as new as the build machine's.
# So by default this builds inside Valve's Steam Runtime "sniper" SDK container
# (glibc 2.31) with Docker or Podman, and the result runs on
# SteamOS, Ubuntu, Debian, Fedora, Mint, Arch, openSUSE, ... alike. The tools
# above are fetched for you there. MCTDE_NATIVE=1 builds on this machine
# instead (the AppImage then only runs on distros as new as this one).
set -euo pipefail
cd "$(dirname "$0")"

if [[ ${MCTDE_IN_CONTAINER:-} != 1 && ${MCTDE_NATIVE:-} != 1 ]]; then
    exec linux/build_in_container.sh
fi

LINUXDEPLOY=${LINUXDEPLOY:-$(command -v linuxdeploy-x86_64.AppImage || command -v linuxdeploy || true)}
APPIMAGETOOL=${APPIMAGETOOL:-$(command -v appimagetool-x86_64.AppImage || command -v appimagetool || true)}
[[ -n $LINUXDEPLOY && -n $APPIMAGETOOL ]] || { echo "linuxdeploy / appimagetool not found (see the top of this script)"; exit 1; }
export APPIMAGE_EXTRACT_AND_RUN=1   # works without FUSE (containers, CI)
export NO_STRIP=1                   # linuxdeploy's bundled strip chokes on newer libraries

./build_linux.sh

OUT=${OUT:-build/linux}
APPDIR=$OUT/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
install -m755 $OUT/mctde-installer-gui $OUT/mctde-installer "$APPDIR/usr/bin/"
install -m644 linux/mctde.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/mctde-installer.png"
install -m644 linux/mctde-installer.desktop "$APPDIR/usr/share/applications/"

# Bundle SDL3, libcurl and their dependencies (glibc and the graphics stack
# stay with the system, per linuxdeploy's exclude list).
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/mctde-installer-gui" \
    --executable "$APPDIR/usr/bin/mctde-installer" \
    --desktop-file "$APPDIR/usr/share/applications/mctde-installer.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/mctde-installer.png"

# APPIMAGE_RUNTIME: a local type2 runtime file, else appimagetool downloads it.
ARCH=x86_64 "$APPIMAGETOOL" --no-appstream ${APPIMAGE_RUNTIME:+--runtime-file "$APPIMAGE_RUNTIME"} \
    "$APPDIR" "$OUT/mctde-Installer-x86_64.AppImage"
echo "Built $OUT/mctde-Installer-x86_64.AppImage"

#!/usr/bin/env bash
# Package an mctde-Installer release: one zip per platform, with docs and checksums.
#
#   tools/package_release.sh 0.4.0
#
# Inputs (build them first):
#   build/mctde-Installer.exe                      Windows build (build_gui.bat, MSVC)
#   build/linux/mctde-Installer-x86_64.AppImage    Linux build (./make_appimage.sh)
# Override with EXE=... / APPIMAGE=... if they live elsewhere.
#
# Output in dist/v<version>/ -- attach EVERYTHING to one GitHub release tagged
# v<version>, marked as the latest release:
#   mctde-Installer-windows.zip (+.sha256)   mctde-Installer.exe + README.md, LICENSE, CREDITS.md
#   mctde-Installer-linux.zip   (+.sha256)   mctde-Installer-x86_64.AppImage + README.md, LICENSE, CREDITS.md
#   mctde-Installer.exe                      bare exe for pre-0.4.0 Windows installers
#                                            (they self-update from releases/latest/download/mctde-Installer.exe)
# and the bare AppImage for pre-0.4.0 Linux installers, which only look at a release
# tagged linux-v<version> (see the steps printed at the end).
set -euo pipefail
cd "$(dirname "$0")/.."

VER=${1:-}
[[ -n $VER ]] || { echo "usage: $0 <version>" >&2; exit 2; }
[[ $VER =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "Use a plain dotted version (e.g. 0.4.0)." >&2; exit 1; }
SRC_VER=$(sed -n 's/^const char\* const kInstallerVersion = "\(.*\)";/\1/p' src/Update.cpp)
[[ $SRC_VER == "$VER" ]] || { echo "src/Update.cpp has kInstallerVersion \"$SRC_VER\", not $VER. Bump it first." >&2; exit 1; }

EXE=${EXE:-build/mctde-Installer.exe}
APPIMAGE=${APPIMAGE:-build/linux/mctde-Installer-x86_64.AppImage}
[[ -f $EXE ]] || { echo "Missing $EXE (build_gui.bat on Windows)." >&2; exit 1; }
[[ -f $APPIMAGE ]] || { echo "Missing $APPIMAGE (./make_appimage.sh)." >&2; exit 1; }
[[ $(head -c2 "$EXE") == MZ ]] || { echo "$EXE isn't a Windows program." >&2; exit 1; }
[[ $(head -c4 "$APPIMAGE" | tail -c3) == ELF ]] || { echo "$APPIMAGE isn't a Linux program." >&2; exit 1; }
DOCS=(README.md LICENSE CREDITS.md)
for d in "${DOCS[@]}"; do [[ -f $d ]] || { echo "Missing $d." >&2; exit 1; }; done

OUT=dist/v$VER
rm -rf "$OUT"; mkdir -p "$OUT"

# zip <out.zip> <file>...: files at the zip root, Unix permissions kept (the
# AppImage stays executable when unzipped).
mkzip() {
    python3 -I - "$@" <<'PY'
import os, sys, zipfile
out, files = sys.argv[1], sys.argv[2:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for f in files:
        z.write(f, os.path.basename(f))
PY
}

# Stage the programs under the exact names the updaters look for.
cp "$EXE" "$OUT/mctde-Installer.exe"
cp "$APPIMAGE" "$OUT/mctde-Installer-x86_64.AppImage"
chmod 755 "$OUT/mctde-Installer-x86_64.AppImage"
mkzip "$OUT/mctde-Installer-windows.zip" "$OUT/mctde-Installer.exe" "${DOCS[@]}"
mkzip "$OUT/mctde-Installer-linux.zip" "$OUT/mctde-Installer-x86_64.AppImage" "${DOCS[@]}"
(cd "$OUT" && for z in *.zip; do sha256sum "$z" > "$z.sha256"; done)

echo "Release files in $OUT:"
(cd "$OUT" && ls -l)
echo
echo "Zip contents:"
for z in "$OUT"/*.zip; do echo "  $(basename "$z"):"; python3 -I -c 'import sys,zipfile; [print("    "+n) for n in zipfile.ZipFile(sys.argv[1]).namelist()]' "$z"; done
cat <<EOF

Next:
  1. GitHub release in mctde-Installer tagged v$VER, marked LATEST. Attach:
       mctde-Installer-windows.zip, mctde-Installer-windows.zip.sha256,
       mctde-Installer-linux.zip,   mctde-Installer-linux.zip.sha256,
       mctde-Installer.exe
  2. (Older Linux installers) a second release tagged linux-v$VER, NOT latest,
     with just mctde-Installer-x86_64.AppImage attached.
  3. Only after both are published: in mctde-Link's latest.txt set
       mctde-installer=$VER
       mctde-installer-linux=$VER
EOF

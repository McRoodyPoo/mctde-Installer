# mctde-Installer

`mctde-Installer` is the one-step installer for the **mctde** Dark Souls: Prepare
to Die Edition (PTDE) setup. You point it at a Dark Souls install and it turns a
clean copy into a ready-to-play mctde install:

1. unpacks the game's `dvdbnd` archives into loose, moddable files and patches
   `DARKSOULS.exe` to load them,
2. downloads and installs the main **mctde** mod, **mctde-Link**, and the **mctde
   launcher**, and
3. sets the game up to launch under the mod's Steam configuration.

The unpack/patch core does in C++ what
[UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding)
(UDSFM) does; the rest is the download-and-deploy flow that lays the whole mctde
stack down for you.

There's a small GUI for normal use (it scans for Dark Souls installs, you pick
one and click **Install**) and a CLI that exposes the individual steps.

## What a full install does

Run against a PTDE `DATA` folder (GUI, or `fullinstall` on the CLI), the
installer:

1. **Unpacks + patches the game** (only if it's still packed). `dvdbnd` archives
   become loose files, `DARKSOULS.exe` is patched to read them, and the original
   exe + archives are backed up to `mctde-backup/` first. (Details below.)
2. **Downloads the mctde mod** from the project's Cloudflare R2 bucket and
   extracts it over the game root, so its `DATA/` contents merge in.
3. **Downloads mctde-Link** (the `d3d9.dll` proxy/overlay) from its latest GitHub
   release and extracts it into `DATA/`.
4. **Downloads the launcher** (`mctde_launcher.exe`, from
   `mctde-launcher-windows.zip` on its latest GitHub release) into `DATA/`. On
   Linux the native launcher (`mctde-launcher-linux.zip`) is installed next to
   it.
5. **Writes `steam_appid.txt`** (`480`, Spacewar) into `DATA/` so the game
   launches under the mod's Steam setup.

It's idempotent: the unpack/patch step backs originals up to `mctde-backup/` and
drops a `.mctde-unpacked` sentinel, so re-running skips work that's already done.

## Unpacking & patching (the core)

The unpack/patch step is three coordinated operations:

1. **Unpack** `dvdbnd0-3.bhd5` / `.bdt` into the 18 top-level game directories
   (`chr event facegen font map menu msg mtd obj other param paramdef parts remo
   script sfx shader sound`), decompressing DCX entries on the way out. Nested
   `tpfbhd` / `hkxbhd` / `chrtpf` archives are then unpacked a second level down
   into `tpf` / `hkx`.
2. **Patch `DARKSOULS.exe`** so the engine reads loose files: UTF-16LE
   virtual-drive string rewrites (`dvdbnd0:` → `dvdroot:`, `tpfbnd:` → `map:/tx`,
   patched in place since they're equal length) and a 2-byte code patch (`EB 12`)
   that turns off the DCX-decompression path. The exe is checked by SHA-256 first,
   so only a known clean build is ever patched.
3. **Delete the archives** so the engine falls back to the loose files.

## File formats (PTDE)

- **BHD5**: header table. LE. 24-byte header, then buckets `{u32 count, u32
  offset}`, then 16-byte records `{u32 nameHash, u32 size, u64 offset}`.
- **BDT**: raw data blob; slice `size` bytes at `offset`.
- **DCX**: compression container. Big-endian header; DS1 uses `DFLT` = a zlib
  (RFC 1950) payload located via the `DCS` (sizes) and `DCA` (payload) markers.
- **Name hash**: DS1 path hash `h = h*37 + c` over the lowercased,
  forward-slash path. A namelist maps hashes back to real paths.

## Layout

```
src/            core decoders + install flow + GUI
  BinaryReader.h  little-endian buffer reader
  Bhd5.{h,cpp}    BHD5 header parser
  Dcx.{h,cpp}     DCX detect + zlib inflate
  Download.{h,cpp} WinHTTP downloader
  Installer.cpp   unpack/patch + full download-and-deploy flow
  Gui.cpp         installer window
third_party/
  miniz.{c,h}     public-domain (Unlicense) zlib implementation
data/           generated namelist(s)
```

## Linux

### Native (recommended)

Download `mctde-Installer-linux.zip` from the
[latest release](https://github.com/McRoodyPoo/mctde-Installer/releases/latest),
unzip it and run `mctde-Installer-x86_64.AppImage` (from your file manager, or
`./mctde-Installer-x86_64.AppImage`; run `chmod +x` on it first if your unzip
tool dropped the executable bit). It's the same installer with a native
Linux window; the game, mctde-Link and the launcher still run through Proton.

- **Where it looks:** your Linux Steam (`~/.local/share/Steam`, `~/.steam/...`,
  Flatpak, Snap) and every library in its `libraryfolders.vdf`, including renamed
  copies in `steamapps/common`; then a bounded scan of your home folder, `/mnt`,
  `/media`, `/run/media` and the non-system top-level folders of `/`.
- **Display settings:** the DSFix tweaks (windowed, no in-game AA) are written to
  `DarkSouls.ini` in Steam's Proton prefix for PTDE
  (`steamapps/compatdata/211420`). If the game has never been started through
  Steam that prefix doesn't exist yet: launch it once, then run the installer
  again.
- **Case-sensitive filesystems:** archives are unpacked in a fixed order (so the
  result is byte-identical to a Windows install) and the mod's files are merged
  into the existing folders whatever their case.
- **Linux launcher:** besides the Windows `mctde_launcher.exe`, the install puts
  the native Linux mctde launcher (`mctde-launcher` + `mctde-launcher-cli`) in
  the `DATA` folder. It comes from `mctde-launcher-linux.zip` on the
  mctde-Launcher release named by `latest.txt`'s `mctde-launcher=` line, checked
  against its published SHA-256. If it can't be fetched, the install carries on
  with just the Windows one. CLI: `mctde-installer install-linux-launcher <DATA dir>`.
- **Playing:** after an install, **Play** opens the native Linux launcher if
  it's installed (or runs its CLI's `play` where Qt 6 isn't available).
  Otherwise it starts that copy's `mctde_launcher.exe` through your Steam Proton (the one that created PTDE's
  prefix, else Proton Experimental, else the newest installed), in PTDE's Proton
  prefix. Steam must be running (it's started if not); it shows the game as
  Spacewar (app 480). Proton's output goes to `~/.cache/mctde-installer/proton.log`.
  From the command line: `mctde-installer play <DATA dir>` (waits until the game
  exits).
- **Add to Steam:** after an install the installer offers to add the copy to
  your Steam library as a non-Steam game ("DARK SOULS™: Prepare To Die Edition
  (mctde)"), the way Quiver
  Launcher does it. The shortcut runs the AppImage with `play <DATA dir>`, which
  starts the game through Proton and stays open until it exits, so Steam,
  Big Picture and Game Mode show it as running. Steam rewrites
  `shortcuts.vdf` when it exits, so the file is only changed while Steam is
  closed: if Steam is running, a small background worker waits for Steam to exit
  and adds the shortcut then (restart Steam once). Other shortcuts are kept, an
  existing entry for the same copy is updated, and the original file is backed
  up as `shortcuts.vdf.mctde-backup`. From the command line:
  `mctde-installer add-to-steam <DATA dir>`. Keep the AppImage where it is
  afterwards; the shortcut points at it.
- **Releases and updates:** Windows and Linux share one version number and one
  release per version (see [Releases](#releases)). On start the installer checks
  the `mctde-installer=` line in mctde-Link's `latest.txt` and, if a newer
  version is listed, replaces the AppImage with the one from that release's
  `mctde-Installer-linux.zip`.
- **Compatibility:** the AppImage runs on any x86_64 distro with glibc 2.31 or
  newer: SteamOS / Steam Deck, Ubuntu 20.04+, Debian 11+, Linux Mint 20+,
  Pop!\_OS, Fedora 32+, openSUSE, Arch and its derivatives, and so on. It is
  built in Valve's Steam Runtime (sniper) SDK, the same base Steam games are
  built against, and needs no FUSE or libfuse2. The GUI works on X11 and
  Wayland; the CLI needs no display.

The command-line tool is inside the AppImage too: run it with arguments, e.g.
`./mctde-Installer-x86_64.AppImage detectall` or
`./mctde-Installer-x86_64.AppImage fullinstall <DATA dir>`.

### Under Wine / Proton

The Windows `mctde-Installer.exe` also works under Wine or Proton. On top of the
Windows Steam registry lookup it finds a native Linux Steam the same way and
turns the Unix paths into the drive Wine maps them to (normally `Z:`, or
`\\?\unix\...` when no drive maps `/`). Wine does not pass `$HOME` through, so
the home folder comes from `WINEHOMEDIR` (then `HOME`, `/home/$USER`, and the
folders in `/home`). Display settings go to the installer's own prefix and to
Steam's Proton prefix for PTDE.

### Diagnostics

If nothing is found, the start of the log (and `mctde-installer detectall`)
shows what detection saw: platform (native or Wine version), `HOME` /
`WINEHOMEDIR`, the homes, each Steam root, Proton prefix and library it checked.

## Build

Windows (MSVC): `build.bat` (CLI) and `build_gui.bat` (GUI), or

```
cmake -S . -B build
cmake --build build --config Release
```

Linux (native): needs g++ (C++17), libcurl and SDL3 (3.2 or newer).

```
sudo pacman -S --needed base-devel curl sdl3     # Arch
./build_linux.sh                                 # build/linux/mctde-installer(-gui), for this machine
```

**Release AppImage:** `./make_appimage.sh` (needs Docker or Podman, e.g.
`sudo pacman -S --needed podman`). It builds inside Valve's Steam Runtime sniper
SDK container (glibc 2.31), so the AppImage runs on every distro listed above
whatever distro you build it on. It builds SDL3 there and fetches
[linuxdeploy](https://github.com/linuxdeploy/linuxdeploy),
[appimagetool](https://github.com/AppImage/appimagetool) and the AppImage
runtime itself (cached in `build/portable/`), then fails the build if anything in
the AppImage needs a glibc newer than 2.31. Output:
`build/linux/mctde-Installer-x86_64.AppImage`. Always release this build: one
made with `MCTDE_NATIVE=1 ./make_appimage.sh` only runs on distros as new as the
build machine. The Windows exe can also be cross-built with MinGW
(`x86_64-w64-mingw32-g++`).

## Releases

Every version is one GitHub release, tagged `vX.Y.Z`, carrying a zip per
platform. Both builds update themselves from it: each one reads the shared
`mctde-installer=` version from mctde-Link's `latest.txt` and downloads the zip
for the OS it runs on.

| Asset | Contents |
|---|---|
| `mctde-Installer-windows.zip` | `mctde-Installer.exe`, README, LICENSE, CREDITS |
| `mctde-Installer-linux.zip` | `mctde-Installer-x86_64.AppImage`, README, LICENSE, CREDITS |
| `*.zip.sha256` | SHA-256 of each zip; the self-update refuses a zip that doesn't match |
| `mctde-Installer.exe` | the bare exe, for Windows installers older than 0.4.0 |

To cut a release:

1. Bump `kInstallerVersion` in `src/Update.cpp`.
2. Build both: `build_gui.bat` on Windows (`build/mctde-Installer.exe`) and
   `./make_appimage.sh` (`build/linux/mctde-Installer-x86_64.AppImage`).
3. `tools/package_release.sh X.Y.Z` writes everything to `dist/vX.Y.Z/` and
   prints the remaining steps: publish the `vX.Y.Z` release (marked latest), a
   `linux-vX.Y.Z` release with the bare AppImage for Linux installers older than
   0.4.0, and only then set `mctde-installer=` and `mctde-installer-linux=` in
   mctde-Link's `latest.txt`.

## Usage

The GUI is the normal way in. The CLI exposes each step:

```
mctde-installer fullinstall <DATA dir> [namelist]   full flow: unpack, patch, download + install the mod, Link, launcher
mctde-installer install     <DATA dir> [namelist]   unpack + patch only (no downloads)
mctde-installer unpack      <DATA dir> <out> [nl]    just unpack dvdbnd + nested archives
mctde-installer patchexe    <inExe> <outExe>         just patch a clean DARKSOULS.exe
```

Diagnostics: `bhd5`, `bnd3`, `dcx`, `extract`, `extractzip`, `nested`, `detect`,
`detectall`, `download`, `gdrive`, `diff`.

## Status

**Complete and validated against a real PTDE install + a reference unpack.**

- Two-level unpack (dvdbnd → loose, then inner `tpfbhd`/`hkxbhd`/`chrtpf` →
  `tpf`/`hkx`) reproduces the reference tree exactly: 13,302/13,302
  dvdbnd-derived files, names and content (SHA-256) verified.
- The exe patch matches the known UDSFM-patched `DARKSOULS.exe` byte-for-byte.
- The one header missing from the game files (`c4110.chrtpfbhd`) is reconstructed.

## Credits

`mctde-Installer` is a C++ take on UnpackDarkSoulsForModding, built up from the
file formats. It stands on a lot of reverse-engineering work by the Dark Souls
modding community:

- **[HotPocketRemix](https://github.com/HotPocketRemix)**:
  [UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding),
  the original this follows, and the reconstructed `c4110.chrtpfbhd` header (the
  one texture header missing from the game files).
- **[Meowmaritus](https://github.com/Meowmaritus)** &
  **[Wulf2k](https://github.com/Wulf2k)**: reverse-engineered the dvdbnd filename
  list, embedded here as the namelist.
- **[TKGP / JKAnderson](https://github.com/JKAnderson)**:
  [SoulsFormats](https://github.com/JKAnderson/SoulsFormats), UXM, and Yabber,
  where the BHD5 / DCX / BND3 format details come from.
- **Burton Radons**: BND3 format reverse-engineering.
- **[Sean Pesce](https://github.com/SeanPesce)**:
  [DsGameFiles](https://github.com/SeanPesce/Dark-Souls-Game-Files-Lib)
  (`FileList.h`), where the namelist was collated.
- **[Rich Geldreich](https://github.com/richgel999)**:
  [miniz](https://github.com/richgel999/miniz), public domain (Unlicense),
  vendored in `third_party/`.
- **[Omar Cornut](https://github.com/ocornut)**:
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT), vendored in
  `third_party/imgui/` for the Linux GUI, with its bundled Roboto and Cousine
  fonts (Apache License 2.0).
- **[SDL](https://www.libsdl.org/)** (zlib license) and
  **[libcurl](https://curl.se/)** (curl license), used by the Linux build.

Original game by **FromSoftware** and **Bandai Namco**.

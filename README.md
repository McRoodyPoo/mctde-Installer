# mctde-Installer

`mctde-Installer` is a native C++ unpacker and installer for the **mctde** Dark
Souls: Prepare to Die Edition (PTDE) mod setup. It does in C++ what
[UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding)
(UDSFM) does: it unpacks the game's `dvdbnd` archives into loose files and patches
`DARKSOULS.exe` to load them, so the engine reads moddable loose files instead of
the packed archives. Being native C++ lets it ship as a single tool inside the
mctde toolchain.

## What it does

`install` runs the whole flow in one shot — back up the originals, unpack, patch
the exe, delete the archives — and is safe to re-run. Under the hood that's three
steps:

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

It's idempotent: originals are backed up to `mctde-backup/`, and a
`.mctde-unpacked` sentinel makes re-runs no-ops.

## File formats (PTDE)

- **BHD5** — header table. LE. 24-byte header, then buckets `{u32 count, u32
  offset}`, then 16-byte records `{u32 nameHash, u32 size, u64 offset}`.
- **BDT** — raw data blob; slice `size` bytes at `offset`.
- **DCX** — compression container. Big-endian header; DS1 uses `DFLT` = a zlib
  (RFC 1950) payload located via the `DCS` (sizes) and `DCA` (payload) markers.
- **Name hash** — DS1 path hash: `h = h*37 + c` over the lowercased,
  forward-slash path. A namelist maps hashes back to real paths.

## Layout

```
src/            core decoders + install flow
  BinaryReader.h  little-endian buffer reader
  Bhd5.{h,cpp}    BHD5 header parser
  Dcx.{h,cpp}     DCX detect + zlib inflate
third_party/
  miniz.{c,h}     public-domain (Unlicense) zlib implementation
data/           generated namelist(s)
```

## Build

```
cmake -S . -B build
cmake --build build --config Release
```

## Usage

```
mctde-installer install <DATA dir> [namelist]   full flow (backup, unpack, patch, cleanup)
mctde-installer unpack  <DATA dir> <out> [nl]    just unpack dvdbnd + nested archives
mctde-installer patchexe <inExe> <outExe>        just patch a clean DARKSOULS.exe
```

Diagnostics: `bhd5`, `hashes`, `hashstr`, `cover`, `dcx`, `extract`, `nested`,
`bnd3`, `diff`.

## Status

**Complete and validated against a real PTDE install + a reference unpack.**

- Two-level unpack (dvdbnd → loose, then inner `tpfbhd`/`hkxbhd`/`chrtpf` →
  `tpf`/`hkx`) reproduces the reference tree exactly — 13,302/13,302
  dvdbnd-derived files, names and content (SHA-256) verified.
- The exe patch matches the known UDSFM-patched `DARKSOULS.exe` byte-for-byte.
- The one header missing from the game files (`c4110.chrtpfbhd`) is reconstructed.

## Credits

`mctde-Installer` is a C++ take on UnpackDarkSoulsForModding, built up from the
file formats. It stands on a lot of reverse-engineering work by the Dark Souls
modding community:

- **HotPocketRemix** — [UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding),
  the original this follows, and the reconstructed `c4110.chrtpfbhd` header (the
  one texture header missing from the game files).
- **Meowmaritus** & **Wulf2k** — reverse-engineered the dvdbnd filename list,
  embedded here as the namelist.
- **TKGP / JKAnderson** — [SoulsFormats](https://github.com/JKAnderson/SoulsFormats),
  UXM, and Yabber, where the BHD5 / DCX / BND3 format details come from.
- **Burton Radons** — BND3 format reverse-engineering.
- **Sean Pesce** — DsGameFiles (`FileList.h`), where the namelist was collated.
- **Rich Geldreich** — [miniz](https://github.com/richgel999/miniz), public domain
  (Unlicense), vendored in `third_party/`.

Original game by **FromSoftware** and **Bandai Namco**.

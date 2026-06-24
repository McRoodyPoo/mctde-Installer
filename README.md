# mctde-Installer

A clean-room, native C++ installer for the **mctde** Dark Souls: Prepare to Die
Edition (PTDE) mod. It unpacks the game's `dvdbnd` archives into loose files and
patches `DARKSOULS.exe` to load them — the same job
[UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding)
(UDSFM) does, reimplemented from the file-format spec so it carries no third-party
licensing baggage and can be embedded directly in the mctde toolchain.

> **Legal stance.** This tool ships **no game files and no decryption keys**. PTDE's
> `dvdbnd` archives are *not* encrypted (unlike DS2/DS3), so no keys are involved at
> all. It only transforms the archives already present in the user's own legally
> owned installation, in place. Don't redistribute unpacked game assets.

## What "unpacking" actually means

Reverse-engineered from UDSFM, the operation is three coordinated steps:

1. **Unpack** `dvdbnd0-3.bhd5` / `.bdt` into 18 top-level directories:
   `chr event facegen font map menu msg mtd obj other param paramdef parts remo
   script sfx shader sound`. DCX-compressed entries are decompressed on the way out.
2. **Patch `DARKSOULS.exe`** so the engine reads loose files:
   - UTF-16LE virtual-drive string rewrites: `dvdbnd0:` → `dvdroot:`,
     `tpfbnd:` → `map:/tx` (equal length, patched in place).
   - A 2-byte code patch (`EB 12`) that disables the DCX-decompression path so the
     engine treats the loose files as uncompressed.
3. **Delete the archives** so the engine falls back to the loose files.

The exe is verified by **SHA-256** before patching, so an unknown/wrong build is
never touched.

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

`install` is idempotent: it backs up the originals to `mctde-backup/`, unpacks
in place, patches the exe, deletes the archives, and drops a `.mctde-unpacked`
sentinel so re-runs are no-ops.

## Status

**Complete and validated against a real PTDE install + a reference unpack.**
- Two-level unpack (dvdbnd → loose, then inner `tpfbhd`/`hkxbhd`/`chrtpf` →
  `tpf`/`hkx`) reproduces the reference tree exactly — 13,302/13,302
  dvdbnd-derived files, names and content (SHA-256) verified.
- The exe patch reproduces the known UDSFM-patched `DARKSOULS.exe` byte-for-byte
  (SHA-gated to the known clean build; the input is never modified).
- The one missing-from-game header (`c4110.chrtpfbhd`) is reconstructed.

Polish remaining: the namelist is currently a sibling `data/` file; for a
single-file distributable it should be embedded or resolved relative to the
installer exe rather than the working directory.

## Credits

Format knowledge derived from the work of HotPocketRemix (UDSFM), TKGP/JKAnderson
(SoulsFormats, UXM, Yabber), and the wider Dark Souls modding community. No code
from those projects is copied here.

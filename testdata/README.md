# testdata/ — drop a *packed* PTDE install here for validation

These files are **gitignored** (`*.bhd5`, `*.bdt`, `DARKSOULS.exe`) — they will
never be committed. They're only used locally to validate the unpacker and the
exe patcher against real data.

## What to drop here

From a **fresh / un-modded** Dark Souls: Prepare to Die Edition install
(Steam: `...\steamapps\common\Dark Souls Prepare to Die Edition\DATA\`):

Required for the unpacker (at minimum one pair; ideally all four):
- `dvdbnd0.bhd5` + `dvdbnd0.bdt`
- `dvdbnd1.bhd5` + `dvdbnd1.bdt`
- `dvdbnd2.bhd5` + `dvdbnd2.bdt`
- `dvdbnd3.bhd5` + `dvdbnd3.bdt`

Required for the exe patcher:
- `DARKSOULS.exe`  ← a **clean, un-patched** copy (not one already run through UDSFM)

## How I'll use it

1. `mctde-installer bhd5 testdata/dvdbnd0.bhd5` — validate BHD5 parsing against real records.
2. Generate the namelist + extract a few real DCX files to confirm zlib `DFLT` decode.
3. Diff the unpacker's output against your already-unpacked `RAW PTDE - mctde\DATA`.
4. SHA-256 + pattern-locate the patch sites in the clean exe.

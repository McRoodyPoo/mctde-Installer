# Credits & Notices

mctde-Installer is a **clean-room reimplementation** — all of its source code is
original work. It relies on file-format knowledge and a small amount of
game-derived *reference data* from the Dark Souls modding community, credited
below. **No source code from those projects is copied.**

## Format knowledge & reference data
- **HotPocketRemix** — [UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding),
  the original unpacker this reimplements, and the reconstructed
  `c4110.chrtpfbhd` header (the one texture header missing from the game files).
- **Meowmaritus** and **Wulf2k** — reverse-engineered the dvdbnd filename list
  that is embedded here as the namelist.
- **TKGP / JKAnderson** — [SoulsFormats](https://github.com/JKAnderson/SoulsFormats),
  UXM, and Yabber, from which the BHD5 / DCX / BND3 format details were learned.
- **Burton Radons** — BND3 format reverse-engineering.
- **Sean Pesce** — DsGameFiles (`FileList.h`), from which the namelist was collated.

## Bundled third-party code
- **miniz** by Rich Geldreich — public domain (Unlicense); see `third_party/miniz.*`.

## Legal
This tool ships **no game files and no decryption keys** (PTDE archives are not
encrypted). It only transforms the user's own legally-owned installation in
place. Do not redistribute unpacked game assets.

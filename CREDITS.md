# Credits

mctde-Installer is a C++ reimplementation of UnpackDarkSoulsForModding, built up
from the file formats. It stands on a lot of reverse-engineering work by the
Dark Souls modding community — thanks to everyone below:

- **HotPocketRemix** — [UnpackDarkSoulsForModding](https://github.com/HotPocketRemix/UnpackDarkSoulsForModding),
  the original this is based on, and the reconstructed `c4110.chrtpfbhd` header
  (the one texture header missing from the game files).
- **Meowmaritus** & **Wulf2k** — reverse-engineered the dvdbnd filename list,
  embedded here as the namelist.
- **TKGP / JKAnderson** — [SoulsFormats](https://github.com/JKAnderson/SoulsFormats),
  UXM, and Yabber, where the BHD5 / DCX / BND3 format details come from.
- **Burton Radons** — BND3 format reverse-engineering.
- **Sean Pesce** — DsGameFiles (`FileList.h`), where the namelist was collated.
- **Rich Geldreich** — [miniz](https://github.com/richgel999/miniz), public
  domain (Unlicense), vendored in `third_party/`.

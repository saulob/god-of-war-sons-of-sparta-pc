# Launcher header art

The header draws a gradient, and `header-art.png` over it when that file sits in `assets/launcher` beside the
executable. This repository ships no header art: the launcher keeps the gradient alone until approved Sons of Sparta art
exists. The art is placed at the header's full height, flush with its top, right and bottom, and a narrow window cuts it
from the left (`launcher/header_art.h`). The file is 8.89:1 (3840x432) RGBA with a horizontal alpha mask that lets the
gradient show through on the left, so the title side stays dark. Without the file, or when it cannot be decoded, the
gradient stays alone.

To add art: put `header-art.png` in this folder and list it with `sponsor-heart.png` in the asset copy loop of the
top-level `CMakeLists.txt`.

# Sponsor heart

`sponsor-heart.svg` is the source of truth for the heart in the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is the runtime asset, a white 64x64 rendering of it
with a transparent background. Do not edit it by hand; regenerate it after every SVG change, from the repository root
with rsvg-convert (MSYS2 `mingw-w64-clang-x86_64-librsvg`):

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```

The launcher draws it before the button label, as a square slightly taller than the label's text.

# Page icons

`icons/` holds the sidebar's nine SVGs, the source of truth, with one file per page and no rendered copies. They are
compiled into the launcher (`launcher/page_icons.cpp`), so they ship in no package as loose files. `icons/SOURCES.md`
records where each one came from and its license.

# Steam Library Artwork

The port is added to Steam as a **non-Steam game**. No game files or artwork are bundled in the release packages.

## Available now

- **Game icon:** `packaging/icons/app-icon-256.png` and `packaging/icons/app-icon.ico` (rendered from the
  project's CC0 `app-icon.svg`).

## Requires approved source artwork

These assets are not generated as placeholders because no approved high-resolution Sons of Sparta artwork is
present in this repository:

| Steam slot | Required size | Status |
|---|---|---|
| Vertical library cover | 600 x 900 PNG | Awaiting approved source art |
| Library header | 920 x 430 PNG | Awaiting approved source art |
| Library hero | 3840 x 1240 PNG | Awaiting approved source art |
| Transparent game logo | High-resolution transparent PNG | Awaiting approved source art |

Preserve aspect ratios; do not stretch or upscale a small source. Put approved files under
`dist/steam-artwork/` and keep them out of the release ZIP and AppImage.

## Adding the port to Steam

1. **Games → Add a Non-Steam Game to My Library...**
2. Select `sons_of_sparta.exe` (Windows) or the `.AppImage` (Linux / Steam Deck Desktop Mode).
3. On Linux, keep the AppImage executable and in a writable folder; do not force Proton.
4. Right-click the library entry and use **Manage → Set custom artwork**, **Set custom background**, and
   **Set custom logo** to assign the files above.

// The Xbox prompt set of the game's Addressables icon bundle. InputOverseer reads its prompt icons and its text sprite atlas from
// the copy of the icon data in this bundle, so the Xbox set is completed here: the left-stick direction tokens (LASUP, LASDOWN,
// LASLEFT, LASRIGHT) and the interaction marker's special icons (Button South/East/North Mask, Button North Mask Filled).
// The game's bundle is only read; a patched copy is written elsewhere and used in its place through an exact path alias.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "controller_icons.h"

namespace sos {

struct BundleGlyph {
    std::uint32_t index = 0;
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    std::int64_t sprite = 0;
};

struct BundleCharacter {
    std::string name;
    std::uint32_t glyph = 0, hash = 0;
};

struct BundleSprite {
    std::string name;
    float rect[4] = {0, 0, 0, 0};
    std::int64_t texture = 0;
};

struct BundleAtlas {
    std::vector<BundleCharacter> characters;
    std::vector<BundleGlyph> glyphs;
    std::int64_t sheet = 0;
};

// A bundle texture: its size and the tiled texels read from the bundle's resource node.
struct BundleTexture {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> tiled;
};

// What the bundle's prompt data holds, read for validation.
struct BundlePrompts {
    using Special = std::vector<std::pair<std::string, std::int64_t>>;
    Special xboxSpecial, ps5Special;
    BundleAtlas xbox, ps5;
    std::map<std::int64_t, BundleSprite> sprites;  // every Sprite of the bundle
    std::vector<std::int64_t> xboxButtons;         // the Xbox South, North, East and West sprites
    std::vector<std::int64_t> preload;             // the icon data's preload list, local objects only
    std::int64_t iconData = 0, xboxAtlas = 0, circle = 0;
    BundleTexture xboxSheet, ps5Sheet;
};

// False when the file is not the analysed bundle.
bool ReadBundlePrompts(const std::filesystem::path& bundle, BundlePrompts& prompts);

// kComplete: the Xbox set has all eight tokens and everything they refer to; kMissing: the analysed bundle without them;
// kPartial: some of them, which is never patched further; kUnrecognised: not the analysed bundle.
IconSetState InspectIconBundle(const std::filesystem::path& bundle, std::string* why = nullptr);

// True when the bundle is the analysed one with the complete set and its new glyph cells hold artwork.
bool HasBundleIcons(const std::filesystem::path& bundle);

// Writes a complete corrected copy of the bundle to `out` when the source lacks the set. Nothing is written for any other
// result. `report` receives one line per step, for the log.
IconPatchResult PatchIconBundle(const std::filesystem::path& source, const std::filesystem::path& out, std::string& report);

}  // namespace sos

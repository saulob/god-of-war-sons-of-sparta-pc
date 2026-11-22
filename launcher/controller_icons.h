// The Xbox prompt set of the game ships no left-stick directions: the text tokens LASUP, LASDOWN, LASLEFT and LASRIGHT have no
// Xbox glyph, and it lacks the special icons (Button South/East/North Mask) the interaction marker and the confirmation panels
// ask for. Prepared copies of resources.assets, resources.assets.resS and the Addressables icon bundle, which InputOverseer reads
// its icons from, add them from the game's own artwork; the game's files are only read, and the copies are used in their place
// through exact path aliases.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace sos {

// Changes when the prepared files would differ, so copies made by an older version are made again. The launcher's own size
// or version is not part of the cache identity.
inline constexpr int kControllerIconsPatchVersion = 2;

inline constexpr char kIconsAssetsName[] = "resources.assets";
inline constexpr char kIconsStreamName[] = "resources.assets.resS";
inline constexpr char kIconsGuestAssets[] = "/app0/Media/resources.assets";
inline constexpr char kIconsGuestStream[] = "/app0/Media/resources.assets.resS";
inline constexpr char kIconsBundleName[] = "0342107720d621839faadc2c6c3deb69.bundle";
inline constexpr char kIconsBundleFolder[] = "StreamingAssets/aa/PS4";
inline constexpr char kIconsGuestBundle[] = "/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle";

// The files of one icon source, always used together. An empty bundle path leaves the icon bundle out.
struct IconFiles {
    std::filesystem::path assets;
    std::filesystem::path streamed;
    std::filesystem::path bundle;
};

enum class IconSetState { kMissing, kComplete, kPartial, kUnrecognised };

// Reads the files only. kComplete: the Xbox prompt set has all four directions, the interaction icons and everything they refer
// to; kMissing: the analysed game files without them; kPartial: some of them, which is never patched further.
IconSetState InspectIconSet(const IconFiles& files, std::string* why = nullptr);

// What the Xbox prompts hold: the special icon map, the glyphs of the sprite asset and where the sprite sheet's pixels are.
struct XboxPromptGlyph {
    std::string name;
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    std::int64_t sprite = 0;
};

struct XboxPrompts {
    std::vector<std::pair<std::string, std::int64_t>> special;  // token, sprite object
    std::vector<XboxPromptGlyph> glyphs;
    std::uint32_t sheetWidth = 0, sheetHeight = 0;
    std::uint64_t sheetOffset = 0;  // of the pixels in resources.assets.resS
};

// False when the assets are not the analysed game data.
bool ReadXboxPrompts(const IconFiles& files, XboxPrompts& prompts);

// True when the files are the analysed game data with the complete set, with the glyph pixels in place.
bool HasControllerIcons(const IconFiles& files);

enum class IconPatchResult { kPrepared, kComplete, kUnsupported, kFailed };

// Writes a complete corrected pair to `out` when the source lacks the directions. Nothing is written for any other result.
// `report` receives one line per step, for the log.
IconPatchResult PatchControllerIcons(const IconFiles& source, const IconFiles& out, std::string& report);

enum class IconPlanKind { kUseOriginal, kUsePrepared, kPrepare };

struct IconPlan {
    IconPlanKind kind = IconPlanKind::kUseOriginal;
    IconFiles files;   // the files the game reads, all three together; empty for kUseOriginal and kPrepare
    std::string note;  // for the log
};

// The game's Media folder and the folder the prepared copies and their stamp live in. Cheap: nothing is written but a stamp
// that records a source that already holds the complete set; kPrepare only when preparing is real work.
IconPlan PlanControllerIcons(const std::filesystem::path& media, const std::filesystem::path& cache);

// Runs the preparation a kPrepare plan asked for, then plans again. `report` receives the log lines.
IconPlan PrepareControllerIcons(const std::filesystem::path& media, const std::filesystem::path& cache, std::string& report);

}  // namespace sos

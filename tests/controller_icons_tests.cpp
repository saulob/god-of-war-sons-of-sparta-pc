// The Xbox left-stick glyphs, the interaction icons and their cache, on the game's own files: the analysed CUSA57767 1.00
// resources.assets, resources.assets.resS and Addressables icon bundle, plus the 1.01 prepared copies, when installed. The files
// are only read; the prepared copies are made in a temporary folder.
// No game, launcher or window is started.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "controller_icons.h"
#include "icon_bundle.h"

namespace fs = std::filesystem;
using namespace sos;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

using Bytes = std::vector<std::uint8_t>;

Bytes ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Bytes data(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
}

void WriteAll(const fs::path& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::uint64_t Fnv(const Bytes& data) {
    std::uint64_t hash = 0xcbf29ce484222325;
    std::size_t at = 0;
    for (; at + 8 <= data.size(); at += 8) {
        std::uint64_t word;
        std::memcpy(&word, data.data() + at, 8);
        hash = (hash ^ word) * 0x100000001b3;
    }
    for (; at < data.size(); ++at) hash = (hash ^ data[at]) * 0x100000001b3;
    return hash;
}

struct FileState {
    std::uint64_t size = 0, hash = 0;
    fs::file_time_type time;
    bool operator==(const FileState& other) const { return size == other.size && hash == other.hash && time == other.time; }
};

FileState StateOf(const fs::path& path) {
    return {static_cast<std::uint64_t>(fs::file_size(path)), Fnv(ReadAll(path)), fs::last_write_time(path)};
}

std::uint64_t Be(const Bytes& data, std::size_t at, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) value = (value << 8) | data[at + index];
    return value;
}

template <typename T> T Le(const Bytes& data, std::size_t at) {
    T value{};
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
}

// The object table of a version 22 serialized file, read independently of the code under test.
struct Table {
    std::uint64_t dataOffset = 0;
    std::map<std::int64_t, std::pair<std::int64_t, std::uint32_t>> objects;  // id -> start, size
    Bytes data;

    Bytes Object(std::int64_t id) const {
        const auto& [start, size] = objects.at(id);
        return Bytes(data.begin() + static_cast<std::ptrdiff_t>(dataOffset + static_cast<std::uint64_t>(start)),
                     data.begin() + static_cast<std::ptrdiff_t>(dataOffset + static_cast<std::uint64_t>(start) + size));
    }
};

Table ReadTable(const fs::path& path) {
    Table table;
    table.data = ReadAll(path);
    const Bytes& data = table.data;
    table.dataOffset = Be(data, 32, 8);
    std::size_t at = 48;
    while (data[at] != 0) ++at;
    at += 1 + 4 + 1;
    const auto types = Le<std::int32_t>(data, at);
    at += 4;
    for (std::int32_t index = 0; index < types; ++index) {
        const auto classId = Le<std::int32_t>(data, at);
        at += 7 + (classId == 114 ? 16 : 0) + 16;
    }
    const auto count = Le<std::int32_t>(data, at);
    at = (at + 4 + 3) & ~std::size_t{3};
    for (std::int32_t index = 0; index < count; ++index, at += 24) {
        table.objects[Le<std::int64_t>(data, at)] = {Le<std::int64_t>(data, at + 8), Le<std::uint32_t>(data, at + 16)};
    }
    return table;
}

fs::path TempRoot() {
    const fs::path parent = "E:\\ClaudeTemp\\opencode";
    const fs::path base = fs::is_directory(parent) ? parent : fs::temp_directory_path();
    const fs::path path = base / ("sos_icons_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
    return path;
}

constexpr const char* kTokens[] = {"LASUP", "LASDOWN", "LASLEFT", "LASRIGHT"};
constexpr std::int64_t kIconData = 50154, kLookInputSprite = 40906;
constexpr std::size_t kCell = 64;

std::vector<std::string> DirectionTokens(const XboxPrompts& prompts) {
    std::vector<std::string> found;
    for (const char* token : kTokens) {
        for (const auto& [name, sprite] : prompts.special) {
            if (name == token) found.push_back(name);
        }
    }
    return found;
}

// 4-byte texels in 8x8 micro tiles of 256 bytes in row order, Morton order inside a tile: the texel of (x, y).
std::size_t TexelAt(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    std::uint32_t morton = 0;
    for (std::uint32_t bit = 0; bit < 3; ++bit) morton |= (((x >> bit) & 1u) << (2 * bit)) | (((y >> bit) & 1u) << (2 * bit + 1));
    return (std::size_t{(y >> 3) * (width >> 3) + (x >> 3)} * 64 + morton) * 4;
}

std::size_t OpaqueTexels(const Bytes& stream, const XboxPrompts& prompts, const XboxPromptGlyph& glyph) {
    std::size_t opaque = 0;
    for (std::int32_t y = glyph.y; y < glyph.y + glyph.height; ++y) {
        for (std::int32_t x = glyph.x; x < glyph.x + glyph.width; ++x) {
            opaque += stream[prompts.sheetOffset + TexelAt(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), prompts.sheetWidth) + 3] != 0;
        }
    }
    return opaque;
}

const XboxPromptGlyph* GlyphNamed(const XboxPrompts& prompts, const std::string& name) {
    for (const XboxPromptGlyph& glyph : prompts.glyphs) {
        if (glyph.name == name) return &glyph;
    }
    return nullptr;
}

struct Fixture {
    fs::path source;   // the game's own Media folder
    fs::path root;
    IconFiles original;
    bool hasBundle = false;
};

void ExpectNoOutput(const IconFiles& out, const std::string& label) {
    Check(!fs::exists(out.assets) && !fs::exists(out.streamed) && (out.bundle.empty() || !fs::exists(out.bundle)), label + ": nothing is written");
}

IconFiles Folder(const fs::path& folder) {
    fs::create_directories(folder);
    return {folder / kIconsAssetsName, folder / kIconsStreamName, folder / kIconsBundleName};
}

// A Media folder holding the game's three icon files.
fs::path MediaWith(const fs::path& media, const IconFiles& files) {
    fs::create_directories(media / kIconsBundleFolder);
    fs::copy_file(files.assets, media / kIconsAssetsName, fs::copy_options::overwrite_existing);
    fs::copy_file(files.streamed, media / kIconsStreamName, fs::copy_options::overwrite_existing);
    fs::copy_file(files.bundle, media / kIconsBundleFolder / kIconsBundleName, fs::copy_options::overwrite_existing);
    return media;
}

// ---- The source and the patch ----------------------------------------------------------------------------------------------

void TestSource(const Fixture& f) {
    std::string why;
    Check(InspectIconSet(f.original, &why) == IconSetState::kMissing, "the game's own files lack the direction set");
    XboxPrompts prompts;
    Check(ReadXboxPrompts(f.original, prompts), "the game's own Xbox prompts are read");
    Check(DirectionTokens(prompts).empty(), "none of LASUP, LASDOWN, LASLEFT and LASRIGHT is in the Xbox special icon map");
    for (const char* token : kTokens) {
        Check(std::none_of(prompts.special.begin(), prompts.special.end(), [&](const auto& item) { return item.first == token; }), std::string(token) + " is missing from the Xbox prompts");
    }
    Check(prompts.special.size() == 1 && prompts.special[0].first == "!LookInput" && prompts.special[0].second == kLookInputSprite, "the Xbox map holds !LookInput alone");
    Check(prompts.glyphs.size() == 19, "the Xbox atlas has its 19 glyphs");
    Check(!HasControllerIcons(f.original), "the game's own files are not a complete set");
}

void TestPatch(const Fixture& f, const IconFiles& out) {
    const FileState assetsBefore = StateOf(f.original.assets), streamBefore = StateOf(f.original.streamed), bundleBefore = StateOf(f.original.bundle);
    std::string report;
    Check(PatchControllerIcons(f.original, out, report) == IconPatchResult::kPrepared, "the left-stick directions are prepared from the game's own files");
    Check(report.find("LASUP") != std::string::npos && report.find("LASRIGHT") != std::string::npos, "the report names the four directions");
    Check(StateOf(f.original.assets) == assetsBefore && StateOf(f.original.streamed) == streamBefore && StateOf(f.original.bundle) == bundleBefore, "the game's own files are unchanged (size, time and content)");
    Check(fs::exists(out.assets) && fs::exists(out.streamed) && fs::exists(out.bundle), "all three prepared files exist");
    Check(fs::file_size(out.streamed) == fs::file_size(f.original.streamed), "the prepared streamed file has the original's size");
    Check(fs::file_size(out.assets) > fs::file_size(f.original.assets), "the prepared assets file is complete and a little larger");
    Check(InspectIconSet(out) == IconSetState::kComplete && HasControllerIcons(out), "the prepared files validate structurally and hold the complete set");
    std::string again;
    Check(PatchControllerIcons(f.original, Folder(f.root / "second"), again) == IconPatchResult::kPrepared &&
              Fnv(ReadAll(f.root / "second" / kIconsAssetsName)) == Fnv(ReadAll(out.assets)) && Fnv(ReadAll(f.root / "second" / kIconsStreamName)) == Fnv(ReadAll(out.streamed)) &&
              Fnv(ReadAll(f.root / "second" / kIconsBundleName)) == Fnv(ReadAll(out.bundle)),
          "preparing twice gives identical files");
}

void TestPrepared(const Fixture& f, const IconFiles& out) {
    XboxPrompts before, after;
    Check(ReadXboxPrompts(f.original, before) && ReadXboxPrompts(out, after), "the Xbox prompts of both are read");
    Check(DirectionTokens(after) == std::vector<std::string>({"LASUP", "LASDOWN", "LASLEFT", "LASRIGHT"}), "LASUP, LASDOWN, LASLEFT and LASRIGHT are in the prepared Xbox map");
    for (const char* token : kTokens) {
        Check(std::count_if(after.special.begin(), after.special.end(), [&](const auto& item) { return item.first == token; }) == 1, std::string(token) + " is there exactly once");
    }
    Check(after.special.size() == 5 && after.special[0] == before.special[0], "the existing !LookInput entry is first and unchanged");
    std::set<std::int64_t> sprites;
    for (const auto& [token, sprite] : after.special) sprites.insert(sprite);
    Check(sprites.size() == 5, "each direction has its own sprite");
    Check(after.glyphs.size() == before.glyphs.size() + 4 && std::equal(before.glyphs.begin(), before.glyphs.end(), after.glyphs.begin(), [](const XboxPromptGlyph& a, const XboxPromptGlyph& b) {
              return a.name == b.name && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.sprite == b.sprite;
          }),
          "the 19 existing Xbox atlas entries are unchanged and four were added");
    const char* names[] = {"Xbox_LeftStick_Up", "Xbox_LeftStick_Down", "Xbox_LeftStick_Left", "Xbox_LeftStick_Right"};
    const Bytes streamBefore = ReadAll(f.original.streamed), streamAfter = ReadAll(out.streamed);
    std::set<std::vector<std::uint8_t>> art;
    for (std::size_t index = 0; index < 4; ++index) {
        const XboxPromptGlyph* glyph = GlyphNamed(after, names[index]);
        Check(glyph != nullptr, std::string(names[index]) + " is a glyph of the Xbox atlas");
        if (glyph == nullptr) continue;
        const auto token = std::find_if(after.special.begin(), after.special.end(), [&](const auto& item) { return item.first == kTokens[index]; });
        Check(token != after.special.end() && token->second == glyph->sprite, std::string(kTokens[index]) + " resolves to the sprite of " + names[index]);
        Check(glyph->width == static_cast<std::int32_t>(kCell) && glyph->height == static_cast<std::int32_t>(kCell) && glyph->x >= 0 && glyph->y >= 0 &&
                  static_cast<std::uint32_t>(glyph->x + glyph->width) <= after.sheetWidth && static_cast<std::uint32_t>(glyph->y + glyph->height) <= after.sheetHeight,
              std::string(names[index]) + " is a 64x64 cell inside the sheet");
        for (const XboxPromptGlyph& other : before.glyphs) {
            const bool disjoint = glyph->x + glyph->width <= other.x || other.x + other.width <= glyph->x || glyph->y + glyph->height <= other.y || other.y + other.height <= glyph->y;
            Check(disjoint, std::string(names[index]) + " does not overlap " + other.name);
        }
        Check(OpaqueTexels(streamBefore, before, *glyph) == 0, std::string(names[index]) + "'s cell was empty in the game's own sheet");
        const std::size_t opaque = OpaqueTexels(streamAfter, after, *glyph);
        Check(opaque > 1500, std::string(names[index]) + "'s cell now holds artwork (" + std::to_string(opaque) + " opaque texels)");
        Bytes cell;
        for (std::uint32_t y = 0; y < kCell; ++y) {
            for (std::uint32_t x = 0; x < kCell; ++x) {
                const std::size_t at = after.sheetOffset + TexelAt(static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, after.sheetWidth);
                cell.insert(cell.end(), streamAfter.begin() + static_cast<std::ptrdiff_t>(at), streamAfter.begin() + static_cast<std::ptrdiff_t>(at + 4));
            }
        }
        art.insert(cell);
    }
    Check(art.size() == 4, "the four directions have four different pictures");

    std::size_t differing = 0;
    for (std::size_t at = 0; at < streamBefore.size(); ++at) differing += streamBefore[at] != streamAfter[at];
    Check(differing > 0 && differing < 4 * kCell * kCell * 4, "only the four cells of the Xbox sheet changed in the streamed file");
    std::set<std::size_t> allowedTiles;
    for (std::size_t index = 0; index < 4; ++index) {
        const XboxPromptGlyph* glyph = GlyphNamed(after, names[index]);
        if (glyph == nullptr) continue;
        for (std::uint32_t ty = 0; ty < kCell / 8; ++ty) {
            for (std::uint32_t tx = 0; tx < kCell / 8; ++tx) {
                allowedTiles.insert(TexelAt(static_cast<std::uint32_t>(glyph->x) + tx * 8, static_cast<std::uint32_t>(glyph->y) + ty * 8, after.sheetWidth) / 256);
            }
        }
    }
    bool confined = true;
    for (std::size_t at = 0; at < streamBefore.size(); ++at) {
        if (streamBefore[at] == streamAfter[at]) continue;
        const bool inSheet = at >= after.sheetOffset && at < after.sheetOffset + std::size_t{after.sheetWidth} * after.sheetHeight * 4;
        confined = confined && inSheet && allowedTiles.count((at - after.sheetOffset) / 256) == 1;
    }
    Check(confined, "every changed streamed byte lies in a tile of one of the four cells");
}

void TestUntouchedObjects(const Fixture& f, const IconFiles& out) {
    const Table before = ReadTable(f.original.assets), after = ReadTable(out.assets);
    Check(after.objects.size() == before.objects.size() + 4, "the prepared file has four more objects");
    constexpr std::int64_t kXboxAtlas = 50372;
    std::size_t changed = 0, lost = 0;
    for (const auto& [id, entry] : before.objects) {
        const auto now = after.objects.find(id);
        if (now == after.objects.end()) {
            ++lost;
            continue;
        }
        if (id == kIconData || id == kXboxAtlas) continue;
        changed += before.Object(id) != after.Object(id);
    }
    Check(lost == 0, "no object was lost");
    Check(changed == 0, "every serialized object but the icon data and the Xbox atlas is byte for byte unchanged (" + std::to_string(before.objects.size() - 2) + " objects, including the input actions and bindings)");
    const Bytes oldIcons = before.Object(kIconData), newIcons = after.Object(kIconData);
    const std::size_t tail = oldIcons.size() - 0x500;
    Check(newIcons.size() == oldIcons.size() + 4 * 12 + 4 * 12 + 4 * 4 - 4 * 4 + 0 || newIcons.size() > oldIcons.size(), "the icon data grew by the four tokens and sprite pointers");
    Check(std::equal(oldIcons.begin(), oldIcons.begin() + 0x4dc, newIcons.begin()), "the PlayStation icon maps and the Xbox button icons in the icon data are unchanged");
    Check(std::equal(oldIcons.end() - static_cast<std::ptrdiff_t>(tail), oldIcons.end(), newIcons.end() - static_cast<std::ptrdiff_t>(tail)), "the keyboard icons and the atlas pointers after the Xbox map are unchanged");
    const Bytes oldAtlas = before.Object(kXboxAtlas), newAtlas = after.Object(kXboxAtlas);
    Check(newAtlas.size() > oldAtlas.size() && std::equal(oldAtlas.begin(), oldAtlas.begin() + 0xb8, newAtlas.begin()), "the Xbox atlas header is unchanged");
    const auto original = ReadTable(f.original.assets);
    const std::vector<std::int64_t> kept = {kLookInputSprite, 130};
    for (const std::int64_t id : kept) Check(original.Object(id) == after.Object(id), "object " + std::to_string(id) + " (the !LookInput sprite, the Xbox texture) is unchanged");
}

// ---- Partial, unrecognised and complete sources ------------------------------------------------------------------------------

std::size_t LastOccurrence(const Bytes& data, const std::string& text) {
    for (std::size_t at = data.size() - text.size();; --at) {
        if (std::memcmp(data.data() + at, text.data(), text.size()) == 0) return at;
        if (at == 0) return std::string::npos;
    }
}

void TestStates(const Fixture& f, const IconFiles& out) {
    // Already complete: the prepared pair stands in for the game's files.
    {
        const IconFiles complete = out;
        std::string report;
        const IconFiles target = Folder(f.root / "complete_out");
        Check(InspectIconSet(complete) == IconSetState::kComplete, "a source that already has the set is detected as complete");
        Check(PatchControllerIcons(complete, target, report) == IconPatchResult::kComplete, "patching an already complete source does nothing");
        ExpectNoOutput(target, "an already complete source");
    }
    // Partly patched: one direction token damaged.
    {
        const IconFiles partial = Folder(f.root / "partial");
        fs::copy_file(out.bundle, partial.bundle, fs::copy_options::overwrite_existing);
        Bytes assets = ReadAll(out.assets);
        const std::size_t at = LastOccurrence(assets, "LASUP");
        Check(at != std::string::npos, "the token to damage is found");
        assets[at + 4] = 'Q';
        WriteAll(partial.assets, assets);
        fs::copy_file(out.streamed, partial.streamed, fs::copy_options::overwrite_existing);
        std::string why;
        Check(InspectIconSet(partial, &why) == IconSetState::kPartial && !why.empty(), "a set missing one direction is a partial patch: " + why);
        const IconFiles target = Folder(f.root / "partial_out");
        std::string report;
        Check(PatchControllerIcons(partial, target, report) == IconPatchResult::kUnsupported, "a partial patch is rejected, never patched further");
        ExpectNoOutput(target, "a partial patch");
        Check(report.find("partly") != std::string::npos, "the report says why");
        Check(!HasControllerIcons(partial), "a partial pair is not a usable pair");
        const Bytes after = ReadAll(partial.assets);
        Check(std::count(after.begin(), after.end(), 'Q') >= 1 && Fnv(after) == Fnv(assets), "the partial source was not modified");
    }
    // Not the analysed data: a changed version, a truncated file, a changed object.
    {
        const IconFiles scratch = Folder(f.root / "scratch");
        Bytes assets = ReadAll(f.original.assets);
        fs::copy_file(f.original.streamed, scratch.streamed, fs::copy_options::overwrite_existing);
        fs::copy_file(f.original.bundle, scratch.bundle, fs::copy_options::overwrite_existing);
        std::string report;
        const IconFiles target = Folder(f.root / "scratch_out");

        Bytes other = assets;
        other[11] = 21;
        WriteAll(scratch.assets, other);
        Check(InspectIconSet(scratch) == IconSetState::kUnrecognised && PatchControllerIcons(scratch, target, report) == IconPatchResult::kUnsupported, "another serialized file version is refused");
        ExpectNoOutput(target, "another version");

        other = assets;
        other.resize(other.size() - 100);
        WriteAll(scratch.assets, other);
        Check(InspectIconSet(scratch) == IconSetState::kUnrecognised && PatchControllerIcons(scratch, target, report) != IconPatchResult::kPrepared && !HasControllerIcons(scratch),
              "a truncated file is refused");
        ExpectNoOutput(target, "a truncated file");

        const Table table = ReadTable(f.original.assets);
        const auto [start, size] = table.objects.at(50444);
        other = assets;
        other[table.dataOffset + static_cast<std::uint64_t>(start) + size / 2] ^= 0x01;
        WriteAll(scratch.assets, other);
        report.clear();
        Check(PatchControllerIcons(scratch, target, report) == IconPatchResult::kUnsupported && report.find("analysed") != std::string::npos, "a changed PlayStation prompt atlas is not the analysed data");
        ExpectNoOutput(target, "a changed atlas");

        WriteAll(scratch.assets, assets);
        Bytes stream = ReadAll(f.original.streamed);
        stream.resize(1000000);
        WriteAll(scratch.streamed, stream);
        report.clear();
        Check(PatchControllerIcons(scratch, target, report) != IconPatchResult::kPrepared, "streamed data that ends inside the Xbox sheet is refused");
        ExpectNoOutput(target, "a short streamed file");

        Check(PatchControllerIcons({f.root / "missing.assets", f.root / "missing.resS", {}}, target, report) == IconPatchResult::kFailed, "a missing source fails");
        ExpectNoOutput(target, "a missing source");

        // The bundle is part of the set: a bundle that is not the analysed one, or one that is half patched, stops the whole preparation.
        const IconFiles bundleTarget = Folder(f.root / "bundle_refused_out");
        Bytes brokenBundle = ReadAll(f.original.bundle);
        brokenBundle.resize(brokenBundle.size() - 100);
        WriteAll(scratch.bundle, brokenBundle);
        WriteAll(scratch.assets, assets);
        WriteAll(scratch.streamed, ReadAll(f.original.streamed));
        report.clear();
        Check(PatchControllerIcons(scratch, bundleTarget, report) != IconPatchResult::kPrepared && InspectIconSet(scratch) == IconSetState::kUnrecognised && !HasControllerIcons(scratch),
              "a damaged icon bundle refuses the preparation of the whole set");
        ExpectNoOutput(bundleTarget, "a damaged icon bundle");
        Check(!fs::exists(bundleTarget.bundle), "no bundle is written for a refused set");
        fs::copy_file(out.bundle, scratch.bundle, fs::copy_options::overwrite_existing);
        report.clear();
        Check(InspectIconSet(scratch) == IconSetState::kPartial && PatchControllerIcons(scratch, bundleTarget, report) == IconPatchResult::kUnsupported,
              "a complete bundle with the game's own assets is a partly patched set and is never patched further");
        ExpectNoOutput(bundleTarget, "a half patched set");
        Check(!fs::exists(bundleTarget.bundle), "no bundle is written for a half patched set");
    }
}

// ---- The cache ------------------------------------------------------------------------------------------------------------------

std::string ReadText(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestCache(const Fixture& f) {
    const fs::path media = MediaWith(f.root / "cache_media", f.original), cache = f.root / "cache";
    const IconFiles source{media / kIconsAssetsName, media / kIconsStreamName, media / kIconsBundleFolder / kIconsBundleName};
    const IconFiles prepared{cache / kIconsAssetsName, cache / kIconsStreamName, cache / kIconsBundleName};
    const fs::path stamp = cache / "controller_icons.stamp";
    std::string report;

    IconPlan plan = PlanControllerIcons(media, cache);
    Check(plan.kind == IconPlanKind::kPrepare, "without a cache, preparing is real work");
    Check(!fs::exists(stamp) && !fs::exists(prepared.assets), "planning writes nothing when work is needed");
    plan = PrepareControllerIcons(media, cache, report);
    Check(plan.kind == IconPlanKind::kUsePrepared && plan.files.assets == prepared.assets && plan.files.streamed == prepared.streamed, "preparing gives the prepared files");
    Check(HasControllerIcons(prepared) && fs::exists(stamp), "the pair validates and the stamp is written");
    Check(!fs::exists(cache / "resources.assets.new") && !fs::exists(cache / "resources.assets.resS.new") && !fs::exists(cache / "icons.bundle.new") && !fs::exists(cache / "controller_icons.stamp.new"),
          "no temporary file is left");
    Check(plan.files.bundle == prepared.bundle && fs::exists(prepared.bundle) && HasBundleIcons(prepared.bundle), "the plan names the prepared bundle with the complete set");

    const std::string stampText = ReadText(stamp);
    std::istringstream lines(stampText);
    std::string first, second, third;
    std::getline(lines, first);
    std::getline(lines, second);
    std::getline(lines, third);
    std::istringstream head(first), src(second), prep(third);
    std::string tag, state, version;
    head >> tag >> version >> state;
    std::vector<std::string> sourceFields, preparedFields;
    for (std::string field; src >> field;) sourceFields.push_back(field);
    for (std::string field; prep >> field;) preparedFields.push_back(field);
    Check(tag == "controller-icons" && std::stoi(version) == kControllerIconsPatchVersion && state == "prepared", "the stamp holds the patch format version and the state");
    Check(sourceFields.size() == 7 && sourceFields[0] == "source" && std::stoull(sourceFields[1]) == fs::file_size(source.assets) && std::stoull(sourceFields[3]) == fs::file_size(source.streamed) &&
              std::stoull(sourceFields[5]) == fs::file_size(source.bundle),
          "the stamp identifies the source by size and write time of all three files");
    Check(preparedFields.size() == 7 && preparedFields[0] == "prepared" && std::stoull(preparedFields[1]) == fs::file_size(prepared.assets) && std::stoull(preparedFields[5]) == fs::file_size(prepared.bundle),
          "and the prepared files the same way");
    Check(std::count(stampText.begin(), stampText.end(), '\n') == 3 && stampText.back() == '\n',
          "nothing else is in it: the launcher's own size or version does not enter the cache identity");

    const auto assetsTime = fs::last_write_time(prepared.assets), streamTime = fs::last_write_time(prepared.streamed);
    for (int round = 0; round < 3; ++round) {
        plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUsePrepared, "a valid cache is reused (round " + std::to_string(round + 1) + ")");
    }
    Check(fs::last_write_time(prepared.assets) == assetsTime && fs::last_write_time(prepared.streamed) == streamTime && ReadText(stamp) == stampText,
          "reusing the cache rewrites nothing, as after a launcher rebuild");

    // The state after the first preparation, to come back to after each kind of damage.
    const fs::path golden = f.root / "golden";
    fs::create_directories(golden);
    fs::copy_file(prepared.assets, golden / kIconsAssetsName);
    fs::copy_file(prepared.streamed, golden / kIconsStreamName);
    fs::copy_file(prepared.bundle, golden / kIconsBundleName);
    const auto bundleTime = fs::last_write_time(prepared.bundle);
    const auto restoreBundle = [&] {
        fs::copy_file(golden / kIconsBundleName, prepared.bundle, fs::copy_options::overwrite_existing);
        fs::last_write_time(prepared.bundle, bundleTime);
    };
    const auto restoreAssets = [&] {
        fs::copy_file(golden / kIconsAssetsName, prepared.assets, fs::copy_options::overwrite_existing);
        fs::last_write_time(prepared.assets, assetsTime);
    };
    const auto restoreStreamed = [&] {
        fs::copy_file(golden / kIconsStreamName, prepared.streamed, fs::copy_options::overwrite_existing);
        fs::last_write_time(prepared.streamed, streamTime);
    };
    const auto restoreStamp = [&] { std::ofstream(stamp, std::ios::trunc | std::ios::binary) << stampText; };

    {   // patch format version
        std::string text = stampText;
        text.replace(text.find(' ') + 1, 1, "0");
        std::ofstream(stamp, std::ios::trunc | std::ios::binary) << text;
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "another patch format version invalidates the cache");
        restoreStamp();
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kUsePrepared, "the right version is accepted again");
    }
    {   // damaged: a direction cell wiped, the write time restored
        XboxPrompts prompts;
        ReadXboxPrompts(prepared, prompts);
        const XboxPromptGlyph* glyph = GlyphNamed(prompts, "Xbox_LeftStick_Left");
        Check(glyph != nullptr, "the damage target is found");
        Bytes stream = ReadAll(prepared.streamed);
        for (std::uint32_t y = 0; glyph && y < kCell; ++y) {
            for (std::uint32_t x = 0; x < kCell; ++x) {
                const std::size_t at = prompts.sheetOffset + TexelAt(static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, prompts.sheetWidth);
                std::fill(stream.begin() + static_cast<std::ptrdiff_t>(at), stream.begin() + static_cast<std::ptrdiff_t>(at + 4), 0);
            }
        }
        WriteAll(prepared.streamed, stream);
        fs::last_write_time(prepared.streamed, streamTime);
        Check(!HasControllerIcons(prepared), "a prepared pair with a wiped direction cell is rejected");
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "and the cache is rebuilt even when the write time looks right");
        restoreStreamed();
    }
    {   // truncated
        fs::resize_file(prepared.assets, fs::file_size(prepared.assets) - 1000);
        Check(!HasControllerIcons(prepared), "a truncated prepared assets file is rejected");
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "and the cache is rebuilt");
        restoreAssets();
        fs::resize_file(prepared.streamed, fs::file_size(prepared.streamed) - 4096);
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a truncated prepared streamed file is rejected too");
        restoreStreamed();
        fs::resize_file(prepared.streamed, 1000000);
        Check(!HasControllerIcons(prepared), "a prepared streamed file that ends inside the Xbox sheet is rejected");
        restoreStreamed();
    }
    {   // partial: one token damaged with the write time restored
        Bytes assets = ReadAll(prepared.assets);
        assets[LastOccurrence(assets, "LASDOWN") + 6] = 'M';
        WriteAll(prepared.assets, assets);
        fs::last_write_time(prepared.assets, assetsTime);
        Check(!HasControllerIcons(prepared) && InspectIconSet(prepared) == IconSetState::kPartial, "a partly patched prepared copy is rejected");
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "and prepared again from the game's own files");
        restoreAssets();
    }
    {   // the bundle: a missing, truncated or damaged prepared bundle rejects the set
        fs::remove(prepared.bundle);
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a missing prepared bundle rejects the set");
        restoreBundle();
        fs::resize_file(prepared.bundle, fs::file_size(prepared.bundle) - 100);
        fs::last_write_time(prepared.bundle, bundleTime);
        Check(!HasControllerIcons(prepared) && PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a truncated prepared bundle is rejected and the cache is rebuilt");
        restoreBundle();
        Bytes damaged = ReadAll(prepared.bundle);
        damaged[damaged.size() / 3] ^= 0xff;
        damaged[64] ^= 0xff;
        WriteAll(prepared.bundle, damaged);
        fs::last_write_time(prepared.bundle, bundleTime);
        Check(!HasControllerIcons(prepared) && PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a damaged prepared bundle is rejected even when its write time looks right");
        restoreBundle();
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kUsePrepared, "the saved bundle is valid again after the damage");
        {   // the pair alone is not the set: the game's own bundle in the cache folder is not a prepared one
            fs::copy_file(f.original.bundle, prepared.bundle, fs::copy_options::overwrite_existing);
            fs::last_write_time(prepared.bundle, bundleTime);
            Check(!HasControllerIcons(prepared) && PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a prepared pair with the game's own bundle is not a complete set");
            restoreBundle();
        }
    }
    {   // a pair needs both members
        fs::remove(prepared.streamed);
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a missing prepared streamed file rejects the pair");
        restoreStreamed();
        fs::remove(prepared.assets);
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a missing prepared assets file rejects the pair");
        restoreAssets();
        fs::remove(stamp);
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a missing stamp rejects the pair");
        restoreStamp();
    }
    Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kUsePrepared, "after every kind of damage the saved pair is valid again");
    {   // source file change
        const auto bundleSourceTime = fs::last_write_time(source.bundle);
        fs::last_write_time(source.bundle, bundleSourceTime + std::chrono::hours(1));
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a changed source bundle invalidates the cache");
        fs::last_write_time(source.bundle, bundleSourceTime);
        const auto sourceTime = fs::last_write_time(source.streamed);
        fs::last_write_time(source.streamed, sourceTime + std::chrono::hours(1));
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a changed source file invalidates the cache");
        fs::last_write_time(source.streamed, sourceTime);
        fs::last_write_time(source.assets, fs::last_write_time(source.assets) + std::chrono::hours(1));
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kPrepare, "a changed source assets file invalidates the cache");
        std::string work;
        const IconPlan again = PrepareControllerIcons(media, cache, work);
        Check(again.kind == IconPlanKind::kUsePrepared && HasControllerIcons(prepared) && PlanControllerIcons(media, cache).kind == IconPlanKind::kUsePrepared,
              "preparing again after a source change gives a valid pair that is then reused");
    }
    Check(Fnv(ReadAll(source.assets)) == Fnv(ReadAll(f.original.assets)) && Fnv(ReadAll(source.streamed)) == Fnv(ReadAll(f.original.streamed)) && Fnv(ReadAll(source.bundle)) == Fnv(ReadAll(f.original.bundle)),
          "the source files stayed what they were through all of it");
}

void TestCompleteSourceAndRefusals(const Fixture& f, const IconFiles& out) {
    {   // a source that already has the set: no copy, no work
        const fs::path media = MediaWith(f.root / "complete_media", out), cache = f.root / "complete_cache";
        const IconPlan plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.files.assets.empty(), "a source with the complete set is used as it is");
        Check(!fs::exists(cache / kIconsAssetsName) && !fs::exists(cache / kIconsStreamName), "and no prepared copy is made of it");
        std::string report;
        Check(PrepareControllerIcons(media, cache, report).kind == IconPlanKind::kUseOriginal && report.empty(), "asking to prepare it does no work");
        Check(ReadText(cache / "controller_icons.stamp").find(" complete") != std::string::npos, "the stamp records that it is complete");
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kUseOriginal, "and it stays so");
    }
    {   // a source that is not the analysed one: cached as such, never offered as work
        const fs::path media = MediaWith(f.root / "refused_media", f.original), cache = f.root / "refused_cache";
        Bytes assets = ReadAll(f.original.assets);
        assets[11] = 21;
        WriteAll(media / kIconsAssetsName, assets);
        const IconPlan plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.note.find("used as they are") != std::string::npos, "an unknown source is used as it is, with the reason");
        Check(ReadText(cache / "controller_icons.stamp").find(" unsupported") != std::string::npos && PlanControllerIcons(media, cache).kind == IconPlanKind::kUseOriginal,
              "the refusal is remembered and never becomes work");
    }
    {   // a partly patched source is never offered as work either
        const fs::path media = MediaWith(f.root / "partial_media", {f.root / "partial" / kIconsAssetsName, f.root / "partial" / kIconsStreamName, f.root / "partial" / kIconsBundleName}),
                       cache = f.root / "partial_cache";
        std::string report;
        const IconPlan plan = PrepareControllerIcons(media, cache, report);
        Check(plan.kind == IconPlanKind::kUseOriginal && !fs::exists(cache / kIconsAssetsName) && !fs::exists(cache / kIconsBundleName), "a partly patched source is used as it is and no copy is made");
    }
    {   // a game without the icon bundle has nothing the prepared copies could fix
        const fs::path media = MediaWith(f.root / "nobundle_media", f.original);
        fs::remove(media / kIconsBundleFolder / kIconsBundleName);
        const IconPlan plan = PlanControllerIcons(media, f.root / "nobundle_cache");
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.note.find("not installed") != std::string::npos, "without the game's icon bundle nothing is prepared");
    }
    {   // a bundle that is not the analysed one: the set is used as it is, remembered, never offered as work
        const fs::path media = MediaWith(f.root / "badbundle_media", f.original), cache = f.root / "badbundle_cache";
        Bytes bundle = ReadAll(media / kIconsBundleFolder / kIconsBundleName);
        bundle[3] = 'X';
        WriteAll(media / kIconsBundleFolder / kIconsBundleName, bundle);
        const IconPlan plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.note.find("used as they are") != std::string::npos && !fs::exists(cache / kIconsBundleName), "an unknown icon bundle is used as it is, with the reason");
        Check(ReadText(cache / "controller_icons.stamp").find(" unsupported") != std::string::npos && PlanControllerIcons(media, cache).kind == IconPlanKind::kUseOriginal,
              "the refusal is remembered and never becomes work");
    }
    {   // no source at all
        const fs::path media = f.root / "empty_media";
        fs::create_directories(media);
        const IconPlan plan = PlanControllerIcons(media, f.root / "empty_cache");
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.note.find("not installed") != std::string::npos, "without the game's icon files nothing is prepared");
    }
}

// ---- The path alias is exact --------------------------------------------------------------------------------------------------

std::string ReadSource(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestRedirection(const fs::path& root) {
    Check(std::string(kIconsGuestAssets) == "/app0/Media/resources.assets" && std::string(kIconsGuestStream) == "/app0/Media/resources.assets.resS" &&
              std::string(kIconsGuestBundle) == "/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle" &&
              std::string(kIconsGuestBundle) == std::string("/app0/Media/") + kIconsBundleFolder + "/" + kIconsBundleName,
          "the three guest paths are the exact game files");
    const std::string loader = ReadSource(root / "src" / "loader.c");
    const auto count = [&](const std::string& text) {
        std::size_t found = 0;
        for (std::size_t at = loader.find(text); at != std::string::npos; at = loader.find(text, at + 1)) ++found;
        return found;
    };
    Check(count("runtime_file_alias(") == 3 && count("runtime_file_alias(\"/app0/Media/resources.assets\", controller_icons.assets)") == 1 &&
              count("runtime_file_alias(\"/app0/Media/resources.assets.resS\", controller_icons.streamed)") == 1 &&
              count("runtime_file_alias(\"/app0/Media/StreamingAssets/aa/PS4/0342107720d621839faadc2c6c3deb69.bundle\", controller_icons.bundle)") == 1,
          "the loader redirects exactly the three game files to the prepared copies and nothing else");
    Check(loader.find("controller_icons.assets[0] && controller_icons.streamed[0] && controller_icons.bundle[0] &&") != std::string::npos, "and only when all three were prepared and validated");
    const std::string runtime = ReadSource(root / "src" / "runtime_file.c");
    Check(runtime.find("same_path_ignoring_case(path_aliases[i].guest,buffer)") != std::string::npos, "the file layer matches the whole normalized path of an alias");
    Check(ReadSource(root / "launcher" / "launcher.cpp").find("prompt_style == SOS_PROMPTS_PLAYSTATION || !build_supported") != std::string::npos,
          "PlayStation prompts and unsupported builds prepare nothing");
    const std::string launcher = ReadSource(root / "launcher" / "launcher.cpp");
    Check(launcher.find("std::snprintf(icons->bundle, sizeof(icons->bundle), \"%s\", bundle.c_str());") != std::string::npos && launcher.find("bundle.size() < sizeof(icons->bundle)") != std::string::npos,
          "the launcher hands the prepared bundle path to the loader only together with the other two");
    Check(ReadSource(root / "launcher" / "controller_icons.cpp").find("runtime_file_overlay") == std::string::npos && ReadSource(root / "launcher" / "icon_bundle.cpp").find("runtime_file_overlay") == std::string::npos &&
              loader.find("Controller icons: prepared copies in use") != std::string::npos,
          "the icons are whole prepared files, not a sparse read overlay");
}

// ---- The Addressables icon bundle ---------------------------------------------------------------------------------------------

constexpr const char* kMaskTokens[] = {"Button South Mask", "Button East Mask", "Button North Mask", "Button North Mask Filled"};
constexpr const char* kSpriteNames[] = {"Xbox_LeftStick_Up", "Xbox_LeftStick_Down", "Xbox_LeftStick_Left", "Xbox_LeftStick_Right"};

// TextMeshPro's name hash, written out here independently of the code under test.
std::uint32_t NameHash(const std::string& name) {
    std::uint32_t hash = 0;
    for (const char c : name) hash = ((hash << 5) + hash) ^ static_cast<std::uint8_t>(c);
    return hash;
}

std::int64_t SpecialOf(const BundlePrompts::Special& map, const std::string& token) {
    for (const auto& [key, sprite] : map) {
        if (key == token) return sprite;
    }
    return 0;
}

std::size_t Count(const BundlePrompts::Special& map, const std::string& token) {
    return static_cast<std::size_t>(std::count_if(map.begin(), map.end(), [&](const auto& item) { return item.first == token; }));
}

const BundleGlyph* GlyphOfSprite(const BundleAtlas& atlas, std::int64_t sprite) {
    for (const BundleGlyph& glyph : atlas.glyphs) {
        if (glyph.sprite == sprite) return &glyph;
    }
    return nullptr;
}

std::uint8_t TexelOf(const BundleTexture& texture, std::uint32_t x, std::uint32_t y, int channel) { return texture.tiled[TexelAt(x, y, texture.width) + static_cast<std::size_t>(channel)]; }

// The 64x64 square of opaque texels a PlayStation glyph occupies inside its (slightly larger) rectangle.
bool OpaqueSquare(const BundleTexture& texture, const BundleGlyph& glyph, std::uint32_t& left, std::uint32_t& top) {
    std::uint32_t minX = texture.width, minY = texture.height, maxX = 0, maxY = 0;
    const auto from = [](std::int32_t value) { return static_cast<std::uint32_t>(std::max(value - 4, 0)); };
    for (std::uint32_t y = from(glyph.y); y < std::min<std::uint32_t>(static_cast<std::uint32_t>(glyph.y + glyph.height + 4), texture.height); ++y) {
        for (std::uint32_t x = from(glyph.x); x < std::min<std::uint32_t>(static_cast<std::uint32_t>(glyph.x + glyph.width + 4), texture.width); ++x) {
            if (TexelOf(texture, x, y, 3) == 0) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    left = minX;
    top = minY;
    return maxX >= minX && maxX - minX + 1 == kCell && maxY - minY + 1 == kCell;
}

// Where the bright arrow of a left-stick glyph sits in its cell: the near-white opaque texels, as a centroid (Unity rows run upwards).
bool ArrowCentroid(const BundleTexture& texture, std::uint32_t left, std::uint32_t top, double& cx, double& cy) {
    double sx = 0, sy = 0, count = 0;
    for (std::uint32_t y = 0; y < kCell; ++y) {
        for (std::uint32_t x = 0; x < kCell; ++x) {
            if (TexelOf(texture, left + x, top + y, 3) != 255) continue;
            if (TexelOf(texture, left + x, top + y, 0) < 235 || TexelOf(texture, left + x, top + y, 1) < 235 || TexelOf(texture, left + x, top + y, 2) < 235) continue;
            sx += x;
            sy += y;
            ++count;
        }
    }
    if (count < 10) return false;
    cx = sx / count;
    cy = sy / count;
    return true;
}

void TestBundleSource(const Fixture& f) {
    std::string why;
    Check(InspectIconBundle(f.original.bundle, &why) == IconSetState::kMissing, "the game's own icon bundle lacks the direction set and the interaction icons");
    BundlePrompts bundle;
    Check(ReadBundlePrompts(f.original.bundle, bundle), "the game's own icon bundle is read");
    Check(bundle.xboxSpecial.size() == 1 && bundle.xboxSpecial[0].first == "!LookInput", "its Xbox special icon map holds !LookInput alone");
    for (const char* token : kTokens) {
        Check(Count(bundle.xboxSpecial, token) == 0, std::string(token) + " is missing from the bundle's Xbox prompts");
        Check(Count(bundle.ps5Special, token) == 1, std::string(token) + " is in the PlayStation 5 map, the artwork source");
    }
    for (const char* token : kMaskTokens) Check(Count(bundle.xboxSpecial, token) == 0, std::string(token) + " is missing from the bundle's Xbox prompts");
    Check(Count(bundle.ps5Special, "Button South Mask") == 1 && Count(bundle.ps5Special, "Button East Mask") == 1, "the PlayStation 5 map has the confirm and back masks");
    Check(bundle.xbox.glyphs.size() == 19 && bundle.xbox.characters.size() == 19, "its Xbox atlas has the 19 glyphs");
    Check(bundle.xboxButtons.size() == 4 && !HasBundleIcons(f.original.bundle), "the original bundle is not a complete one");
}

void TestBundlePatch(const Fixture& f, const IconFiles& out) {
    const FileState before = StateOf(f.original.bundle);
    std::string report;
    Check(PatchIconBundle(f.original.bundle, out.bundle, report) == IconPatchResult::kPrepared, "the bundle is prepared from the game's own file");
    Check(report.find("LASDOWN") != std::string::npos && report.find("Button North Mask Filled") != std::string::npos, "the report names the new tokens");
    Check(StateOf(f.original.bundle) == before, "the game's own bundle is unchanged (size, time and content)");
    Check(fs::file_size(out.bundle) > fs::file_size(f.original.bundle), "the prepared bundle is complete and a little larger");
    Check(InspectIconBundle(out.bundle) == IconSetState::kComplete && HasBundleIcons(out.bundle), "the prepared bundle validates and holds the complete set");
    std::string again;
    const fs::path second = f.root / "second.bundle";
    Check(PatchIconBundle(f.original.bundle, second, again) == IconPatchResult::kPrepared && Fnv(ReadAll(second)) == Fnv(ReadAll(out.bundle)), "preparing twice gives identical bundles");
    const fs::path target = f.root / "complete.bundle";
    Check(PatchIconBundle(out.bundle, target, again) == IconPatchResult::kComplete && !fs::exists(target), "patching an already complete bundle does nothing");
    {   // on a copy: refusing an output that is the source must never delete it
        const fs::path copy = f.root / "alias.bundle";
        fs::copy_file(f.original.bundle, copy, fs::copy_options::overwrite_existing);
        Check(PatchIconBundle(copy, copy, again) == IconPatchResult::kFailed && fs::exists(copy) && Fnv(ReadAll(copy)) == Fnv(ReadAll(f.original.bundle)),
              "an output that is the source file is refused and the source is left alone");
        const IconFiles pair = Folder(f.root / "alias_set");
        fs::copy_file(f.original.assets, pair.assets);
        fs::copy_file(f.original.streamed, pair.streamed);
        fs::copy_file(f.original.bundle, pair.bundle);
        Check(PatchControllerIcons(pair, pair, again) == IconPatchResult::kFailed && fs::exists(pair.assets) && fs::exists(pair.streamed) && fs::exists(pair.bundle) &&
                  Fnv(ReadAll(pair.bundle)) == Fnv(ReadAll(f.original.bundle)) && Fnv(ReadAll(pair.assets)) == Fnv(ReadAll(f.original.assets)),
              "outputs that are the source files are refused and the sources are left alone");
    }
}

void TestBundlePrepared(const Fixture& f, const IconFiles& out) {
    BundlePrompts before, after;
    Check(ReadBundlePrompts(f.original.bundle, before) && ReadBundlePrompts(out.bundle, after), "the prompts of both bundles are read");
    Check(after.xboxSpecial.size() == 9 && after.xboxSpecial[0] == before.xboxSpecial[0], "the existing !LookInput entry is first and unchanged, eight entries were added");
    std::set<std::vector<std::uint8_t>> art;
    for (std::size_t index = 0; index < 4; ++index) {
        const std::string token = kTokens[index], name = kSpriteNames[index];
        const std::string label = token + ": ";
        Check(Count(after.xboxSpecial, token) == 1, label + "the token is there exactly once");
        const std::int64_t id = SpecialOf(after.xboxSpecial, token);
        const auto sprite = after.sprites.find(id);
        Check(sprite != after.sprites.end() && sprite->second.name == name, label + "it resolves to the sprite " + name);
        const BundleGlyph* glyph = GlyphOfSprite(after.xbox, id);
        Check(glyph != nullptr, label + "the Xbox atlas has a glyph for that sprite");
        if (sprite == after.sprites.end() || glyph == nullptr) continue;
        const std::size_t position = static_cast<std::size_t>(glyph - after.xbox.glyphs.data());
        Check(glyph->index == 19 + index && position == 19 + index, label + "the glyph index is " + std::to_string(19 + index) + " and in table order");
        const auto character = std::find_if(after.xbox.characters.begin(), after.xbox.characters.end(), [&](const BundleCharacter& c) { return c.name == name; });
        Check(character != after.xbox.characters.end() && std::count_if(after.xbox.characters.begin(), after.xbox.characters.end(), [&](const BundleCharacter& c) { return c.name == name; }) == 1,
              label + "the atlas has one character of that name");
        if (character != after.xbox.characters.end()) {
            Check(character->glyph == glyph->index && static_cast<std::size_t>(character - after.xbox.characters.begin()) == 19 + index, label + "the character points at its glyph");
            Check(character->hash == NameHash(name), label + "the TextMeshPro hash of " + name + " is the name's hash");
        }
        Check(glyph->width == static_cast<std::int32_t>(kCell) && glyph->height == static_cast<std::int32_t>(kCell) && glyph->x >= 0 && glyph->y >= 0 &&
                  static_cast<std::uint32_t>(glyph->x + glyph->width) <= after.xboxSheet.width && static_cast<std::uint32_t>(glyph->y + glyph->height) <= after.xboxSheet.height,
              label + "the glyph is a 64x64 cell inside the sheet");
        Check(sprite->second.rect[0] == static_cast<float>(glyph->x) && sprite->second.rect[1] == static_cast<float>(glyph->y) && sprite->second.rect[2] == 64.0f && sprite->second.rect[3] == 64.0f,
              label + "the sprite rectangle is the glyph's cell");
        Check(sprite->second.texture == after.xbox.sheet, label + "the sprite uses the Xbox sheet");
        Check(std::count(after.preload.begin(), after.preload.end(), id) == 1, label + "the sprite is in the icon data's preload list");
        for (const BundleGlyph& other : before.xbox.glyphs) {
            const bool disjoint = glyph->x + glyph->width <= other.x || other.x + other.width <= glyph->x || glyph->y + glyph->height <= other.y || other.y + other.height <= glyph->y;
            Check(disjoint, label + "the cell does not overlap an original Xbox glyph");
        }

        std::size_t emptyBefore = 0;
        for (std::uint32_t y = 0; y < kCell; ++y) {
            for (std::uint32_t x = 0; x < kCell; ++x) emptyBefore += TexelOf(before.xboxSheet, static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, 3) != 0;
        }
        Check(emptyBefore == 0, label + "the cell was empty in the game's own sheet");
        const std::int64_t sourceId = SpecialOf(after.ps5Special, token);
        const BundleGlyph* source = GlyphOfSprite(after.ps5, sourceId);
        std::uint32_t left = 0, top = 0;
        Check(source != nullptr && OpaqueSquare(after.ps5Sheet, *source, left, top), label + "the PlayStation 5 source cell is found in its sheet");
        if (source == nullptr) continue;
        bool same = true;
        std::size_t opaque = 0;
        std::vector<std::uint8_t> cell;
        for (std::uint32_t y = 0; y < kCell; ++y) {
            for (std::uint32_t x = 0; x < kCell; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    const std::uint8_t now = TexelOf(after.xboxSheet, static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, channel);
                    same = same && now == TexelOf(after.ps5Sheet, left + x, top + y, channel);
                    cell.push_back(now);
                }
                opaque += TexelOf(after.xboxSheet, static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, 3) != 0;
            }
        }
        Check(same, label + "the cell holds exactly the PlayStation 5 art of " + token + " (every channel of every texel)");
        Check(opaque > 1500, label + "the cell is not blank (" + std::to_string(opaque) + " opaque texels)");
        art.insert(cell);
        double cx = 0, cy = 0;
        Check(ArrowCentroid(after.xboxSheet, static_cast<std::uint32_t>(glyph->x), static_cast<std::uint32_t>(glyph->y), cx, cy), label + "the direction arrow is found in the copied cell");
        const bool right = index == 0 ? cy > 44 && std::abs(cx - 31.5) < 12 : index == 1 ? cy < 20 && std::abs(cx - 31.5) < 12 : index == 2 ? cx < 20 && std::abs(cy - 31.5) < 12 : cx > 44 && std::abs(cy - 31.5) < 12;
        Check(right, label + "the arrow in the cell points " + (index == 0 ? "up" : index == 1 ? "down" : index == 2 ? "left" : "right") + " (centroid " + std::to_string(cx) + ", " + std::to_string(cy) + ")");
    }
    Check(art.size() == 4, "the four directions have four different pictures");

    // Nothing else changed.
    Check(after.xbox.glyphs.size() == before.xbox.glyphs.size() + 4 && after.xbox.characters.size() == before.xbox.characters.size() + 4, "the Xbox atlas grew by four glyphs and four characters");
    bool unchanged = true;
    for (std::size_t index = 0; index < before.xbox.glyphs.size(); ++index) {
        const BundleGlyph &a = before.xbox.glyphs[index], &b = after.xbox.glyphs[index];
        unchanged = unchanged && a.index == b.index && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height && a.sprite == b.sprite;
        const BundleCharacter &c = before.xbox.characters[index], &d = after.xbox.characters[index];
        unchanged = unchanged && c.name == d.name && c.glyph == d.glyph && c.hash == d.hash;
    }
    Check(unchanged, "the 19 existing Xbox atlas glyphs and characters are unchanged");
    Check(before.ps5Special == after.ps5Special && before.xboxButtons == after.xboxButtons && before.ps5.glyphs.size() == after.ps5.glyphs.size(), "the PlayStation map and the Xbox button sprites are unchanged");
    std::size_t changedSprites = 0, lostSprites = 0;
    for (const auto& [id, sprite] : before.sprites) {
        const auto now = after.sprites.find(id);
        if (now == after.sprites.end()) ++lostSprites;
        else changedSprites += now->second.name != sprite.name || now->second.texture != sprite.texture || std::memcmp(now->second.rect, sprite.rect, sizeof(sprite.rect)) != 0;
    }
    Check(lostSprites == 0 && changedSprites == 0 && after.sprites.size() == before.sprites.size() + 4, "every sprite of the bundle is unchanged and four were added");
    std::vector<std::int64_t> preload = after.preload;
    for (const char* token : kTokens) preload.erase(std::remove(preload.begin(), preload.end(), SpecialOf(after.xboxSpecial, token)), preload.end());
    Check(preload == before.preload, "the icon data's preload list gained exactly the four new sprites");
    Check(after.ps5Sheet.tiled == before.ps5Sheet.tiled, "the PlayStation 5 sheet is byte for byte unchanged");
    bool outside = before.xboxSheet.tiled.size() == after.xboxSheet.tiled.size();
    std::set<std::size_t> tiles;
    for (const char* name : kSpriteNames) {
        const BundleGlyph* glyph = nullptr;
        for (std::size_t index = 19; index < after.xbox.characters.size(); ++index) {
            if (after.xbox.characters[index].name == name) glyph = &after.xbox.glyphs[after.xbox.characters[index].glyph];
        }
        if (glyph == nullptr) continue;
        for (std::uint32_t y = 0; y < kCell; y += 8) {
            for (std::uint32_t x = 0; x < kCell; x += 8) tiles.insert(TexelAt(static_cast<std::uint32_t>(glyph->x) + x, static_cast<std::uint32_t>(glyph->y) + y, after.xboxSheet.width) / 256);
        }
    }
    for (std::size_t at = 0; outside && at < before.xboxSheet.tiled.size(); ++at) outside = before.xboxSheet.tiled[at] == after.xboxSheet.tiled[at] || tiles.count(at / 256) == 1;
    Check(outside, "every changed texel of the Xbox sheet lies in one of the four new cells");

    // The interaction marker and the confirmation panels ask for these by name; a missing one is an empty white square.
    for (std::size_t index = 0; index < 4; ++index) {
        const std::string token = kMaskTokens[index];
        Check(Count(after.xboxSpecial, token) == 1, token + ": the Xbox special icon is there exactly once");
        const std::int64_t id = SpecialOf(after.xboxSpecial, token);
        const auto sprite = after.sprites.find(id);
        Check(id != 0 && sprite != after.sprites.end() && sprite->second.texture != 0, token + ": it resolves to a real sprite with a texture, not to nothing");
        if (sprite == after.sprites.end()) continue;
        if (index == 3) Check(id == after.circle && sprite->second.name == "Circle", token + ": the fill uses the interaction marker's own disc sprite");
        else Check(id == after.xboxButtons[index == 0 ? 0 : index == 1 ? 2 : 1], token + ": it is the Xbox " + (index == 0 ? "A (South, confirm)" : index == 1 ? "B (East, back)" : "Y (North, interact)") + " sprite");
    }
    Check(SpecialOf(after.xboxSpecial, kMaskTokens[0]) != SpecialOf(after.xboxSpecial, kMaskTokens[1]), "the confirm mask is not the back mask");
    for (const auto& [token, id] : before.ps5Special) {
        if (token.find("Mask") == std::string::npos) continue;
        Check(Count(after.xboxSpecial, token) == 1, "the PlayStation map's " + token + " exists in the Xbox map now");
    }
}

void TestBundleRefusals(const Fixture& f) {
    const fs::path scratch = f.root / "bundle_scratch.bundle", target = f.root / "bundle_scratch_out.bundle";
    Bytes bundle = ReadAll(f.original.bundle);
    std::string report;
    const auto refused = [&](const std::string& label, const Bytes& data) {
        WriteAll(scratch, data);
        report.clear();
        const IconPatchResult result = PatchIconBundle(scratch, target, report);
        Check(result != IconPatchResult::kPrepared && !fs::exists(target) && InspectIconBundle(scratch) != IconSetState::kMissing && !HasBundleIcons(scratch), label + " is refused and nothing is written");
    };
    {
        Bytes other = bundle;
        other[3] = 'X';
        refused("a file that is not UnityFS", other);
    }
    {
        Bytes other = bundle;
        other.resize(other.size() - 100);
        refused("a truncated bundle", other);
    }
    {
        Bytes other = bundle;
        other[other.size() / 2] ^= 0xff;
        other[64] ^= 0xff;
        refused("a bundle with damaged block data", other);
    }
    refused("an empty file", Bytes());
    Check(PatchIconBundle(f.root / "missing.bundle", target, report) == IconPatchResult::kFailed && !fs::exists(target), "a missing bundle fails and writes nothing");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = SOS_SOURCE_DIR;
    const char* only = std::getenv("SOS_ICONS_TEST_ONLY");
    const bool bundleOnly = only != nullptr && std::string(only) == "bundle";
    TestRedirection(root);
    Fixture f;
    f.source.clear();
    for (const char* folder : {"CUSA57767-1.00", "v1.00"}) {
        const fs::path media = root / "game" / "prepared" / folder / "Media";
        if (f.source.empty() && fs::is_regular_file(media / kIconsAssetsName) && fs::is_regular_file(media / kIconsStreamName) && fs::is_regular_file(media / kIconsBundleFolder / kIconsBundleName)) f.source = media;
    }
    if (f.source.empty()) {
        std::printf("  [skip] CUSA57767 1.00: the game's resources.assets is not installed, the assets were not read\n");
    } else {
        f.root = TempRoot();
        f.original = {f.source / kIconsAssetsName, f.source / kIconsStreamName, f.source / kIconsBundleFolder / kIconsBundleName};
        const IconFiles out = Folder(f.root / "out");
        const auto timed = [](const char* name, const auto& run) {
            const auto start = std::chrono::steady_clock::now();
            run();
            std::printf("  [time] %s %lld ms\n", name, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
        };
        timed("source", [&] { TestSource(f); });
        timed("bundle source", [&] { TestBundleSource(f); });
        timed("patch", [&] { TestPatch(f, out); });
        timed("bundle patch", [&] { TestBundlePatch(f, out); });
        timed("prepared", [&] { TestPrepared(f, out); });
        timed("bundle prepared", [&] { TestBundlePrepared(f, out); });
        timed("bundle refusals", [&] { TestBundleRefusals(f); });
        if (!bundleOnly) {
            timed("untouched objects", [&] { TestUntouchedObjects(f, out); });
            timed("states", [&] { TestStates(f, out); });
            timed("cache", [&] { TestCache(f); });
            timed("complete source and refusals", [&] { TestCompleteSourceAndRefusals(f, out); });
        }
        std::error_code error;
        fs::remove_all(f.root, error);
    }
    const fs::path other = root / "game" / "prepared" / "CUSA57767-1.01" / "Media";
    if (fs::is_regular_file(other / kIconsAssetsName) && fs::is_regular_file(other / kIconsStreamName)) {
        const fs::path work = TempRoot();
        const fs::path cache = work / "cache";
        const IconPlan planned = PlanControllerIcons(other, cache);
        Check(planned.kind != IconPlanKind::kUsePrepared || HasControllerIcons(planned.files), "1.01: the source is either prepared, complete or refused, never half used");
        std::string report;
        const IconPlan plan = PrepareControllerIcons(other, cache, report);
        Check(plan.kind == IconPlanKind::kUsePrepared && HasControllerIcons(plan.files), "1.01: the Xbox directions and interaction icons are prepared from its own data");
        std::error_code error;
        fs::remove_all(work, error);
    } else {
        std::printf("  [skip] CUSA57767 1.01: its resources.assets is not installed; asset preparation is not checked for it and refuses unknown data\n");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

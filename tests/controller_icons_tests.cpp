// The Xbox left-stick glyphs and their cache, on the game's own files: the analysed CUSA57767 1.00 resources.assets and
// resources.assets.resS when they are installed. The files are only read; the prepared copies are made in a temporary folder.
// No game, launcher or window is started.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "controller_icons.h"

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
};

void ExpectNoOutput(const IconFiles& out, const std::string& label) {
    Check(!fs::exists(out.assets) && !fs::exists(out.streamed), label + ": nothing is written");
}

IconFiles Folder(const fs::path& folder) {
    fs::create_directories(folder);
    return {folder / kIconsAssetsName, folder / kIconsStreamName};
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
    Check(!HasControllerIcons(f.original), "the game's own files are not a complete pair");
}

void TestPatch(const Fixture& f, const IconFiles& out) {
    const FileState assetsBefore = StateOf(f.original.assets), streamBefore = StateOf(f.original.streamed);
    std::string report;
    Check(PatchControllerIcons(f.original, out, report) == IconPatchResult::kPrepared, "the left-stick directions are prepared from the game's own files");
    Check(report.find("LASUP") != std::string::npos && report.find("LASRIGHT") != std::string::npos, "the report names the four directions");
    Check(StateOf(f.original.assets) == assetsBefore && StateOf(f.original.streamed) == streamBefore, "the game's own files are unchanged (size, time and content)");
    Check(fs::exists(out.assets) && fs::exists(out.streamed), "both prepared files exist");
    Check(fs::file_size(out.streamed) == fs::file_size(f.original.streamed), "the prepared streamed file has the original's size");
    Check(fs::file_size(out.assets) > fs::file_size(f.original.assets), "the prepared assets file is complete and a little larger");
    Check(InspectIconSet(out) == IconSetState::kComplete && HasControllerIcons(out), "the prepared pair validates structurally and holds the complete set");
    std::string again;
    Check(PatchControllerIcons(f.original, Folder(f.root / "second"), again) == IconPatchResult::kPrepared &&
              Fnv(ReadAll(f.root / "second" / kIconsAssetsName)) == Fnv(ReadAll(out.assets)) && Fnv(ReadAll(f.root / "second" / kIconsStreamName)) == Fnv(ReadAll(out.streamed)),
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

        Check(PatchControllerIcons({f.root / "missing.assets", f.root / "missing.resS"}, target, report) == IconPatchResult::kFailed, "a missing source fails");
        ExpectNoOutput(target, "a missing source");
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
    const fs::path media = f.root / "cache_media", cache = f.root / "cache";
    fs::create_directories(media);
    fs::copy_file(f.original.assets, media / kIconsAssetsName);
    fs::copy_file(f.original.streamed, media / kIconsStreamName);
    const IconFiles source{media / kIconsAssetsName, media / kIconsStreamName};
    const IconFiles prepared{cache / kIconsAssetsName, cache / kIconsStreamName};
    const fs::path stamp = cache / "controller_icons.stamp";
    std::string report;

    IconPlan plan = PlanControllerIcons(media, cache);
    Check(plan.kind == IconPlanKind::kPrepare, "without a cache, preparing is real work");
    Check(!fs::exists(stamp) && !fs::exists(prepared.assets), "planning writes nothing when work is needed");
    plan = PrepareControllerIcons(media, cache, report);
    Check(plan.kind == IconPlanKind::kUsePrepared && plan.files.assets == prepared.assets && plan.files.streamed == prepared.streamed, "preparing gives the prepared pair");
    Check(HasControllerIcons(prepared) && fs::exists(stamp), "the pair validates and the stamp is written");
    Check(!fs::exists(cache / "resources.assets.new") && !fs::exists(cache / "resources.assets.resS.new") && !fs::exists(cache / "controller_icons.stamp.new"), "no temporary file is left");

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
    Check(sourceFields.size() == 5 && sourceFields[0] == "source" && std::stoull(sourceFields[1]) == fs::file_size(source.assets) && std::stoull(sourceFields[3]) == fs::file_size(source.streamed),
          "the stamp identifies the source by size and write time of both files");
    Check(preparedFields.size() == 5 && preparedFields[0] == "prepared" && std::stoull(preparedFields[1]) == fs::file_size(prepared.assets), "and the prepared pair the same way");
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
    Check(Fnv(ReadAll(source.assets)) == Fnv(ReadAll(f.original.assets)) && Fnv(ReadAll(source.streamed)) == Fnv(ReadAll(f.original.streamed)),
          "the source files stayed what they were through all of it");
}

void TestCompleteSourceAndRefusals(const Fixture& f, const IconFiles& out) {
    {   // a source that already has the set: no copy, no work
        const fs::path media = f.root / "complete_media", cache = f.root / "complete_cache";
        fs::create_directories(media);
        fs::copy_file(out.assets, media / kIconsAssetsName);
        fs::copy_file(out.streamed, media / kIconsStreamName);
        const IconPlan plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.files.assets.empty(), "a source with the complete set is used as it is");
        Check(!fs::exists(cache / kIconsAssetsName) && !fs::exists(cache / kIconsStreamName), "and no prepared copy is made of it");
        std::string report;
        Check(PrepareControllerIcons(media, cache, report).kind == IconPlanKind::kUseOriginal && report.empty(), "asking to prepare it does no work");
        Check(ReadText(cache / "controller_icons.stamp").find(" complete") != std::string::npos, "the stamp records that it is complete");
        Check(PlanControllerIcons(media, cache).kind == IconPlanKind::kUseOriginal, "and it stays so");
    }
    {   // a source that is not the analysed one: cached as such, never offered as work
        const fs::path media = f.root / "refused_media", cache = f.root / "refused_cache";
        fs::create_directories(media);
        Bytes assets = ReadAll(f.original.assets);
        assets[11] = 21;
        WriteAll(media / kIconsAssetsName, assets);
        fs::copy_file(f.original.streamed, media / kIconsStreamName);
        const IconPlan plan = PlanControllerIcons(media, cache);
        Check(plan.kind == IconPlanKind::kUseOriginal && plan.note.find("used as they are") != std::string::npos, "an unknown source is used as it is, with the reason");
        Check(ReadText(cache / "controller_icons.stamp").find(" unsupported") != std::string::npos && PlanControllerIcons(media, cache).kind == IconPlanKind::kUseOriginal,
              "the refusal is remembered and never becomes work");
    }
    {   // a partly patched source is never offered as work either
        const fs::path media = f.root / "partial_media", cache = f.root / "partial_cache";
        fs::create_directories(media);
        fs::copy_file(f.root / "partial" / kIconsAssetsName, media / kIconsAssetsName);
        fs::copy_file(f.root / "partial" / kIconsStreamName, media / kIconsStreamName);
        std::string report;
        const IconPlan plan = PrepareControllerIcons(media, cache, report);
        Check(plan.kind == IconPlanKind::kUseOriginal && !fs::exists(cache / kIconsAssetsName), "a partly patched source is used as it is and no copy is made");
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
    Check(std::string(kIconsGuestAssets) == "/app0/Media/resources.assets" && std::string(kIconsGuestStream) == "/app0/Media/resources.assets.resS",
          "the two guest paths are the exact game files");
    const std::string loader = ReadSource(root / "src" / "loader.c");
    const auto count = [&](const std::string& text) {
        std::size_t found = 0;
        for (std::size_t at = loader.find(text); at != std::string::npos; at = loader.find(text, at + 1)) ++found;
        return found;
    };
    Check(count("runtime_file_alias(") == 2 && count("runtime_file_alias(\"/app0/Media/resources.assets\", controller_icons.assets)") == 1 &&
              count("runtime_file_alias(\"/app0/Media/resources.assets.resS\", controller_icons.streamed)") == 1,
          "the loader redirects exactly the two game files to the prepared pair and nothing else");
    Check(loader.find("controller_icons.assets[0] && controller_icons.streamed[0] &&") != std::string::npos, "and only when both members were prepared and validated");
    const std::string runtime = ReadSource(root / "src" / "runtime_file.c");
    Check(runtime.find("same_path_ignoring_case(path_aliases[i].guest,buffer)") != std::string::npos, "the file layer matches the whole normalized path of an alias");
    Check(ReadSource(root / "launcher" / "launcher.cpp").find("prompt_style == SOS_PROMPTS_PLAYSTATION || !build_supported") != std::string::npos,
          "PlayStation prompts and unsupported builds prepare nothing");
    Check(ReadSource(root / "launcher" / "controller_icons.cpp").find("runtime_file_overlay") == std::string::npos && loader.find("Controller icons: prepared copies in use") != std::string::npos,
          "the icons are whole prepared files, not a sparse read overlay");
}

}  // namespace

int main() {
    const fs::path root = SOS_SOURCE_DIR;
    TestRedirection(root);
    Fixture f;
    f.source.clear();
    for (const char* folder : {"CUSA57767-1.00", "v1.00"}) {
        const fs::path media = root / "game" / "prepared" / folder / "Media";
        if (f.source.empty() && fs::is_regular_file(media / kIconsAssetsName) && fs::is_regular_file(media / kIconsStreamName)) f.source = media;
    }
    if (f.source.empty()) {
        std::printf("  [skip] CUSA57767 1.00: the game's resources.assets is not installed, the assets were not read\n");
    } else {
        f.root = TempRoot();
        f.original = {f.source / kIconsAssetsName, f.source / kIconsStreamName};
        const IconFiles out = Folder(f.root / "out");
        const auto timed = [](const char* name, const auto& run) {
            const auto start = std::chrono::steady_clock::now();
            run();
            std::printf("  [time] %s %lld ms\n", name, static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
        };
        timed("source", [&] { TestSource(f); });
        timed("patch", [&] { TestPatch(f, out); });
        timed("prepared", [&] { TestPrepared(f, out); });
        timed("untouched objects", [&] { TestUntouchedObjects(f, out); });
        timed("states", [&] { TestStates(f, out); });
        timed("cache", [&] { TestCache(f); });
        timed("complete source and refusals", [&] { TestCompleteSourceAndRefusals(f, out); });
        std::error_code error;
        fs::remove_all(f.root, error);
    }
    const fs::path other = root / "game" / "prepared" / "CUSA57767-1.01" / "Media";
    if (fs::is_regular_file(other / kIconsAssetsName) && fs::is_regular_file(other / kIconsStreamName)) {
        const fs::path work = TempRoot();
        const IconPlan plan = PlanControllerIcons(other, work / "cache");
        Check(plan.kind != IconPlanKind::kUsePrepared || HasControllerIcons(plan.files), "1.01: the source is either prepared, complete or refused, never half used");
        std::error_code error;
        fs::remove_all(work, error);
    } else {
        std::printf("  [skip] CUSA57767 1.01: its resources.assets is not installed; asset preparation is not checked for it and refuses unknown data\n");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

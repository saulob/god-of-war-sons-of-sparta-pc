#include "controller_icons.h"

#include "icon_bundle.h"
#include "icon_common.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace sos {
namespace {

namespace fs = std::filesystem;
using namespace icons;

// Object IDs of the analysed resources.assets (Unity 2022.3, serialized file version 22, no type tree).
constexpr std::int64_t kIconData = 50154;   // PS5InputIconData
constexpr std::int64_t kXboxAtlas = 50372;  // Xbox_ButtonPromptsAtlas, the TextMeshPro sprite asset of Xbox prompt text
constexpr std::int64_t kPs5Atlas = 50444;   // New_PS_ButtonPromptsAtlas
constexpr std::int64_t kLookInputSprite = 40906;
constexpr std::int32_t kMonoBehaviourClass = 114, kTextureClass = 28, kSpriteClass = 213;
constexpr std::int32_t kRgba32 = 4;
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kMonoBehaviourHeader = 28;
// FNV-1a of the objects the patch reads, so only the analysed game data is patched.
constexpr std::uint64_t kSourceFingerprint = 0xb4fd2fbbba9f5f03;

std::string ReadString(const Bytes& data, std::size_t at, std::size_t* end = nullptr) {
    const auto length = Get<std::uint32_t>(data, at);
    Require(length <= 256 && at + 4 + length <= data.size(), "a string is not in the data");
    if (end != nullptr) *end = Align(at + 4 + length, 4);
    return std::string(reinterpret_cast<const char*>(data.data() + at + 4), length);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The serialized file: Unity 2022.3, version 22, little-endian, no type tree. Only the header, the type list and the object
// table are interpreted; every object is opaque bytes until a layout below reads it.

struct Object {
    std::int64_t id = 0;
    std::int64_t start = 0;  // from the data offset
    std::uint32_t size = 0;
    std::int32_t type = 0;
};

class AssetFile {
public:
    explicit AssetFile(const fs::path& path) : path_(path), stream_(path, std::ios::binary) {
        Require(stream_.is_open(), "the file could not be opened: " + path.string());
        std::error_code error;
        const std::uint64_t actual = fs::file_size(path, error);
        Require(!error, "the file size is unknown: " + path.string());
        const Bytes head = ReadAt(0, kHeaderSize);
        Recognise(BigEndian(head, 8, 4) == 22 && head[16] == 0, "not a version 22 little-endian serialized file: " + path.string());
        const std::uint64_t metadataSize = BigEndian(head, 20, 4);
        fileSize_ = BigEndian(head, 24, 8);
        dataOffset_ = BigEndian(head, 32, 8);
        Require(fileSize_ == actual, "the file is not the size its header says (truncated or damaged): " + path.string());
        Recognise(metadataSize > 0 && metadataSize < (64u << 20) && kHeaderSize + metadataSize <= dataOffset_ && dataOffset_ <= fileSize_ &&
                      dataOffset_ == Align(kHeaderSize + metadataSize, 16),
                  "the serialized file layout is not the analysed one");
        head_ = head;
        metadata_ = ReadAt(kHeaderSize, metadataSize);
        ParseMetadata();
    }

    // Reads the file into memory, which preparation does once.
    void LoadAll() {
        whole_ = ReadAt(0, fileSize_);
    }

    Bytes ReadAt(std::uint64_t offset, std::size_t size) const {
        if (!whole_.empty()) {
            Require(offset <= whole_.size() && size <= whole_.size() - offset, "a read goes past the end of the file");
            return Bytes(whole_.begin() + static_cast<std::ptrdiff_t>(offset), whole_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }
        Bytes data(size);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        Require(static_cast<std::size_t>(stream_.gcount()) == size, "the file ended early: " + path_.string());
        return data;
    }

    Bytes Read(const Object& object) const { return ReadAt(dataOffset_ + static_cast<std::uint64_t>(object.start), object.size); }

    const Object* Find(std::int64_t id) const {
        const auto at = byId_.find(id);
        return at == byId_.end() ? nullptr : &objects_[at->second];
    }

    std::int32_t ClassOf(const Object& object) const { return classes_[static_cast<std::size_t>(object.type)]; }

    // The object's name when its class stores one first (MonoBehaviour after its 28-byte header, Sprite and Texture2D at 0).
    std::string NameOf(const Object& object) const {
        const std::int32_t classId = ClassOf(object);
        const std::size_t at = classId == kMonoBehaviourClass ? 28 : 0;
        if (classId != kMonoBehaviourClass && classId != kSpriteClass && classId != kTextureClass) return {};
        if (object.size < at + 4) return {};
        const std::uint64_t base = dataOffset_ + static_cast<std::uint64_t>(object.start) + at;
        const auto length = Get<std::uint32_t>(ReadAt(base, 4), 0);
        if (length > 256 || length > object.size - at - 4) return {};
        const Bytes text = ReadAt(base + 4, length);
        return std::string(text.begin(), text.end());
    }

    const fs::path& path() const { return path_; }
    std::uint64_t fileSize() const { return fileSize_; }
    std::uint64_t dataOffset() const { return dataOffset_; }
    const Bytes& head() const { return head_; }
    const Bytes& metadata() const { return metadata_; }
    const std::vector<Object>& objects() const { return objects_; }
    std::size_t objectCountAt() const { return objectCountAt_; }
    std::size_t entryAt(std::size_t index) const { return entriesAt_ + index * 24; }
    std::size_t IndexOf(std::int64_t id) const { return byId_.at(id); }

private:
    void ParseMetadata() {
        std::size_t at = 0;
        while (at < metadata_.size() && metadata_[at] != 0) ++at;
        Require(at < metadata_.size(), "the metadata has no version string");
        at += 1 + 4;  // version string, target platform
        Recognise(Get<std::uint8_t>(metadata_, at) == 0, "the serialized file has a type tree");
        ++at;
        const auto typeCount = Get<std::int32_t>(metadata_, at);
        at += 4;
        Require(typeCount > 0 && typeCount < 100000, "the type count is not plausible");
        for (std::int32_t index = 0; index < typeCount; ++index) {
            const auto classId = Get<std::int32_t>(metadata_, at);
            at += 4 + 1 + 2;
            if (classId == kMonoBehaviourClass) at += 16;
            at += 16;
            classes_.push_back(classId);
        }
        objectCountAt_ = at;
        const auto count = Get<std::int32_t>(metadata_, at);
        entriesAt_ = Align(at + 4, 4);
        Require(count > 0 && count < 10000000 && entriesAt_ + std::size_t{static_cast<std::uint32_t>(count)} * 24 <= metadata_.size(), "the object table is not plausible");
        objects_.reserve(static_cast<std::size_t>(count));
        for (std::int32_t index = 0; index < count; ++index) {
            const std::size_t entry = entriesAt_ + static_cast<std::size_t>(index) * 24;
            Object object;
            object.id = Get<std::int64_t>(metadata_, entry);
            object.start = Get<std::int64_t>(metadata_, entry + 8);
            object.size = Get<std::uint32_t>(metadata_, entry + 16);
            object.type = Get<std::int32_t>(metadata_, entry + 20);
            Require(object.type >= 0 && static_cast<std::size_t>(object.type) < classes_.size(), "an object has an unknown type");
            Require(object.start >= 0 && static_cast<std::uint64_t>(object.start) + object.size <= fileSize_ - dataOffset_, "an object lies outside the file");
            Require(byId_.emplace(object.id, objects_.size()).second, "two objects share an ID");
            objects_.push_back(object);
        }
        objectsEnd_ = entriesAt_ + static_cast<std::size_t>(count) * 24;
    }

    fs::path path_;
    mutable std::ifstream stream_;
    std::uint64_t fileSize_ = 0, dataOffset_ = 0;
    Bytes head_, metadata_, whole_;
    std::vector<std::int32_t> classes_;
    std::vector<Object> objects_;
    std::map<std::int64_t, std::size_t> byId_;
    std::size_t objectCountAt_ = 0, entriesAt_ = 0, objectsEnd_ = 0;

public:
    std::size_t objectsEnd() const { return objectsEnd_; }
};

// ---------------------------------------------------------------------------------------------------------------------------
// Layouts of the objects the patch reads or changes, read against the analysed data. Each parser consumes its object to the
// last byte, so a layout that is not the analysed one is refused instead of half understood.

using SpecialIcons = std::vector<std::pair<std::string, std::int64_t>>;

struct SpecialMap {
    std::size_t keysCountAt = 0, keysEnd = 0, valuesCountAt = 0, valuesEnd = 0;
    SpecialIcons icons;
};

struct IconData {
    Bytes bytes;
    std::array<SpecialMap, 3> maps;  // PlayStation 5, PlayStation 4, Xbox Series
    std::array<std::int64_t, 4> atlases{};
};

constexpr std::size_t kXboxMap = 2;

IconData ParseIconData(const Bytes& bytes) {
    IconData data;
    data.bytes = bytes;
    Recognise(ReadString(bytes, 28) == "PS5InputIconData", "the icon data object is not the analysed one");
    std::size_t at = 0x30;
    for (SpecialMap& map : data.maps) {
        at += 20 * 12;  // one sprite for each of the twenty buttons
        map.keysCountAt = at;
        const auto keys = Get<std::int32_t>(bytes, at);
        Recognise(keys >= 0 && keys < 64, "a special icon map is not plausible");
        at += 4;
        std::vector<std::string> names;
        for (std::int32_t index = 0; index < keys; ++index) {
            std::size_t end = 0;
            names.push_back(ReadString(bytes, at, &end));
            at = end;
        }
        map.keysEnd = at;
        map.valuesCountAt = at;
        Recognise(Get<std::int32_t>(bytes, at) == keys, "a special icon map has another number of keys and values");
        at += 4;
        for (std::int32_t index = 0; index < keys; ++index) {
            Recognise(Get<std::int32_t>(bytes, at) == 0, "a special icon points outside the file");
            map.icons.emplace_back(names[static_cast<std::size_t>(index)], Get<std::int64_t>(bytes, at + 4));
            at += 12;
        }
        map.valuesEnd = at;
    }
    Recognise(bytes.size() >= at + 4 + 4 + 4 * 12, "the icon data object is shorter than analysed");
    for (std::size_t index = 0; index < data.atlases.size(); ++index) {
        const std::size_t pointer = bytes.size() - (data.atlases.size() - index) * 12;
        Recognise(Get<std::int32_t>(bytes, pointer) == 0, "an atlas pointer is not local");
        data.atlases[index] = Get<std::int64_t>(bytes, pointer + 4);
    }
    Recognise(data.atlases[0] == kPs5Atlas && data.atlases[2] == kXboxAtlas, "the icon data uses other atlases");
    return data;
}

struct Character {
    std::string name;
    std::uint32_t glyph = 0, hash = 0;
    std::size_t begin = 0, end = 0;
};

struct Glyph {
    std::uint32_t index = 0;
    Rect rect;
    std::int64_t sprite = 0;
    std::size_t begin = 0, end = 0;
};

constexpr std::size_t kGlyphSize = 64;

struct Atlas {
    Bytes bytes;
    std::int64_t sheet = 0;
    std::vector<Character> characters;
    std::vector<Glyph> glyphs;
    std::size_t charactersCountAt = 0, charactersEnd = 0, glyphsCountAt = 0, glyphsEnd = 0;
};

Atlas ParseAtlas(const Bytes& bytes, bool exactTail) {
    Atlas atlas;
    atlas.bytes = bytes;
    std::size_t at = 0;
    ReadString(bytes, 28, &at);
    at += 4 + 12 + 4;  // hashCode, material, materialHashCode
    ReadString(bytes, at, &at);
    at += 4;  // FaceInfo: face index, two names, then point size, scale, units per em and fifteen metrics
    ReadString(bytes, at, &at);
    ReadString(bytes, at, &at);
    at += 72;
    Recognise(Get<std::int32_t>(bytes, at) == 0, "the sprite sheet is not a local object");
    atlas.sheet = Get<std::int64_t>(bytes, at + 4);
    at += 12;
    atlas.charactersCountAt = at;
    const auto characters = Get<std::int32_t>(bytes, at);
    Recognise(characters > 0 && characters < 1000, "the character table is not plausible");
    at += 4;
    for (std::int32_t index = 0; index < characters; ++index) {
        Character character;
        character.begin = at;
        Recognise(Get<std::int32_t>(bytes, at) == 2 && Get<std::uint32_t>(bytes, at + 4) == 0xfffe, "a character is not a sprite");
        character.glyph = Get<std::uint32_t>(bytes, at + 8);
        std::size_t end = 0;
        character.name = ReadString(bytes, at + 16, &end);
        character.hash = Get<std::uint32_t>(bytes, end);
        at = end + 4;
        character.end = at;
        atlas.characters.push_back(character);
    }
    atlas.charactersEnd = at;
    atlas.glyphsCountAt = at;
    const auto glyphs = Get<std::int32_t>(bytes, at);
    Recognise(glyphs > 0 && glyphs < 1000, "the glyph table is not plausible");
    at += 4;
    for (std::int32_t index = 0; index < glyphs; ++index) {
        Glyph glyph;
        glyph.begin = at;
        glyph.index = Get<std::uint32_t>(bytes, at);
        glyph.rect = {Get<std::int32_t>(bytes, at + 24), Get<std::int32_t>(bytes, at + 28), Get<std::int32_t>(bytes, at + 32), Get<std::int32_t>(bytes, at + 36)};
        Recognise(Get<std::int32_t>(bytes, at + 52) == 0, "a glyph sprite is not a local object");
        glyph.sprite = Get<std::int64_t>(bytes, at + 56);
        at += kGlyphSize;
        glyph.end = at;
        atlas.glyphs.push_back(glyph);
    }
    atlas.glyphsEnd = at;
    if (exactTail) Recognise(bytes.size() == at + 8 && Get<std::int64_t>(bytes, at) == 0, "the sprite asset has other data after its glyphs");
    for (const Character& character : atlas.characters) {
        Recognise(character.glyph < atlas.glyphs.size(), "a character refers to a glyph that does not exist");
        Recognise(TextMeshProHash(character.name) == character.hash, "the sprite asset uses another name hash");
    }
    return atlas;
}

// A Sprite as 2022.3 serializes it, from its name to the empty bone list.
struct SpriteLayout {
    std::size_t nameEnd = 0, rect = 0, keySecond = 0, texture = 0;
    std::size_t subMeshes = 0, subMeshesEnd = 0, indices = 0, indicesEnd = 0, vertexCount = 0, vertices = 0, verticesEnd = 0;
    std::size_t textureRect = 0, textureRectOffset = 0, uvTransform = 0, physics = 0, physicsEnd = 0;
    std::array<std::uint8_t, 16> key{};
};

SpriteLayout ParseSprite(const Bytes& bytes) {
    SpriteLayout layout;
    std::size_t at = 0;
    ReadString(bytes, 0, &at);
    layout.nameEnd = at;
    layout.rect = at;
    at += 16 + 8 + 16 + 4 + 8 + 4 + 4;  // rect, offset, border, pixels per unit, pivot, extrude, polygon flag
    std::memcpy(layout.key.data(), bytes.data() + at, 16);
    layout.keySecond = at + 16;
    at += 24;
    const auto tags = Get<std::int32_t>(bytes, at);
    Recognise(tags == 0, "a sprite has atlas tags");
    at += 4 + 12;  // tags, sprite atlas
    layout.texture = at;
    at += 12 + 12;  // texture, alpha texture
    Recognise(Get<std::int32_t>(bytes, at) == 0, "a sprite has secondary textures");
    at += 4;
    layout.subMeshes = at;
    const auto subMeshes = Get<std::int32_t>(bytes, at);
    Recognise(subMeshes == 1, "a sprite has several sub meshes");
    at += 4 + 48;
    layout.subMeshesEnd = at;
    layout.indices = at;
    at = Align(at + 4 + Get<std::uint32_t>(bytes, at), 4);
    layout.indicesEnd = at;
    layout.vertexCount = at;
    at += 4;
    const auto channels = Get<std::int32_t>(bytes, at);
    Recognise(channels > 0 && channels < 32, "a sprite has an implausible vertex layout");
    at += 4 + static_cast<std::size_t>(channels) * 4;
    layout.vertices = at;
    at = Align(at + 4 + Get<std::uint32_t>(bytes, at), 4);
    layout.verticesEnd = at;
    Recognise(Get<std::int32_t>(bytes, at) == 0, "a sprite has a bind pose");
    at += 4;
    layout.textureRect = at;
    layout.textureRectOffset = at + 16;
    at += 16 + 8 + 8 + 4;  // texture rect, its offset, the atlas rect offset, settings
    layout.uvTransform = at;
    at += 16 + 4;
    layout.physics = at;
    const auto outlines = Get<std::int32_t>(bytes, at);
    Recognise(outlines >= 0 && outlines < 1000, "a sprite has an implausible physics shape");
    at += 4;
    for (std::int32_t outline = 0; outline < outlines; ++outline) {
        const auto points = Get<std::int32_t>(bytes, at);
        Recognise(points >= 0 && points < 100000, "a sprite has an implausible outline");
        at += 4 + static_cast<std::size_t>(points) * 8;
    }
    layout.physicsEnd = at;
    Recognise(Get<std::int32_t>(bytes, at) == 0, "a sprite has bones");
    at += 4;
    Recognise(at == bytes.size(), "a sprite has other data than analysed");
    return layout;
}

struct Texture {
    std::int64_t id = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t streamOffset = 0;
    std::uint32_t streamSize = 0;
    Bytes tiled;

    // PlayStation 4 1D thin tiling of 4-byte texels: 8x8 micro tiles in row order, each in Morton order.
    static std::size_t Offset(std::uint32_t x, std::uint32_t y, std::uint32_t width) { return TexelOffset(x, y, width); }
    std::uint8_t Texel(std::uint32_t x, std::uint32_t y, int channel) const { return tiled[Offset(x, y, width) + static_cast<std::size_t>(channel)]; }
};

struct TextureObject {
    std::string name;
    std::uint32_t width = 0, height = 0;
    std::uint64_t streamOffset = 0;
    std::uint32_t streamSize = 0;
};

TextureObject ParseTextureObject(const Bytes& bytes) {
    TextureObject texture;
    std::size_t at = 0;
    texture.name = ReadString(bytes, 0, &at);
    texture.width = Get<std::uint32_t>(bytes, at + 8);
    texture.height = Get<std::uint32_t>(bytes, at + 12);
    const auto format = Get<std::int32_t>(bytes, at + 24);
    const auto mips = Get<std::int32_t>(bytes, at + 28);
    const std::size_t path = bytes.size() - Align(4 + sizeof(kIconsStreamName) - 1, 4);
    Recognise(path > at + 80 && ReadString(bytes, path) == kIconsStreamName, "a texture does not stream from resources.assets.resS");
    Recognise(Get<std::int64_t>(bytes, path - 20) == 0, "a texture holds its pixels itself");
    texture.streamOffset = Get<std::uint64_t>(bytes, path - 12);
    texture.streamSize = Get<std::uint32_t>(bytes, path - 4);
    Recognise(format == kRgba32 && mips == 1 && texture.width % 8 == 0 && texture.height % 8 == 0 &&
                  texture.streamSize == std::size_t{texture.width} * texture.height * 4,
              "a prompt texture is not uncompressed RGBA32 without mip maps");
    return texture;
}

Texture ReadTexture(const AssetFile& file, std::int64_t id, const fs::path& streamed) {
    const Object* object = file.Find(id);
    Recognise(object != nullptr && file.ClassOf(*object) == kTextureClass, "a prompt texture is missing");
    const TextureObject parsed = ParseTextureObject(file.Read(*object));
    Texture texture;
    texture.id = id;
    texture.width = parsed.width;
    texture.height = parsed.height;
    texture.streamOffset = parsed.streamOffset;
    texture.streamSize = parsed.streamSize;
    std::ifstream stream(streamed, std::ios::binary);
    Require(stream.is_open(), "the streamed data could not be opened: " + streamed.string());
    std::error_code error;
    const std::uint64_t size = fs::file_size(streamed, error);
    Require(!error && texture.streamOffset + texture.streamSize <= size, "the streamed data ends before a prompt texture");
    texture.tiled.resize(texture.streamSize);
    stream.seekg(static_cast<std::streamoff>(texture.streamOffset));
    stream.read(reinterpret_cast<char*>(texture.tiled.data()), static_cast<std::streamsize>(texture.tiled.size()));
    Require(static_cast<std::size_t>(stream.gcount()) == texture.tiled.size(), "the streamed data ended early");
    return texture;
}

// ---------------------------------------------------------------------------------------------------------------------------
// What the file holds now.

struct Inspection {
    IconSetState state = IconSetState::kUnrecognised;
    std::string why;
    IconData icons;
    Atlas xbox;
};

bool IsXboxStickName(const std::string& name) { return name.rfind("Xbox_LeftStick_", 0) == 0; }

const std::int64_t* FindIcon(const SpecialIcons& icons, const std::string& key) {
    for (const auto& [name, sprite] : icons) {
        if (name == key) return &sprite;
    }
    return nullptr;
}

Inspection Inspect(const AssetFile& file, bool scanSprites) {
    Inspection result;
    const Object* icon = file.Find(kIconData);
    const Object* xboxAtlas = file.Find(kXboxAtlas);
    Recognise(icon != nullptr && xboxAtlas != nullptr && file.ClassOf(*icon) == kMonoBehaviourClass && file.ClassOf(*xboxAtlas) == kMonoBehaviourClass &&
                  file.NameOf(*xboxAtlas) == "Xbox_ButtonPromptsAtlas",
              "the file is not the analysed game data");
    result.icons = ParseIconData(file.Read(*icon));
    result.xbox = ParseAtlas(file.Read(*xboxAtlas), true);
    const SpecialIcons& xboxIcons = result.icons.maps[kXboxMap].icons;

    std::size_t tokens = 0, characters = 0, sprites = 0;
    for (const Icon& item : kIcons) tokens += FindIcon(xboxIcons, item.token) != nullptr;
    for (const Character& character : result.xbox.characters) characters += IsXboxStickName(character.name);
    if (scanSprites) {
        for (const Object& object : file.objects()) {
            if (file.ClassOf(object) == kSpriteClass && IsXboxStickName(file.NameOf(object))) ++sprites;
        }
    } else {
        for (const auto& [key, id] : xboxIcons) {
            const Object* object = file.Find(id);
            sprites += object != nullptr && file.ClassOf(*object) == kSpriteClass && IsXboxStickName(file.NameOf(*object));
        }
    }

    if (tokens == 0 && characters == 0 && sprites == 0) {
        result.state = IconSetState::kMissing;
        return result;
    }
    const bool full = tokens == kIconCount && characters == kIconCount && sprites == kIconCount && xboxIcons.size() == kIconCount + 1;
    bool consistent = full;
    for (const Icon& item : kIcons) {
        const std::int64_t* id = FindIcon(xboxIcons, item.token);
        const Object* sprite = id != nullptr ? file.Find(*id) : nullptr;
        const auto character = std::find_if(result.xbox.characters.begin(), result.xbox.characters.end(), [&](const Character& c) { return c.name == item.sprite; });
        const bool ok = id != nullptr && sprite != nullptr && file.ClassOf(*sprite) == kSpriteClass && file.NameOf(*sprite) == item.sprite &&
                        character != result.xbox.characters.end() && result.xbox.glyphs[character->glyph].sprite == *id;
        consistent = consistent && ok;
    }
    if (full && consistent) {
        result.state = IconSetState::kComplete;
    } else {
        result.state = IconSetState::kPartial;
        result.why = "the Xbox prompt set is only partly patched";
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The patch.

struct NewSprite {
    const Icon* icon = nullptr;
    std::int64_t id = 0, renderKey = 0;
    Cell cell, source;
};

struct Plan {
    std::array<NewSprite, kIconCount> sprites;
    Bytes templateSprite;
    SpriteLayout templateLayout;
    Texture xbox, playStation;
    Bytes iconData, atlas;
    std::vector<Bytes> spriteObjects;
    std::int64_t xboxSheet = 0;
};

// The 64x64 opaque square of a PlayStation glyph: its rectangle is a little larger.
Cell LocateSource(const Texture& texture, const Rect& rect, const std::string& name) {
    const auto from = [](std::int32_t value) { return static_cast<std::uint32_t>(std::max(value - 4, 0)); };
    const std::uint32_t left = from(rect.x), top = from(rect.y);
    const std::uint32_t right = std::min(static_cast<std::uint32_t>(rect.x + rect.width + 4), texture.width);
    const std::uint32_t bottom = std::min(static_cast<std::uint32_t>(rect.y + rect.height + 4), texture.height);
    std::uint32_t minX = texture.width, minY = texture.height, maxX = 0, maxY = 0;
    for (std::uint32_t y = top; y < bottom; ++y) {
        for (std::uint32_t x = left; x < right; ++x) {
            if (texture.Texel(x, y, 3) == 0) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    Recognise(maxX >= minX && maxX - minX + 1 == kCell && maxY - minY + 1 == kCell, "the PlayStation glyph " + name + " is not " + std::to_string(kCell) + " texels square");
    return {minX, minY};
}

Bytes MakeSprite(const Bytes& object, const SpriteLayout& layout, const NewSprite& sprite) {
    const float x = static_cast<float>(sprite.cell.x), y = static_cast<float>(sprite.cell.y), size = static_cast<float>(kCell);
    const float half = size / 200.0f;
    Writer quad;
    quad.Floats({-half, half, 0.0f, half, -half, 0.0f, half, half, 0.0f, -half, -half, 0.0f});
    quad.Floats({0, 0, 0, 0, 0, 0, 0, 0});
    Writer indices;
    for (const int index : {3, 0, 1, 2, 1, 0}) indices.Put(static_cast<std::uint16_t>(index));
    Writer rect;
    rect.Floats({x, y, size, size});
    Writer uv;
    uv.Floats({100.0f, x + size / 2.0f, 100.0f, y + size / 2.0f});
    Writer offset;
    offset.Floats({0.0f, 0.0f});

    Bytes subMesh(object.begin() + static_cast<std::ptrdiff_t>(layout.subMeshes + 4), object.begin() + static_cast<std::ptrdiff_t>(layout.subMeshesEnd));
    Require(subMesh.size() == 48, "the template sprite sub mesh is not as analysed");
    const Bytes indexCount = Raw(std::uint32_t{6}), vertexCount = Raw(std::uint32_t{4});
    std::copy(indexCount.begin(), indexCount.end(), subMesh.begin() + 4);
    std::copy(vertexCount.begin(), vertexCount.end(), subMesh.begin() + 20);
    Bytes subMeshArray = Raw(std::int32_t{1});
    subMeshArray.insert(subMeshArray.end(), subMesh.begin(), subMesh.end());
    Bytes indexArray = Raw(static_cast<std::int32_t>(indices.data.size()));
    indexArray.insert(indexArray.end(), indices.data.begin(), indices.data.end());
    indexArray.resize(Align(indexArray.size(), 4));
    Bytes vertexArray = Raw(static_cast<std::int32_t>(quad.data.size()));
    vertexArray.insert(vertexArray.end(), quad.data.begin(), quad.data.end());
    vertexArray.resize(Align(vertexArray.size(), 4));

    return Splice(object, {{0, layout.nameEnd, StringBytes(sprite.icon->sprite)},
                           {layout.rect, layout.rect + 16, rect.data},
                           {layout.keySecond, layout.keySecond + 8, Raw(sprite.renderKey)},
                           {layout.subMeshes, layout.subMeshesEnd, subMeshArray},
                           {layout.indices, layout.indicesEnd, indexArray},
                           {layout.vertexCount, layout.vertexCount + 4, Raw(std::uint32_t{4})},
                           {layout.vertices, layout.verticesEnd, vertexArray},
                           {layout.textureRect, layout.textureRect + 16, rect.data},
                           {layout.textureRectOffset, layout.textureRectOffset + 8, offset.data},
                           {layout.uvTransform, layout.uvTransform + 16, uv.data},
                           {layout.physics, layout.physicsEnd, Raw(std::int32_t{0})}});
}

Bytes MakeIconData(const IconData& data, const Plan& plan) {
    const SpecialMap& map = data.maps[kXboxMap];
    Writer keys, values;
    for (const NewSprite& sprite : plan.sprites) {
        keys.String(sprite.icon->token);
        values.Pointer(sprite.id);
    }
    const auto count = [&](std::size_t at, std::size_t size) {
        return Edit{at, at + 4, Raw(static_cast<std::int32_t>(size + kIconCount))};
    };
    return Splice(data.bytes, {count(map.keysCountAt, map.icons.size()), {map.keysEnd, map.keysEnd, keys.data}, count(map.valuesCountAt, map.icons.size()),
                               {map.valuesEnd, map.valuesEnd, values.data}});
}

Bytes MakeAtlas(const Atlas& atlas, const Plan& plan) {
    const Character& characterTemplate = atlas.characters.back();
    const Glyph& glyphTemplate = atlas.glyphs.back();
    Bytes characters, glyphs;
    for (std::size_t index = 0; index < kIconCount; ++index) {
        const NewSprite& sprite = plan.sprites[index];
        const auto glyphIndex = static_cast<std::uint32_t>(atlas.glyphs.size() + index);
        Bytes character(atlas.bytes.begin() + static_cast<std::ptrdiff_t>(characterTemplate.begin), atlas.bytes.begin() + static_cast<std::ptrdiff_t>(characterTemplate.end));
        std::size_t nameEnd = 0;
        ReadString(character, 16, &nameEnd);
        Writer name;
        name.String(sprite.icon->sprite);
        name.Put(TextMeshProHash(sprite.icon->sprite));
        const Bytes head = Raw(glyphIndex);
        std::copy(head.begin(), head.end(), character.begin() + 8);
        character = Splice(character, {{16, nameEnd + 4, name.data}});
        Bytes glyph(atlas.bytes.begin() + static_cast<std::ptrdiff_t>(glyphTemplate.begin), atlas.bytes.begin() + static_cast<std::ptrdiff_t>(glyphTemplate.end));
        const auto put = [&](std::size_t at, const Bytes& value) { std::copy(value.begin(), value.end(), glyph.begin() + static_cast<std::ptrdiff_t>(at)); };
        put(0, Raw(glyphIndex));
        put(24, Raw(static_cast<std::int32_t>(sprite.cell.x)));
        put(28, Raw(static_cast<std::int32_t>(sprite.cell.y)));
        put(32, Raw(static_cast<std::int32_t>(kCell)));
        put(36, Raw(static_cast<std::int32_t>(kCell)));
        put(56, Raw(sprite.id));
        characters.insert(characters.end(), character.begin(), character.end());
        glyphs.insert(glyphs.end(), glyph.begin(), glyph.end());
    }
    const auto count = [&](std::size_t at, std::size_t size) {
        return Edit{at, at + 4, Raw(static_cast<std::int32_t>(size + kIconCount))};
    };
    return Splice(atlas.bytes, {count(atlas.charactersCountAt, atlas.characters.size()), {atlas.charactersEnd, atlas.charactersEnd, characters},
                                count(atlas.glyphsCountAt, atlas.glyphs.size()), {atlas.glyphsEnd, atlas.glyphsEnd, glyphs}});
}

// The update renumbers the 28-byte MonoBehaviour script header; the patch never reads it.
Bytes FingerprintObject(const AssetFile& file, const Object& object) {
    Bytes bytes = file.Read(object);
    if (file.ClassOf(object) != kMonoBehaviourClass) return bytes;
    Require(bytes.size() > kMonoBehaviourHeader, "a MonoBehaviour object is shorter than its header");
    bytes.erase(bytes.begin(), bytes.begin() + kMonoBehaviourHeader);
    return bytes;
}

std::uint64_t SourceFingerprint(const AssetFile& file, const Plan& plan, const std::int64_t (&sources)[kIconCount], std::int64_t playStationSheet) {
    std::uint64_t hash = 0xcbf29ce484222325;
    const auto mix = [&](const Bytes& bytes) { hash = Fnv64(bytes.data(), bytes.size(), hash); };
    mix(FingerprintObject(file, *file.Find(kIconData)));
    mix(FingerprintObject(file, *file.Find(kXboxAtlas)));
    mix(FingerprintObject(file, *file.Find(kPs5Atlas)));
    mix(plan.templateSprite);
    mix(FingerprintObject(file, *file.Find(kLookInputSprite)));
    for (const std::int64_t id : sources) mix(FingerprintObject(file, *file.Find(id)));
    mix(FingerprintObject(file, *file.Find(plan.xboxSheet)));
    mix(FingerprintObject(file, *file.Find(playStationSheet)));
    return hash;
}

Plan Analyse(const AssetFile& file, const IconFiles& source, const Inspection& inspection) {
    Plan plan;
    const SpecialIcons& xboxIcons = inspection.icons.maps[kXboxMap].icons;
    Recognise(xboxIcons.size() == 1 && xboxIcons[0].first == "!LookInput" && xboxIcons[0].second == kLookInputSprite, "the Xbox special icon map is not the analysed one");
    const Atlas& atlas = inspection.xbox;
    const std::size_t existing = atlas.characters.size();
    Recognise(existing == 19 && atlas.glyphs.size() == existing, "the Xbox atlas is not the analysed one");
    std::vector<bool> glyphUsed(existing, false);
    for (std::size_t index = 0; index < existing; ++index) {
        const Character& character = atlas.characters[index];
        Recognise(!glyphUsed[character.glyph], "the Xbox atlas characters do not map to distinct glyphs");
        glyphUsed[character.glyph] = true;
        Recognise(atlas.glyphs[index].index == index, "the Xbox atlas glyphs are not in index order");
        const Glyph& glyph = atlas.glyphs[index];
        const Glyph& last = atlas.glyphs.back();
        for (const auto& [from, to] : {std::pair<std::size_t, std::size_t>{4, 24}, {40, 52}}) {
            Recognise(std::equal(atlas.bytes.begin() + static_cast<std::ptrdiff_t>(glyph.begin + from), atlas.bytes.begin() + static_cast<std::ptrdiff_t>(glyph.begin + to),
                                 atlas.bytes.begin() + static_cast<std::ptrdiff_t>(last.begin + from)),
                      "the Xbox glyphs do not share one metrics set");
        }
    }

    // A template for the new sprites: the first Xbox glyph sprite that is one cell square.
    std::vector<std::int64_t> usedKeys;
    std::vector<std::array<float, 4>> spriteRects;
    bool haveTemplate = false;
    for (const Glyph& glyph : atlas.glyphs) {
        const Object* object = file.Find(glyph.sprite);
        Recognise(object != nullptr && file.ClassOf(*object) == kSpriteClass, "an Xbox glyph does not use a Sprite");
        const Bytes bytes = file.Read(*object);
        const SpriteLayout layout = ParseSprite(bytes);
        std::array<float, 4> rect{};
        for (std::size_t part = 0; part < 4; ++part) rect[part] = Get<float>(bytes, layout.rect + part * 4);
        spriteRects.push_back(rect);
        Recognise(ReadString(bytes, 0).rfind("ABXY_Controller_Sheet", 0) == 0 && Get<std::int32_t>(bytes, layout.texture) == 0 &&
                      Get<std::int64_t>(bytes, layout.texture + 4) == atlas.sheet,
                  "an Xbox glyph sprite uses another texture");
        if (!haveTemplate && rect[2] == static_cast<float>(kCell) && rect[3] == static_cast<float>(kCell)) {
            plan.templateSprite = bytes;
            plan.templateLayout = layout;
            haveTemplate = true;
        }
    }
    Recognise(haveTemplate, "the Xbox atlas has no sprite to copy");
    for (const Object& object : file.objects()) {
        if (file.ClassOf(object) != kSpriteClass || object.size < 80) continue;
        const Bytes bytes = file.Read(object);
        std::size_t nameEnd = 0;
        ReadString(bytes, 0, &nameEnd);
        if (nameEnd + 84 > bytes.size()) continue;
        if (std::equal(plan.templateLayout.key.begin(), plan.templateLayout.key.end(), bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd + 60))) {
            usedKeys.push_back(Get<std::int64_t>(bytes, nameEnd + 76));
        }
    }

    plan.xboxSheet = atlas.sheet;
    plan.xbox = ReadTexture(file, atlas.sheet, source.streamed);
    const Object* ps5Object = file.Find(kPs5Atlas);
    Recognise(ps5Object != nullptr && file.NameOf(*ps5Object) == "New_PS_ButtonPromptsAtlas", "the PlayStation prompt atlas is missing");
    const Atlas playStation = ParseAtlas(file.Read(*ps5Object), false);
    plan.playStation = ReadTexture(file, playStation.sheet, source.streamed);
    for (const Glyph& glyph : atlas.glyphs) {
        Recognise(glyph.rect.x >= 0 && glyph.rect.y >= 0 && static_cast<std::uint32_t>(glyph.rect.x + glyph.rect.width) <= plan.xbox.width &&
                      static_cast<std::uint32_t>(glyph.rect.y + glyph.rect.height) <= plan.xbox.height,
                  "an Xbox glyph lies outside its texture");
    }

    std::vector<Cell> cells;
    for (std::uint32_t y = 0; y + kCell <= plan.xbox.height; y += kCell) {
        for (std::uint32_t x = 0; x + kCell <= plan.xbox.width; x += kCell) {
            const Rect cell{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(kCell), static_cast<std::int32_t>(kCell)};
            bool free = true;
            for (const Glyph& glyph : atlas.glyphs) free = free && !Overlaps(cell, static_cast<float>(glyph.rect.x), static_cast<float>(glyph.rect.y), static_cast<float>(glyph.rect.width), static_cast<float>(glyph.rect.height));
            for (const auto& rect : spriteRects) free = free && !Overlaps(cell, rect[0], rect[1], rect[2], rect[3]);
            for (std::uint32_t row = 0; free && row < kCell; ++row) {
                for (std::uint32_t column = 0; free && column < kCell; ++column) free = plan.xbox.Texel(x + column, y + row, 3) == 0;
            }
            if (free) cells.push_back({x, y});
        }
    }
    Recognise(cells.size() >= kIconCount, "the Xbox texture has no free cells for the new glyphs");

    const SpecialIcons& playStationIcons = inspection.icons.maps[0].icons;
    std::int64_t sources[kIconCount];
    std::int64_t nextId = 0;
    for (const Object& object : file.objects()) nextId = std::max(nextId, object.id);
    for (std::size_t index = 0; index < kIconCount; ++index) {
        NewSprite& sprite = plan.sprites[index];
        sprite.icon = &kIcons[index];
        const std::int64_t* source = FindIcon(playStationIcons, sprite.icon->token);
        Recognise(source != nullptr, std::string("the PlayStation set has no ") + sprite.icon->token);
        sources[index] = *source;
        const Object* sourceObject = file.Find(*source);
        Recognise(sourceObject != nullptr && file.ClassOf(*sourceObject) == kSpriteClass, std::string("the PlayStation glyph for ") + sprite.icon->token + " is missing");
        const std::string spriteName = file.NameOf(*sourceObject);
        std::optional<Cell> located;
        for (std::size_t at = 0; at < playStation.characters.size() && !located; ++at) {
            const Glyph& glyph = playStation.glyphs[playStation.characters[at].glyph];
            if (playStation.characters[at].name == spriteName && glyph.sprite == *source) located = LocateSource(plan.playStation, glyph.rect, spriteName);
        }
        Recognise(located.has_value(), "the PlayStation glyph " + spriteName + " is not in the PlayStation prompt atlas");
        sprite.source = *located;
        sprite.cell = cells[index];
        sprite.id = ++nextId;
        sprite.renderKey = NewRenderKey(sprite.icon->sprite, usedKeys);
        usedKeys.push_back(sprite.renderKey);
    }

    Recognise(SourceFingerprint(file, plan, sources, playStation.sheet) == kSourceFingerprint, "the game data is not the analysed version");
    plan.iconData = MakeIconData(inspection.icons, plan);
    plan.atlas = MakeAtlas(inspection.xbox, plan);
    for (const NewSprite& sprite : plan.sprites) plan.spriteObjects.push_back(MakeSprite(plan.templateSprite, plan.templateLayout, sprite));
    return plan;
}

void CopyCell(const Texture& from, std::uint32_t fromX, std::uint32_t fromY, Texture& to, std::uint32_t toX, std::uint32_t toY) {
    for (std::uint32_t row = 0; row < kCell; ++row) {
        for (std::uint32_t column = 0; column < kCell; ++column) {
            std::memcpy(to.tiled.data() + Texture::Offset(toX + column, toY + row, to.width), from.tiled.data() + Texture::Offset(fromX + column, fromY + row, from.width), 4);
        }
    }
}

void WriteBytes(std::ofstream& out, const Bytes& data) {
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void CopyRange(std::ifstream& in, std::ofstream& out, std::uint64_t offset, std::uint64_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    Bytes buffer(std::size_t{1} << 22);
    while (size > 0) {
        const std::size_t step = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(step));
        Require(static_cast<std::size_t>(in.gcount()) == step, "the source ended early");
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(step));
        size -= step;
    }
}

// The new serialized file: the original data kept where it is, the two changed objects and the four new ones after it, the
// object table and header updated.
void WriteAssets(const AssetFile& file, const Plan& plan, const fs::path& output) {
    const std::uint64_t oldData = file.fileSize() - file.dataOffset();
    std::uint64_t at = Align(oldData, 8);
    struct Appended {
        std::int64_t id;
        const Bytes* bytes;
        std::uint64_t start;
        std::int32_t type;
    };
    std::vector<Appended> appended;
    const auto append = [&](std::int64_t id, const Bytes& bytes, std::int32_t type) {
        appended.push_back({id, &bytes, at, type});
        at = Align(at + bytes.size(), 8);
    };
    append(kIconData, plan.iconData, 0);
    append(kXboxAtlas, plan.atlas, 0);
    Bytes metadata = file.metadata();
    const std::int32_t templateType = [&] {
        for (const Object& object : file.objects()) {
            if (file.ClassOf(object) == kSpriteClass) return object.type;
        }
        return std::int32_t{0};
    }();
    for (std::size_t index = 0; index < kIconCount; ++index) append(plan.sprites[index].id, plan.spriteObjects[index], templateType);

    for (const Appended& item : appended) {
        if (file.Find(item.id) == nullptr) continue;
        const std::size_t entry = file.entryAt(file.IndexOf(item.id));
        std::memcpy(metadata.data() + entry + 8, &item.start, 8);
        const auto size = static_cast<std::uint32_t>(item.bytes->size());
        std::memcpy(metadata.data() + entry + 16, &size, 4);
        std::int32_t keep = 0;
        std::memcpy(&keep, metadata.data() + entry + 20, 4);
        Require(keep == file.objects()[file.IndexOf(item.id)].type, "an object entry moved");
    }
    Bytes added;
    for (const Appended& item : appended) {
        if (file.Find(item.id) != nullptr) continue;
        Writer entry;
        entry.Put(item.id);
        entry.Put(static_cast<std::int64_t>(item.start));
        entry.Put(static_cast<std::uint32_t>(item.bytes->size()));
        entry.Put(item.type);
        added.insert(added.end(), entry.data.begin(), entry.data.end());
    }
    metadata.insert(metadata.begin() + static_cast<std::ptrdiff_t>(file.objectsEnd()), added.begin(), added.end());
    const auto count = Get<std::int32_t>(metadata, file.objectCountAt()) + static_cast<std::int32_t>(kIconCount);
    std::memcpy(metadata.data() + file.objectCountAt(), &count, 4);

    const std::uint64_t dataOffset = Align(kHeaderSize + metadata.size(), 16);
    Bytes head = file.head();
    SetBigEndian(head, 20, 4, metadata.size());
    SetBigEndian(head, 24, 8, dataOffset + at);
    SetBigEndian(head, 32, 8, dataOffset);

    std::ifstream in(file.path(), std::ios::binary);
    Require(in.is_open(), "the source could not be opened again");
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    Require(out.is_open(), "could not create " + output.string());
    WriteBytes(out, head);
    WriteBytes(out, metadata);
    WriteBytes(out, Bytes(dataOffset - kHeaderSize - metadata.size()));
    CopyRange(in, out, file.dataOffset(), oldData);
    std::uint64_t written = oldData;
    for (const Appended& item : appended) {
        WriteBytes(out, Bytes(item.start - written));
        WriteBytes(out, *item.bytes);
        written = item.start + item.bytes->size();
    }
    WriteBytes(out, Bytes(at - written));
    out.close();
    Require(!out.fail(), "could not write " + output.string());
}

void WriteStream(const IconFiles& source, const Plan& plan, const fs::path& output) {
    Texture patched = plan.xbox;
    for (const NewSprite& sprite : plan.sprites) CopyCell(plan.playStation, sprite.source.x, sprite.source.y, patched, sprite.cell.x, sprite.cell.y);
    std::error_code error;
    const std::uint64_t size = fs::file_size(source.streamed, error);
    Require(!error, "the streamed data size is unknown");
    std::ifstream in(source.streamed, std::ios::binary);
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    Require(in.is_open() && out.is_open(), "could not copy the streamed data");
    CopyRange(in, out, 0, plan.xbox.streamOffset);
    WriteBytes(out, patched.tiled);
    CopyRange(in, out, plan.xbox.streamOffset + plan.xbox.tiled.size(), size - plan.xbox.streamOffset - plan.xbox.tiled.size());
    out.close();
    Require(!out.fail(), "could not write " + output.string());
}

// Reads the written pair back and checks it against the source: only the intended changes may differ.
void Verify(const AssetFile& original, const IconFiles& source, const IconFiles& written, const Plan& plan) {
    AssetFile now(written.assets);
    now.LoadAll();
    Require(now.objects().size() == original.objects().size() + kIconCount, "the written object table has an unexpected size");
    for (const Object& object : original.objects()) {
        const Object* moved = now.Find(object.id);
        Require(moved != nullptr && moved->type == object.type, "an object was lost or retyped");
        if (object.id == kIconData || object.id == kXboxAtlas) continue;
        Require(now.Read(*moved) == original.Read(object), "an unchanged object differs");
    }
    Require(now.Read(*now.Find(kIconData)) == plan.iconData && now.Read(*now.Find(kXboxAtlas)) == plan.atlas, "a changed object was not written as built");
    for (std::size_t index = 0; index < kIconCount; ++index) {
        const Object* object = now.Find(plan.sprites[index].id);
        Require(object != nullptr && now.ClassOf(*object) == kSpriteClass && now.Read(*object) == plan.spriteObjects[index], "a new sprite was not written as built");
    }
    const Inspection inspection = Inspect(now, true);
    Require(inspection.state == IconSetState::kComplete, "the written assets do not hold the complete direction set");
    const Inspection before = Inspect(original, true);
    for (std::size_t map = 0; map < 2; ++map) Require(inspection.icons.maps[map].icons == before.icons.maps[map].icons, "another special icon map changed");
    const SpecialIcons& icons = inspection.icons.maps[kXboxMap].icons;
    Require(icons.size() == before.icons.maps[kXboxMap].icons.size() + kIconCount && std::equal(before.icons.maps[kXboxMap].icons.begin(), before.icons.maps[kXboxMap].icons.end(), icons.begin()),
            "the Xbox special icons do not extend the original");
    for (std::size_t index = 0; index < before.xbox.characters.size(); ++index) {
        Require(std::equal(before.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(before.xbox.characters[index].begin), before.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(before.xbox.characters[index].end),
                           inspection.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(inspection.xbox.characters[index].begin)) &&
                    std::equal(before.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(before.xbox.glyphs[index].begin), before.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(before.xbox.glyphs[index].end),
                               inspection.xbox.bytes.begin() + static_cast<std::ptrdiff_t>(inspection.xbox.glyphs[index].begin)),
                "an original Xbox atlas entry changed");
    }

    std::error_code error;
    const std::uint64_t size = fs::file_size(source.streamed, error);
    Require(!error && fs::file_size(written.streamed, error) == size, "the written streamed data has another size");
    std::ifstream was(source.streamed, std::ios::binary), is(written.streamed, std::ios::binary);
    Bytes a(std::size_t{1} << 22), b(std::size_t{1} << 22);
    const std::uint64_t texBegin = plan.xbox.streamOffset, texEnd = texBegin + plan.xbox.tiled.size();
    for (std::uint64_t at = 0; at < size;) {
        const std::size_t step = static_cast<std::size_t>(std::min<std::uint64_t>(size - at, a.size()));
        was.read(reinterpret_cast<char*>(a.data()), static_cast<std::streamsize>(step));
        is.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(step));
        Require(static_cast<std::size_t>(was.gcount()) == step && static_cast<std::size_t>(is.gcount()) == step, "the streamed data could not be compared");
        for (std::size_t index = 0; index < step; ++index) {
            const std::uint64_t position = at + index;
            if (position >= texBegin && position < texEnd) continue;
            Require(a[index] == b[index], "streamed data outside the Xbox texture changed");
        }
        at += step;
    }
    const Texture patched = ReadTexture(now, plan.xbox.id, written.streamed);
    for (std::uint32_t y = 0; y < patched.height; ++y) {
        for (std::uint32_t x = 0; x < patched.width; ++x) {
            const NewSprite* cell = nullptr;
            for (const NewSprite& sprite : plan.sprites) {
                if (x >= sprite.cell.x && x < sprite.cell.x + kCell && y >= sprite.cell.y && y < sprite.cell.y + kCell) cell = &sprite;
            }
            for (int channel = 0; channel < 4; ++channel) {
                const std::uint8_t expected = cell == nullptr ? plan.xbox.Texel(x, y, channel) : plan.playStation.Texel(cell->source.x + x - cell->cell.x, cell->source.y + y - cell->cell.y, channel);
                Require(patched.Texel(x, y, channel) == expected, "the written Xbox texture is not as designed");
            }
        }
    }
}

__attribute__((format(printf, 2, 3))) void Note(std::string& report, const char* format, ...) {
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    report += line;
}

void RemoveQuietly(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
}

// Every direction cell of a prepared pair holds artwork: the sprites point at texels that are not empty.
bool DirectionCellsDrawn(const AssetFile& file, const Inspection& inspection, const IconFiles& files) {
    const Texture texture = ReadTexture(file, inspection.xbox.sheet, files.streamed);
    for (const Icon& item : kIcons) {
        const auto character = std::find_if(inspection.xbox.characters.begin(), inspection.xbox.characters.end(), [&](const Character& c) { return c.name == item.sprite; });
        if (character == inspection.xbox.characters.end()) return false;
        const Rect rect = inspection.xbox.glyphs[character->glyph].rect;
        if (rect.x < 0 || rect.y < 0 || static_cast<std::uint32_t>(rect.x + rect.width) > texture.width || static_cast<std::uint32_t>(rect.y + rect.height) > texture.height) return false;
        std::size_t opaque = 0;
        for (std::int32_t y = rect.y; y < rect.y + rect.height; ++y) {
            for (std::int32_t x = rect.x; x < rect.x + rect.width; ++x) opaque += texture.Texel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), 3) != 0;
        }
        if (opaque < 200) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------

namespace {

IconSetState InspectPair(const IconFiles& files, std::string* why) {
    try {
        const AssetFile file(files.assets);
        const Inspection inspection = Inspect(file, false);
        if (why != nullptr) *why = inspection.why;
        return inspection.state;
    } catch (const std::exception& failure) {
        if (why != nullptr) *why = failure.what();
        return IconSetState::kUnrecognised;
    }
}

// The set is only as complete as its least complete member; any other mix of states is a partial patch.
IconSetState Combine(IconSetState pair, IconSetState bundle) {
    if (pair == IconSetState::kUnrecognised || bundle == IconSetState::kUnrecognised) return IconSetState::kUnrecognised;
    return pair == bundle ? pair : IconSetState::kPartial;
}

}  // namespace

IconSetState InspectIconSet(const IconFiles& files, std::string* why) {
    std::string pairWhy, bundleWhy;
    const IconSetState pair = InspectPair(files, &pairWhy);
    if (files.bundle.empty()) {
        if (why != nullptr) *why = pairWhy;
        return pair;
    }
    const IconSetState bundle = InspectIconBundle(files.bundle, &bundleWhy);
    const IconSetState state = Combine(pair, bundle);
    if (why != nullptr) *why = !pairWhy.empty() ? pairWhy : !bundleWhy.empty() ? bundleWhy : (state == IconSetState::kPartial ? "the Xbox prompt set is only partly patched" : "");
    return state;
}

bool ReadXboxPrompts(const IconFiles& files, XboxPrompts& prompts) {
    try {
        const AssetFile file(files.assets);
        const Inspection inspection = Inspect(file, false);
        prompts = {};
        prompts.special = inspection.icons.maps[kXboxMap].icons;
        for (const Character& character : inspection.xbox.characters) {
            const Glyph& glyph = inspection.xbox.glyphs[character.glyph];
            prompts.glyphs.push_back({character.name, glyph.rect.x, glyph.rect.y, glyph.rect.width, glyph.rect.height, glyph.sprite});
        }
        const Object* sheet = file.Find(inspection.xbox.sheet);
        Recognise(sheet != nullptr && file.ClassOf(*sheet) == kTextureClass, "the sprite sheet is missing");
        const TextureObject texture = ParseTextureObject(file.Read(*sheet));
        prompts.sheetWidth = texture.width;
        prompts.sheetHeight = texture.height;
        prompts.sheetOffset = texture.streamOffset;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool HasControllerIcons(const IconFiles& files) {
    try {
        const AssetFile file(files.assets);
        const Inspection inspection = Inspect(file, false);
        if (inspection.state != IconSetState::kComplete || !DirectionCellsDrawn(file, inspection, files)) return false;
    } catch (const std::exception&) {
        return false;
    }
    return files.bundle.empty() || HasBundleIcons(files.bundle);
}

namespace {

void RemoveOutputs(const IconFiles& out) {
    RemoveQuietly(out.assets);
    RemoveQuietly(out.streamed);
    if (!out.bundle.empty()) RemoveQuietly(out.bundle);
}

bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::exists(b, error) && fs::equivalent(a, b, error);
}

}  // namespace

IconPatchResult PatchControllerIcons(const IconFiles& source, const IconFiles& out, std::string& report) {
    std::error_code error;
    bool owned = false;  // the outputs are only ours to remove once they are known not to be the game's own files
    try {
        const bool withBundle = !source.bundle.empty();
        Require(fs::is_regular_file(source.assets, error) && fs::is_regular_file(source.streamed, error), "the game's icon files do not exist: " + source.assets.string());
        Require(!withBundle || (fs::is_regular_file(source.bundle, error) && !out.bundle.empty()), "the game's icon bundle does not exist: " + source.bundle.string());
        Require(!SameFile(source.assets, out.assets) && !SameFile(source.streamed, out.streamed) && !(withBundle && SameFile(source.bundle, out.bundle)), "an output file is the game's own file");
        owned = true;
        AssetFile file(source.assets);
        file.LoadAll();
        Note(report, "Source %s: %llu bytes, %zu objects\n", source.assets.string().c_str(), static_cast<unsigned long long>(file.fileSize()), file.objects().size());
        const Inspection inspection = Inspect(file, true);
        const IconSetState bundleState = withBundle ? InspectIconBundle(source.bundle) : inspection.state;
        if (inspection.state == IconSetState::kComplete && bundleState == IconSetState::kComplete) {
            Note(report, "The Xbox prompt set already has the left-stick directions and the interaction icons; nothing to prepare\n");
            return IconPatchResult::kComplete;
        }
        Recognise(bundleState != IconSetState::kUnrecognised, "the game's icon bundle is not the analysed one");
        Recognise(inspection.state == IconSetState::kMissing && bundleState == IconSetState::kMissing,
                  inspection.why.empty() ? "the Xbox prompt set is only partly patched" : inspection.why);
        const Plan plan = Analyse(file, source, inspection);
        for (const NewSprite& sprite : plan.sprites) {
            Note(report, "%s: sprite %lld, cell (%u,%u), from PlayStation art at (%u,%u)\n", sprite.icon->token, static_cast<long long>(sprite.id), sprite.cell.x, sprite.cell.y, sprite.source.x, sprite.source.y);
        }
        WriteAssets(file, plan, out.assets);
        WriteStream(source, plan, out.streamed);
        Verify(file, source, out, plan);
        Note(report, "Prepared %s (%llu bytes) and %s\n", out.assets.string().c_str(), static_cast<unsigned long long>(fs::file_size(out.assets, error)), out.streamed.string().c_str());
        if (withBundle) {
            const IconPatchResult bundleResult = PatchIconBundle(source.bundle, out.bundle, report);
            Recognise(bundleResult != IconPatchResult::kUnsupported, "the game's icon bundle is not the analysed one");
            Require(bundleResult == IconPatchResult::kPrepared, "the icon bundle could not be prepared");
        }
        return IconPatchResult::kPrepared;
    } catch (const Unsupported& failure) {
        Note(report, "Not supported: %s\n", failure.what());
        if (owned) RemoveOutputs(out);
        return IconPatchResult::kUnsupported;
    } catch (const std::exception& failure) {
        Note(report, "Failed: %s\n", failure.what());
        if (owned) RemoveOutputs(out);
        return IconPatchResult::kFailed;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// The cache: the prepared pair and a stamp of what it was made from.

namespace {

struct Identity {
    std::uint64_t assetsSize = 0, assetsTime = 0, streamSize = 0, streamTime = 0, bundleSize = 0, bundleTime = 0;
    bool operator==(const Identity& other) const {
        return assetsSize == other.assetsSize && assetsTime == other.assetsTime && streamSize == other.streamSize && streamTime == other.streamTime && bundleSize == other.bundleSize &&
               bundleTime == other.bundleTime;
    }
};

std::optional<Identity> IdentityOf(const IconFiles& files) {
    std::error_code error;
    Identity identity;
    identity.assetsSize = fs::file_size(files.assets, error);
    if (error) return std::nullopt;
    identity.assetsTime = static_cast<std::uint64_t>(fs::last_write_time(files.assets, error).time_since_epoch().count());
    if (error) return std::nullopt;
    identity.streamSize = fs::file_size(files.streamed, error);
    if (error) return std::nullopt;
    identity.streamTime = static_cast<std::uint64_t>(fs::last_write_time(files.streamed, error).time_since_epoch().count());
    if (error) return std::nullopt;
    if (!files.bundle.empty()) {
        identity.bundleSize = fs::file_size(files.bundle, error);
        if (error) return std::nullopt;
        identity.bundleTime = static_cast<std::uint64_t>(fs::last_write_time(files.bundle, error).time_since_epoch().count());
    }
    return error ? std::nullopt : std::optional<Identity>(identity);
}

struct Stamp {
    int version = 0;
    std::string state;  // prepared, complete or unsupported
    Identity source, prepared;
};

std::string StampText(const Stamp& stamp) {
    std::ostringstream text;
    text << "controller-icons " << stamp.version << ' ' << stamp.state << '\n'
         << "source " << stamp.source.assetsSize << ' ' << stamp.source.assetsTime << ' ' << stamp.source.streamSize << ' ' << stamp.source.streamTime << ' ' << stamp.source.bundleSize << ' '
         << stamp.source.bundleTime << '\n'
         << "prepared " << stamp.prepared.assetsSize << ' ' << stamp.prepared.assetsTime << ' ' << stamp.prepared.streamSize << ' ' << stamp.prepared.streamTime << ' ' << stamp.prepared.bundleSize
         << ' ' << stamp.prepared.bundleTime << '\n';
    return text.str();
}

std::optional<Stamp> ReadStamp(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    Stamp stamp;
    std::string tag, source, prepared;
    if (!(in >> tag >> stamp.version >> stamp.state) || tag != "controller-icons") return std::nullopt;
    if (!(in >> source >> stamp.source.assetsSize >> stamp.source.assetsTime >> stamp.source.streamSize >> stamp.source.streamTime >> stamp.source.bundleSize >> stamp.source.bundleTime) ||
        source != "source")
        return std::nullopt;
    if (!(in >> prepared >> stamp.prepared.assetsSize >> stamp.prepared.assetsTime >> stamp.prepared.streamSize >> stamp.prepared.streamTime >> stamp.prepared.bundleSize >>
          stamp.prepared.bundleTime) ||
        prepared != "prepared")
        return std::nullopt;
    return stamp;
}

bool WriteStamp(const fs::path& path, const Stamp& stamp) {
    const fs::path temporary = path.string() + ".new";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << StampText(stamp);
        if (!out) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

IconFiles PreparedFiles(const fs::path& cache) { return {cache / kIconsAssetsName, cache / kIconsStreamName, cache / kIconsBundleName}; }

IconFiles SourceFiles(const fs::path& media) {
    return {media / kIconsAssetsName, media / kIconsStreamName, media / kIconsBundleFolder / kIconsBundleName};
}

}  // namespace

IconPlan PlanControllerIcons(const fs::path& media, const fs::path& cache) {
    IconPlan plan;
    const IconFiles source = SourceFiles(media);
    const std::optional<Identity> identity = IdentityOf(source);
    if (!identity) {
        plan.note = "the game's icon files are not installed";
        return plan;
    }
    const fs::path stampFile = cache / "controller_icons.stamp";
    const std::optional<Stamp> stamp = ReadStamp(stampFile);
    if (stamp && stamp->version == kControllerIconsPatchVersion && stamp->source == *identity) {
        if (stamp->state == "unsupported") {
            plan.note = "the game's icon files are not the analysed ones; they are used as they are";
            return plan;
        }
        if (stamp->state == "complete" && HasControllerIcons(source)) {
            plan.note = "the game's icon files already have the left-stick directions";
            return plan;
        }
        const IconFiles prepared = PreparedFiles(cache);
        if (stamp->state == "prepared" && IdentityOf(prepared) == std::optional<Identity>(stamp->prepared) && HasControllerIcons(prepared)) {
            plan.kind = IconPlanKind::kUsePrepared;
            plan.files = prepared;
            plan.note = "prepared copies in use";
            return plan;
        }
    }
    std::string why;
    const IconSetState state = InspectIconSet(source, &why);
    if (state != IconSetState::kMissing) {
        std::error_code error;
        fs::create_directories(cache, error);
        Stamp now;
        now.version = kControllerIconsPatchVersion;
        now.source = *identity;
        if (state == IconSetState::kComplete && HasControllerIcons(source)) {
            now.state = "complete";
            plan.note = "the game's icon files already have the left-stick directions";
        } else {
            now.state = "unsupported";
            plan.note = "the game's icon files are used as they are: " + (why.empty() ? std::string("not the analysed data") : why);
        }
        WriteStamp(stampFile, now);
        return plan;
    }
    plan.kind = IconPlanKind::kPrepare;
    plan.note = "the left-stick directions have to be prepared";
    return plan;
}

IconPlan PrepareControllerIcons(const fs::path& media, const fs::path& cache, std::string& report) {
    IconPlan plan = PlanControllerIcons(media, cache);
    if (plan.kind != IconPlanKind::kPrepare) return plan;
    const IconFiles source = SourceFiles(media);
    const std::optional<Identity> identity = IdentityOf(source);
    std::error_code error;
    fs::create_directories(cache, error);
    const fs::path stampFile = cache / "controller_icons.stamp";
    const IconFiles prepared = PreparedFiles(cache);
    const IconFiles temporary{cache / "resources.assets.new", cache / "resources.assets.resS.new", cache / "icons.bundle.new"};
    RemoveQuietly(stampFile);
    const IconPatchResult result = PatchControllerIcons(source, temporary, report);

    Stamp stamp;
    stamp.version = kControllerIconsPatchVersion;
    if (identity) stamp.source = *identity;
    IconPlan done;
    if (result == IconPatchResult::kPrepared) {
        fs::rename(temporary.assets, prepared.assets, error);
        if (!error) fs::rename(temporary.streamed, prepared.streamed, error);
        if (!error) fs::rename(temporary.bundle, prepared.bundle, error);
        const std::optional<Identity> now = IdentityOf(prepared);
        if (!error && now && HasControllerIcons(prepared)) {
            stamp.state = "prepared";
            stamp.prepared = *now;
            WriteStamp(stampFile, stamp);
            done.kind = IconPlanKind::kUsePrepared;
            done.files = prepared;
            done.note = "prepared copies in use";
            return done;
        }
        report += "Failed: the prepared copies could not be put in place\n";
        RemoveOutputs(prepared);
    } else if (result == IconPatchResult::kComplete) {
        stamp.state = "complete";
        WriteStamp(stampFile, stamp);
        done.note = "the game's icon files already have the left-stick directions";
        return done;
    } else if (result == IconPatchResult::kUnsupported) {
        stamp.state = "unsupported";
        WriteStamp(stampFile, stamp);
        done.note = "the game's icon files are not the analysed ones; they are used as they are";
        return done;
    }
    RemoveOutputs(temporary);
    done.note = "the icons could not be prepared; the game's own files are used";
    return done;
}

}  // namespace sos

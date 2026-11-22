#include "icon_bundle.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <optional>
#include <tuple>

#include "icon_common.h"

namespace sos {
namespace {

namespace fs = std::filesystem;
using namespace icons;

// Object IDs of the analysed bundle (Unity 2022.3, UnityFS 8, LZ4HC); any other bundle is refused.
constexpr std::int64_t kIconData = -1941347887603528766;        // PS5InputIconData
constexpr std::int64_t kXboxAtlas = 3925109087051863750;        // Xbox_ButtonPromptsAtlas, the TextMeshPro sprite asset of Xbox prompt text
constexpr std::int64_t kLookInputSprite = 3865735037816995483;  // the Xbox !LookInput sprite
constexpr std::int64_t kCircleSprite = -6315621532518302987;    // the interaction marker's own fill disc
constexpr std::int64_t kPreloadHolder = 3180981156664841393;    // SystemPreloadDependencies, which references the icon data
constexpr std::int32_t kMonoBehaviourClass = 114, kTextureClass = 28, kSpriteClass = 213, kAssetBundleClass = 142;
constexpr std::int32_t kRgba32 = 4;
constexpr std::uint32_t kBlockSize = 131072;
// FNV-1a of the objects the patch reads, so only the analysed game data is patched.
constexpr std::uint64_t kSourceFingerprint = 0x190d4048fbb3675d;

const char kCommonStrings[] =
    "AABB\0AnimationClip\0AnimationCurve\0AnimationState\0Array\0Base\0BitField\0bitset\0bool\0char\0ColorRGBA\0Component\0data\0deque\0"
    "double\0dynamic_array\0FastPropertyName\0first\0float\0Font\0GameObject\0Generic Mono\0GradientNEW\0GUID\0GUIStyle\0int\0list\0"
    "long long\0map\0Matrix4x4f\0MdFour\0MonoBehaviour\0MonoScript\0m_ByteSize\0m_Curve\0m_EditorClassIdentifier\0m_EditorHideFlags\0"
    "m_Enabled\0m_ExtensionPtr\0m_GameObject\0m_Index\0m_IsArray\0m_IsStatic\0m_MetaFlag\0m_Name\0m_ObjectHideFlags\0m_PrefabInternal\0"
    "m_PrefabParentObject\0m_Script\0m_StaticEditorFlags\0m_Type\0m_Version\0Object\0pair\0PPtr<Component>\0PPtr<GameObject>\0"
    "PPtr<Material>\0PPtr<MonoBehaviour>\0PPtr<MonoScript>\0PPtr<Object>\0PPtr<Prefab>\0PPtr<Sprite>\0PPtr<TextAsset>\0PPtr<Texture>\0"
    "PPtr<Texture2D>\0PPtr<Transform>\0Prefab\0Quaternionf\0Rectf\0RectInt\0RectOffset\0second\0set\0short\0size\0SInt16\0SInt32\0SInt64\0"
    "SInt8\0staticvector\0string\0TextAsset\0TextMesh\0Texture\0Texture2D\0Transform\0TypelessData\0UInt16\0UInt32\0UInt64\0UInt8\0"
    "unsigned int\0unsigned long long\0unsigned short\0vector\0Vector2f\0Vector3f\0Vector4f\0m_ScriptingClassIdentifier\0Gradient\0"
    "Type*\0int2_storage\0int3_storage\0BoundsInt\0m_CorrespondingSourceObject\0m_PrefabInstance\0m_PrefabAsset\0FileSize\0Hash128\0";

bool ReadFile(const fs::path& path, Bytes& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    data.resize(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

void WriteFile(const fs::path& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close();
    Require(!file.fail(), "could not write " + path.string());
}

void PutBigEndian(Bytes& data, std::size_t size, std::uint64_t value) {
    data.resize(data.size() + size);
    SetBigEndian(data, data.size() - size, size, value);
}

// ---------------------------------------------------------------------------------------------------------------------------
// LZ4 blocks

void Lz4Decompress(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* target, std::size_t targetSize) {
    std::size_t in = 0, out = 0;
    while (in < sourceSize) {
        const std::uint8_t token = source[in++];
        std::size_t literals = token >> 4;
        if (literals == 15) {
            std::uint8_t more;
            do {
                Recognise(in < sourceSize, "corrupt LZ4 block");
                more = source[in++];
                literals += more;
            } while (more == 255);
        }
        Recognise(in + literals <= sourceSize && out + literals <= targetSize, "corrupt LZ4 block");
        std::memcpy(target + out, source + in, literals);
        in += literals;
        out += literals;
        if (in >= sourceSize) break;
        Recognise(in + 2 <= sourceSize, "corrupt LZ4 block");
        const std::size_t offset = source[in] | (std::size_t{source[in + 1]} << 8);
        in += 2;
        std::size_t length = token & 15;
        if (length == 15) {
            std::uint8_t more;
            do {
                Recognise(in < sourceSize, "corrupt LZ4 block");
                more = source[in++];
                length += more;
            } while (more == 255);
        }
        length += 4;
        Recognise(offset != 0 && offset <= out && out + length <= targetSize, "corrupt LZ4 block");
        for (std::size_t index = 0; index < length; ++index, ++out) target[out] = target[out - offset];
    }
    Recognise(out == targetSize, "an LZ4 block has the wrong size");
}

Bytes Lz4Compress(const std::uint8_t* source, std::size_t size) {
    constexpr std::size_t kLastLiterals = 5, kMatchStartLimit = 12, kHashBits = 14;
    Bytes out;
    out.reserve(size + size / 255 + 16);
    std::vector<std::uint32_t> table(std::size_t{1} << kHashBits, 0xffffffffu);
    const auto read32 = [&](std::size_t at) {
        std::uint32_t value;
        std::memcpy(&value, source + at, 4);
        return value;
    };
    const auto putLength = [&](std::size_t length) {
        for (; length >= 255; length -= 255) out.push_back(255);
        out.push_back(static_cast<std::uint8_t>(length));
    };
    std::size_t anchor = 0, in = 0;
    if (size > kMatchStartLimit) {
        while (in <= size - kMatchStartLimit) {
            const std::uint32_t sequence = read32(in);
            const std::size_t slot = (sequence * 2654435761u) >> (32 - kHashBits);
            const std::uint32_t candidate = table[slot];
            table[slot] = static_cast<std::uint32_t>(in);
            if (candidate == 0xffffffffu || in - candidate > 65535 || read32(candidate) != sequence) {
                ++in;
                continue;
            }
            std::size_t length = 4;
            while (in + length < size - kLastLiterals && source[candidate + length] == source[in + length]) ++length;
            const std::size_t literals = in - anchor;
            out.push_back(static_cast<std::uint8_t>((std::min<std::size_t>(literals, 15) << 4) | std::min<std::size_t>(length - 4, 15)));
            if (literals >= 15) putLength(literals - 15);
            out.insert(out.end(), source + anchor, source + in);
            const std::size_t offset = in - candidate;
            out.push_back(static_cast<std::uint8_t>(offset));
            out.push_back(static_cast<std::uint8_t>(offset >> 8));
            if (length - 4 >= 15) putLength(length - 4 - 15);
            in += length;
            anchor = in;
        }
    }
    const std::size_t literals = size - anchor;
    out.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(literals, 15) << 4));
    if (literals >= 15) putLength(literals - 15);
    out.insert(out.end(), source + anchor, source + size);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The UnityFS archive: a block table and node table in one LZ4 block, then the data blocks.

struct Block {
    std::uint32_t uncompressed = 0, compressed = 0;
    std::uint16_t flags = 0;
};

struct Node {
    std::uint64_t offset = 0, size = 0;
    std::uint32_t flags = 0;
    std::string name;
};

struct Bundle {
    Bytes file;
    std::string player, engine;
    std::uint32_t version = 0, flags = 0;
    Bytes infoHash;
    std::vector<Block> blocks;
    std::vector<Node> nodes;
    std::vector<std::uint64_t> streamStart, fileStart;
    std::size_t dataOffset = 0;
    std::uint64_t streamSize = 0;

    std::uint32_t Compression() const { return flags & 0x3f; }

    void Parse() {
        Recognise(file.size() > 64 && std::memcmp(file.data(), "UnityFS\0", 8) == 0, "not a UnityFS bundle");
        version = static_cast<std::uint32_t>(BigEndian(file, 8, 4));
        std::size_t at = 12;
        const auto text = [&]() {
            std::string value;
            while (at < file.size() && file[at] != 0) value.push_back(static_cast<char>(file[at++]));
            Recognise(at < file.size(), "truncated bundle header");
            ++at;
            return value;
        };
        player = text();
        engine = text();
        const std::uint64_t size = BigEndian(file, at, 8);
        const auto compressedInfo = static_cast<std::size_t>(BigEndian(file, at + 8, 4));
        const auto uncompressedInfo = static_cast<std::size_t>(BigEndian(file, at + 12, 4));
        flags = static_cast<std::uint32_t>(BigEndian(file, at + 16, 4));
        at += 20;
        Recognise(version == 8, "the bundle is not a UnityFS 8 archive");
        Require(size == file.size(), "the bundle is not the size its header says (truncated or damaged)");
        Recognise((Compression() == 2 || Compression() == 3) && (flags & 0x40) != 0 && (flags & 0x80) == 0, "the bundle does not use LZ4 with a combined block and node table");
        const std::size_t infoAt = Align(at, 16);
        Recognise(infoAt + compressedInfo <= file.size() && uncompressedInfo > 20, "truncated bundle block table");
        Bytes info(uncompressedInfo);
        Lz4Decompress(file.data() + infoAt, compressedInfo, info.data(), info.size());
        infoHash.assign(info.begin(), info.begin() + 16);
        const auto blockCount = BigEndian(info, 16, 4);
        std::size_t cursor = 20;
        dataOffset = Align(infoAt + compressedInfo, 16);
        std::uint64_t stream = 0, stored = dataOffset;
        for (std::uint64_t index = 0; index < blockCount; ++index) {
            Block block;
            block.uncompressed = static_cast<std::uint32_t>(BigEndian(info, cursor, 4));
            block.compressed = static_cast<std::uint32_t>(BigEndian(info, cursor + 4, 4));
            block.flags = static_cast<std::uint16_t>(BigEndian(info, cursor + 8, 2));
            cursor += 10;
            blocks.push_back(block);
            streamStart.push_back(stream);
            fileStart.push_back(stored);
            stream += block.uncompressed;
            stored += block.compressed;
        }
        streamSize = stream;
        Recognise(stored == file.size(), "the bundle blocks do not end at the end of the file");
        const auto nodeCount = BigEndian(info, cursor, 4);
        cursor += 4;
        for (std::uint64_t index = 0; index < nodeCount; ++index) {
            Node node;
            node.offset = BigEndian(info, cursor, 8);
            node.size = BigEndian(info, cursor + 8, 8);
            node.flags = static_cast<std::uint32_t>(BigEndian(info, cursor + 16, 4));
            cursor += 20;
            while (cursor < info.size() && info[cursor] != 0) node.name.push_back(static_cast<char>(info[cursor++]));
            Recognise(cursor < info.size(), "truncated bundle node table");
            ++cursor;
            Recognise(node.offset + node.size <= streamSize, "a bundle node lies outside the data");
            nodes.push_back(node);
        }
        Recognise(cursor == info.size() && !nodes.empty(), "the bundle block table has trailing data");
    }

    Bytes ReadBlock(std::size_t index) const {
        const Block& block = blocks[index];
        const std::uint8_t* source = file.data() + fileStart[index];
        if ((block.flags & 0x3f) == 0) return Bytes(source, source + block.uncompressed);
        Recognise((block.flags & 0x3f) == Compression(), "a block uses another compression");
        Bytes data(block.uncompressed);
        Lz4Decompress(source, block.compressed, data.data(), data.size());
        return data;
    }

    Bytes Read(std::uint64_t start, std::uint64_t size) const {
        Require(start + size <= streamSize, "a read goes past the end of the bundle data");
        Bytes out;
        out.reserve(static_cast<std::size_t>(size));
        for (std::size_t index = 0; index < blocks.size() && out.size() < size; ++index) {
            if (streamStart[index] + blocks[index].uncompressed <= start) continue;
            const Bytes data = ReadBlock(index);
            const std::uint64_t from = std::max<std::uint64_t>(start, streamStart[index]) - streamStart[index];
            const std::uint64_t count = std::min<std::uint64_t>(data.size() - from, size - out.size());
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(from), data.begin() + static_cast<std::ptrdiff_t>(from + count));
        }
        return out;
    }

    Bytes ReadNode(const Node& node) const { return Read(node.offset, node.size); }

    const Node* Find(const std::string& name) const {
        for (const Node& node : nodes) {
            if (node.name == name) return &node;
        }
        return nullptr;
    }
};

Bytes CompressBlock(const Bundle& source, const std::uint8_t* data, std::size_t size, Block& block) {
    Bytes packed = Lz4Compress(data, size);
    block.uncompressed = static_cast<std::uint32_t>(size);
    if (packed.size() >= size) {
        block.compressed = static_cast<std::uint32_t>(size);
        block.flags = 0;
        return Bytes(data, data + size);
    }
    block.compressed = static_cast<std::uint32_t>(packed.size());
    block.flags = static_cast<std::uint16_t>(source.Compression());
    return packed;
}

struct StreamPatch {
    std::uint64_t offset;
    Bytes bytes;
};

// Writes the new serialized file first; later blocks that no patch touches are copied as they are.
Bytes Repack(const Bundle& source, const Bytes& serialized, const std::vector<StreamPatch>& patches) {
    for (std::size_t index = 0; index + 1 < source.blocks.size(); ++index) {
        Recognise(source.blocks[index].uncompressed == kBlockSize, "the bundle blocks are not of a uniform size");
    }
    Recognise(source.nodes.size() >= 2 && source.nodes[0].offset == 0, "unexpected bundle nodes");
    for (std::size_t index = 1; index < source.nodes.size(); ++index) {
        Recognise(source.nodes[index].offset == source.nodes[index - 1].offset + source.nodes[index - 1].size, "the bundle nodes are not contiguous");
    }
    const std::uint64_t firstOffset = source.nodes[1].offset;
    std::uint64_t newFirst = firstOffset;
    while (newFirst < serialized.size()) newFirst += kBlockSize;
    const std::uint64_t shift = newFirst - firstOffset;
    const std::size_t firstWhole = static_cast<std::size_t>((firstOffset + kBlockSize - 1) / kBlockSize);
    for (const StreamPatch& patch : patches) {
        Require(patch.offset >= std::uint64_t{firstWhole} * kBlockSize, "a texture patch lies in the rewritten blocks");
    }

    std::vector<Block> blocks;
    std::vector<Bytes> packed;
    Bytes head((firstWhole + shift / kBlockSize) * kBlockSize, 0);
    std::memcpy(head.data(), serialized.data(), serialized.size());
    const Bytes tail = source.Read(firstOffset, std::uint64_t{firstWhole} * kBlockSize - firstOffset);
    std::memcpy(head.data() + newFirst, tail.data(), tail.size());
    for (std::size_t offset = 0; offset < head.size(); offset += kBlockSize) {
        Block block;
        packed.push_back(CompressBlock(source, head.data() + offset, kBlockSize, block));
        blocks.push_back(block);
    }
    for (std::size_t index = firstWhole; index < source.blocks.size(); ++index) {
        const std::uint64_t begin = source.streamStart[index], end = begin + source.blocks[index].uncompressed;
        Bytes data;
        for (const StreamPatch& patch : patches) {
            if (patch.offset >= end || patch.offset + patch.bytes.size() <= begin) continue;
            if (data.empty()) data = source.ReadBlock(index);
            const std::uint64_t from = std::max(patch.offset, begin), to = std::min(patch.offset + patch.bytes.size(), end);
            std::memcpy(data.data() + (from - begin), patch.bytes.data() + (from - patch.offset), static_cast<std::size_t>(to - from));
        }
        Block block;
        if (data.empty()) {
            block = source.blocks[index];
            const auto* stored = source.file.data() + source.fileStart[index];
            packed.emplace_back(stored, stored + block.compressed);
        } else {
            packed.push_back(CompressBlock(source, data.data(), data.size(), block));
        }
        blocks.push_back(block);
    }

    Bytes info(source.infoHash);
    PutBigEndian(info, 4, blocks.size());
    for (const Block& block : blocks) {
        PutBigEndian(info, 4, block.uncompressed);
        PutBigEndian(info, 4, block.compressed);
        PutBigEndian(info, 2, block.flags);
    }
    PutBigEndian(info, 4, source.nodes.size());
    for (std::size_t index = 0; index < source.nodes.size(); ++index) {
        PutBigEndian(info, 8, index == 0 ? 0 : source.nodes[index].offset + shift);
        PutBigEndian(info, 8, index == 0 ? serialized.size() : source.nodes[index].size);
        PutBigEndian(info, 4, source.nodes[index].flags);
        info.insert(info.end(), source.nodes[index].name.begin(), source.nodes[index].name.end());
        info.push_back(0);
    }
    const Bytes packedInfo = Lz4Compress(info.data(), info.size());

    Bytes result;
    result.insert(result.end(), source.file.begin(), source.file.begin() + 12);
    result.insert(result.end(), source.player.begin(), source.player.end());
    result.push_back(0);
    result.insert(result.end(), source.engine.begin(), source.engine.end());
    result.push_back(0);
    const std::size_t sizeAt = result.size();
    PutBigEndian(result, 8, 0);
    PutBigEndian(result, 4, packedInfo.size());
    PutBigEndian(result, 4, info.size());
    PutBigEndian(result, 4, source.flags);
    result.resize(Align(result.size(), 16));
    result.insert(result.end(), packedInfo.begin(), packedInfo.end());
    result.resize(Align(result.size(), 16));
    for (const Bytes& block : packed) result.insert(result.end(), block.begin(), block.end());
    SetBigEndian(result, sizeAt, 8, result.size());
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The serialized file inside the bundle: Unity 2022.3, format 22, with type trees.

struct TypeNode {
    std::uint8_t level = 0, flags = 0;
    std::uint32_t meta = 0;
    std::int32_t byteSize = 0;
    std::string type, name;
};

struct Type {
    std::int32_t classId = 0;
    std::vector<TypeNode> nodes;
    std::vector<std::vector<std::int32_t>> children;
};

struct ObjectEntry {
    std::int64_t pathId;
    std::int64_t start;
    std::uint32_t size;
    std::int32_t type;
};

std::string TypeString(const Bytes& data, std::size_t table, std::size_t tableSize, std::uint32_t offset) {
    const char* text;
    std::size_t limit;
    if (offset & 0x80000000u) {
        offset &= 0x7fffffffu;
        text = kCommonStrings;
        limit = sizeof(kCommonStrings);
    } else {
        text = reinterpret_cast<const char*>(data.data() + table);
        limit = tableSize;
    }
    Recognise(offset < limit, "a type tree string is out of range");
    std::string value;
    for (std::size_t at = offset; at < limit && text[at] != 0; ++at) value.push_back(text[at]);
    return value;
}

struct SerializedFile {
    Bytes data;
    std::size_t metadataEnd = 0, dataOffset = 0, countAt = 0, tableAt = 0, tableEnd = 0;
    std::vector<Type> types;
    std::vector<ObjectEntry> objects;

    void Parse() {
        Recognise(data.size() > 64 && BigEndian(data, 8, 4) == 22 && data[16] == 0, "the bundle's serialized file is not format 22");
        metadataEnd = 48 + BigEndian(data, 20, 4);
        dataOffset = BigEndian(data, 32, 8);
        Recognise(BigEndian(data, 24, 8) == data.size() && dataOffset >= metadataEnd && dataOffset <= data.size(), "the serialized file header is inconsistent");
        std::size_t at = 48;
        while (at < metadataEnd && data[at] != 0) ++at;
        at += 1 + 4;
        Recognise(at + 5 <= metadataEnd && data[at] == 1, "the serialized file has no type trees");
        const auto typeCount = Get<std::int32_t>(data, at + 1);
        at += 5;
        Recognise(typeCount > 0, "the serialized file has no types");
        for (std::int32_t index = 0; index < typeCount; ++index) {
            Type type;
            type.classId = Get<std::int32_t>(data, at);
            at += 7 + (type.classId == kMonoBehaviourClass || type.classId < 0 ? 16 : 0) + 16;
            const auto nodeCount = Get<std::int32_t>(data, at);
            const auto stringSize = Get<std::int32_t>(data, at + 4);
            at += 8;
            Recognise(nodeCount > 0 && stringSize >= 0 && at + std::size_t{static_cast<std::uint32_t>(nodeCount)} * 32 + static_cast<std::uint32_t>(stringSize) <= metadataEnd,
                      "a type tree is truncated");
            const std::size_t strings = at + std::size_t{static_cast<std::uint32_t>(nodeCount)} * 32;
            for (std::int32_t node = 0; node < nodeCount; ++node) {
                const std::size_t entry = at + std::size_t{static_cast<std::uint32_t>(node)} * 32;
                TypeNode item;
                item.level = data[entry + 2];
                item.flags = data[entry + 3];
                item.type = TypeString(data, strings, static_cast<std::uint32_t>(stringSize), Get<std::uint32_t>(data, entry + 4));
                item.name = TypeString(data, strings, static_cast<std::uint32_t>(stringSize), Get<std::uint32_t>(data, entry + 8));
                item.byteSize = Get<std::int32_t>(data, entry + 12);
                item.meta = Get<std::uint32_t>(data, entry + 20);
                type.nodes.push_back(std::move(item));
            }
            at = strings + static_cast<std::uint32_t>(stringSize);
            at += 4 + std::size_t{static_cast<std::uint32_t>(Get<std::int32_t>(data, at))} * 4;
            type.children.resize(type.nodes.size());
            for (std::size_t node = 0; node < type.nodes.size(); ++node) {
                for (std::size_t next = node + 1; next < type.nodes.size() && type.nodes[next].level > type.nodes[node].level; ++next) {
                    if (type.nodes[next].level == type.nodes[node].level + 1) type.children[node].push_back(static_cast<std::int32_t>(next));
                }
            }
            types.push_back(std::move(type));
        }
        countAt = at;
        const auto count = Get<std::int32_t>(data, at);
        tableAt = Align(at + 4, 4);
        Recognise(count > 0, "the serialized file has no objects");
        tableEnd = tableAt + std::size_t{static_cast<std::uint32_t>(count)} * 24;
        Recognise(tableEnd <= metadataEnd, "the object table is truncated");
        for (std::int32_t index = 0; index < count; ++index) {
            const std::size_t entry = tableAt + std::size_t{static_cast<std::uint32_t>(index)} * 24;
            ObjectEntry object{Get<std::int64_t>(data, entry), Get<std::int64_t>(data, entry + 8), Get<std::uint32_t>(data, entry + 16), Get<std::int32_t>(data, entry + 20)};
            Recognise(object.type >= 0 && static_cast<std::size_t>(object.type) < types.size() && object.start >= 0 &&
                          dataOffset + static_cast<std::uint64_t>(object.start) + object.size <= data.size(),
                      "an object lies outside the serialized file");
            objects.push_back(object);
        }
    }

    const ObjectEntry* Find(std::int64_t pathId) const {
        for (const auto& object : objects) {
            if (object.pathId == pathId) return &object;
        }
        return nullptr;
    }

    const Type& TypeOf(const ObjectEntry& object) const { return types[static_cast<std::size_t>(object.type)]; }

    Bytes Read(const ObjectEntry& object) const {
        const auto begin = data.begin() + static_cast<std::ptrdiff_t>(dataOffset + static_cast<std::uint64_t>(object.start));
        return Bytes(begin, begin + object.size);
    }

    // Changed objects move to the end of the data; new ones are appended there and sorted into the table. Other objects keep their bytes.
    Bytes Rebuild(const std::map<std::int64_t, Bytes>& changed, const std::vector<std::tuple<std::int64_t, std::int32_t, Bytes>>& added) const {
        std::vector<ObjectEntry> table = objects;
        Bytes appended;
        const std::size_t dataEnd = data.size() - dataOffset;
        const auto place = [&](const Bytes& bytes) {
            const std::size_t start = Align(dataEnd + appended.size(), 8);
            appended.resize(start - dataEnd);
            appended.insert(appended.end(), bytes.begin(), bytes.end());
            return static_cast<std::int64_t>(start);
        };
        for (const auto& [pathId, bytes] : changed) {
            for (auto& object : table) {
                if (object.pathId == pathId) object = {pathId, place(bytes), static_cast<std::uint32_t>(bytes.size()), object.type};
            }
        }
        for (const auto& [pathId, type, bytes] : added) {
            const ObjectEntry object{pathId, place(bytes), static_cast<std::uint32_t>(bytes.size()), type};
            table.insert(std::upper_bound(table.begin(), table.end(), object, [](const ObjectEntry& a, const ObjectEntry& b) { return a.pathId < b.pathId; }), object);
        }

        Bytes metadata(data.begin() + 48, data.begin() + static_cast<std::ptrdiff_t>(tableAt));
        Set(metadata, countAt - 48, static_cast<std::int32_t>(table.size()));
        for (const auto& object : table) {
            Writer entry;
            entry.Put(object.pathId);
            entry.Put(object.start);
            entry.Put(object.size);
            entry.Put(object.type);
            metadata.insert(metadata.end(), entry.data.begin(), entry.data.end());
        }
        metadata.insert(metadata.end(), data.begin() + static_cast<std::ptrdiff_t>(tableEnd), data.begin() + static_cast<std::ptrdiff_t>(metadataEnd));

        const std::size_t newDataOffset = Align(48 + metadata.size(), 16);
        Bytes result(data.begin(), data.begin() + 48);
        result.insert(result.end(), metadata.begin(), metadata.end());
        result.resize(newDataOffset);
        result.insert(result.end(), data.begin() + static_cast<std::ptrdiff_t>(dataOffset), data.end());
        result.insert(result.end(), appended.begin(), appended.end());
        SetBigEndian(result, 20, 4, metadata.size());
        SetBigEndian(result, 24, 8, result.size());
        SetBigEndian(result, 32, 8, newDataOffset);
        return result;
    }
};

// A field of a serialized object, located by walking its type tree.
struct Field {
    std::int32_t node = 0;
    std::size_t begin = 0, end = 0;
    std::int32_t count = 0;
    std::vector<Field> items;
};

void WalkNode(const Type& type, std::int32_t index, const Bytes& object, std::size_t& at, Field& field) {
    const TypeNode& node = type.nodes[static_cast<std::size_t>(index)];
    const auto& children = type.children[static_cast<std::size_t>(index)];
    field.node = index;
    field.begin = at;
    if (children.empty()) {
        Recognise(node.byteSize > 0 && at + static_cast<std::size_t>(node.byteSize) <= object.size(), "a field lies outside its object");
        at += static_cast<std::size_t>(node.byteSize);
    } else if (node.flags & 1) {
        Recognise(children.size() == 2, "an array has an unexpected layout");
        field.count = Get<std::int32_t>(object, at);
        at += 4;
        Recognise(field.count >= 0 && static_cast<std::size_t>(field.count) <= object.size(), "an array is too long");
        const std::int32_t element = children[1];
        if (type.children[static_cast<std::size_t>(element)].empty()) {
            const TypeNode& item = type.nodes[static_cast<std::size_t>(element)];
            Recognise(item.byteSize > 0 && at + static_cast<std::size_t>(field.count) * static_cast<std::size_t>(item.byteSize) <= object.size(), "an array is truncated");
            at += static_cast<std::size_t>(field.count) * static_cast<std::size_t>(item.byteSize);
        } else {
            field.items.resize(static_cast<std::size_t>(field.count));
            for (Field& item : field.items) WalkNode(type, element, object, at, item);
        }
    } else {
        field.items.resize(children.size());
        for (std::size_t child = 0; child < children.size(); ++child) WalkNode(type, children[child], object, at, field.items[child]);
    }
    if (node.meta & 0x4000) at = Align(at, 4);
    field.end = at;
}

Field Walk(const Type& type, const Bytes& object) {
    Field root;
    std::size_t at = 0;
    WalkNode(type, 0, object, at, root);
    Recognise(at <= object.size() && object.size() - at < 4, "an object does not match its type tree");
    return root;
}

const Field& Child(const Type& type, const Field& parent, const char* name) {
    for (const Field& child : parent.items) {
        if (type.nodes[static_cast<std::size_t>(child.node)].name == name) return child;
    }
    throw Unsupported(std::string("a field is missing: ") + name);
}

const Field& At(const Type& type, const Field& parent, std::initializer_list<const char*> path) {
    const Field* field = &parent;
    for (const char* name : path) field = &Child(type, *field, name);
    return *field;
}

std::string ReadString(const Bytes& object, const Type& type, const Field& string) {
    const Field& array = Child(type, string, "Array");
    return std::string(reinterpret_cast<const char*>(object.data() + array.begin + 4), static_cast<std::size_t>(array.count));
}

struct Reference {
    std::int32_t file;
    std::int64_t path;
};

Reference ReadReference(const Bytes& object, const Type& type, const Field& pointer) {
    return {Get<std::int32_t>(object, Child(type, pointer, "m_FileID").begin), Get<std::int64_t>(object, Child(type, pointer, "m_PathID").begin)};
}

// ---------------------------------------------------------------------------------------------------------------------------
// Textures, atlases and the objects of the prompt set

struct Texture {
    std::int64_t id = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t streamOffset = 0;  // in the bundle data
    Bytes tiled;

    std::uint8_t Texel(std::uint32_t x, std::uint32_t y, int channel) const { return tiled[TexelOffset(x, y, width) + static_cast<std::size_t>(channel)]; }
};

Texture ReadTexture(const Bundle& bundle, const SerializedFile& file, std::int64_t id) {
    const ObjectEntry* entry = file.Find(id);
    Recognise(entry != nullptr && file.TypeOf(*entry).classId == kTextureClass, "a prompt texture is missing");
    const Bytes object = file.Read(*entry);
    const Type& type = file.TypeOf(*entry);
    const Field root = Walk(type, object);
    Texture texture;
    texture.id = id;
    texture.width = Get<std::uint32_t>(object, Child(type, root, "m_Width").begin);
    texture.height = Get<std::uint32_t>(object, Child(type, root, "m_Height").begin);
    const Field& stream = Child(type, root, "m_StreamData");
    const auto offset = Get<std::uint32_t>(object, Child(type, stream, "offset").begin);
    const auto size = Get<std::uint32_t>(object, Child(type, stream, "size").begin);
    const std::string path = ReadString(object, type, Child(type, stream, "path"));
    Recognise(Get<std::int32_t>(object, Child(type, root, "m_TextureFormat").begin) == kRgba32 && Get<std::int32_t>(object, Child(type, root, "m_MipCount").begin) == 1 &&
                  texture.width % 8 == 0 && texture.height % 8 == 0 && size == std::size_t{texture.width} * texture.height * 4,
              "a prompt texture is not uncompressed RGBA32 without mip maps");
    const std::size_t slash = path.rfind('/');
    const Node* node = bundle.Find(slash == std::string::npos ? path : path.substr(slash + 1));
    Recognise(node != nullptr && offset + std::uint64_t{size} <= node->size, "a prompt texture streams from an unknown bundle node");
    texture.streamOffset = node->offset + offset;
    texture.tiled = bundle.Read(texture.streamOffset, size);
    return texture;
}

struct Atlas {
    std::int64_t id = 0;
    Bytes object;
    const Type* type = nullptr;
    Field root;
    std::int64_t sheet = 0;
    std::vector<std::string> names;
    std::vector<std::uint32_t> hashes;
    std::vector<std::int64_t> sprites;  // by glyph
    std::vector<Rect> rects;            // by glyph
    std::vector<std::int32_t> glyphOfCharacter;
};

Atlas ReadAtlas(const SerializedFile& file, std::int64_t id) {
    const ObjectEntry* entry = file.Find(id);
    Recognise(entry != nullptr && file.TypeOf(*entry).classId == kMonoBehaviourClass, "a TextMeshPro sprite asset is missing");
    Atlas atlas;
    atlas.id = id;
    atlas.object = file.Read(*entry);
    atlas.type = &file.TypeOf(*entry);
    atlas.root = Walk(*atlas.type, atlas.object);
    const Type& type = *atlas.type;
    atlas.sheet = ReadReference(atlas.object, type, Child(type, atlas.root, "spriteSheet")).path;
    const Field& characters = At(type, atlas.root, {"m_SpriteCharacterTable", "Array"});
    const Field& glyphs = At(type, atlas.root, {"m_SpriteGlyphTable", "Array"});
    for (const Field& character : characters.items) {
        atlas.names.push_back(ReadString(atlas.object, type, Child(type, character, "m_Name")));
        atlas.hashes.push_back(Get<std::uint32_t>(atlas.object, Child(type, character, "m_HashCode").begin));
        atlas.glyphOfCharacter.push_back(Get<std::int32_t>(atlas.object, Child(type, character, "m_GlyphIndex").begin));
    }
    for (const Field& glyph : glyphs.items) {
        const Field& rect = Child(type, glyph, "m_GlyphRect");
        atlas.rects.push_back({Get<std::int32_t>(atlas.object, Child(type, rect, "m_X").begin), Get<std::int32_t>(atlas.object, Child(type, rect, "m_Y").begin),
                               Get<std::int32_t>(atlas.object, Child(type, rect, "m_Width").begin), Get<std::int32_t>(atlas.object, Child(type, rect, "m_Height").begin)});
        atlas.sprites.push_back(ReadReference(atlas.object, type, Child(type, glyph, "sprite")).path);
    }
    return atlas;
}

std::string ObjectName(const SerializedFile& file, const ObjectEntry& entry) {
    const std::int32_t classId = file.TypeOf(entry).classId;
    const std::size_t at = classId == kMonoBehaviourClass ? 28 : 0;
    if (classId != kMonoBehaviourClass && classId != kSpriteClass && classId != kTextureClass) return {};
    if (entry.size < at + 4) return {};
    const std::size_t base = file.dataOffset + static_cast<std::size_t>(entry.start);
    const auto length = Get<std::uint32_t>(file.data, base + at);
    if (length > entry.size - at - 4) return {};
    return std::string(reinterpret_cast<const char*>(file.data.data() + base + at + 4), length);
}

std::int64_t FindNamed(const SerializedFile& file, std::int32_t classId, const std::string& name) {
    std::int64_t found = 0;
    int matches = 0;
    for (const ObjectEntry& entry : file.objects) {
        if (file.TypeOf(entry).classId == classId && ObjectName(file, entry) == name) {
            found = entry.pathId;
            ++matches;
        }
    }
    Recognise(matches == 1, "the bundle does not contain exactly one " + name);
    return found;
}

using SpecialIcons = std::vector<std::pair<std::string, std::int64_t>>;

SpecialIcons ReadSpecialIcons(const Bytes& object, const Type& type, const Field& root, const char* set) {
    const Field& keys = At(type, root, {set, "specialIcons", "keyData", "Array"});
    const Field& values = At(type, root, {set, "specialIcons", "valueData", "Array"});
    Recognise(keys.count == values.count, "a special icon map has a different number of keys and values");
    SpecialIcons icons;
    for (std::size_t index = 0; index < keys.items.size(); ++index) {
        const Reference value = ReadReference(object, type, values.items[index]);
        Recognise(value.file == 0, "a special icon points outside the bundle");
        icons.emplace_back(ReadString(object, type, keys.items[index]), value.path);
    }
    return icons;
}

std::int64_t ButtonSprite(const Bytes& object, const Type& type, const Field& root, const char* set, const char* button) {
    const Reference reference = ReadReference(object, type, At(type, root, {set, button}));
    Recognise(reference.file == 0 && reference.path != 0, "a button sprite is not a local object");
    return reference.path;
}

struct Held {
    std::int64_t id = 0;
    std::int32_t typeIndex = 0;
    Bytes object;
    const Type* type = nullptr;
    Field root;
};

Held Hold(const SerializedFile& file, std::int64_t id) {
    const ObjectEntry* entry = file.Find(id);
    Recognise(entry != nullptr, "an expected object is missing");
    Held held;
    held.id = id;
    held.typeIndex = entry->type;
    held.object = file.Read(*entry);
    held.type = &file.TypeOf(*entry);
    held.root = Walk(*held.type, held.object);
    return held;
}

Bytes Slice(const Bytes& object, const Field& field) {
    return Bytes(object.begin() + static_cast<std::ptrdiff_t>(field.begin), object.begin() + static_cast<std::ptrdiff_t>(field.end));
}

bool IsXboxStickName(const std::string& name) { return name.rfind("Xbox_LeftStick_", 0) == 0; }

const char* const kXboxSlots[] = {"buttonSouth", "buttonNorth", "buttonEast", "buttonWest"};

// What the bundle holds now, and (when it lacks them) where the new glyphs go.
struct NewSprite {
    const Icon* icon = nullptr;
    std::int64_t id = 0, renderKey = 0;
    Cell cell, source;
    std::string sourceSprite;
    std::int64_t sourceId = 0;
};

struct Plan {
    IconSetState state = IconSetState::kUnrecognised;
    std::string why;
    Held icon, assetBundle, spriteTemplate;
    Atlas xboxAtlas;
    Texture xbox, playStation;
    std::vector<NewSprite> sprites;
    std::array<std::int64_t, kMaskCount> maskSprites{};
    std::vector<std::int64_t> xboxButtons;
};

std::int64_t MaskSprite(const Plan& plan, std::size_t index) { return plan.maskSprites[index]; }

std::array<std::int64_t, kMaskCount> MaskTargets(const std::vector<std::int64_t>& buttons) {
    std::array<std::int64_t, kMaskCount> targets{};
    for (std::size_t index = 0; index < kMaskCount; ++index) targets[index] = kMaskIcons[index].slot < 0 ? kCircleSprite : buttons[static_cast<std::size_t>(kMaskIcons[index].slot)];
    return targets;
}

std::int64_t NewPathId(const SerializedFile& file, const std::string& name, const std::vector<std::int64_t>& chosen) {
    for (std::uint64_t salt = 0;; ++salt) {
        const auto candidate = static_cast<std::int64_t>(Fnv64("xbox_prompt_icons/" + name + "/" + std::to_string(salt)));
        if (candidate != 0 && file.Find(candidate) == nullptr && std::find(chosen.begin(), chosen.end(), candidate) == chosen.end()) return candidate;
    }
}

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

Bytes MakeSprite(const Bytes& object, const Type& type, const Field& root, const NewSprite& sprite, std::int64_t texture) {
    const Field& rd = Child(type, root, "m_RD");
    Recognise(ReadReference(object, type, Child(type, rd, "texture")).path == texture, "the template sprite uses another texture");
    const float x = static_cast<float>(sprite.cell.x), y = static_cast<float>(sprite.cell.y), size = static_cast<float>(kCell);
    const float half = size / 200.0f;

    Writer quad;
    quad.Floats({-half, half, 0.0f, half, -half, 0.0f, half, half, 0.0f, -half, -half, 0.0f});
    quad.Floats({0, 0, 0, 0, 0, 0, 0, 0});
    Writer indices;
    for (const int index : {3, 0, 1, 2, 1, 0}) indices.Put(static_cast<std::uint16_t>(index));
    Writer rect;
    rect.Floats({x, y, size, size});

    const Field& subMeshes = At(type, rd, {"m_SubMeshes", "Array"});
    Recognise(subMeshes.count == 1, "the template sprite has several sub meshes");
    const Field& subMesh = subMeshes.items[0];
    const Bytes subMeshBytes = Splice(object, subMesh.begin, subMesh.end,
                                      {{Child(type, subMesh, "indexCount").begin, Child(type, subMesh, "indexCount").end, Raw(std::uint32_t{6})},
                                       {Child(type, subMesh, "vertexCount").begin, Child(type, subMesh, "vertexCount").end, Raw(std::uint32_t{4})}});
    Bytes subMeshArray = Raw(std::int32_t{1});
    subMeshArray.insert(subMeshArray.end(), subMeshBytes.begin(), subMeshBytes.end());
    Bytes indexArray = Raw(static_cast<std::int32_t>(indices.data.size()));
    indexArray.insert(indexArray.end(), indices.data.begin(), indices.data.end());
    indexArray.resize(Align(indexArray.size(), 4));
    Bytes vertexArray = Raw(static_cast<std::int32_t>(quad.data.size()));
    vertexArray.insert(vertexArray.end(), quad.data.begin(), quad.data.end());
    vertexArray.resize(Align(vertexArray.size(), 4));

    Writer uv;
    uv.Floats({100.0f, x + size / 2.0f, 100.0f, y + size / 2.0f});
    Writer offset;
    offset.Floats({0.0f, 0.0f});
    const Field& vertexData = Child(type, rd, "m_VertexData");
    const auto replace = [&](const Field& field, Bytes bytes) { return Edit{field.begin, field.end, std::move(bytes)}; };
    return Splice(object, {
        replace(Child(type, root, "m_Name"), StringBytes(sprite.icon->sprite)),
        replace(Child(type, root, "m_Rect"), rect.data),
        replace(At(type, root, {"m_RenderDataKey", "second"}), Raw(sprite.renderKey)),
        replace(subMeshes, subMeshArray),
        replace(At(type, rd, {"m_IndexBuffer", "Array"}), indexArray),
        replace(Child(type, vertexData, "m_VertexCount"), Raw(std::uint32_t{4})),
        replace(Child(type, vertexData, "m_DataSize"), vertexArray),
        replace(Child(type, rd, "textureRect"), rect.data),
        replace(Child(type, rd, "textureRectOffset"), offset.data),
        replace(Child(type, rd, "uvTransform"), uv.data),
        replace(At(type, root, {"m_PhysicsShape", "Array"}), Raw(std::int32_t{0})),
    });
}

Bytes MakeIconData(const Plan& plan) {
    const Bytes& object = plan.icon.object;
    const Type& type = *plan.icon.type;
    const Field& keys = At(type, plan.icon.root, {"xboxSeries", "specialIcons", "keyData", "Array"});
    const Field& values = At(type, plan.icon.root, {"xboxSeries", "specialIcons", "valueData", "Array"});
    Writer newKeys, newValues;
    for (const NewSprite& sprite : plan.sprites) {
        newKeys.String(sprite.icon->token);
        newValues.Pointer(sprite.id);
    }
    for (std::size_t index = 0; index < kMaskCount; ++index) {
        newKeys.String(kMaskIcons[index].token);
        newValues.Pointer(MaskSprite(plan, index));
    }
    const auto added = static_cast<std::int32_t>(plan.sprites.size() + kMaskCount);
    const auto count = [&](const Field& array) { return Edit{array.begin, array.begin + 4, Raw(array.count + added)}; };
    return Splice(object, {count(keys), {keys.end, keys.end, newKeys.data}, count(values), {values.end, values.end, newValues.data}});
}

Bytes MakeAtlas(const Atlas& atlas, const std::vector<NewSprite>& sprites) {
    const Type& type = *atlas.type;
    const Field& characters = At(type, atlas.root, {"m_SpriteCharacterTable", "Array"});
    const Field& glyphs = At(type, atlas.root, {"m_SpriteGlyphTable", "Array"});
    const Field& characterTemplate = characters.items.back();
    const Field& glyphTemplate = glyphs.items.back();
    Bytes newCharacters, newGlyphs;
    for (std::size_t index = 0; index < sprites.size(); ++index) {
        const NewSprite& sprite = sprites[index];
        const auto glyphIndex = static_cast<std::uint32_t>(glyphs.items.size() + index);
        const auto field = [&](const Field& parent, const char* name) -> const Field& { return Child(type, parent, name); };
        const Bytes character = Splice(atlas.object, characterTemplate.begin, characterTemplate.end,
                                       {{field(characterTemplate, "m_GlyphIndex").begin, field(characterTemplate, "m_GlyphIndex").end, Raw(glyphIndex)},
                                        {field(characterTemplate, "m_Name").begin, field(characterTemplate, "m_Name").end, StringBytes(sprite.icon->sprite)},
                                        {field(characterTemplate, "m_HashCode").begin, field(characterTemplate, "m_HashCode").end, Raw(TextMeshProHash(sprite.icon->sprite))}});
        const Field& rect = field(glyphTemplate, "m_GlyphRect");
        const Field& pointer = field(glyphTemplate, "sprite");
        const Bytes glyph = Splice(atlas.object, glyphTemplate.begin, glyphTemplate.end,
                                   {{field(glyphTemplate, "m_Index").begin, field(glyphTemplate, "m_Index").end, Raw(glyphIndex)},
                                    {field(rect, "m_X").begin, field(rect, "m_X").end, Raw(static_cast<std::int32_t>(sprite.cell.x))},
                                    {field(rect, "m_Y").begin, field(rect, "m_Y").end, Raw(static_cast<std::int32_t>(sprite.cell.y))},
                                    {field(rect, "m_Width").begin, field(rect, "m_Width").end, Raw(static_cast<std::int32_t>(kCell))},
                                    {field(rect, "m_Height").begin, field(rect, "m_Height").end, Raw(static_cast<std::int32_t>(kCell))},
                                    {Child(type, pointer, "m_PathID").begin, Child(type, pointer, "m_PathID").end, Raw(sprite.id)}});
        newCharacters.insert(newCharacters.end(), character.begin(), character.end());
        newGlyphs.insert(newGlyphs.end(), glyph.begin(), glyph.end());
    }
    const auto count = [&](const Field& array) { return Edit{array.begin, array.begin + 4, Raw(array.count + static_cast<std::int32_t>(sprites.size()))}; };
    return Splice(atlas.object, {count(characters), {characters.end, characters.end, newCharacters}, count(glyphs), {glyphs.end, glyphs.end, newGlyphs}});
}

// The bundle's AssetBundle object lists, per asset, the objects to preload; the new sprites join the icon data's list, which is kept sorted.
Bytes MakeAssetBundle(const Held& held, const std::vector<NewSprite>& sprites) {
    const Bytes& object = held.object;
    const Type& type = *held.type;
    const Field& preload = At(type, held.root, {"m_PreloadTable", "Array"});
    const Field& container = At(type, held.root, {"m_Container", "Array"});
    struct Entry {
        std::int32_t index, size;
        const Field* indexField;
        const Field* sizeField;
        std::int64_t asset;
    };
    std::vector<Entry> entries;
    for (const Field& item : container.items) {
        const Field& info = Child(type, item, "second");
        const Field& index = Child(type, info, "preloadIndex");
        const Field& size = Child(type, info, "preloadSize");
        const Reference asset = ReadReference(object, type, Child(type, info, "asset"));
        entries.push_back({Get<std::int32_t>(object, index.begin), Get<std::int32_t>(object, size.begin), &index, &size, asset.file == 0 ? asset.path : 0});
    }
    const Entry* icon = nullptr;
    for (const Entry& entry : entries) {
        if (entry.asset == kIconData) {
            Recognise(icon == nullptr, "the icon data has several container entries");
            icon = &entry;
        }
    }
    Recognise(icon != nullptr && icon->index >= 0 && icon->size > 0 && icon->index + icon->size <= preload.count, "the icon data has no preload list");

    std::vector<Reference> table;
    for (const Field& item : preload.items) table.push_back(ReadReference(object, type, item));
    const std::int32_t first = icon->index, last = icon->index + icon->size;
    std::int64_t previous = INT64_MIN;
    for (std::int32_t at = first; at < last; ++at) {
        if (table[static_cast<std::size_t>(at)].file != 0) continue;
        Recognise(table[static_cast<std::size_t>(at)].path > previous, "the icon data's preload list is not sorted");
        previous = table[static_cast<std::size_t>(at)].path;
    }

    std::vector<std::int64_t> ids;
    for (const NewSprite& sprite : sprites) ids.push_back(sprite.id);
    std::sort(ids.begin(), ids.end());
    std::map<std::int32_t, Writer> inserted;
    for (const std::int64_t id : ids) {
        std::int32_t at = first;
        while (at < last && (table[static_cast<std::size_t>(at)].file != 0 || table[static_cast<std::size_t>(at)].path < id)) ++at;
        Recognise(at == last || table[static_cast<std::size_t>(at)].path != id, "a new sprite is already in the preload list");
        inserted[at].Pointer(id);
    }

    const auto added = static_cast<std::int32_t>(ids.size());
    std::vector<Edit> edits = {{preload.begin, preload.begin + 4, Raw(preload.count + added)}};
    for (const auto& [at, writer] : inserted) {
        const std::size_t position = at < preload.count ? preload.items[static_cast<std::size_t>(at)].begin : preload.items.back().end;
        edits.push_back({position, position, writer.data});
    }
    for (const Entry& entry : entries) {
        std::int32_t index = entry.index, size = entry.size;
        if (entry.index == first && entry.size == icon->size) {
            size += added;
        } else if (entry.index >= last) {
            index += added;
        } else {
            Recognise(entry.index + entry.size <= first, "a preload list overlaps the icon data's");
        }
        if (index != entry.index) edits.push_back({entry.indexField->begin, entry.indexField->end, Raw(index)});
        if (size != entry.size) edits.push_back({entry.sizeField->begin, entry.sizeField->end, Raw(size)});
    }
    return Splice(object, std::move(edits));
}

std::vector<std::int64_t> IconPreload(const Held& held, const Bytes& object) {
    const Type& type = *held.type;
    const Field& preload = At(type, held.root, {"m_PreloadTable", "Array"});
    const Field& container = At(type, held.root, {"m_Container", "Array"});
    for (const Field& item : container.items) {
        const Field& info = Child(type, item, "second");
        const Reference asset = ReadReference(object, type, Child(type, info, "asset"));
        if (asset.file != 0 || asset.path != kIconData) continue;
        const auto from = Get<std::int32_t>(object, Child(type, info, "preloadIndex").begin);
        const auto size = Get<std::int32_t>(object, Child(type, info, "preloadSize").begin);
        Recognise(from >= 0 && size >= 0 && from + size <= preload.count, "the icon data has no preload list");
        std::vector<std::int64_t> ids;
        for (std::int32_t at = from; at < from + size; ++at) {
            const Reference reference = ReadReference(object, type, preload.items[static_cast<std::size_t>(at)]);
            if (reference.file == 0) ids.push_back(reference.path);
        }
        return ids;
    }
    throw Unsupported("the icon data has no container entry");
}

Held AssetBundleOf(const SerializedFile& file) {
    std::int64_t bundleObject = 0;
    for (const ObjectEntry& entry : file.objects) {
        if (file.TypeOf(entry).classId != kAssetBundleClass) continue;
        Recognise(bundleObject == 0, "the serialized file has several AssetBundle objects");
        bundleObject = entry.pathId;
    }
    Recognise(bundleObject != 0, "the serialized file has no AssetBundle object");
    return Hold(file, bundleObject);
}

// Confirms the bundle holds the analysed objects and relations; classifies the Xbox set. When it lacks the set, decides what to copy where.
Plan Analyse(const Bundle& bundle, const SerializedFile& file, bool detectOnly) {
    Plan plan;
    const std::int64_t icon = FindNamed(file, kMonoBehaviourClass, "PS5InputIconData");
    const std::int64_t xboxAtlasId = FindNamed(file, kMonoBehaviourClass, "Xbox_ButtonPromptsAtlas");
    const std::int64_t holder = FindNamed(file, kMonoBehaviourClass, "SystemPreloadDependencies");
    Recognise(icon == kIconData && xboxAtlasId == kXboxAtlas && holder == kPreloadHolder, "the bundle is not the analysed game version");
    {
        const Bytes holderBytes = file.Read(*file.Find(holder));
        Writer reference;
        reference.Pointer(icon);
        Recognise(std::search(holderBytes.begin(), holderBytes.end(), reference.data.begin(), reference.data.end()) != holderBytes.end(), "SystemPreloadDependencies does not reference the icon data");
    }
    plan.icon = Hold(file, icon);
    const Type& iconType = *plan.icon.type;
    const Bytes& iconObject = plan.icon.object;
    Recognise(ReadReference(iconObject, iconType, Child(iconType, plan.icon.root, "xboxIconAtlas")).path == kXboxAtlas, "the Xbox prompts use another atlas");
    const std::int64_t playStationAtlasId = ReadReference(iconObject, iconType, Child(iconType, plan.icon.root, "ps5IconAtlas")).path;
    const SpecialIcons xboxIcons = ReadSpecialIcons(iconObject, iconType, plan.icon.root, "xboxSeries");
    const SpecialIcons playStationIcons = ReadSpecialIcons(iconObject, iconType, plan.icon.root, "ps5");
    for (const char* button : kXboxSlots) plan.xboxButtons.push_back(ButtonSprite(iconObject, iconType, plan.icon.root, "xboxSeries", button));
    plan.maskSprites = MaskTargets(plan.xboxButtons);
    plan.xboxAtlas = ReadAtlas(file, kXboxAtlas);
    const Atlas& atlas = plan.xboxAtlas;
    for (std::size_t index = 0; index < kMaskCount; ++index) {
        const ObjectEntry* target = file.Find(plan.maskSprites[index]);
        Recognise(target != nullptr && file.TypeOf(*target).classId == kSpriteClass, std::string("the sprite for ") + kMaskIcons[index].token + " is missing");
    }
    Recognise(ObjectName(file, *file.Find(kCircleSprite)) == "Circle", "the interaction marker's fill disc is not the analysed sprite");

    std::size_t tokens = 0, characters = 0, sprites = 0, masks = 0;
    const auto tokenCount = [&](const char* token) { return std::count_if(xboxIcons.begin(), xboxIcons.end(), [&](const auto& pair) { return pair.first == token; }); };
    for (const Icon& item : kIcons) tokens += static_cast<std::size_t>(tokenCount(item.token));
    for (const MaskIcon& item : kMaskIcons) masks += static_cast<std::size_t>(tokenCount(item.token));
    for (const std::string& name : atlas.names) characters += IsXboxStickName(name);
    for (const ObjectEntry& entry : file.objects) sprites += file.TypeOf(entry).classId == kSpriteClass && IsXboxStickName(ObjectName(file, entry));
    if (tokens == 0 && characters == 0 && sprites == 0 && masks == 0) {
        plan.state = IconSetState::kMissing;
    } else {
        bool full = tokens == kIconCount && characters == kIconCount && sprites == kIconCount && masks == kMaskCount && xboxIcons.size() == 1 + kIconCount + kMaskCount;
        for (const Icon& item : kIcons) {
            const auto key = std::find_if(xboxIcons.begin(), xboxIcons.end(), [&](const auto& pair) { return pair.first == item.token; });
            const ObjectEntry* sprite = key == xboxIcons.end() ? nullptr : file.Find(key->second);
            const auto name = std::find(atlas.names.begin(), atlas.names.end(), std::string(item.sprite));
            full = full && sprite != nullptr && ObjectName(file, *sprite) == item.sprite && name != atlas.names.end() &&
                   atlas.sprites[static_cast<std::size_t>(atlas.glyphOfCharacter[static_cast<std::size_t>(name - atlas.names.begin())])] == key->second;
        }
        for (std::size_t index = 0; index < kMaskCount; ++index) {
            const auto key = std::find_if(xboxIcons.begin(), xboxIcons.end(), [&](const auto& pair) { return pair.first == kMaskIcons[index].token; });
            full = full && key != xboxIcons.end() && key->second == plan.maskSprites[index];
        }
        plan.state = full ? IconSetState::kComplete : IconSetState::kPartial;
        if (!full) plan.why = "the Xbox prompt set is only partly patched";
    }
    if (plan.state != IconSetState::kMissing || detectOnly) return plan;

    Recognise(xboxIcons.size() == 1 && xboxIcons[0].first == "!LookInput" && xboxIcons[0].second == kLookInputSprite, "the Xbox special icon map is not the analysed one");
    const std::size_t existing = atlas.names.size();
    Recognise(existing == 19 && atlas.rects.size() == existing, "the Xbox atlas is not the analysed one");
    const Type& atlasType = *atlas.type;
    const Field& characterTable = At(atlasType, atlas.root, {"m_SpriteCharacterTable", "Array"});
    const Field& glyphTable = At(atlasType, atlas.root, {"m_SpriteGlyphTable", "Array"});
    const Field& lastGlyph = glyphTable.items.back();
    std::vector<bool> glyphUsed(existing, false);
    for (std::size_t index = 0; index < existing; ++index) {
        Recognise(TextMeshProHash(atlas.names[index]) == atlas.hashes[index], "the Xbox atlas uses another name hash");
        const auto glyph = static_cast<std::size_t>(atlas.glyphOfCharacter[index]);
        Recognise(glyph < existing && !glyphUsed[glyph], "the Xbox atlas characters do not map to distinct glyphs");
        glyphUsed[glyph] = true;
        const Field& item = glyphTable.items[index];
        Recognise(Get<std::uint32_t>(atlas.object, Child(atlasType, item, "m_Index").begin) == index, "the Xbox atlas glyphs are not in index order");
        for (const char* name : {"m_Metrics", "m_Scale", "m_AtlasIndex", "m_ClassDefinitionType"}) {
            Recognise(Slice(atlas.object, Child(atlasType, item, name)) == Slice(atlas.object, Child(atlasType, lastGlyph, name)), "the Xbox glyphs do not share one metrics set");
        }
    }
    (void)characterTable;

    plan.spriteTemplate = Hold(file, atlas.sprites[0]);
    const ObjectEntry& templateEntry = *file.Find(atlas.sprites[0]);
    Recognise(file.TypeOf(templateEntry).classId == kSpriteClass, "the Xbox atlas glyph does not use a Sprite");
    const Type& spriteType = *plan.spriteTemplate.type;
    const Field& templateKey = Child(spriteType, plan.spriteTemplate.root, "m_RenderDataKey");
    const Bytes templateGuid = Slice(plan.spriteTemplate.object, Child(spriteType, templateKey, "first"));
    std::vector<std::int64_t> usedKeys;
    for (const ObjectEntry& entry : file.objects) {
        if (entry.type != templateEntry.type) continue;
        const Bytes object = file.Read(entry);
        const Field root = Walk(spriteType, object);
        const Field& key = Child(spriteType, root, "m_RenderDataKey");
        if (Slice(object, Child(spriteType, key, "first")) == templateGuid) usedKeys.push_back(Get<std::int64_t>(object, Child(spriteType, key, "second").begin));
    }

    plan.xbox = ReadTexture(bundle, file, atlas.sheet);
    const Atlas playStationAtlas = ReadAtlas(file, playStationAtlasId);
    plan.playStation = ReadTexture(bundle, file, playStationAtlas.sheet);
    std::vector<std::array<float, 4>> spriteRects;
    for (std::size_t index = 0; index < existing; ++index) {
        const Rect& rect = atlas.rects[index];
        Recognise(rect.x >= 0 && rect.y >= 0 && static_cast<std::uint32_t>(rect.x + rect.width) <= plan.xbox.width && static_cast<std::uint32_t>(rect.y + rect.height) <= plan.xbox.height,
                  "an Xbox glyph lies outside its texture");
        const Held sprite = Hold(file, atlas.sprites[index]);
        Recognise(file.TypeOf(*file.Find(sprite.id)).classId == kSpriteClass && sprite.typeIndex == templateEntry.type, "an Xbox glyph does not use a Sprite");
        const Field& box = Child(spriteType, sprite.root, "m_Rect");
        std::array<float, 4> values{};
        for (std::size_t part = 0; part < 4; ++part) values[part] = Get<float>(sprite.object, box.items[part].begin);
        spriteRects.push_back(values);
    }

    std::vector<Cell> cells;
    for (std::uint32_t y = 0; y + kCell <= plan.xbox.height; y += kCell) {
        for (std::uint32_t x = 0; x + kCell <= plan.xbox.width; x += kCell) {
            const Rect cell{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(kCell), static_cast<std::int32_t>(kCell)};
            bool free = true;
            for (const Rect& rect : atlas.rects) free = free && !Overlaps(cell, static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width), static_cast<float>(rect.height));
            for (const auto& rect : spriteRects) free = free && !Overlaps(cell, rect[0], rect[1], rect[2], rect[3]);
            for (std::uint32_t row = 0; free && row < kCell; ++row) {
                for (std::uint32_t column = 0; free && column < kCell; ++column) free = plan.xbox.Texel(x + column, y + row, 3) == 0;
            }
            if (free) cells.push_back({x, y});
        }
    }
    Recognise(cells.size() >= kIconCount, "the Xbox texture has no free cells for the new glyphs");

    std::vector<std::int64_t> chosen;
    for (std::size_t index = 0; index < kIconCount; ++index) {
        NewSprite sprite;
        sprite.icon = &kIcons[index];
        const auto source = std::find_if(playStationIcons.begin(), playStationIcons.end(), [&](const auto& pair) { return pair.first == sprite.icon->token; });
        Recognise(source != playStationIcons.end(), std::string("the PlayStation set has no ") + sprite.icon->token);
        const ObjectEntry* sourceEntry = file.Find(source->second);
        Recognise(sourceEntry != nullptr && file.TypeOf(*sourceEntry).classId == kSpriteClass, std::string("the PlayStation glyph for ") + sprite.icon->token + " is missing");
        sprite.sourceSprite = ObjectName(file, *sourceEntry);
        sprite.sourceId = source->second;
        std::optional<Cell> located;
        for (std::size_t at = 0; at < playStationAtlas.names.size() && !located; ++at) {
            const auto glyph = static_cast<std::size_t>(playStationAtlas.glyphOfCharacter[at]);
            if (playStationAtlas.names[at] == sprite.sourceSprite && glyph < playStationAtlas.rects.size() && playStationAtlas.sprites[glyph] == sprite.sourceId)
                located = LocateSource(plan.playStation, playStationAtlas.rects[glyph], sprite.sourceSprite);
        }
        Recognise(located.has_value(), "the PlayStation glyph " + sprite.sourceSprite + " is not in the PlayStation prompt atlas");
        sprite.source = *located;
        sprite.cell = cells[index];
        sprite.id = NewPathId(file, sprite.icon->sprite, chosen);
        chosen.push_back(sprite.id);
        sprite.renderKey = NewRenderKey(sprite.icon->sprite, usedKeys);
        usedKeys.push_back(sprite.renderKey);
        plan.sprites.push_back(sprite);
    }

    plan.assetBundle = AssetBundleOf(file);

    std::uint64_t hash = 0xcbf29ce484222325;
    const auto mix = [&](const Bytes& bytes) { hash = Fnv64(bytes.data(), bytes.size(), hash); };
    mix(plan.icon.object);
    mix(atlas.object);
    mix(file.Read(*file.Find(playStationAtlasId)));
    mix(plan.assetBundle.object);
    mix(plan.spriteTemplate.object);
    mix(file.Read(*file.Find(kLookInputSprite)));
    mix(file.Read(*file.Find(kCircleSprite)));
    for (const std::int64_t id : plan.xboxButtons) mix(file.Read(*file.Find(id)));
    for (const NewSprite& sprite : plan.sprites) mix(file.Read(*file.Find(sprite.sourceId)));
    mix(plan.xbox.tiled);
    mix(plan.playStation.tiled);
    Recognise(hash == kSourceFingerprint, "the game data is not the analysed version");
    return plan;
}

void CopyCell(const Texture& from, std::uint32_t fromX, std::uint32_t fromY, Texture& to, std::uint32_t toX, std::uint32_t toY) {
    for (std::uint32_t row = 0; row < kCell; ++row) {
        for (std::uint32_t column = 0; column < kCell; ++column) {
            std::memcpy(to.tiled.data() + TexelOffset(toX + column, toY + row, to.width), from.tiled.data() + TexelOffset(fromX + column, fromY + row, from.width), 4);
        }
    }
}

void Load(const fs::path& bundlePath, Bundle& bundle, SerializedFile& file) {
    Require(ReadFile(bundlePath, bundle.file), "the bundle could not be read: " + bundlePath.string());
    bundle.Parse();
    Recognise(bundle.nodes[0].name.rfind("CAB-", 0) == 0 && (bundle.nodes[0].flags & 4) != 0, "the first bundle node is not a serialized file");
    file.data = bundle.ReadNode(bundle.nodes[0]);
    file.Parse();
}

// Reads the written bytes back and checks them against the original: only the intended changes may differ.
void Verify(const Bytes& written, const Bundle& original, const SerializedFile& originalFile, const Plan& plan, const Texture& patchedXbox, const std::map<std::int64_t, Bytes>& changed) {
    Bundle patched;
    patched.file = written;
    patched.Parse();
    Require(patched.nodes.size() == original.nodes.size() && patched.flags == original.flags && patched.player == original.player && patched.engine == original.engine, "the patched bundle header differs");
    for (std::size_t index = 0; index < patched.nodes.size(); ++index) {
        Require(patched.nodes[index].name == original.nodes[index].name && patched.nodes[index].flags == original.nodes[index].flags &&
                    patched.nodes[index].size == (index == 0 ? patched.nodes[0].size : original.nodes[index].size),
                "a patched bundle node differs");
    }
    for (std::size_t index = 0; index < patched.blocks.size(); ++index) patched.ReadBlock(index);

    SerializedFile out;
    out.data = patched.ReadNode(patched.nodes[0]);
    out.Parse();
    Require(out.types.size() == originalFile.types.size() && out.objects.size() == originalFile.objects.size() + kIconCount, "the patched object table has an unexpected size");
    for (std::size_t index = 0; index < out.types.size(); ++index) Require(out.types[index].classId == originalFile.types[index].classId, "a type differs");
    for (std::size_t index = 0; index + 1 < out.objects.size(); ++index) Require(out.objects[index].pathId < out.objects[index + 1].pathId, "the object table is not sorted");
    for (const ObjectEntry& entry : originalFile.objects) {
        const ObjectEntry* now = out.Find(entry.pathId);
        Require(now != nullptr && now->type == entry.type, "an object was lost or retyped");
        if (changed.count(entry.pathId) == 0) Require(out.Read(*now) == originalFile.Read(entry), "an unchanged object differs");
    }
    for (const auto& [id, bytes] : changed) Require(out.Read(*out.Find(id)) == bytes, "a changed object was not written as built");

    const Held newIcon = Hold(out, kIconData);
    SpecialIcons expected = ReadSpecialIcons(plan.icon.object, *plan.icon.type, plan.icon.root, "xboxSeries");
    for (const NewSprite& sprite : plan.sprites) expected.emplace_back(sprite.icon->token, sprite.id);
    for (std::size_t index = 0; index < kMaskCount; ++index) expected.emplace_back(kMaskIcons[index].token, plan.maskSprites[index]);
    Require(ReadSpecialIcons(newIcon.object, *newIcon.type, newIcon.root, "xboxSeries") == expected, "the Xbox special icon map is not as designed");
    for (const char* set : {"ps5", "ps4"}) {
        Require(ReadSpecialIcons(newIcon.object, *newIcon.type, newIcon.root, set) == ReadSpecialIcons(plan.icon.object, *plan.icon.type, plan.icon.root, set), "another special icon map changed");
    }

    const Atlas now = ReadAtlas(out, kXboxAtlas);
    const Atlas& before = plan.xboxAtlas;
    const std::size_t old = before.names.size();
    Require(now.names.size() == old + kIconCount && now.rects.size() == old + kIconCount && now.sheet == before.sheet, "the Xbox atlas counts are not as designed");
    const Type& atlasType = *now.type;
    const Field& newCharacters = At(atlasType, now.root, {"m_SpriteCharacterTable", "Array"});
    const Field& newGlyphs = At(atlasType, now.root, {"m_SpriteGlyphTable", "Array"});
    const Field& oldCharacters = At(*before.type, before.root, {"m_SpriteCharacterTable", "Array"});
    const Field& oldGlyphs = At(*before.type, before.root, {"m_SpriteGlyphTable", "Array"});
    for (std::size_t index = 0; index < old; ++index) {
        Require(Slice(now.object, newCharacters.items[index]) == Slice(before.object, oldCharacters.items[index]) && Slice(now.object, newGlyphs.items[index]) == Slice(before.object, oldGlyphs.items[index]),
                "an original Xbox atlas entry changed");
    }
    for (std::size_t index = 0; index < kIconCount; ++index) {
        const NewSprite& sprite = plan.sprites[index];
        const std::size_t at = old + index;
        const Rect rect = now.rects[at];
        Require(now.names[at] == sprite.icon->sprite && now.glyphOfCharacter[at] == static_cast<std::int32_t>(at) && now.sprites[at] == sprite.id && rect.x == static_cast<std::int32_t>(sprite.cell.x) &&
                    rect.y == static_cast<std::int32_t>(sprite.cell.y) && rect.width == static_cast<std::int32_t>(kCell) && rect.height == static_cast<std::int32_t>(kCell) &&
                    now.hashes[at] == TextMeshProHash(sprite.icon->sprite) && Get<std::uint32_t>(now.object, Child(atlasType, newGlyphs.items[at], "m_Index").begin) == at,
                "a new Xbox atlas entry is not as designed");
        for (const char* name : {"m_Metrics", "m_Scale", "m_AtlasIndex", "m_ClassDefinitionType"}) {
            Require(Slice(now.object, Child(atlasType, newGlyphs.items[at], name)) == Slice(now.object, Child(atlasType, newGlyphs.items[0], name)), "a new Xbox glyph has other metrics");
        }

        const ObjectEntry* entry = out.Find(sprite.id);
        Require(entry != nullptr && out.TypeOf(*entry).classId == kSpriteClass && entry->type == plan.spriteTemplate.typeIndex, "a new sprite is missing or has another type");
        const Held held = Hold(out, sprite.id);
        const Type& type = *held.type;
        const Field& rd = Child(type, held.root, "m_RD");
        std::vector<float> box;
        for (const Field& part : Child(type, held.root, "m_Rect").items) box.push_back(Get<float>(held.object, part.begin));
        Require(ReadString(held.object, type, Child(type, held.root, "m_Name")) == sprite.icon->sprite && box == std::vector<float>{static_cast<float>(sprite.cell.x), static_cast<float>(sprite.cell.y), 64.0f, 64.0f} &&
                    ReadReference(held.object, type, Child(type, rd, "texture")).path == before.sheet &&
                    Get<std::int64_t>(held.object, At(type, held.root, {"m_RenderDataKey", "second"}).begin) == sprite.renderKey &&
                    Get<std::uint32_t>(held.object, At(type, rd, {"m_VertexData", "m_VertexCount"}).begin) == 4 && At(type, rd, {"m_IndexBuffer", "Array"}).count == 12 &&
                    At(type, held.root, {"m_PhysicsShape", "Array"}).count == 0,
                "a new sprite is not as designed");
    }

    const Held newBundle = Hold(out, plan.assetBundle.id);
    const std::vector<std::int64_t> was = IconPreload(plan.assetBundle, plan.assetBundle.object), is = IconPreload(newBundle, newBundle.object);
    std::vector<std::int64_t> rest = is;
    for (const NewSprite& sprite : plan.sprites) {
        const auto at = std::find(rest.begin(), rest.end(), sprite.id);
        Require(at != rest.end(), "a new sprite is not in the icon data's preload list");
        rest.erase(at);
    }
    Require(rest == was, "the icon data's preload list changed other than intended");
    const Type& bundleType = *newBundle.type;
    Require(At(bundleType, newBundle.root, {"m_Container", "Array"}).count == At(*plan.assetBundle.type, plan.assetBundle.root, {"m_Container", "Array"}).count &&
                At(bundleType, newBundle.root, {"m_PreloadTable", "Array"}).count == At(*plan.assetBundle.type, plan.assetBundle.root, {"m_PreloadTable", "Array"}).count + static_cast<std::int32_t>(kIconCount),
            "the preload table has an unexpected size");

    for (std::size_t index = 1; index < original.nodes.size(); ++index) {
        const Bytes before1 = original.ReadNode(original.nodes[index]), after = patched.ReadNode(patched.nodes[index]);
        Require(before1.size() == after.size(), "a resource node has another size");
        if (original.nodes[index].offset + original.nodes[index].size <= plan.xbox.streamOffset || original.nodes[index].offset > plan.xbox.streamOffset) {
            Require(before1 == after, "a resource node changed");
            continue;
        }
        const std::size_t at = static_cast<std::size_t>(plan.xbox.streamOffset - original.nodes[index].offset);
        const std::size_t size = patchedXbox.tiled.size();
        Require(std::equal(before1.begin(), before1.begin() + static_cast<std::ptrdiff_t>(at), after.begin()) &&
                    std::equal(before1.begin() + static_cast<std::ptrdiff_t>(at + size), before1.end(), after.begin() + static_cast<std::ptrdiff_t>(at + size)),
                "bytes outside the Xbox texture changed");
        Texture now1;
        now1.width = plan.xbox.width;
        now1.height = plan.xbox.height;
        now1.tiled.assign(after.begin() + static_cast<std::ptrdiff_t>(at), after.begin() + static_cast<std::ptrdiff_t>(at + size));
        for (std::uint32_t y = 0; y < now1.height; ++y) {
            for (std::uint32_t x = 0; x < now1.width; ++x) {
                const NewSprite* cell = nullptr;
                for (const NewSprite& sprite : plan.sprites) {
                    if (x >= sprite.cell.x && x < sprite.cell.x + kCell && y >= sprite.cell.y && y < sprite.cell.y + kCell) cell = &sprite;
                }
                for (int channel = 0; channel < 4; ++channel) {
                    const std::uint8_t expectedTexel = cell == nullptr ? plan.xbox.Texel(x, y, channel) : plan.playStation.Texel(cell->source.x + x - cell->cell.x, cell->source.y + y - cell->cell.y, channel);
                    Require(now1.Texel(x, y, channel) == expectedTexel, "the patched Xbox texture is not as designed");
                }
            }
        }
    }
}

__attribute__((format(printf, 2, 3))) void Note(std::string& report, const char* format, ...) {
    char line[2048];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    report += line;
}

// Every new glyph cell of a prepared bundle holds artwork: the Xbox texture's texels under the sprites are not empty.
bool GlyphCellsDrawn(const Bundle& bundle, const SerializedFile& file, const Plan& plan) {
    const Texture texture = ReadTexture(bundle, file, plan.xboxAtlas.sheet);
    for (const Icon& item : kIcons) {
        const auto name = std::find(plan.xboxAtlas.names.begin(), plan.xboxAtlas.names.end(), std::string(item.sprite));
        if (name == plan.xboxAtlas.names.end()) return false;
        const Rect rect = plan.xboxAtlas.rects[static_cast<std::size_t>(plan.xboxAtlas.glyphOfCharacter[static_cast<std::size_t>(name - plan.xboxAtlas.names.begin())])];
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

IconSetState InspectIconBundle(const fs::path& bundlePath, std::string* why) {
    try {
        Bundle bundle;
        SerializedFile file;
        Load(bundlePath, bundle, file);
        const Plan plan = Analyse(bundle, file, true);
        if (why != nullptr) *why = plan.why;
        return plan.state;
    } catch (const std::exception& failure) {
        if (why != nullptr) *why = failure.what();
        return IconSetState::kUnrecognised;
    }
}

bool HasBundleIcons(const fs::path& bundlePath) {
    try {
        Bundle bundle;
        SerializedFile file;
        Load(bundlePath, bundle, file);
        const Plan plan = Analyse(bundle, file, true);
        return plan.state == IconSetState::kComplete && GlyphCellsDrawn(bundle, file, plan);
    } catch (const std::exception&) {
        return false;
    }
}

bool ReadBundlePrompts(const fs::path& bundlePath, BundlePrompts& prompts) {
    try {
        Bundle bundle;
        SerializedFile file;
        Load(bundlePath, bundle, file);
        const Plan plan = Analyse(bundle, file, true);
        prompts = {};
        prompts.iconData = kIconData;
        prompts.xboxAtlas = kXboxAtlas;
        prompts.circle = kCircleSprite;
        prompts.xboxSpecial = ReadSpecialIcons(plan.icon.object, *plan.icon.type, plan.icon.root, "xboxSeries");
        prompts.ps5Special = ReadSpecialIcons(plan.icon.object, *plan.icon.type, plan.icon.root, "ps5");
        prompts.xboxButtons = plan.xboxButtons;
        const Held assetBundle = AssetBundleOf(file);
        prompts.preload = IconPreload(assetBundle, assetBundle.object);
        const std::int64_t playStationAtlas = ReadReference(plan.icon.object, *plan.icon.type, Child(*plan.icon.type, plan.icon.root, "ps5IconAtlas")).path;
        const auto fill = [&](const Atlas& atlas, BundleAtlas& target) {
            target.sheet = atlas.sheet;
            for (std::size_t index = 0; index < atlas.names.size(); ++index) {
                target.characters.push_back({atlas.names[index], static_cast<std::uint32_t>(atlas.glyphOfCharacter[index]), atlas.hashes[index]});
            }
            for (std::size_t index = 0; index < atlas.rects.size(); ++index) {
                const Rect& rect = atlas.rects[index];
                const Field& glyph = At(*atlas.type, atlas.root, {"m_SpriteGlyphTable", "Array"}).items[index];
                target.glyphs.push_back({Get<std::uint32_t>(atlas.object, Child(*atlas.type, glyph, "m_Index").begin), rect.x, rect.y, rect.width, rect.height, atlas.sprites[index]});
            }
        };
        fill(plan.xboxAtlas, prompts.xbox);
        const Atlas ps5 = ReadAtlas(file, playStationAtlas);
        fill(ps5, prompts.ps5);
        const auto texture = [&](std::int64_t id, BundleTexture& target) {
            const Texture read = ReadTexture(bundle, file, id);
            target.width = read.width;
            target.height = read.height;
            target.tiled = read.tiled;
        };
        texture(plan.xboxAtlas.sheet, prompts.xboxSheet);
        texture(ps5.sheet, prompts.ps5Sheet);
        for (const ObjectEntry& entry : file.objects) {
            if (file.TypeOf(entry).classId != kSpriteClass) continue;
            const Held held = Hold(file, entry.pathId);
            const Type& type = *held.type;
            BundleSprite sprite;
            sprite.name = ReadString(held.object, type, Child(type, held.root, "m_Name"));
            const Field& box = Child(type, held.root, "m_Rect");
            for (std::size_t part = 0; part < 4; ++part) sprite.rect[part] = Get<float>(held.object, box.items[part].begin);
            sprite.texture = ReadReference(held.object, type, Child(type, Child(type, held.root, "m_RD"), "texture")).path;
            prompts.sprites[entry.pathId] = sprite;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

IconPatchResult PatchIconBundle(const fs::path& source, const fs::path& out, std::string& report) {
    std::error_code error;
    bool owned = false;  // the output is only ours to remove once it is known not to be the source
    const auto discard = [&] {
        if (owned) fs::remove(out, error);
    };
    try {
        Require(fs::is_regular_file(source, error), "the game's icon bundle does not exist: " + source.string());
        Require(!(fs::exists(out, error) && fs::equivalent(source, out, error)), "the output file is the game's bundle: " + out.string());
        owned = true;
        Bundle bundle;
        SerializedFile file;
        Load(source, bundle, file);
        Note(report, "Bundle %s: %llu bytes, UnityFS %u (%s), %llu blocks, serialized node %s (%llu types, %llu objects)\n", source.string().c_str(), static_cast<unsigned long long>(bundle.file.size()),
             bundle.version, bundle.engine.c_str(), static_cast<unsigned long long>(bundle.blocks.size()), bundle.nodes[0].name.c_str(), static_cast<unsigned long long>(file.types.size()),
             static_cast<unsigned long long>(file.objects.size()));
        Plan plan = Analyse(bundle, file, false);
        if (plan.state == IconSetState::kComplete) {
            Note(report, "The bundle's Xbox prompt set is already complete; nothing to prepare\n");
            return IconPatchResult::kComplete;
        }
        Recognise(plan.state == IconSetState::kMissing, plan.why.empty() ? "the Xbox prompt set is only partly patched" : plan.why);

        std::map<std::int64_t, Bytes> changed;
        changed[plan.icon.id] = MakeIconData(plan);
        changed[plan.xboxAtlas.id] = MakeAtlas(plan.xboxAtlas, plan.sprites);
        changed[plan.assetBundle.id] = MakeAssetBundle(plan.assetBundle, plan.sprites);
        std::vector<std::tuple<std::int64_t, std::int32_t, Bytes>> added;
        Texture patchedXbox = plan.xbox;
        for (const NewSprite& sprite : plan.sprites) {
            added.emplace_back(sprite.id, plan.spriteTemplate.typeIndex, MakeSprite(plan.spriteTemplate.object, *plan.spriteTemplate.type, plan.spriteTemplate.root, sprite, plan.xboxAtlas.sheet));
            CopyCell(plan.playStation, sprite.source.x, sprite.source.y, patchedXbox, sprite.cell.x, sprite.cell.y);
            Note(report, "%s: %s (%lld) from PlayStation %s (%lld) at (%u,%u) -> %s %lld at (%u,%u)\n", sprite.icon->token, sprite.icon->sprite, static_cast<long long>(sprite.id),
                 sprite.sourceSprite.c_str(), static_cast<long long>(sprite.sourceId), sprite.source.x, sprite.source.y, sprite.icon->sprite, static_cast<long long>(sprite.id), sprite.cell.x, sprite.cell.y);
        }
        for (std::size_t index = 0; index < kMaskCount; ++index) {
            Note(report, "%s: sprite %lld\n", kMaskIcons[index].token, static_cast<long long>(plan.maskSprites[index]));
        }
        const Bytes serialized = file.Rebuild(changed, added);
        const Bytes patched = Repack(bundle, serialized, {{plan.xbox.streamOffset, patchedXbox.tiled}});
        Verify(patched, bundle, file, plan, patchedXbox, changed);

        try {
            WriteFile(out, patched);
            Bytes onDisk;
            Require(ReadFile(out, onDisk) && onDisk == patched, "the written bundle does not read back as built");
            Verify(onDisk, bundle, file, plan, patchedXbox, changed);
        } catch (...) {
            std::error_code ignored;
            fs::remove(out, ignored);
            throw;
        }
        Note(report, "Prepared %s (%llu bytes)\n", out.string().c_str(), static_cast<unsigned long long>(patched.size()));
        return IconPatchResult::kPrepared;
    } catch (const Unsupported& failure) {
        Note(report, "Not supported: %s\n", failure.what());
        discard();
        return IconPatchResult::kUnsupported;
    } catch (const std::exception& failure) {
        Note(report, "Failed: %s\n", failure.what());
        discard();
        return IconPatchResult::kFailed;
    }
}

}  // namespace sos

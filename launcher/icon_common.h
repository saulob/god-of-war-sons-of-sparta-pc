// Helpers shared by the two Xbox prompt patchers: the serialized-file patch of resources.assets (controller_icons.cpp) and the
// Addressables icon bundle patch (icon_bundle.cpp).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace sos::icons {

using Bytes = std::vector<std::uint8_t>;

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The analysed game data differs: nothing is patched, and the answer is the same until the files change.
struct Unsupported : Failure {
    using Failure::Failure;
};

inline void Require(bool condition, const std::string& message) {
    if (!condition) throw Failure(message);
}

inline void Recognise(bool condition, const std::string& message) {
    if (!condition) throw Unsupported(message);
}

struct Icon {
    const char* token;
    const char* sprite;
};
inline constexpr Icon kIcons[] = {{"LASUP", "Xbox_LeftStick_Up"}, {"LASDOWN", "Xbox_LeftStick_Down"}, {"LASLEFT", "Xbox_LeftStick_Left"},
                                  {"LASRIGHT", "Xbox_LeftStick_Right"}};
inline constexpr std::size_t kIconCount = std::size(kIcons);
inline constexpr std::uint32_t kCell = 64;

// Special icons the PlayStation maps have and the Xbox map lacks: the interaction marker and the confirmation panels ask for them
// by name and draw an empty white square without them.
struct MaskIcon {
    const char* token;
    int slot;  // the Xbox button whose sprite it uses; -1: the interaction marker's fill disc
};
inline constexpr int kSouth = 0, kNorth = 1, kEast = 2;
inline constexpr MaskIcon kMaskIcons[] = {{"Button South Mask", kSouth}, {"Button East Mask", kEast}, {"Button North Mask", kNorth}, {"Button North Mask Filled", -1}};
inline constexpr std::size_t kMaskCount = std::size(kMaskIcons);

inline std::uint64_t Fnv64(const std::uint8_t* data, std::size_t size, std::uint64_t hash = 0xcbf29ce484222325) {
    for (std::size_t index = 0; index < size; ++index) hash = (hash ^ data[index]) * 0x100000001b3;
    return hash;
}

inline std::uint64_t Fnv64(const std::string& text) { return Fnv64(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

// TextMeshPro's case-sensitive name hash, which its sprite assets store for every character.
inline std::uint32_t TextMeshProHash(const std::string& text) {
    std::uint32_t hash = 0;
    for (const char character : text) hash = ((hash << 5) + hash) ^ static_cast<std::uint8_t>(character);
    return hash;
}

inline std::size_t Align(std::size_t value, std::size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

template <typename T> T Get(const Bytes& data, std::size_t at) {
    Require(at <= data.size() && sizeof(T) <= data.size() - at, "a read goes past the end of the data");
    T value{};
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
}

template <typename T> void Set(Bytes& data, std::size_t at, T value) {
    Require(at <= data.size() && sizeof(T) <= data.size() - at, "a write goes past the end of the data");
    std::memcpy(data.data() + at, &value, sizeof(value));
}

template <typename T> Bytes Raw(T value) {
    Bytes bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

inline std::uint64_t BigEndian(const Bytes& data, std::size_t at, std::size_t size) {
    Require(at <= data.size() && size <= data.size() - at, "a read goes past the end of the data");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) value = (value << 8) | data[at + index];
    return value;
}

inline void SetBigEndian(Bytes& data, std::size_t at, std::size_t size, std::uint64_t value) {
    for (std::size_t index = size; index-- > 0; value >>= 8) data[at + index] = static_cast<std::uint8_t>(value);
}

struct Writer {
    Bytes data;
    template <typename T> void Put(T value) {
        const Bytes raw = Raw(value);
        data.insert(data.end(), raw.begin(), raw.end());
    }
    void Floats(std::initializer_list<float> values) {
        for (const float value : values) Put(value);
    }
    void String(const std::string& text) {
        Put(static_cast<std::uint32_t>(text.size()));
        data.insert(data.end(), text.begin(), text.end());
        data.resize(Align(data.size(), 4));
    }
    void Pointer(std::int64_t path) {
        Put(std::int32_t{0});
        Put(path);
    }
};

inline Bytes StringBytes(const std::string& text) {
    Writer writer;
    writer.String(text);
    return writer.data;
}

struct Rect {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};

struct Cell {
    std::uint32_t x = 0, y = 0;
};

inline bool Overlaps(const Rect& a, float bx, float by, float bw, float bh) {
    return static_cast<float>(a.x) < bx + bw - 0.5f && bx < static_cast<float>(a.x + a.width) - 0.5f && static_cast<float>(a.y) < by + bh - 0.5f &&
           by < static_cast<float>(a.y + a.height) - 0.5f;
}

// PlayStation 4 1D thin tiling of 4-byte texels: 8x8 micro tiles in row order, each in Morton order.
inline std::size_t TexelOffset(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    std::uint32_t morton = 0;
    for (std::uint32_t bit = 0; bit < 3; ++bit) morton |= (((x >> bit) & 1u) << (2 * bit)) | (((y >> bit) & 1u) << (2 * bit + 1));
    return (std::size_t{(y >> 3) * (width >> 3) + (x >> 3)} * 64 + morton) * 4;
}

inline std::int64_t NewRenderKey(const std::string& name, const std::vector<std::int64_t>& used) {
    for (std::uint64_t salt = 0;; ++salt) {
        const auto candidate = static_cast<std::int64_t>(static_cast<std::int32_t>(Fnv64("xbox_prompt_icons/key/" + name + "/" + std::to_string(salt))));
        if (std::find(used.begin(), used.end(), candidate) == used.end()) return candidate;
    }
}

struct Edit {
    std::size_t begin, end;
    Bytes bytes;
};

// The object's bytes in [begin, end) with the edits (positions in the object) applied.
inline Bytes Splice(const Bytes& object, std::size_t begin, std::size_t end, std::vector<Edit> edits) {
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin || (a.begin == b.begin && a.end < b.end); });
    Bytes result;
    std::size_t at = begin;
    for (const Edit& edit : edits) {
        Require(edit.begin >= at && edit.end >= edit.begin && edit.end <= end, "overlapping edits");
        result.insert(result.end(), object.begin() + static_cast<std::ptrdiff_t>(at), object.begin() + static_cast<std::ptrdiff_t>(edit.begin));
        result.insert(result.end(), edit.bytes.begin(), edit.bytes.end());
        at = edit.end;
    }
    result.insert(result.end(), object.begin() + static_cast<std::ptrdiff_t>(at), object.begin() + static_cast<std::ptrdiff_t>(end));
    return result;
}

inline Bytes Splice(const Bytes& object, std::vector<Edit> edits) { return Splice(object, 0, object.size(), std::move(edits)); }

}  // namespace sos::icons

"""Static check of the game's button prompt assets in resources.assets (Unity 2022.3 serialized file, version 22).

    python scripts/prompt_assets_check.py [MEDIA_FOLDER]

The game's InputIconData holds one icon map per controller type (ps5, ps4, xboxSeries, pc) and four TMP sprite atlases.
Checks that the Xbox atlas and the Xbox icon map are populated, that every sprite resolves to a texture, that every sprite
of an icon map is a glyph of the matching atlas (the game finds a text glyph by the sprite's name), that the Xbox sheet's
pixels are in resources.assets.resS, and lists the special icons the Xbox map lacks. Nothing is executed; exit status 0 when
every check passes.
"""
import mmap
import struct
import sys
from pathlib import Path

DEFAULT_MEDIA = Path(__file__).resolve().parent.parent / "game" / "prepared" / "CUSA57767-1.00" / "Media"
CLASS_TEXTURE2D, CLASS_SPRITE, CLASS_MONOBEHAVIOUR, CLASS_MATERIAL = 28, 213, 114, 21
ATLASES = ("New_PS_ButtonPromptsAtlas", "PS4_ButtonPromptsAtlas", "Xbox_ButtonPromptsAtlas", "PC_ButtonPromptsAtlas")
XBOX_SPRITES = "ABXY_Controller_Sheet(384,256)"
failures = []


def check(ok, text):
    print(("  ok    " if ok else "  FAIL  ") + text)
    if not ok:
        failures.append(text)
    return ok


class Reader:
    def __init__(self, data, pos=0, little=False):
        self.data, self.pos, self.little = data, pos, little

    def _unpack(self, fmt, size):
        value = struct.unpack_from(("<" if self.little else ">") + fmt, self.data, self.pos)[0]
        self.pos += size
        return value

    def u8(self):
        value = self.data[self.pos]
        self.pos += 1
        return value

    def i16(self):
        return self._unpack("h", 2)

    def u32(self):
        return self._unpack("I", 4)

    def i32(self):
        return self._unpack("i", 4)

    def i64(self):
        return self._unpack("q", 8)

    def cstr(self):
        end = self.data.find(b"\0", self.pos)
        text = self.data[self.pos:end].decode("utf-8", "replace")
        self.pos = end + 1
        return text

    def align(self):
        self.pos = (self.pos + 3) & ~3


def parse(path):
    with open(path, "rb") as file:
        data = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
    reader = Reader(data)
    reader.u32()
    reader.u32()
    version = reader.u32()
    data_offset = reader.u32()
    endian = reader.u8()
    reader.pos += 3
    if version >= 22:
        reader.u32()
        reader.i64()
        data_offset = reader.i64()
        reader.i64()
    reader.little = endian == 0
    reader.cstr()
    reader.i32()
    has_type_tree = reader.u8() if version >= 13 else 0
    types = []
    for _ in range(reader.i32()):
        class_id = reader.i32()
        reader.u8()
        reader.i16()
        if class_id == CLASS_MONOBEHAVIOUR:
            reader.pos += 16
        reader.pos += 16
        if has_type_tree:
            nodes, strings = reader.i32(), reader.i32()
            reader.pos += nodes * 32 + strings
            reader.pos += 4 * reader.i32()
        types.append(class_id)
    objects = {}
    for _ in range(reader.i32()):
        reader.align()
        path_id = reader.i64()
        start = reader.i64()
        size = reader.u32()
        objects[path_id] = (data_offset + start, size, types[reader.i32()])
    return data, objects


def object_name(data, offset, size, class_id):
    base = offset + (28 if class_id == CLASS_MONOBEHAVIOUR else 0)
    length = struct.unpack_from("<I", data, base)[0]
    if size < 32 or length > 200 or base + 4 + length > offset + size:
        return None
    return bytes(data[base + 4:base + 4 + length]).decode("utf-8", "replace")


def pointers(data, objects, path_id, first=0x30):
    """Every 4-aligned PPtr<> to a local object in the object, with the offset it sits at."""
    offset, size, _ = objects[path_id]
    body = bytes(data[offset:offset + size])
    found = []
    for at in range(first, size - 11, 4):
        file_id, target = struct.unpack_from("<iq", body, at)
        if file_id == 0 and target in objects and target != path_id:
            found.append((at, target))
    return found, body


GAMEPAD_ICON_SLOTS = ("buttonSouth buttonNorth buttonEast buttonWest startButton selectButton leftTrigger rightTrigger "
                      "leftShoulder rightShoulder dpad dpadUp dpadDown dpadLeft dpadRight leftStick rightStick "
                      "leftStickPress rightStickPress touchPad").split()
ICON_MAPS = (("ps5", "New_PS_ButtonPromptsAtlas"), ("ps4", "PS4_ButtonPromptsAtlas"), ("xbox", "Xbox_ButtonPromptsAtlas"))


def sprite_name(data, objects, target):
    if target == 0 or target not in objects:
        return None
    offset, size, class_id = objects[target]
    return object_name(data, offset, size, class_id)


def icon_maps(data, objects, icon_data):
    """[(slots, specials)] of the ps5, ps4 and xboxSeries GamepadIcons: 20 sprite names, then the special icons by key."""
    offset, size, _ = objects[icon_data]
    body = bytes(data[offset:offset + size])
    at = 0x30
    maps = []
    for _ in ICON_MAPS:
        slots = []
        for _ in GAMEPAD_ICON_SLOTS:
            slots.append(sprite_name(data, objects, struct.unpack_from("<iq", body, at)[1]))
            at += 12
        count = struct.unpack_from("<i", body, at)[0]
        at += 4
        keys = []
        for _ in range(count):
            length = struct.unpack_from("<I", body, at)[0]
            keys.append(body[at + 4:at + 4 + length].decode())
            at = (at + 4 + length + 3) & ~3
        count = struct.unpack_from("<i", body, at)[0]
        at += 4
        values = []
        for _ in range(count):
            values.append(sprite_name(data, objects, struct.unpack_from("<iq", body, at)[1]))
            at += 12
        maps.append((slots, dict(zip(keys, values))))
    return maps


def atlas_glyph_names(data, objects, atlas):
    """The sprite names a TMP sprite asset knows, in table order (length-prefixed strings of its serialized form)."""
    offset, size, _ = objects[atlas]
    body = bytes(data[offset:offset + size])
    names, at = [], 0x30
    while at < size - 4:
        length = struct.unpack_from("<I", body, at)[0]
        text = body[at + 4:at + 4 + length]
        if 1 <= length <= 80 and at + 4 + length <= size and all(32 <= c < 127 for c in text):
            names.append(text.decode())
            at = (at + 4 + length + 3) & ~3
        else:
            at += 4
    return names


def texture_pixels(data, objects, texture, resources):
    """(width, height, format, bytes read from resources.assets.resS, how many are not zero) of a streamed Texture2D."""
    offset, size, _ = objects[texture]
    body = bytes(data[offset:offset + size])
    at = (4 + struct.unpack_from("<I", body, 0)[0] + 3) & ~3
    width, height = struct.unpack_from("<2i", body, at + 8)
    texture_format = struct.unpack_from("<i", body, at + 24)[0]
    path_at = body.rfind(b"resources.assets.resS") - 4
    stream_offset, stream_size = struct.unpack_from("<QI", body, path_at - 12)
    if not resources.is_file() or resources.stat().st_size < stream_offset + stream_size:
        return width, height, texture_format, 0, 0
    with open(resources, "rb") as file:
        file.seek(stream_offset)
        pixels = file.read(stream_size)
    return width, height, texture_format, len(pixels), sum(1 for byte in pixels if byte)


def main():
    media = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_MEDIA
    path = media / "resources.assets"
    if not path.is_file():
        print(f"  [skip] {path} is not installed: the prompt assets were not read")
        return 0
    data, objects = parse(path)
    names = {}
    for path_id, (offset, size, class_id) in objects.items():
        if class_id == CLASS_MONOBEHAVIOUR and size > 40:
            name = object_name(data, offset, size, class_id)
            if name in ATLASES or (name and name.endswith("InputIconData")):
                names[name] = path_id
    for atlas in ATLASES:
        check(atlas in names, f"{atlas} exists")
    icon_data = next((path_id for name, path_id in names.items() if name.endswith("InputIconData")), None)
    if not check(icon_data is not None, "the InputIconData object exists") or len(names) < 5:
        return 1

    refs, _ = pointers(data, objects, icon_data)
    atlas_refs = [target for _, target in refs if target in [names[a] for a in ATLASES]]
    check(atlas_refs == [names[a] for a in ATLASES], "InputIconData references the four atlases in ps5, ps4, xbox, pc order")

    xbox = names["Xbox_ButtonPromptsAtlas"]
    atlas_refs, _ = pointers(data, objects, xbox)
    kinds = {objects[target][2] for _, target in atlas_refs}
    check(CLASS_MATERIAL in kinds, "the Xbox atlas has a material")
    sheets = [target for _, target in atlas_refs if objects[target][2] == CLASS_TEXTURE2D]
    check(len(sheets) == 1 and object_name(data, *objects[sheets[0]][:2], CLASS_TEXTURE2D) == XBOX_SPRITES,
          "the Xbox atlas sprite sheet resolves to the Xbox Texture2D")

    sprites = []
    for at, target in refs:
        offset, size, class_id = objects[target]
        if class_id == CLASS_SPRITE:
            sprites.append((at, target, object_name(data, offset, size, class_id)))
    xbox_sprites = [item for item in sprites if item[2] and item[2].startswith(XBOX_SPRITES)]
    ps_sprites = [item for item in sprites if item[2] and item[2].startswith(("PS_ButtonPrompts", "PS4_ButtonPrompts"))]
    check(len(xbox_sprites) >= 19, f"the Xbox icon map holds {len(xbox_sprites)} sprites")
    unresolved = 0
    for _, target, name in xbox_sprites:
        found, _ = pointers(data, objects, target, first=4)
        if not any(objects[t][2] == CLASS_TEXTURE2D for _, t in found):
            unresolved += 1
    check(unresolved == 0, "every Xbox sprite resolves to a Texture2D")
    first, last = xbox_sprites[0][0], xbox_sprites[-1][0]
    _, body = pointers(data, objects, icon_data)
    window = [struct.unpack_from("<iq", body, at) for at in range(first, last + 12, 12)]
    nulls = sum(1 for file_id, target in window if file_id == 0 and target == 0)
    check(nulls == 0, "no null slot inside the Xbox icon map")
    maps = icon_maps(data, objects, icon_data)
    glyphs = {}
    for (key, atlas), (slots, specials) in zip(ICON_MAPS, maps):
        glyphs[key] = set(atlas_glyph_names(data, objects, names[atlas]))
        check(all(slots), f"the {key} icon map has all {len(slots)} button icons")
        missing = [slot for slot, sprite in zip(GAMEPAD_ICON_SLOTS, slots) if sprite not in glyphs[key]]
        check(not missing, f"every {key} button icon is a glyph of {atlas} (the text path finds glyphs by name)"
              + (f": missing {missing}" if missing else ""))
    sheet_name = XBOX_SPRITES
    sheet = sheets[0] if sheets else None
    if sheet is not None:
        width, height, texture_format, size, opaque = texture_pixels(data, objects, sheet, path.with_name("resources.assets.resS"))
        check(size > 0 and opaque > 0, f"the Xbox sheet {sheet_name} ({width}x{height}, format {texture_format}) has "
              f"{size} bytes of pixels in resources.assets.resS")
    lacking = sorted(set(maps[0][1]) - set(maps[2][1]))
    print(f"  info  the Xbox icon map has {len(maps[2][1])} special icon(s) ({', '.join(maps[2][1])}); the PlayStation 5 map also has "
          f"{', '.join(lacking)}. Those icons are not drawn in Xbox mode.")
    print(f"  info  Xbox icon map {len(xbox_sprites)} sprites, PlayStation maps {len(ps_sprites)} sprites")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

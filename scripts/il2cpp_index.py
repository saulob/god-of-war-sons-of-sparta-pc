"""Method and field index of a built boot image: IL2CPP metadata joined with the code registration of the image.

Used by the static checks (scripts/prompt_sites.py). Nothing is executed.

    from il2cpp_index import open_build
    index = open_build("1.00")            # None when the build's boot image or metadata is not installed
    index.method_address("InputOverseer", ".ctor")
"""
import bisect
import importlib.util
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EBOOT_SHA256 = {
    "1.00": "19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31",
    "1.01": "aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58",
}
IMAGE_FOLDERS = {"1.00": ("CUSA57767-1.00", "v1.00"), "1.01": ("CUSA57767-1.01", "v1.01")}
GAME_CODE_IMAGE = "Il2cppUserAssemblies.prx"


def _load_checker():
    spec = importlib.util.spec_from_file_location("il2cpp_check", ROOT / "scripts" / "il2cpp_check.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_check = _load_checker()


def boot_image_path(build):
    for folder in IMAGE_FOLDERS[build]:
        manifest = ROOT / "build" / "image" / folder / "image.json"
        boot = manifest.with_name("boot.bin")
        if manifest.is_file() and boot.is_file():
            if json.loads(manifest.read_text()).get("eboot_sha256") == EBOOT_SHA256[build]:
                return boot
    return None


def metadata_path(build):
    path = ROOT / "game" / "prepared" / f"CUSA57767-{build}" / "Media" / "Metadata" / "global-metadata.dat"
    return path if path.is_file() else None


class Index:
    def __init__(self, boot_path, metadata_file):
        self.meta = _check.Metadata(Path(metadata_file).read_bytes())
        self.boot = _check.Boot(boot_path)
        meta = self.meta
        self.images = meta.records("images", "<10i")
        self.types = meta.records("typeDefinitions", "<16i8H2I")
        self.methods = meta.records("methods", "<7i4H")
        self.fields = meta.records("fields", "<3i")
        self.image_names = [meta.string(i[0]) for i in self.images]
        self.image_of_type = {}
        for number, image in enumerate(self.images):
            for type_index in range(image[2], image[2] + image[3]):
                self.image_of_type[type_index] = number
        module = max(self.boot.modules, key=lambda m: m[1])
        self.module_base, self.module_size = module[0], module[1]
        registrations = _check.find_metadata_registration(self.boot, module, len(self.types))
        if len(registrations) != 1:
            raise ValueError("metadata registration not found: the metadata does not belong to this image")
        self.metadata_registration = registrations[0]
        self.code_modules = self._code_modules(module)
        self._starts = None
        self._names = None

    def _code_modules(self, module):
        boot = self.boot
        count = len(self.image_names)
        for offset in range(module[0], module[0] + module[1], 8):
            if boot.pointer(offset + 0x80) and boot.u32(offset + 0x78) == count and boot.pointer(offset + 8):
                table = boot.pointer(offset + 0x80)
                found = {}
                for i in range(count):
                    entry = boot.pointer(table + 8 * i)
                    name = entry and boot.pointer(entry)
                    if not name:
                        break
                    found[boot.cstring(name)] = (boot.u32(entry + 8), boot.pointer(entry + 16))
                if sorted(found) == sorted(self.image_names):
                    return found
        raise ValueError("code registration not found")

    def type_name(self, type_index):
        t = self.types[type_index]
        namespace = self.meta.string(t[1])
        return (namespace + "." if namespace else "") + self.meta.string(t[0])

    def type_indices(self, name, image="Assembly-CSharp.dll"):
        return [i for i in range(len(self.types)) if self.image_names[self.image_of_type[i]] == image
                and self.type_name(i) == name]

    def type_methods(self, type_index):
        t = self.types[type_index]
        for method_index in range(t[9], t[9] + t[16]):
            yield method_index, self.methods[method_index]

    def method_address(self, type_name, method_name, image="Assembly-CSharp.dll"):
        found = []
        for type_index in self.type_indices(type_name, image):
            for method_index, method in self.type_methods(type_index):
                if self.meta.string(method[0]) == method_name:
                    found.append(self._address(method_index))
        found = [a for a in found if a]
        if len(found) != 1:
            raise LookupError(f"{type_name}::{method_name}: {len(found)} matches")
        return found[0]

    def _address(self, method_index):
        method = self.methods[method_index]
        rid = method[6] & 0xFFFFFF
        count, pointers = self.code_modules[self.image_names[self.image_of_type[method[1]]]]
        if rid == 0 or rid > count:
            return None
        slot = pointers + 8 * (rid - 1)
        return self.boot.pointer(slot) or self.boot.u64(slot)

    def all_method_names(self):
        if self._names is None:
            names = {}
            for type_index in range(len(self.types)):
                for method_index, method in self.type_methods(type_index):
                    address = self._address(method_index)
                    if address:
                        names[address] = self.type_name(type_index) + "::" + self.meta.string(method[0])
            self._names = names
            self._starts = sorted(names)
        return self._names

    def method_bounds(self, address):
        """The bytes from a method's start to the next method's start."""
        self.all_method_names()
        at = bisect.bisect_right(self._starts, address)
        return address, self._starts[at] if at < len(self._starts) else address + 0x4000

    def method_at(self, address):
        names = self.all_method_names()
        return names[self._starts[bisect.bisect_right(self._starts, address) - 1]]

    def enum_values(self, type_name):
        """{field name: value} of an int enum, read from the metadata default values (compressed ints)."""
        meta = self.meta
        defaults = {r[0]: r for r in meta.records("fieldDefaultValues", "<3i")}
        blob = meta.section["fieldAndParameterDefaultValueData"][0]
        values = {}
        for type_index in self.type_indices(type_name):
            t = self.types[type_index]
            for field_index in range(t[8], t[8] + t[18]):
                record = defaults.get(field_index)
                if record and record[2] >= 0:
                    first = meta.data[blob + record[2]]
                    if first < 0x80:
                        values[meta.string(self.fields[field_index][0])] = first >> 1
        return values

    def field_offset(self, type_name, field_name):
        type_index = self.type_indices(type_name)[0]
        t = self.types[type_index]
        table = self.boot.pointer(self.metadata_registration + 0x58)
        base = self.boot.pointer(table + 8 * type_index)
        for k in range(t[18]):
            if self.meta.string(self.fields[t[8] + k][0]) == field_name:
                return self.boot.u32(base + 4 * k)
        raise LookupError(field_name)

    def game_code(self):
        """(start, bytes) covering the game's own methods."""
        names = self.all_method_names()
        image = self.image_names.index("Assembly-CSharp.dll")
        first = self.images[image]
        addresses = []
        for type_index in range(first[2], first[2] + first[3]):
            for method_index, _ in self.type_methods(type_index):
                address = self._address(method_index)
                if address:
                    addresses.append(address)
        low, high = min(addresses), max(addresses)
        return low, bytes(self.boot.read(low, high - low + 0x4000))


def open_build(build):
    boot, metadata = boot_image_path(build), metadata_path(build)
    if not boot or not metadata:
        return None
    return Index(boot, metadata)


def relative_calls(code, start, targets):
    """[(address, target)] of every call or jmp rel32 in `code` (loaded at `start`) to one of `targets`."""
    found = []
    for match in re.finditer(rb"[\xe8\xe9]", code):
        at = match.start()
        if at + 5 > len(code):
            break
        target = start + at + 5 + struct.unpack_from("<i", code, at + 1)[0]
        if target in targets:
            found.append((start + at, target))
    return found

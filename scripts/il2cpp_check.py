"""Static check of an IL2CPP global-metadata.dat and, optionally, of the registration structures in a boot image.

    python scripts/il2cpp_check.py METADATA [BOOT_IMAGE]

Exit status 0 when every check passes. Nothing is executed.
"""
import mmap
import re
import struct
import sys
from pathlib import Path

SANITY = 0xFAB11BAF
SECTIONS = ('stringLiteral stringLiteralData string events properties methods parameterDefaultValues fieldDefaultValues '
            'fieldAndParameterDefaultValueData fieldMarshaledSizes parameters fields genericParameters '
            'genericParameterConstraints genericContainers nestedTypes interfaces vtableMethods interfaceOffsets '
            'typeDefinitions images assemblies fieldRefs referencedAssemblies attributeData attributeDataRange '
            'unresolvedIndirectCallParameterTypes unresolvedIndirectCallParameterRanges windowsRuntimeTypeNames '
            'windowsRuntimeStrings exportedTypeDefinitions').split()
RECORD = dict(events=24, properties=20, methods=36, fields=12, nestedTypes=4, interfaces=4, vtableMethods=4,
              interfaceOffsets=8, typeDefinitions=88, images=40, assemblies=64)
CORE_CLASSES = ('Object Void Boolean Byte SByte Int16 UInt16 Int32 UInt32 Int64 UInt64 IntPtr UIntPtr Single Double Char '
                'String Array Exception Type Enum Delegate MulticastDelegate ValueType Attribute').split()
failures = []


def check(ok, text):
    print(('  ok    ' if ok else '  FAIL  ') + text)
    if not ok:
        failures.append(text)
    return ok


class Metadata:
    def __init__(self, data):
        self.data = data
        sanity, self.version = struct.unpack_from('<Ii', data, 0)
        self.sanity = sanity
        values = struct.unpack_from(f'<{2 * len(SECTIONS)}i', data, 8)
        self.section = {name: (values[2 * i], values[2 * i + 1]) for i, name in enumerate(SECTIONS)}
        self.strings = self.section['string']

    def string(self, index):
        start, size = self.strings
        if not 0 <= index < size:
            return None
        end = self.data.find(b'\0', start + index, start + size)
        return None if end < 0 else self.data[start + index:end].decode('utf-8')

    def records(self, name, fmt):
        offset, size = self.section[name]
        step = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, self.data, offset + i * step) for i in range(size // step)]


def check_metadata(meta):
    print('metadata header')
    check(meta.sanity == SANITY, f'sanity 0x{meta.sanity:08X}')
    check(meta.version == 31, f'version {meta.version}')
    end = 8 + 8 * len(SECTIONS)
    contiguous = True
    for name in SECTIONS:
        offset, size = meta.section[name]
        contiguous &= 0 <= offset - end < 8 and size >= 0
        end = offset + size
    check(contiguous and end == len(meta.data), f'{len(SECTIONS)} sections follow each other (alignment padding only) and end at the file end ({len(meta.data)} bytes)')
    for name, record in RECORD.items():
        check(meta.section[name][1] % record == 0, f'{name} size is a multiple of {record}')

    images = meta.records('images', '<10i')
    types = meta.records('typeDefinitions', '<16i8H2I')
    next_type = 0
    for image in images:
        if image[2] != next_type:
            break
        next_type += image[3]
    check(next_type == len(types), f'{len(images)} images cover {len(types)} type definitions contiguously')
    limits = dict(fields=len(meta.records('fields', '<3i')), methods=meta.section['methods'][1] // 36,
                  events=meta.section['events'][1] // 24, properties=meta.section['properties'][1] // 20,
                  nested=meta.section['nestedTypes'][1] // 4, interfaces=meta.section['interfaces'][1] // 4,
                  vtable=meta.section['vtableMethods'][1] // 4, offsets=meta.section['interfaceOffsets'][1] // 8)
    bad = 0
    for t in types:
        ranges = ((t[8], t[18], 'fields'), (t[9], t[16], 'methods'), (t[10], t[19], 'events'), (t[11], t[17], 'properties'),
                  (t[12], t[20], 'nested'), (t[13], t[22], 'interfaces'), (t[14], t[21], 'vtable'), (t[15], t[23], 'offsets'))
        bad += any(count and not 0 <= start <= start + count <= limits[kind] for start, count, kind in ranges)
        bad += meta.string(t[0]) is None or meta.string(t[1]) is None
    check(bad == 0, 'every type definition has in-bounds member ranges and valid name/namespace strings')

    mscorlib = next((i for i in images if meta.string(i[0]) == 'mscorlib.dll'), None)
    if not check(mscorlib is not None, 'mscorlib.dll image present'):
        return None
    start, count = mscorlib[2], mscorlib[3]
    check(count > 0, f'mscorlib type count {count}')
    present = {meta.string(t[0]) for t in types[start:start + count] if meta.string(t[1]) == 'System'}
    missing = [n for n in CORE_CLASSES if n not in present]
    check(not missing, 'mscorlib has every core System class' + (f' (missing {missing})' if missing else ''))
    return [meta.string(i[0]) for i in images], len(types)


class Boot:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if self.map[:8] != b'SOSBOOT1':
            raise ValueError('not a boot image')
        (self.size, self.entry, segments, relocs, names, _env, _pp, *_tls, modules) = struct.unpack_from('<12Q', self.map, 8)
        at = 8 + 96
        self.modules = [struct.unpack_from('<7Q', self.map, at + 56 * i) for i in range(modules)]
        at += 56 * modules
        bindings, = struct.unpack_from('<Q', self.map, at)
        at += 8 + 24 * bindings + 24 * segments + 128 * names
        self.relocs = {}
        for i in range(relocs):
            target, kind, value, _ = struct.unpack_from('<4Q', self.map, at + 32 * i)
            if kind == 0:
                self.relocs[target] = value
        self.image = at + 32 * relocs

    def read(self, address, size):
        return self.map[self.image + address:self.image + address + size]

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]

    def u64(self, address):
        return struct.unpack('<Q', self.read(address, 8))[0]

    def pointer(self, address):
        return self.relocs.get(address)

    def cstring(self, address):
        return self.read(address, 128).split(b'\0')[0].decode()


def find_metadata_registration(boot, module, type_count):
    base, size = module[0], module[1]
    needle = struct.pack('<I', type_count)
    found = []
    at = base
    window = boot.map
    while True:
        hit = window.find(needle, boot.image + at, boot.image + base + size)
        if hit < 0:
            break
        at = hit - boot.image + 1
        offset = hit - boot.image - 0x50
        if offset % 8 == 0 and offset >= base and boot.u32(offset + 0x60) == type_count and \
                all(boot.pointer(offset + k) for k in (0x08, 0x18, 0x28, 0x38, 0x48, 0x58, 0x68)):
            found.append(offset)
    return found


def check_registration(boot, image_names, type_count):
    print('registration structures')
    module = max(boot.modules, key=lambda m: m[1])
    found = find_metadata_registration(boot, module, type_count)
    if not check(len(found) == 1, f'MetadataRegistration located by type-definition count ({[hex(a) for a in found]})'):
        return
    meta_reg = found[0]
    counts = {name: boot.u32(meta_reg + off) for name, off in
              (('genericClasses', 0), ('genericInsts', 0x10), ('genericMethodTable', 0x20), ('types', 0x30),
               ('methodSpecs', 0x40), ('fieldOffsets', 0x50), ('typeDefinitionsSizes', 0x60))}
    print('        ' + ', '.join(f'{k}={v}' for k, v in counts.items()))
    check(all(counts[k] for k in ('types', 'methodSpecs', 'genericClasses')), 'MetadataRegistration tables are populated')
    check(counts['fieldOffsets'] == counts['typeDefinitionsSizes'] == type_count,
          'field offsets and type sizes cover every metadata type definition')

    code_regs = []
    for off in range(module[0], module[0] + module[1], 8):
        if boot.pointer(off + 0x80) and boot.u32(off + 0x78) == len(image_names) and boot.pointer(off + 0x8):
            code_regs.append(off)
            if len(code_regs) > 4:
                break
    code_reg = None
    for candidate in code_regs:
        modules_array = boot.pointer(candidate + 0x80)
        names = []
        for i in range(len(image_names)):
            entry = boot.pointer(modules_array + 8 * i)
            name = entry and boot.pointer(entry)
            names.append(boot.cstring(name) if name else None)
        if sorted(n or '' for n in names) == sorted(image_names):
            code_reg = candidate
            break
    if not check(code_reg is not None, f'CodeRegistration located, {len(image_names)} code-gen modules match the metadata images'):
        return
    print(f'        CodeRegistration 0x{code_reg:x}, MetadataRegistration 0x{meta_reg:x}')
    pointers = [off for off in range(0, 0x80, 8) if boot.u64(code_reg + off) > 0xffff and off not in (0x78,)]
    check(all(boot.pointer(code_reg + off) or boot.u64(code_reg + off) < 0x100000000 for off in pointers),
          'CodeRegistration pointer fields are relocated')

    pattern = re.compile(rb'\x48\x8d\x0d....\x48\x8d\x05....\x48\x89\x0d', re.S)
    function = None
    code = boot.read(module[0], module[1])
    for match in pattern.finditer(code):
        at = module[0] + match.start()
        rcx = at + 7 + struct.unpack_from('<i', code, match.start() + 3)[0]
        rax = at + 14 + struct.unpack_from('<i', code, match.start() + 10)[0]
        if rcx == meta_reg and rax == code_reg:
            function = at
            break
    if not check(function is not None, 'registration function stores both structures'):
        return
    slots = [t for t, v in boot.relocs.items() if v == function]
    check(len(slots) == 1, f'registration function 0x{function:x} is reachable through a relocated pointer ({[hex(s) for s in slots]})')


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    meta = Metadata(Path(argv[1]).read_bytes())
    summary = check_metadata(meta)
    if len(argv) == 3 and summary:
        check_registration(Boot(argv[2]), *summary)
    print('FAILED' if failures else 'PASSED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

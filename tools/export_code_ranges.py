"""Writes each module's executable bytes (NAME.code) and function ranges from its .eh_frame_hdr (NAME.fn) for isa_tests --scan.

usage: export_code_ranges.py <prepared game directory> <output directory>
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import ps4_module  # noqa: E402

PT_GNU_EH_FRAME = 0x6474e550


def functions(elf, ph, image):
    """(begin, end) of every FDE in the module's .eh_frame_hdr table (pc-relative sdata4 FDE entries)."""
    header = next((p for p in ph if p['type'] == PT_GNU_EH_FRAME), None)
    if not header:
        return []
    base = header['vaddr']
    version, frame_enc, count_enc, table_enc = image[base:base + 4]
    if (version, frame_enc, count_enc, table_enc) != (1, 0x1b, 0x03, 0x3b):
        raise ValueError('unsupported .eh_frame_hdr encoding')
    count = struct.unpack_from('<I', image, base + 8)[0]
    result = []
    for i in range(count):
        _, fde_offset = struct.unpack_from('<ii', image, base + 12 + 8 * i)
        fde = base + fde_offset
        begin = fde + 8 + struct.unpack_from('<i', image, fde + 8)[0]
        size = struct.unpack_from('<i', image, fde + 12)[0]
        if size > 0:
            result.append((begin, begin + size))
    return sorted(result)


def main():
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for path in [root / 'eboot.bin'] + sorted(root.rglob('*.prx')):
        module = ps4_module.read_module(path)
        elf = module['elf']
        executable = [p for p in module['ph'] if p['type'] == ps4_module.PT_LOAD and p['flags'] & 1]
        loads = [p for p in module['ph'] if p['type'] == ps4_module.PT_LOAD]
        image = bytearray(max(p['vaddr'] + p['filesz'] for p in loads))
        for p in loads:
            image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
        end = max(p['vaddr'] + p['filesz'] for p in executable)
        ranges = [(b, e) for b, e in functions(elf, module['ph'], image) if e <= end]
        (out / (path.name + '.code')).write_bytes(bytes(image[:end]))
        (out / (path.name + '.fn')).write_text(''.join('%x %x\n' % r for r in ranges))
        print('%-40s %9d bytes, %7d functions' % (path.name, end, len(ranges)))


if __name__ == '__main__':
    main()

"""Cuts a small raw ATRAC9 fixture for tools/ajm_tests.c out of an installed FMOD bank (FSB5, codec 13).

    python scripts/at9_fixture.py OUTPUT [BANK_FOLDER]

OUTPUT holds the 4-byte ATRAC9 config word followed by the first stream's superframes. Nothing from the game is added
to the repository: the fixture is written wherever OUTPUT points and the test reads it through AJM_TEST_AT9.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIMIT = 32 * 1024


def first_at9_stream(data):
    at = data.find(b'FSB5')
    while at >= 0:
        version, count, sample_headers, names, data_size, mode = struct.unpack_from('<6I', data, at + 4)
        if mode == 13 and count:
            flags, = struct.unpack_from('<Q', data, at + 60)
            offset = ((flags >> 6) & 0xfffffff) * 16
            pos, more, config = at + 68, flags & 1, None
            while more:
                header, = struct.unpack_from('<I', data, pos)
                size, kind, more = header & 0xffffff, header >> 25, (header >> 24) & 1
                if kind == 9 and size >= 8:
                    config = data[pos + 8:pos + 12]
                pos += 4 + size
            if config:
                start = at + 60 + sample_headers + names + offset
                return config, data[start:start + LIMIT]
        at = data.find(b'FSB5', at + 4)
    return None


def main(argv):
    folder = Path(argv[2]) if len(argv) > 2 else ROOT / 'game' / 'extracted'
    for bank in sorted(folder.rglob('*.streams.bank')):
        found = first_at9_stream(bank.read_bytes())
        if found:
            Path(argv[1]).write_bytes(found[0] + found[1])
            return 0
    print('no ATRAC9 stream found', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv) if len(sys.argv) > 1 else 2)

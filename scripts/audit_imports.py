"""Static import audit of a boot image: resolved/unresolved counts and what each resolved import is bound to.

    python scripts/audit_imports.py build/image/CUSA57767-1.00/image.json [--list stub|fatal|unresolved]

Bindings are read from the runtime's export tables ({"symbol", function}) and the one-line bodies of the functions they
name: a body that only returns 0 is a "fake success" stub, one that only returns a constant error is "unavailable", one
that calls runtime_exit is "fatal". Nothing is executed.
"""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / 'src'
TABLE_ENTRY = re.compile(r'\{\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}')
NID_ENTRY = re.compile(r'\{\s*"([A-Za-z0-9+\-]{11}#[A-Za-z0-9_]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}')
FUNCTION = re.compile(r'static\s+(?:ABI\s+)?(?:__attribute__\(\([a-z_]+\)\)\s+)?[\w\s\*]+?\b([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{([^{}]*)\}', re.S)
NAMES = re.compile(r'\{"([^"]+)","([^"]+)"\}')


def load_names():
    names = {}
    for line in (SRC / 'import_names.inc').read_text(errors='replace').splitlines():
        m = NAMES.match(line.strip())
        if m:
            names[m.group(1)] = m.group(2)
    return names


def classify_body(body):
    text = ' '.join(body.split())
    if 'runtime_exit' in text:
        return 'fatal'
    if re.fullmatch(r'(\(void\)\s*\w+;\s*)*return\s+(0|NULL|0u|0ll)\s*;', text):
        return 'fake-success'
    if re.fullmatch(r'(\(void\)\s*\w+;\s*)*return\s+(ERR\(\w+\)|-?\d+|\(?\w*ERR\w*\)?)\s*;', text):
        return 'unavailable'
    return 'implemented'


def load_bindings():
    functions = {}
    exports = defaultdict(set)
    for path in sorted(SRC.glob('*.c')) + sorted(SRC.glob('*.cpp')):
        text = path.read_text(errors='replace')
        for m in FUNCTION.finditer(text):
            functions[m.group(1)] = classify_body(m.group(3))
        for m in TABLE_ENTRY.finditer(text):
            if '#' not in m.group(1):
                exports[m.group(1)].add(m.group(2))
        for m in NID_ENTRY.finditer(text):
            exports[m.group(1)].add(m.group(2))
    return functions, exports


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    image = json.loads(Path(argv[1]).read_text())
    names = load_names()
    functions, exports = load_bindings()
    imports = image['import_list']
    unresolved = set(image['unresolved_imports'])
    counts = Counter()
    listing = defaultdict(list)
    for nid in imports:
        symbol = names.get(nid, nid)
        bound = exports.get(symbol) or exports.get(nid)
        if not bound:
            kind = 'unresolved-at-boot' if nid in unresolved else 'bound-elsewhere'
        else:
            kinds = {functions.get(f, 'implemented') for f in bound}
            kind = next((k for k in ('fatal', 'fake-success', 'unavailable') if k in kinds), 'implemented')
        counts[kind] += 1
        listing[kind].append(symbol)
    print(f"{argv[1]}: {len(imports)} imports")
    for kind, n in sorted(counts.items()):
        print(f"  {kind:20s} {n}")
    if '--list' in argv:
        wanted = argv[argv.index('--list') + 1]
        for kind, symbols in listing.items():
            if wanted == kind or (wanted == 'stub' and kind in ('fake-success', 'unavailable')):
                print(f"{kind}:")
                for symbol in sorted(symbols):
                    print(f"  {symbol}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

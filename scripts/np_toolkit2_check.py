"""Checks the UnityNpToolkit2.prx imports of every prepared build against src/np_toolkit2_table.inc.

Every libSceNpToolkit2 import must be listed in the table, and every import PrxInitialize/PrxUpdate reach must be
bound. The libSceNpManager/libSceUserService imports on that path must have a runtime binding by name.
"""
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_evidence as evidence  # noqa: E402
import ps4_module as pm  # noqa: E402

LIBRARY = 'libSceNpToolkit2'
STARTUP_EXTERNAL = {'A2CQ3kgSopQ': 'sceNpSetContentRestriction', 'CdWp0oHWGr0': 'sceUserServiceGetInitialUser'}


def parse_table():
    bound, unbound, startup = {}, {}, set()
    text = (ROOT / 'src' / 'np_toolkit2_table.inc').read_text()
    for m in re.finditer(r'TK2_(\w+)\(([^)]*)\)', text):
        kind, args = m.group(1), m.group(2)
        nid = re.search(r'"([^"]+)"', args).group(1)
        if kind == 'STARTUP':
            startup.add(nid)
        elif kind == 'UNBOUND':
            unbound[nid] = kind
        else:
            bound[nid] = kind
    return bound, unbound, startup


def runtime_symbols():
    names = set()
    for path in glob.glob(str(ROOT / 'src' / '*.c')):
        names.update(re.findall(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*[A-Za-z_]\w*\s*\}', Path(path).read_text(errors='replace')))
    return names


def main():
    bound, unbound, startup = parse_table()
    symbols = runtime_symbols()
    plugins = {}
    for version in evidence.versions():
        evidence.report_content(version, 'its UnityNpToolkit2.prx')
        path = evidence.game_file(version, 'Media/Plugins/UnityNpToolkit2.prx')
        if path:
            plugins[evidence.key_of(version)] = path
    failures = 0
    for label, path in plugins.items():
        module = pm.read_module(path)
        imports = {s['identity'][0] for s in module['symbols']
                   if s['identity'] and s['section'] == 0 and s['identity'][1][0] == LIBRARY}
        external = {s['identity'][0] for s in module['symbols'] if s['identity'] and s['section'] == 0 and s['identity'][1][0] != LIBRARY}
        missing = sorted(imports - set(bound) - set(unbound))
        stale = sorted((set(bound) | set(unbound)) - imports)
        startup_unbound = sorted(n for n in startup if n not in bound)
        print(f'  {label}: {len(imports)} NpToolkit2 imports, {len(imports & set(bound))} bound, '
              f'{len(imports & set(unbound))} left unresolved on purpose')
        for ok, text in ((not missing, f'every import is in the table (missing: {missing[:5]})'),
                         (not stale, f'the table lists no import the plugin lacks (stale: {stale[:5]})'),
                         (startup and not startup_unbound, f'the PrxInitialize/PrxUpdate imports are bound (unbound: {startup_unbound})')):
            print(f'  [{"ok  " if ok else "fail"}] {label}: {text}')
            failures += 0 if ok else 1
        for nid, symbol in STARTUP_EXTERNAL.items():
            ok = nid in external and symbol in symbols and pm.nid(symbol) == nid
            print(f'  [{"ok  " if ok else "fail"}] {label}: startup import {symbol} is bound by the runtime')
            failures += 0 if ok else 1
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())

"""Per-title facts for the games this port knows, keyed by TITLE_ID in sce_sys/param.sfo.

A version's layers are its packages, the base first and then the updates applied over it; the
last layer's param.sfo is the finished game's.
"""
from pathlib import Path

from ps4_module import parse_sfo

PROFILES = {
    'CUSA57767': {
        'name': 'God of War: Sons of Sparta',
        'region': 'USA',
        'content_id': 'UP9000-CUSA57767_00-SONSOFSPARTAPS40',
        'versions': {
            '1.00': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},),
                'eboot_sha256': '19e1759adbb8038164bc9c4c40af876ca68fea7c3442034d2b665bbdb9be4a31',
                'folder': 'CUSA57767-1.00',
                'regional_archive': None,
                'extra_files': ('Media/globalgamemanagers.assets',
                                'Media/Resources/unity default resources',
                                'Media/StreamingAssets/aa/catalog.json'),
            },
            '1.01': {
                'layers': ({'app_ver': '01.00', 'sfo_version': '01.00', 'update': False},
                           {'app_ver': '01.01', 'sfo_version': '01.00', 'update': True}),
                'eboot_sha256': 'aafe8a2ab8e55e0044fe371248749026c494a07d6864fbb6a7fa89fe39999d58',
                'folder': 'CUSA57767-1.01',
                'regional_archive': None,
                'extra_files': ('sce_discmap_patch.plt', 'Media/globalgamemanagers.assets',
                                'Media/Resources/unity default resources',
                                'Media/StreamingAssets/aa/catalog.json'),
            },
        },
        'entry': 'eboot.bin',
        # The game's own modules linked after the eboot, in init order: libc first, then its
        # libSceFios2 dependency, then the Unity IL2CPP graph (PS4Util before Il2cppUserAssemblies,
        # which imports it). Bare names are under sce_module/.
        'extra_executables': (),
        'modules': ('libc.prx', 'libSceFios2.prx',
                    'Media/Modules/PS4Util.prx', 'Media/Modules/Il2cppUserAssemblies.prx'),
        # What the loader needs to reach game initialization; the Unity player also needs
        # Media/globalgamemanagers* and the Media data tree at boot, unpacked with --scope full.
        'core_patterns': ('eboot.bin', 'sce_module/*', 'sce_sys/*', '*.plt',
                          'Media/Modules/*', 'Media/Plugins/*', 'Media/Metadata/*',
                          'Media/boot.config', 'Media/ScriptingAssemblies.json',
                          'Media/RuntimeInitializeOnLoads.json', 'Media/globalgamemanagers'),
    },
}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        return parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, KeyError):
        return None


def profile(game_or_title):
    """The profile for a TITLE_ID or a game folder, or None for an unknown title."""
    key = str(game_or_title)
    if key not in PROFILES:
        key = title_id(game_or_title) or ''
    return PROFILES.get(key)


def finished_layer(build):
    """The layer whose param.sfo the installed game has."""
    return build['layers'][-1]


def layer_identity(title, layer):
    """(TITLE_ID, APP_VER, VERSION, is an update) of the package a layer is."""
    return title, layer['app_ver'], layer['sfo_version'], layer['update']


def package_identity(info):
    """The identity of a package, as layer_identity() names it, or None when it is not a package of a supported title."""
    sfo = info['param_sfo']
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found or sfo.get('CONTENT_ID') != found['content_id']:
        return None
    return sfo['TITLE_ID'], sfo.get('APP_VER'), sfo.get('VERSION'), sfo.get('CATEGORY') == 'gp'


def finishing_version(title, identity):
    """The version of `title` whose finished game is the package with this identity, or None."""
    for version, build in PROFILES[title]['versions'].items():
        if layer_identity(title, finished_layer(build)) == identity:
            return version
    return None


def version_of(game):
    """The profile version whose finished param.sfo matches the game folder, or None."""
    try:
        sfo = parse_sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError):
        return None
    found = PROFILES.get(sfo.get('TITLE_ID', ''))
    if not found:
        return None
    return next((v for v, build in found['versions'].items()
                 if (finished_layer(build)['app_ver'], finished_layer(build)['sfo_version'])
                 == (sfo.get('APP_VER'), sfo.get('VERSION'))), None)


def package_layer(info):
    """(TITLE_ID, profile version, layer index) of the package of a supported build's finished game, or None.

    `info` is what scripts/pkg_info.py reads. The index is the package's place in the build's layers.
    """
    identity = package_identity(info)
    if identity is None:
        return None
    version = finishing_version(identity[0], identity)
    if version is None:
        return None
    return identity[0], version, len(PROFILES[identity[0]]['versions'][version]['layers']) - 1

"""Where the local evidence of a supported build is read from, and how a gate reports it.

A supported build is a contract of the port, checked on every run. What is found here is extra real data, only read: the launcher's
boot image (build/image), a research copy of a build the launcher has not prepared (build/research/image), the prepared game folder
and the extracted package layers. A build without any of it is reported as NO DATA, never as a failure.

    EVIDENCE|<image, content or contract>|CUSA57767-1.01|<state>|<detail>

SOS_EVIDENCE_ROOT replaces the project folder as the place of build/ and game/, for the tests of this module.
"""
import hashlib
import json
import os
import sys
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SOURCE_ROOT / "scripts"))
from game_profiles import PROFILES  # noqa: E402

TITLE = "CUSA57767"
REAL_IMAGE, RESEARCH_IMAGE, NO_DATA = "REAL IMAGE", "RESEARCH IMAGE", "NO DATA"
AVAILABLE, LAYERS, NOT_INSTALLED, SYNTHETIC = "AVAILABLE", "EXTRACTED LAYERS", "NOT INSTALLED", "SYNTHETIC CONTRACT"


def data_root():
    return Path(os.environ.get("SOS_EVIDENCE_ROOT") or SOURCE_ROOT)


def versions():
    return sorted(PROFILES[TITLE]["versions"])


def key_of(version):
    return f"{TITLE}-{version}"


def eboot_sha256(version):
    return PROFILES[TITLE]["versions"][version]["eboot_sha256"]


def _folders(version):
    return (PROFILES[TITLE]["versions"][version]["folder"], f"v{version}")


def _layer_versions(version):
    layers = PROFILES[TITLE]["versions"][version]["layers"]
    found = [str(int(layer["app_ver"].split(".")[0])) + "." + layer["app_ver"].split(".")[1] for layer in layers]
    return found[::-1]


def boot_image(version):
    """(boot.bin, REAL_IMAGE or RESEARCH_IMAGE) of the build, or (None, NO_DATA). The image must be of this build's eboot."""
    root = data_root() / "build"
    for state, base in ((REAL_IMAGE, root / "image"), (RESEARCH_IMAGE, root / "research" / "image")):
        for folder in _folders(version):
            manifest = base / folder / "image.json"
            boot = manifest.with_name("boot.bin")
            if not (manifest.is_file() and boot.is_file()):
                continue
            try:
                found = json.loads(manifest.read_text()).get("eboot_sha256")
            except (OSError, ValueError):
                continue
            if found == eboot_sha256(version):
                return boot, state
    return None, NO_DATA


def prepared_folder(version):
    for folder in _folders(version):
        path = data_root() / "game" / "prepared" / folder
        if (path / "eboot.bin").is_file():
            return path
    return None


def prepared_state(version):
    return AVAILABLE if prepared_folder(version) else NOT_INSTALLED


def _roots(version):
    roots = []
    prepared = prepared_folder(version)
    if prepared:
        roots.append(prepared)
    for layer in _layer_versions(version):
        for folder in (PROFILES[TITLE]["versions"][layer]["folder"], f"v{layer}"):
            path = data_root() / "game" / "extracted" / folder
            if path.is_dir():
                roots.append(path)
    return roots


def roots(version):
    return _roots(version)


def game_file(version, relative):
    """The file of the build, first in its prepared folder and then in its extracted package layers (update before base)."""
    for root in _roots(version):
        path = root / relative
        if path.is_file():
            return path
    return None


class LayeredPath:
    """A path inside the game folder of a build; it resolves to the first existing file of the prepared folder and the layers."""

    def __init__(self, version, relative=""):
        self.version, self.relative = version, relative

    def __truediv__(self, name):
        return LayeredPath(self.version, f"{self.relative}/{name}" if self.relative else str(name))

    def resolved(self):
        roots = _roots(self.version)
        for root in roots:
            path = root / self.relative
            if path.exists():
                return path
        return (roots[0] if roots else data_root() / "game" / "prepared" / _folders(self.version)[0]) / self.relative

    def __fspath__(self):
        return str(self.resolved())

    def glob(self, pattern):
        found = {}
        for root in _roots(self.version):
            for path in sorted((root / self.relative).glob(pattern)):
                found.setdefault(path.name, path)
        return [found[name] for name in sorted(found)]

    def __getattr__(self, name):
        return getattr(self.resolved(), name)

    def __str__(self):
        return str(self.resolved())


def media(version):
    return LayeredPath(version, "Media")


def eboot(version):
    path = game_file(version, "eboot.bin")
    if path and hashlib.sha256(path.read_bytes()).hexdigest() == eboot_sha256(version):
        return path
    return None


def report(kind, version, state, detail):
    print(f"EVIDENCE|{kind}|{key_of(version)}|{state}|{detail}")


def report_image(version, what):
    """Prints the image evidence of a build and returns (boot.bin, state)."""
    boot, state = boot_image(version)
    if boot:
        report("image", version, state, boot.relative_to(data_root()).as_posix())
    else:
        report("image", version, NO_DATA, f"no boot image in build/image or build/research/image; not read from the real code: {what}")
    return boot, state


def content_state(version):
    if prepared_state(version) == AVAILABLE:
        return AVAILABLE
    return LAYERS if _roots(version) else NOT_INSTALLED


def report_content(version, what, read=True):
    """The game files a group read for a build: the prepared folder, the extracted layers, or none (`read` False: not all it needs)."""
    state = content_state(version) if read else NOT_INSTALLED
    detail = {AVAILABLE: "game/prepared", LAYERS: "game/extracted, no prepared game folder"}.get(state, f"not read: {what}")
    report("content", version, state, detail)
    return state


def report_content_all(what, read=True):
    for version in versions():
        report_content(version, what, read)


def report_contract(version, what):
    report("contract", version, SYNTHETIC, what)

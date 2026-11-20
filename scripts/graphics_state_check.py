"""Pixel input state invariants of the video core, and the extraction of the shaders embedded in a build's eboot.

    python scripts/graphics_state_check.py                    check the fragment input path in the sources
    python scripts/graphics_state_check.py --self-test        prove that each invariant fails on a mutated copy of the sources
    python scripts/graphics_state_check.py --dump VERSION DIR write the eboot's embedded shaders of a build to DIR

The guest's own Gnm code writes SPI_PS_INPUT_CNTL; the host reads those registers when a pipeline is built. A stale or
partial input table makes a draw interpolate the wrong attribute without an error, so the path must stay deterministic:
the runtime info of a stage is cleared before it is filled, the fragment inputs are filled from the registers for exactly
NUM_INTERP entries, nothing else writes them, and every consumer is bounded by the same count.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

SOURCES = {
    "cache": "gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp",
    "info": "gpu/shadps4/shader_recompiler/runtime_info.h",
    "regs": "gpu/shadps4/video_core/amdgpu/regs.h",
    "shader_regs": "gpu/shadps4/video_core/amdgpu/regs_shader.h",
    "emit": "gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp",
    "quad": "gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp",
    "interp": "gpu/shadps4/shader_recompiler/frontend/translate/vector_interpolation.cpp",
}
FIELD_MAP = (("param_index", "input_offset"), ("is_default", "use_default"), ("is_flat", "flat_shade"), ("default_value", "default_value"))


def read_sources():
    return {name: (ROOT / path).read_text(encoding="utf-8", errors="replace") for name, path in SOURCES.items()}


def function_body(text, signature):
    start = text.find(signature)
    if start < 0:
        return ""
    brace = text.index("{", start)
    depth = 0
    for at in range(brace, len(text)):
        depth += (text[at] == "{") - (text[at] == "}")
        if depth == 0:
            return text[brace:at + 1]
    return ""


def checks(sources):
    """[(description, ok)] for the fragment input path."""
    cache, info, regs, shader_regs = (sources[k] for k in ("cache", "info", "regs", "shader_regs"))
    build = function_body(cache, "PipelineCache::BuildRuntimeInfo(")
    initialize = function_body(info, "void Initialize(HwStage stage")
    loop = re.search(r"for \(u32 i = 0; i < regs\.num_interp; i\+\+\) \{(.*?)\n        \}", build, re.S)
    loop_body = loop.group(1) if loop else ""
    writers = len(re.findall(r"hw\.fs\.inputs\[", build)) + len(re.findall(r"hw\.fs\.inputs\[", cache.replace(build, "")))
    results = [
        ("BuildRuntimeInfo clears the stage's runtime info before the stage switch",
         0 <= build.find("info.Initialize(stage, l_stage);") < build.find("switch (stage)")),
        ("RuntimeInfo::Initialize clears the whole object", "memset(this, 0, sizeof(*this))" in initialize),
        ("the fragment input count is NUM_INTERP", build.count("info.hw.fs.num_inputs = regs.num_interp;") == 1),
        ("the fragment inputs are filled for exactly NUM_INTERP entries from the registers", bool(loop_body) and all(
            re.search(rf"\.{field} = u16\(ps_inputs\[i\]\.{source}\)", loop_body) for field, source in FIELD_MAP)),
        ("nothing but that loop writes the fragment inputs", writers == 1),
        ("the register table and the fragment input table have the same size",
         (m := re.search(r"std::array<PsInputControl, (\d+)> ps_inputs;", regs)) is not None
         and (n := re.search(r"std::array<PsInput, (\d+)> inputs;", info)) is not None and m.group(1) == n.group(1) == "32"),
        ("a PS input control entry keeps the Sea Islands layout: offset 0-4, use default 5, default 8-9, flat 10",
         re.search(r"struct PsInputControl \{\s*u32 input_offset : 5;\s*u32 use_default : 1;\s*u32 : 2;\s*u32 default_value : 2;\s*u32 flat_shade : 1;\s*\};", shader_regs) is not None),
        ("the fragment runtime info equality compares exactly the first num_inputs entries",
         "std::ranges::equal(inputs.begin(), inputs.begin() + num_inputs, other.inputs.begin()," in info
         and "other.inputs.begin() + num_inputs);" in info),
    ]
    for name in ("emit", "quad"):
        text = sources[name]
        sites = [m.start() for m in re.finditer(r"(?:hw\.fs|fs_info)\.inputs\[", text)]
        bounded = all("num_inputs" in text[text.rfind("for (", 0, at):at] for at in sites)
        results.append((f"every read of the fragment inputs in {Path(SOURCES[name]).name} is inside a loop bounded by num_inputs", bool(sites) and bounded))
    return results


def self_test():
    base = read_sources()
    results = [("the real sources pass", all(ok for _t, ok in checks(base)))]

    def mutated(name, replace, count=1):
        source = dict(base)
        before = source[name]
        source[name] = before.replace(replace[0], replace[1], count)
        if source[name] == before:
            results.append((f"mutation did not apply: {replace[0]!r}", False))
            return
        results.append((f"fails when {replace[2]}", not all(ok for _t, ok in checks(source))))

    mutated("cache", ("info.Initialize(stage, l_stage);", "", "the runtime info is not cleared before it is filled"))
    mutated("info", ("memset(this, 0, sizeof(*this));", "", "Initialize stops clearing the object"))
    mutated("cache", ("i < regs.num_interp; i++", "i < 32; i++", "the fill loop is bounded by 32 instead of NUM_INTERP"))
    mutated("cache", (".is_flat = u16(ps_inputs[i].flat_shade),", "", "the flat shading bit stops being copied"))
    mutated("cache", (".default_value = u16(ps_inputs[i].default_value),", ".default_value = u16(ps_inputs[i].use_default),", "the default value is read from the wrong field"))
    mutated("cache", ("info.hw.fs.num_inputs = regs.num_interp;", "info.hw.fs.num_inputs = 32;", "the input count stops following NUM_INTERP"))
    mutated("cache", ("        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {\n            info.hw.fs.color_buffers",
                      "        info.hw.fs.inputs[31] = {};\n        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {\n            info.hw.fs.color_buffers", "a second writer touches the fragment inputs"))
    mutated("regs", ("std::array<PsInputControl, 32> ps_inputs;", "std::array<PsInputControl, 16> ps_inputs;", "the register table is shorter than the input table"))
    mutated("shader_regs", ("u32 flat_shade : 1;", "u32 flat_shade : 2;", "the PS input control layout changes"))
    mutated("info", ("inputs.begin() + num_inputs, other.inputs.begin(),", "inputs.end(), other.inputs.begin(),", "equality compares stale entries beyond num_inputs"))
    mutated("emit", ("for (s32 i = 0; i < num_inputs; i++) {\n            const auto& input = runtime_info.hw.fs.inputs[i];",
                     "for (s32 i = 0; i < 32; i++) {\n            const auto& input = runtime_info.hw.fs.inputs[i];", "a consumer loops over all 32 entries"))
    ok = True
    for name, passed in results:
        print(f"  [{'ok  ' if passed else 'fail'}] {name}")
        ok = ok and passed
    print(f"Graphics state gate self-test {'passed' if ok else 'FAILED'}: {len(results)} cases")
    return 0 if ok else 1


def dump(version, folder):
    """Writes the shaders embedded in the eboot of a build as t<type>_<index>_<image offset>.bin; returns their count."""
    import ps4_module as pm
    from game_profiles import PROFILES
    eboot = ROOT / "game" / "prepared" / PROFILES["CUSA57767"]["versions"][version]["folder"] / "eboot.bin"
    if not eboot.is_file():
        print(f"  [fail] CUSA57767 {version}: {eboot.relative_to(ROOT)} is not installed")
        return 0
    module = pm.read_module(eboot)
    loads = [p for p in module["ph"] if p["type"] == 1]
    image = bytearray(max(p["vaddr"] + p["memsz"] for p in loads))
    for p in loads:
        image[p["vaddr"]:p["vaddr"] + p["filesz"]] = module["elf"][p["offset"]:p["offset"] + p["filesz"]]
    image = bytes(image)
    Path(folder).mkdir(parents=True, exist_ok=True)
    count, at = 0, 0
    while (at := image.find(b"OrbShdr", at)) >= 0:
        word = struct.unpack_from("<I", image, at + 8)[0]
        kind, length = (word >> 2) & 0xF, word >> 8
        if 0 < length <= at:
            (Path(folder) / f"t{kind}_{count:02d}_{at:x}.bin").write_bytes(image[at - length:at])
            count += 1
        at += 1
    return count


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--dump" in argv:
        version, folder = argv[argv.index("--dump") + 1:argv.index("--dump") + 3]
        count = dump(version, folder)
        print(f"  CUSA57767 {version}: {count} embedded shaders written")
        return 0 if count else 1
    results = checks(read_sources())
    for text, ok in results:
        print(f"  [{'ok  ' if ok else 'fail'}] {text}")
    failed = sum(1 for _text, ok in results if not ok)
    print(f"Fragment input state FAILED: {failed} of {len(results)} invariants broken" if failed
          else f"Fragment input state passed: {len(results)} invariants, shared host code for both builds")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

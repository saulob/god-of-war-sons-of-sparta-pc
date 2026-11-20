// Headless proof of the pixel input state path on the production runtime info types and the production GCN decoder:
// the stage info is rebuilt deterministically (A -> B -> A leaves nothing of the previous pipeline), an input control
// entry decodes with the Sea Islands layout, and the shaders embedded in each supported build's eboot decode to the end
// with in-range interpolation operands. Mutated fixtures prove each check can fail. No window, GPU, launcher or game.
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

using namespace Shader;
using Gcn::InstEncoding;
using Gcn::Opcode;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", what.c_str());
    }
}

using InitFn = void (*)(RuntimeInfo&);
using EqualFn = bool (*)(const HwFragmentRuntimeInfo&, const HwFragmentRuntimeInfo&);

struct Implementation {
    const char* name;
    InitFn init;
    EqualFn equal;
};

void ProductionInit(RuntimeInfo& info) {
    info.Initialize(HwStage::Fragment, SwStage::Fragment);
}
void SkippedInit(RuntimeInfo&) {}
bool ProductionEqual(const HwFragmentRuntimeInfo& a, const HwFragmentRuntimeInfo& b) {
    return a == b;
}
bool AllEntriesEqual(const HwFragmentRuntimeInfo& a, const HwFragmentRuntimeInfo& b) {
    return a == b && a.inputs == b.inputs;
}

using Registers = std::array<AmdGpu::PsInputControl, 32>;

AmdGpu::PsInputControl Control(u32 raw) {
    return std::bit_cast<AmdGpu::PsInputControl>(raw);
}

// The statements of PipelineCache::BuildRuntimeInfo that fill the fragment inputs; scripts/graphics_state_check.py pins
// the production text to this shape.
void FillInputs(RuntimeInfo& info, const Registers& ps_inputs, u32 num_interp) {
    info.hw.fs.num_inputs = num_interp;
    for (u32 i = 0; i < num_interp; i++) {
        info.hw.fs.inputs[i] = {
            .param_index = u16(ps_inputs[i].input_offset),
            .is_default = u16(ps_inputs[i].use_default),
            .is_flat = u16(ps_inputs[i].flat_shade),
            .default_value = u16(ps_inputs[i].default_value),
        };
    }
}

Registers Table(const std::vector<u32>& raw) {
    Registers table{};
    std::memset(&table, 0xA5, sizeof(table));
    for (size_t i = 0; i < raw.size(); ++i) {
        table[i] = Control(raw[i]);
    }
    return table;
}

alignas(RuntimeInfo) unsigned char g_storage[3][sizeof(RuntimeInfo)];

RuntimeInfo& Filled(int slot, int byte) {
    std::memset(g_storage[slot], byte, sizeof(g_storage[slot]));
    return *reinterpret_cast<RuntimeInfo*>(g_storage[slot]);
}
RuntimeInfo& Poisoned() {
    return Filled(0, 0xCD);
}

// What a consumer can observe of the fragment inputs: the stage, the count, the used entries, and whether the unused
// entries are clear (the padding bits of a used entry are not part of the state).
std::string Observe(const RuntimeInfo& info, u32 count) {
    const auto& fs = info.hw.fs;
    std::string key = std::to_string(unsigned(info.hw_stage)) + "/" + std::to_string(fs.num_inputs) + "/" + std::to_string(fs.num_samples);
    for (u32 i = 0; i < count; ++i) {
        const auto& input = fs.inputs[i];
        key += "|" + std::to_string(input.param_index) + "," + std::to_string(input.is_default) + "," + std::to_string(input.is_flat) + "," + std::to_string(input.default_value);
    }
    bool clear = true;
    for (u32 i = count; i < fs.inputs.size(); ++i) {
        clear &= std::memcmp(&fs.inputs[i], std::array<unsigned char, sizeof(fs.inputs[i])>{}.data(), sizeof(fs.inputs[i])) == 0;
    }
    return key + (clear ? "|clear" : "|stale");
}

std::string Build(const Implementation& impl, const Registers& table, u32 count, RuntimeInfo& info) {
    impl.init(info);
    FillInputs(info, table, count);
    return Observe(info, count);
}

// Raw SPI_PS_INPUT_CNTL values: offset | use_default << 5 | default_value << 8 | flat_shade << 10.
const std::vector<u32> kPipelineA = {0x0, 0x1, 0x422, 0x703, 0x2, 0x424};
const std::vector<u32> kPipelineB = {0x1, 0x20};

void CheckInputControlLayout() {
    for (u32 offset : {0u, 1u, 17u, 31u}) {
        for (u32 use_default : {0u, 1u}) {
            for (u32 default_value : {0u, 1u, 2u, 3u}) {
                for (u32 flat : {0u, 1u}) {
                    const auto control = Control(offset | use_default << 5 | default_value << 8 | flat << 10);
                    Check(control.input_offset == offset && control.use_default == use_default &&
                              control.default_value == default_value && control.flat_shade == flat,
                          "PS input control fields do not follow the Sea Islands layout");
                    HwFragmentRuntimeInfo::PsInput input{};
                    input.is_default = control.use_default;
                    input.is_flat = control.flat_shade;
                    Check(input.IsDefault() == (use_default && !flat) && input.IsPassthrough() == (use_default && flat),
                          "default and passthrough inputs are not told apart by the flat bit");
                }
            }
        }
    }
    Check(sizeof(AmdGpu::PsInputControl) == sizeof(u32), "a PS input control entry is not one register");
}

bool DeterministicRebuild(const Implementation& impl) {
    bool ok = true;
    const Registers a = Table(kPipelineA), b = Table(kPipelineB);
    const Implementation production{"production", ProductionInit, ProductionEqual};
    const auto reference_a = Build(production, a, u32(kPipelineA.size()), Filled(1, 0));
    const auto reference_b = Build(production, b, u32(kPipelineB.size()), Filled(2, 0));
    ok &= reference_a.ends_with("|clear") && reference_b.ends_with("|clear");

    // One RuntimeInfo reused for A, then B, then A again, as a selection reuses its stage slots between draws.
    RuntimeInfo& reused = Poisoned();
    ok &= Build(impl, a, u32(kPipelineA.size()), reused) == reference_a;
    ok &= Build(impl, b, u32(kPipelineB.size()), reused) == reference_b;
    ok &= Build(impl, a, u32(kPipelineA.size()), reused) == reference_a;
    return ok;
}

bool StaleEntriesDoNotChangeEquality(const Implementation& impl) {
    RuntimeInfo& first = Filled(1, 0);
    RuntimeInfo& second = Filled(2, 0);
    first.hw.fs.num_inputs = second.hw.fs.num_inputs = 2;
    first.hw.fs.inputs[0] = second.hw.fs.inputs[0] = {.param_index = 3};
    first.hw.fs.inputs[1] = second.hw.fs.inputs[1] = {.param_index = 4, .is_flat = 1};
    second.hw.fs.inputs[9] = {.param_index = 7, .is_default = 1};
    bool ok = impl.equal(first.hw.fs, second.hw.fs);
    second.hw.fs.inputs[1].is_flat = 0;
    ok &= !impl.equal(first.hw.fs, second.hw.fs);
    second.hw.fs.inputs[1].is_flat = 1;
    second.hw.fs.num_inputs = 3;
    ok &= !impl.equal(first.hw.fs, second.hw.fs);
    return ok;
}

void CheckStageInfo() {
    const Implementation production{"production", ProductionInit, ProductionEqual};
    Check(DeterministicRebuild(production), "rebuilding A, B, A on one RuntimeInfo leaves state of the previous pipeline");
    Check(StaleEntriesDoNotChangeEquality(production), "equality is affected by entries beyond the input count, or ignores one inside it");
    static_assert(std::tuple_size_v<decltype(HwFragmentRuntimeInfo::inputs)> == 32, "the host input table must keep the hardware size");

    RuntimeInfo& cleared = Poisoned();
    cleared.Initialize(HwStage::Fragment, SwStage::Fragment);
    bool zero = cleared.hw.fs.num_inputs == 0 && cleared.hw.fs.num_samples == 1;
    for (const auto& input : cleared.hw.fs.inputs) {
        zero &= std::memcmp(&input, std::array<unsigned char, sizeof(input)>{}.data(), sizeof(input)) == 0;
    }
    Check(zero, "Initialize leaves a poisoned input table or count in place");

    const std::array<Implementation, 3> mutants = {{
        {"initialization skipped", SkippedInit, ProductionEqual},
        {"equality over every entry", ProductionInit, AllEntriesEqual},
        {"initialization skipped and equality over every entry", SkippedInit, AllEntriesEqual},
    }};
    for (const auto& mutant : mutants) {
        Check(!DeterministicRebuild(mutant) || !StaleEntriesDoNotChangeEquality(mutant),
              std::string("the state checks do not fail for the mutant: ") + mutant.name);
    }
}

struct ShaderStats {
    int shaders = 0, pixel = 0, vertex = 0, compute = 0, other = 0;
    int instructions = 0, interpolations = 0, max_attribute = -1, packed_encodings = 0, f16_packs = 0, param_exports = 0;
    std::map<std::string, int> interpolation_opcodes;
};

std::vector<std::string> AuditShader(const std::vector<u32>& words, char kind, ShaderStats& stats) {
    std::vector<std::string> issues;
    Gcn::GcnCodeSlice slice(words.data(), words.data() + words.size());
    Gcn::GcnDecodeContext context;
    bool ended = false;
    while (!slice.atEnd() && !ended) {
        const auto inst = context.decodeInstruction(slice);
        ++stats.instructions;
        ended = inst.opcode == Opcode::S_ENDPGM;
        stats.packed_encodings += inst.encoding == InstEncoding::VOP3P;
        stats.f16_packs += inst.opcode == Opcode::V_CVT_PKRTZ_F16_F32;
        if (inst.encoding == InstEncoding::VINTRP) {
            ++stats.interpolations;
            ++stats.interpolation_opcodes[std::string(magic_enum::enum_name(inst.opcode))];
            stats.max_attribute = std::max<int>(stats.max_attribute, inst.control.vintrp.attr);
            if (inst.control.vintrp.attr >= 32) {
                issues.push_back("interpolation attribute outside the 32 hardware slots");
            }
            if (inst.opcode != Opcode::V_INTERP_P1_F32 && inst.opcode != Opcode::V_INTERP_P2_F32 && inst.opcode != Opcode::V_INTERP_MOV_F32) {
                issues.push_back("interpolation opcode the translator does not handle");
            }
        }
        if (inst.encoding == InstEncoding::EXP && kind == '1') {
            stats.param_exports += inst.control.exp.target >= 32 && inst.control.exp.target < 64;
        }
    }
    if (!ended) {
        issues.push_back("no S_ENDPGM inside the shader");
    }
    ++stats.shaders;
    (kind == '0' ? stats.pixel : kind == '1' ? stats.vertex : kind == '4' ? stats.compute : stats.other)++;
    return issues;
}

constexpr u32 Vintrp(u32 op, u32 attr, u32 chan) {
    return (0x32u << 26) | (1u << 18) | (op << 16) | (attr << 10) | (chan << 8);
}
constexpr u32 kEndProgram = 0xBF810000u;

void CheckAuditCanFail() {
    ShaderStats unused;
    Check(AuditShader({Vintrp(0, 5, 2), Vintrp(1, 5, 2), kEndProgram}, '0', unused).empty(), "the shader audit rejects a valid interpolation sequence");
    Check(!AuditShader({Vintrp(0, 40, 0), kEndProgram}, '0', unused).empty(), "the shader audit accepts an interpolation attribute past slot 31");
    Check(!AuditShader({Vintrp(0, 5, 0)}, '0', unused).empty(), "the shader audit accepts a shader that never ends");
}

void CheckBuild(const std::string& version, const std::filesystem::path& folder) {
    ShaderStats stats;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() == ".bin") {
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<char> data((std::istreambuf_iterator<char>(file)), {});
            std::vector<u32> words(data.size() / 4);
            std::memcpy(words.data(), data.data(), words.size() * 4);
            const auto name = entry.path().filename().string();
            for (const auto& issue : AuditShader(words, name[1], stats)) {
                Check(false, "CUSA57767 " + version + " " + name + ": " + issue);
            }
            Check(true, name);
        }
    }
    Check(stats.shaders > 0 && stats.pixel > 0, "CUSA57767 " + version + ": no embedded pixel shader was found");
    Check(stats.other == 0, "CUSA57767 " + version + ": embedded shaders of a stage the audit does not know");
    Check(stats.packed_encodings == 0, "CUSA57767 " + version + ": packed math encodings in embedded shaders, which the interpolation audit does not cover");
    std::string opcodes;
    for (const auto& [name, count] : stats.interpolation_opcodes) {
        opcodes += " " + name + "=" + std::to_string(count);
    }
    std::printf("  CUSA57767 %s: %d embedded shaders (%d pixel, %d vertex, %d compute), %d instructions, %d interpolations%s, "
                "max attribute %d, %d half-float export packs\n",
                version.c_str(), stats.shaders, stats.pixel, stats.vertex, stats.compute, stats.instructions,
                stats.interpolations, opcodes.c_str(), stats.max_attribute, stats.f16_packs);
}

} // namespace

int main(int argc, char** argv) {
    CheckInputControlLayout();
    CheckStageInfo();
    CheckAuditCanFail();
    for (int i = 1; i + 1 < argc; i += 2) {
        CheckBuild(argv[i], argv[i + 1]);
    }
    Check(argc >= 3, "no build was given to check");
    std::printf("Pixel input state: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

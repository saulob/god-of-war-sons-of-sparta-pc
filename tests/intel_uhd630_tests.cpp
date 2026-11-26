// Issue #2 (Intel UHD Graphics 630, driver 101.2135, Vulkan 1.3.215): an access violation reading 0x298 in a host DLL
// that v0.1.0 named only "module+0x5cfab2", under Scheduler::BeginRendering on the Vulkan recorder thread. Checks that a
// report now names the DLL with its base, offset and file version and no path, that the Bug Report scrubber keeps that
// line, that the newest vkCmdBeginRendering is reported with its attachments and whether the driver was still inside it,
// the UHD 630 capability profile (interpolation route, no pipeline binaries without maintenance5), and the production
// wiring. No window, GPU, launcher or game.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include "crash_guard.h"
}
#include "dcp_rendering_trace.h"
#include "host_module.h"
#include "log_scrubber.h"
#include "video_core/renderer_vulkan/vk_gpu_compat.h"

#define SOS_STRING_(x) #x
#define SOS_STRING(x) SOS_STRING_(x)

namespace {

int g_checks = 0, g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("%s %s\n", ok ? "[ok]  " : "[fail]", what.c_str());
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string Source(const char* path) {
    std::ifstream in(std::string(SOS_STRING(SOS_SOURCE_DIR)) + "/" + path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(in), {}}, unix_text;
    for (const char c : text) {
        if (c != '\r') unix_text += c;
    }
    return unix_text;
}

std::string Lower(std::string text) {
    for (char& c : text) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}

// A PE image as the driver's would sit in memory: export name, version resource, timestamp.
constexpr uintptr_t kDriverBase = 0x7ffd029d0000ull;
constexpr uint32_t kDriverSize = 0x700000;

struct SyntheticImage {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x3000);
    uintptr_t base = kDriverBase;

    template <typename T> T* At(size_t offset) { return reinterpret_cast<T*>(bytes.data() + offset); }
};

// Memory past the synthetic bytes reads as 'A': other mappings follow a real image, so only the parser's own bounds keep
// it inside the image.
int ReadSynthetic(void* context, uintptr_t address, void* out, size_t size) {
    const auto* image = static_cast<const SyntheticImage*>(context);
    if (address < image->base) return 0;
    auto* bytes = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < size; ++i) {
        const uintptr_t at = address - image->base + i;
        bytes[i] = at < image->bytes.size() ? image->bytes[at] : uint8_t('A');
    }
    return 1;
}

SyntheticImage MakeDriverImage(const char* export_name, bool with_version = true) {
    SyntheticImage image;
    auto* dos = image.At<IMAGE_DOS_HEADER>(0);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = image.At<IMAGE_NT_HEADERS64>(0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = 0x62a1b2c3;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = kDriverSize;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x1000, sizeof(IMAGE_EXPORT_DIRECTORY)};
    image.At<IMAGE_EXPORT_DIRECTORY>(0x1000)->Name = 0x1100;
    std::memcpy(image.At<char>(0x1100), export_name, std::strlen(export_name) + 1);
    if (!with_version) return image;

    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE] = {0x2000, 0x400};
    const auto directory = [&](size_t offset, uint16_t ids) {
        auto* dir = image.At<IMAGE_RESOURCE_DIRECTORY>(offset);
        dir->NumberOfIdEntries = ids;
        return reinterpret_cast<IMAGE_RESOURCE_DIRECTORY_ENTRY*>(dir + 1);
    };
    auto* root = directory(0x2000, 2);
    root[0].Name = 3; // RT_ICON first: the walk must look for RT_VERSION, not take the first type
    root[0].OffsetToData = 0x80000000u | 0x1c0;
    root[1].Name = 16;
    root[1].OffsetToData = 0x80000000u | 0x40;
    directory(0x21c0, 0);
    auto* names = directory(0x2040, 1);
    names[0].Name = 1;
    names[0].OffsetToData = 0x80000000u | 0x60;
    auto* languages = directory(0x2060, 1);
    languages[0].Name = 0x409;
    languages[0].OffsetToData = 0x80;
    auto* data = image.At<IMAGE_RESOURCE_DATA_ENTRY>(0x2080);
    data->OffsetToData = 0x2100;
    data->Size = 0x5c;
    uint8_t* info = image.At<uint8_t>(0x2100);
    const uint16_t header[3] = {0x5c, 0x34, 0};
    std::memcpy(info, header, sizeof(header));
    const wchar_t key[] = L"VS_VERSION_INFO";
    std::memcpy(info + 6, key, sizeof(key));
    const uint32_t fixed[4] = {0xFEEF04BDu, 0x10000u, (31u << 16) | 0u, (101u << 16) | 2135u};
    std::memcpy(info + 40, fixed, sizeof(fixed));
    return image;
}

std::string Format(const SosHostModule& module, uintptr_t address) {
    char text[256];
    sos_host_module_format(&module, address, text, sizeof(text));
    return text;
}

void TestSyntheticDriverImage() {
    SyntheticImage image = MakeDriverImage("igvk64.dll");
    SosHostModule module{};
    Check(sos_host_module_parse(image.base, ReadSynthetic, &image, &module) == 1, "a 64-bit PE image is read");
    Check(std::string(module.name) == "igvk64.dll", "the DLL is named by its export directory");
    Check(module.base == kDriverBase && module.image_size == kDriverSize && module.timestamp == 0x62a1b2c3,
          "base, image size and link timestamp");
    Check(module.has_version && module.version[0] == 31 && module.version[1] == 0 && module.version[2] == 101 &&
              module.version[3] == 2135,
          "the file version comes from VS_FIXEDFILEINFO (RT_VERSION found after another resource type)");
    const std::string line = Format(module, kDriverBase + 0x5cfab2);
    Check(line == "module=igvk64.dll module_base=0x7ffd029d0000 module_offset=0x5cfab2 module_size=0x700000 "
                  "file_version=31.0.101.2135 timestamp=0x62a1b2c3",
          "issue #2's fault reads as DLL, base, offset, size, file version, timestamp: " + line);

    SyntheticImage pathed = MakeDriverImage("C:\\Users\\chick\\Drivers\\igvk64.dll");
    Check(sos_host_module_parse(pathed.base, ReadSynthetic, &pathed, &module) && std::string(module.name) == "igvk64.dll",
          "a path inside an export name is reduced to the file name");
    SyntheticImage odd = MakeDriverImage("ig vk\"64%.dll");
    Check(sos_host_module_parse(odd.base, ReadSynthetic, &odd, &module) && std::string(module.name) == "ig_vk_64_.dll",
          "characters outside a plain file name are replaced");

    SyntheticImage plain = MakeDriverImage("overlay64.dll", false);
    Check(sos_host_module_parse(plain.base, ReadSynthetic, &plain, &module) && !module.has_version &&
              Contains(Format(module, kDriverBase + 0x10), "file_version=unknown"),
          "a DLL without a version resource says unknown");

    SyntheticImage unnamed = MakeDriverImage("x");
    unnamed.At<IMAGE_EXPORT_DIRECTORY>(0x1000)->Name = kDriverSize + 0x10;
    Check(sos_host_module_parse(unnamed.base, ReadSynthetic, &unnamed, &module) && module.name[0] == 0 &&
              Contains(Format(module, kDriverBase), "module=unnamed "),
          "an export name outside the image is not followed");

    SyntheticImage bad = MakeDriverImage("igvk64.dll");
    bad.At<IMAGE_DOS_HEADER>(0)->e_magic = 0;
    Check(!sos_host_module_parse(bad.base, ReadSynthetic, &bad, &module), "no MZ header: not an image");
    bad = MakeDriverImage("igvk64.dll");
    bad.At<IMAGE_DOS_HEADER>(0)->e_lfanew = 0x7fffffff;
    Check(!sos_host_module_parse(bad.base, ReadSynthetic, &bad, &module), "a far e_lfanew is refused");
    bad = MakeDriverImage("igvk64.dll");
    bad.At<IMAGE_NT_HEADERS64>(0x80)->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Check(!sos_host_module_parse(bad.base, ReadSynthetic, &bad, &module), "a 32-bit image is refused");
    bad = MakeDriverImage("igvk64.dll");
    bad.At<IMAGE_RESOURCE_DATA_ENTRY>(0x2080)->OffsetToData = kDriverSize - 4;
    Check(sos_host_module_parse(bad.base, ReadSynthetic, &bad, &module) && !module.has_version,
          "a version resource that leaves the image is ignored, the rest is kept");
}

void TestLoadedDll() {
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    const auto function = reinterpret_cast<uintptr_t>(GetProcAddress(kernel, "GetTickCount64"));
    SosHostModule module{};
    Check(kernel && function && sos_host_module_at(function, &module) == 1, "an address in a loaded DLL is found");
    Check(module.base == reinterpret_cast<uintptr_t>(kernel) && Lower(module.name) == "kernel32.dll",
          std::string("the loaded DLL is named and based correctly: ") + module.name);
    Check(module.has_version && module.version[0] >= 6, "the loaded DLL's file version is read from memory");

    std::vector<char> heap(4096);
    Check(!sos_host_module_at(reinterpret_cast<uintptr_t>(heap.data()), &module), "heap memory is no module");
    Check(!sos_host_module_at(0x298, &module), "the fault address of issue #2 is no module");

    char text[512];
    sos_crash_describe_address(text, sizeof(text), function);
    const std::string described = text;
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llx", static_cast<unsigned long long>(function - module.base));
    Check(Lower(described).rfind("kernel32.dll+", 0) == 0 && Contains(described, offset),
          "a report names the DLL with its offset instead of module+: " + described.substr(0, described.find('\n')));
    Check(Contains(described, "\n  host module: module=") && Contains(described, " module_base=0x") &&
              Contains(described, " module_offset=0x") && Contains(described, " file_version="),
          "and adds the host module line");
    Check(!Contains(described, ":\\") && !Contains(described, "\\Windows"), "with no path");
}

void TestScrubberKeepsModuleLine() {
    sos::PrivateFacts facts;
    facts.user_names = {"chick"};
    facts.machine_names = {"CHICK-PC"};
    const std::string line = "  host module: module=igvk64.dll module_base=0x7ffd029d0000 module_offset=0x5cfab2 "
                             "module_size=0x700000 file_version=31.0.101.2135 timestamp=0x62a1b2c3\n";
    Check(sos::ScrubText(line, facts) == line, "the Bug Report keeps the host module line, file version included");
    const std::string address_like = "  host module: module=overlay64.dll module_base=0x7ffd10000000 "
                                     "module_offset=0x1234 module_size=0x80000 file_version=7.3.4.112 timestamp=0x1\n";
    Check(sos::ScrubText(address_like, facts) == address_like,
          "a file version that looks like an IPv4 address is kept too");
    Check(Contains(sos::ScrubText("peer 10.1.2.3 joined\n", facts), "<ipv4>"), "an address is still scrubbed");
    Check(Contains(sos::ScrubText("remote_api=10.1.2.3\n", facts), "<ipv4>"),
          "only a *_version key exempts a dotted number");
}

void TestRenderingTrace() {
    static std::vector<std::string> lines;
    DcpRendering::Tracker trace{[](const char* line) { lines.emplace_back(line); }};
    char text[256];
    trace.Describe(text, sizeof(text));
    Check(std::string(text) == "no vkCmdBeginRendering recorded", "nothing recorded yet");

    const DcpRendering::Shape bound{.colors = 1, .depth = true, .width = 1280, .height = 720, .layers = 1};
    trace.Begin(bound, 8912);
    trace.End();
    Check(lines.empty(), "a fully bound pass logs nothing");
    trace.Describe(text, sizeof(text));
    Check(std::string(text) == "vkCmdBeginRendering #1 returned on thread 8912: colors=1 unbound_colors=0 depth=yes "
                               "stencil=no null_depth_stencil_view=no area=1280x720 layers=1",
          std::string("a returned pass: ") + text);

    // A depth-only pass with its colour slot unbound (CB_COLOR_CONTROL mode Disable).
    const DcpRendering::Shape depth_only{.colors = 2, .unbound_colors = 0x3, .depth = true, .stencil = true,
                                         .width = 1280, .height = 720, .layers = 1};
    trace.Begin(depth_only, 8912);
    trace.Describe(text, sizeof(text));
    Check(trace.InDriver() && std::string(text) == "vkCmdBeginRendering #2 IN PROGRESS on thread 8912: colors=2 "
                                                   "unbound_colors=0x3 depth=yes stencil=yes "
                                                   "null_depth_stencil_view=no area=1280x720 layers=1",
          std::string("a fault inside the driver names the pass it was given: ") + text);
    trace.End();
    trace.Begin(depth_only, 8912);
    trace.End();
    Check(lines.size() == 1 && lines[0] == "Runtime: first rendering pass with an unbound color attachment: colors=2 "
                                           "unbound_colors=0x3 depth=yes stencil=yes null_depth_stencil_view=no "
                                           "area=1280x720 layers=1",
          "the first pass with an unbound colour attachment is logged once");

    const DcpRendering::Shape widest{.colors = 8, .unbound_colors = 0xa5, .depth = true, .stencil = true,
                                     .null_depth_stencil_view = true, .width = 16384, .height = 16384, .layers = 2048};
    const DcpRendering::Shape back = DcpRendering::Unpack(DcpRendering::Pack(widest));
    Check(back.colors == 8 && back.unbound_colors == 0xa5 && back.depth && back.stencil && back.null_depth_stencil_view &&
              back.width == 16384 && back.height == 16384 && back.layers == 2048,
          "the packed shape keeps every field at the device limits");
    char small[16];
    Check(trace.Describe(small, sizeof(small)) == 15 && small[15] == 0, "a short buffer stays terminated");
}

void TestUhd630Profile() {
    using namespace Vulkan::GpuCompat;
    // From the issue log: no VK_AMD_shader_explicit_vertex_parameter, no VK_KHR_fragment_shader_barycentric, no
    // VK_AMD_mixed_attachment_samples / VK_NV_framebuffer_mixed_samples, VK_EXT_provoking_vertex enabled.
    const Capabilities uhd630{.vendor_id = 0x8086, .nvidia_proprietary = false, .amd_explicit_vertex_parameter = false,
                              .fragment_shader_barycentric = false, .mixed_depth_samples = false,
                              .provoking_vertex = true};
    const InterpolationRoute route = ChooseInterpolation(uhd630);
    Check(route.interpolation == Interpolation::Fallback && !route.manual_interpolation && route.integer_fix,
          "UHD 630: driver interpolation with the integer fix, no barycentric or AMD path");
    Check(Summary(uhd630) == "GPU: compatibility vendor=0x8086 interpolation=fallback manual_interpolation=off "
                             "integer_fix=on mixed_samples=no provoking_vertex=yes",
          "UHD 630 compatibility line: " + Summary(uhd630));
    const std::uint32_t colors[2] = {1, 1};
    Check(!NeedsMixedSamplesInfo(uhd630.mixed_depth_samples, 1, colors, 2, true, 1),
          "UHD 630: no VkAttachmentSampleCountInfoAMD");
    Check(!PipelineBinariesUsable(false, true, true), "UHD 630 (no VK_KHR_maintenance5): no pipeline binaries");
    Check(!PipelineBinariesUsable(true, false, true) && !PipelineBinariesUsable(true, true, false),
          "an extension without its feature does not enable pipeline binaries");
    Check(PipelineBinariesUsable(true, true, true), "pipeline binaries with maintenance5 and both features");
}

void TestWiring() {
    const std::string scheduler = Source("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    const size_t begin = scheduler.find("trace.Begin(RenderingShape(info), Common::GpuTrace::CurrentThreadId());");
    const size_t call = scheduler.find("cmdbuf.beginRendering(info);", begin);
    const size_t end = scheduler.find("trace.End();", call);
    Check(begin != std::string::npos && call != std::string::npos && end != std::string::npos &&
              call - begin < 120 && end - call < 60,
          "every replayed vkCmdBeginRendering is bracketed by the rendering trace");
    Check(Contains(scheduler, "if (!info.pColorAttachments[i].imageView) {\n            shape.unbound_colors |= 1u << i;"),
          "unbound colour attachments are the ones with a null image view");

    const std::string guard = Source("src/crash_guard.c");
    size_t uses = 0;
    for (size_t at = guard.find("put_host_module(&text, (uintptr_t)c->Rip);"); at != std::string::npos;
         at = guard.find("put_host_module(&text, (uintptr_t)c->Rip);", at + 1))
        ++uses;
    Check(uses == 2, "both the CRASH report and the first-chance line name the host module of rip");

    const std::string loader = Source("src/loader.c");
    Check(Contains(loader, "fprintf(stderr, \"  rendering: %s\\n\", rendering);") &&
              Contains(loader, "\"Crash: rendering: %s\\n\""),
          "the Windows and Linux crash reports print the newest rendering pass");
    Check(Contains(loader, "    if (gpu_enabled) dcpgpu_crash_trace(\"crash\");\n}"),
          "the Windows crash report prints the GPU trace");

    const std::string thread = Source("gpu/shadps4/common/thread.cpp");
    const size_t windows = thread.find("SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());");
    Check(windows != std::string::npos && thread.find("sos_thread_label_set(name);", windows) - windows < 120,
          "host threads such as dcp:VkRecorder are named in crash reports");

    Check(Contains(Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp"), "GpuCompat::PipelineBinariesUsable("),
          "the device uses the tested pipeline-binary rule");
    Check(Contains(Source("CMakeLists.txt"), "src/crash_guard.c src/host_module.c"), "the game links host_module.c");
}

} // namespace

int main() {
    TestSyntheticDriverImage();
    TestLoadedDll();
    TestScrubberKeepsModuleLine();
    TestRenderingTrace();
    TestUhd630Profile();
    TestWiring();
    std::printf("intel-uhd630: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

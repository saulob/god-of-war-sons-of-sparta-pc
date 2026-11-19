// Headless proof of the Texture filtering path on real sampler descriptors (AmdGpu::Sampler): which samplers the setting
// changes, that it never lowers one, that the device limit clamps it, and that the sampler cache key follows the
// setting. No window, GPU or game.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "dcp_graphics.h"
#include "video_core/texture_cache/forced_anisotropy.h"

extern "C" FILE* sos_stderr_stream(void) {
    return stdout;
}

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  [fail] %s\n", name.c_str());
    }
}

AmdGpu::Sampler TrilinearSampler() {
    AmdGpu::Sampler sampler{};
    sampler.xy_min_filter.Assign(AmdGpu::Filter::Bilinear);
    sampler.xy_mag_filter.Assign(AmdGpu::Filter::Bilinear);
    sampler.mip_filter.Assign(AmdGpu::MipFilter::Linear);
    return sampler;
}

AmdGpu::Sampler Forced(AmdGpu::Sampler sampler, int setting, bool excluded = false) {
    VideoCore::ForceSamplerAnisotropy(sampler, setting, excluded);
    return sampler;
}

// The key TextureCache::GetSampler derives from the sampler it is given.
u64 CacheKey(const AmdGpu::Sampler& sampler) {
    return XXH3_64bits(&sampler, sizeof(sampler));
}

float VulkanMaxAnisotropy(const AmdGpu::Sampler& sampler, float device_max) {
    const bool enabled = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) || AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    return DcpGraphics::EffectiveMaxAnisotropy(true, enabled, sampler.MaxAniso(), device_max);
}

void TestWhichSamplersChange() {
    const AmdGpu::Sampler game = TrilinearSampler();
    Check(Forced(game, 0) == game, "Game leaves the sampler as the game made it");
    for (const int ratio : {2, 4, 8, 16}) {
        const AmdGpu::Sampler forced = Forced(game, ratio);
        Check(AmdGpu::IsAnisoFilter(forced.xy_min_filter) && AmdGpu::IsAnisoFilter(forced.xy_mag_filter),
              "a trilinear sampler becomes anisotropic at " + std::to_string(ratio) + "x");
        Check(forced.MaxAniso() == float(ratio), "the sampler asks for " + std::to_string(ratio) + "x");
        Check(forced.mip_filter == game.mip_filter && forced.clamp_x == game.clamp_x, "nothing else about the sampler changes");
    }
    AmdGpu::Sampler point = game;
    point.xy_min_filter.Assign(AmdGpu::Filter::Point);
    point.xy_mag_filter.Assign(AmdGpu::Filter::Point);
    Check(Forced(point, 16) == point, "a point filtered sampler is left alone");
    AmdGpu::Sampler mipless = game;
    mipless.mip_filter.Assign(AmdGpu::MipFilter::None);
    Check(Forced(mipless, 16) == mipless, "a sampler without mips (interface art) is left alone");
    Check(Forced(game, 16, true) == game, "a depth or compute sampler is left alone");
    AmdGpu::Sampler sharp = Forced(game, 16);
    Check(Forced(sharp, 4) == sharp, "a sampler that already asks for more is never lowered");
    Check(Forced(Forced(game, 2), 8).MaxAniso() == 8.0f, "a sampler that asks for less is raised");
    AmdGpu::Sampler magnify_point = game;
    magnify_point.xy_mag_filter.Assign(AmdGpu::Filter::Point);
    Check(Forced(magnify_point, 8).xy_mag_filter == AmdGpu::Filter::Point, "a point magnification filter is kept");
}

void TestDeviceLimit() {
    const AmdGpu::Sampler game = TrilinearSampler();
    Check(VulkanMaxAnisotropy(Forced(game, 16), 16.0f) == 16.0f, "16x on a 16x device");
    Check(VulkanMaxAnisotropy(Forced(game, 16), 8.0f) == 8.0f, "16x is clamped to an 8x device");
    Check(VulkanMaxAnisotropy(Forced(game, 4), 16.0f) == 4.0f, "4x stays 4x on a 16x device");
    Check(VulkanMaxAnisotropy(game, 16.0f) == 1.0f, "Game leaves the Vulkan sampler isotropic");
}

void TestSamplerCacheKey() {
    const AmdGpu::Sampler game = TrilinearSampler();
    std::map<u64, int> cache;
    int created = 0;
    const auto lookup = [&](int setting) {
        const u64 key = CacheKey(Forced(game, setting));
        const auto [it, inserted] = cache.try_emplace(key, created);
        created += inserted ? 1 : 0;
        return it->second;
    };
    const int original = lookup(0);
    const int at4 = lookup(4);
    const int at16 = lookup(16);
    Check(original != at4 && at4 != at16 && original != at16, "Game, 4x and 16x are three different cached samplers");
    Check(lookup(0) == original, "16x back to Game finds the game's own sampler");
    Check(lookup(4) == at4 && lookup(16) == at16, "returning to a setting finds its sampler");
    Check(CacheKey(Forced(game, 4)) != CacheKey(game) && CacheKey(Forced(game, 16)) != CacheKey(Forced(game, 4)),
          "the effective ratio is part of the key");
    Check(CacheKey(Forced(game, 16)) == CacheKey(Forced(Forced(game, 16), 4)), "a sampler never lowered keeps its key");
    Check(CacheKey(Forced(game, 16, true)) == CacheKey(game), "an untouched sampler keeps the game's key at any setting");
}

} // namespace

int main() {
    TestWhichSamplersChange();
    TestDeviceLimit();
    TestSamplerCacheKey();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

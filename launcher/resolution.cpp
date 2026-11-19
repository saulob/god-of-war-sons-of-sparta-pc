#include "sos_resolution.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "trg/settings.h"

extern "C" {

const SosResolution sos_resolutions[SOS_RESOLUTION_COUNT] = {
    {"native", "Native", 1920, 1080}, {"720p", "720p", 1280, 720},   {"1440p", "1440p", 2560, 1440},
    {"1800p", "1800p", 3200, 1800},   {"4K", "4K", 3840, 2160},
};

const SosAspectRatio sos_aspect_ratios[SOS_ASPECT_COUNT] = {
    {"16:9", "16:9 Original", 16, 9}, {"16:10", "16:10", 16, 10},         {"21:9", "21:9 Ultrawide", 21, 9},
    {"32:9", "32:9 Super Ultrawide", 32, 9}, {"4:3", "4:3", 4, 3},        {"5:4", "5:4", 5, 4},
};

int sos_resolution_index(const char *key) {
    for (int i = 0; key && i < SOS_RESOLUTION_COUNT; ++i)
        if (!std::strcmp(key, sos_resolutions[i].key)) return i;
    return 0;
}

int sos_aspect_index(const char *key) {
    for (int i = 0; key && i < SOS_ASPECT_COUNT; ++i)
        if (!std::strcmp(key, sos_aspect_ratios[i].key)) return i;
    return 0;
}

} // extern "C"

namespace {

constexpr long kBaseMinWidth = 1280, kNeoMinWidth = 1920, kMaxWidth = 3840;
constexpr long kNativePixels = 1920L * 1080;
constexpr uint64_t kBytesPerExtraPixel = 96;
constexpr uint64_t kBudgetStep = 2u << 20;

// The game's screen manager takes widths in multiples of these.
long WidthStep(long width, bool neo) {
    if (width < 1920) return 32;
    return neo && width >= 3200 ? 160 : 320;
}

long EvenHeight(long width, long ratio_width, long ratio_height) {
    const long height = (width * ratio_height * 2 + ratio_width) / (2 * ratio_width);
    return (height + 1) & ~1L;
}

long NineSixteenths(long width) { return (width * 9) >> 4; }

void FillNops(unsigned char *at, size_t length) {
    static const unsigned char kNops[][7] = {
        {0x90}, {0x66, 0x90}, {0x0F, 0x1F, 0x00}, {0x0F, 0x1F, 0x40, 0x00}, {0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00}, {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}};
    while (length) {
        const size_t step = std::min<size_t>(length, 7);
        std::memcpy(at, kNops[step - 1], step);
        at += step;
        length -= step;
    }
}

bool Needed(const SosResolutionSite &site, bool height_sites) {
    return site.kind == SOS_SITE_WIDTH || height_sites;
}

int Fail(char *error, size_t size, const char *what, uint32_t offset) {
    if (error && size) std::snprintf(error, size, "render resolution patch: %s at image offset 0x%x", what, unsigned(offset));
    return 0;
}

bool Fits(const SosResolutionSite &site) {
    switch (site.kind) {
    case SOS_SITE_WIDTH: return site.length >= 8;
    case SOS_SITE_HEIGHT_CALC: return site.length >= 9;
    case SOS_SITE_HEIGHT_CHECK: return site.length >= 6;
    }
    return false;
}

} // namespace

extern "C" {

SosRenderTarget sos_native_render_target(void) { return {1920, 1080, 0, 1920, 1080}; }

int sos_render_target_is_native(const SosRenderTarget *target) {
    return target->width == 1920 && target->height == 1080;
}

SosRenderTarget sos_render_target(const SosResolution *resolution, const SosAspectRatio *aspect) {
    const long ratio_width = aspect->width, ratio_height = aspect->height;
    long width = (long(resolution->height) * ratio_width * 2 + ratio_height) / (2 * ratio_height);
    long height = resolution->height;
    bool neo = false;
    for (int pass = 0; pass < 3; ++pass) {
        const long minimum = neo ? kNeoMinWidth : kBaseMinWidth;
        long clamped = std::clamp(width, minimum, kMaxWidth);
        const long step = WidthStep(clamped, neo);
        clamped = std::min(kMaxWidth, (clamped + step / 2) / step * step);
        width = std::max(clamped, (minimum + step - 1) / step * step);
        height = EvenHeight(width, ratio_width, ratio_height);
        const bool needs_neo = width > 1920 || height > 1080;
        if (needs_neo == neo) break;
        neo = needs_neo;
    }
    SosRenderTarget target{int(width), int(height), neo ? 1 : 0, 1920, 1080};
    if (neo) {
        target.display_width = 3840;
        target.display_height = 2160;
    }
    return target;
}

uint64_t sos_budget_growth(const SosRenderTarget *target) {
    const long pixels = long(target->width) * target->height;
    if (!target->neo || pixels <= kNativePixels) return 0;
    const uint64_t bytes = uint64_t(pixels - kNativePixels) * kBytesPerExtraPixel;
    return (bytes + kBudgetStep - 1) / kBudgetStep * kBudgetStep;
}

int sos_target_needs_height_sites(const SosRenderTarget *target) {
    return target->height != NineSixteenths(target->width);
}

int sos_apply_render_target(unsigned char *image, uint64_t image_size, const SosResolutionSite *sites, size_t count,
                            const SosRenderTarget *target, char *error, size_t error_size) {
    const bool height_sites = sos_target_needs_height_sites(target);
    bool has[3] = {false, false, false};
    for (size_t i = 0; i < count; ++i) {
        const SosResolutionSite &site = sites[i];
        if (site.length > SOS_SITE_MAX_BYTES || site.kind > SOS_SITE_HEIGHT_CHECK || !Fits(site))
            return Fail(error, error_size, "malformed site", site.offset);
        has[site.kind] = true;
        if (!Needed(site, height_sites)) continue;
        if (uint64_t(site.offset) + site.length > image_size) return Fail(error, error_size, "site outside the image", site.offset);
        if (std::memcmp(image + site.offset, site.original, site.length))
            return Fail(error, error_size, "unexpected instruction", site.offset);
    }
    if (!has[SOS_SITE_WIDTH] || (height_sites && !(has[SOS_SITE_HEIGHT_CALC] && has[SOS_SITE_HEIGHT_CHECK])))
        return Fail(error, error_size, "site missing for this shape", 0);
    for (size_t i = 0; i < count; ++i) {
        const SosResolutionSite &site = sites[i];
        if (!Needed(site, height_sites)) continue;
        unsigned char *at = image + site.offset;
        switch (site.kind) {
        case SOS_SITE_WIDTH: {
            const uint32_t width = uint32_t(target->width);
            std::memcpy(at, &width, 4);
            std::memcpy(at + 4, &width, 4);
            break;
        }
        case SOS_SITE_HEIGHT_CALC: {
            const uint32_t height = uint32_t(target->height);
            const unsigned char head[] = {0x41, 0xBC, 0, 0, 0, 0, 0x44, 0x89, 0xFE}; /* mov r12d, height; mov esi, r15d */
            std::memcpy(at, head, sizeof(head));
            std::memcpy(at + 2, &height, 4);
            FillNops(at + sizeof(head), site.length - sizeof(head));
            break;
        }
        case SOS_SITE_HEIGHT_CHECK:
            FillNops(at + site.length - 6, 6);
            break;
        }
    }
    return 1;
}

SosRenderTarget sos_render_target_from_settings(const char *settings_file, int supported, int *resolution, int *aspect) {
    int resolution_index = 0, aspect_index = 0;
    if (supported && settings_file && *settings_file) {
        const trg::TextFileSettings settings{std::string(settings_file)};
        resolution_index = sos_resolution_index(settings.Get("render_resolution", "native").c_str());
        aspect_index = sos_aspect_index(settings.Get("aspect_ratio", "16:9").c_str());
    }
    if (resolution) *resolution = resolution_index;
    if (aspect) *aspect = aspect_index;
    if (!resolution_index && !aspect_index) return sos_native_render_target();
    return sos_render_target(&sos_resolutions[resolution_index], &sos_aspect_ratios[aspect_index]);
}

} // extern "C"

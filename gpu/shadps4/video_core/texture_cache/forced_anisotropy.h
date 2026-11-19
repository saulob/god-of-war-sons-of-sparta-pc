// SPDX-License-Identifier: GPL-2.0-or-later
// The Texture filtering setting applied to a game sampler before it is looked up, so the sampler cache is keyed by the
// filtering the sampler actually gets.
#pragma once

#include "dcp_graphics.h"
#include "video_core/amdgpu/resource.h"

namespace VideoCore {

// Raises `sharp` to at least `forced_setting` times anisotropy. Returns the ratio applied, 0 when the sampler is left
// as the game made it.
inline int ForceSamplerAnisotropy(AmdGpu::Sampler& sharp, int forced_setting, bool excluded) {
    const bool linear_min = sharp.xy_min_filter == AmdGpu::Filter::Bilinear ||
                            sharp.xy_min_filter == AmdGpu::Filter::AnisoLinear;
    const bool aniso = AmdGpu::IsAnisoFilter(sharp.xy_min_filter);
    const int forced = DcpGraphics::ForcedAnisotropyRatio(forced_setting, excluded,
                                                          sharp.mip_filter != AmdGpu::MipFilter::None, linear_min,
                                                          aniso, aniso ? sharp.MaxAniso() : 1.0f);
    if (forced == 0) {
        return 0;
    }
    const auto ratio = forced == 16 ? AmdGpu::AnisoRatio::Sixteen
        : forced == 8 ? AmdGpu::AnisoRatio::Eight
        : forced == 4 ? AmdGpu::AnisoRatio::Four : AmdGpu::AnisoRatio::Two;
    sharp.xy_min_filter.Assign(AmdGpu::Filter::AnisoLinear);
    if (sharp.xy_mag_filter == AmdGpu::Filter::Bilinear) {
        sharp.xy_mag_filter.Assign(AmdGpu::Filter::AnisoLinear);
    }
    sharp.max_aniso.Assign(ratio);
    return forced;
}

} // namespace VideoCore

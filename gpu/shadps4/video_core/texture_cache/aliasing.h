// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "video_core/texture_cache/types.h"

namespace VideoCore {

struct ImageInfo;

[[nodiscard]] std::optional<Extent3D> GetAliasCopyExtent(const ImageInfo& src,
                                                         const ImageInfo& dst);

// The same guest address now holds a linear image where a tiled one was cached, or the reverse.
[[nodiscard]] bool IsLayoutReuse(const ImageInfo& cached, const ImageInfo& requested);

} // namespace VideoCore

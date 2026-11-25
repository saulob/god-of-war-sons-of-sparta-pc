#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace sos {

// SOS_LAUNCHER_RENDERER: "software" or "direct3d11" (Windows only); anything else is automatic.
inline constexpr const char* kLauncherRendererVariable = "SOS_LAUNCHER_RENDERER";

enum class RendererRequest { kAuto, kSoftware, kDirect3D11 };

inline RendererRequest ParseRendererRequest(const char* text) {
    const std::string_view value = text ? text : "";
    if (value == "software") {
        return RendererRequest::kSoftware;
    }
    if (value == "direct3d11") {
        return RendererRequest::kDirect3D11;
    }
    return RendererRequest::kAuto;
}

inline const char* RendererRequestName(RendererRequest request) {
    switch (request) {
    case RendererRequest::kSoftware:
        return "software";
    case RendererRequest::kDirect3D11:
        return "direct3d11";
    default:
        return "auto";
    }
}

// SDL renderer names in the order they are tried, each at most once; an empty name lets SDL pick its first working
// renderer. Windows prefers Direct3D 11, and software is always behind it.
inline std::vector<std::string_view> LauncherRendererOrder(bool windows, RendererRequest request) {
    if (request == RendererRequest::kSoftware) {
        return {"software"};
    }
    if (!windows) {
        return {"", "software"};
    }
    return {"direct3d11", "software"};
}

// System modules a renderer loads that must stay mapped until the process exits: SDL unloads d3d11.dll right after the
// device's last Release while an NVIDIA driver thread can still be returning through it.
inline std::vector<std::string_view> RendererModulesToPin(std::string_view renderer) {
    if (renderer == "direct3d11") {
        return {"d3d11.dll", "dxgi.dll"};
    }
    return {};
}

struct RendererSelection {
    std::string name;
    bool created = false;
    bool fallback = false;
    int pinned = 0;
    int attempts = 0;
};

// Walks the order once. A renderer whose modules cannot be pinned is skipped without being created; a renderer that
// fails to create is not retried. `pin(module)` and `create(name)` return success.
template <class Pin, class Create>
RendererSelection SelectRenderer(const std::vector<std::string_view>& order, Pin&& pin, Create&& create) {
    RendererSelection selection;
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string name(order[i]);
        bool pinned = true;
        int pinned_now = 0;
        for (const std::string_view module : RendererModulesToPin(name)) {
            if (!pin(module)) {
                pinned = false;
                break;
            }
            ++pinned_now;
        }
        selection.pinned += pinned_now;
        if (!pinned) {
            continue;
        }
        ++selection.attempts;
        if (create(name)) {
            selection.name = name;
            selection.created = true;
            selection.fallback = i > 0;
            return selection;
        }
    }
    return selection;
}

inline std::string RendererLabel(std::string_view name) {
    std::string label;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        label += plain ? c : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : '_');
    }
    return label.empty() ? "unknown" : label;
}

inline std::string RendererSelectedLine(std::string_view renderer, RendererRequest request, bool fallback, int pinned) {
    char line[160];
    std::snprintf(line, sizeof(line), "Launcher renderer: selected=%s requested=%s fallback=%d pinned_modules=%d",
                  RendererLabel(renderer).c_str(), RendererRequestName(request), fallback ? 1 : 0, pinned);
    return line;
}

} // namespace sos

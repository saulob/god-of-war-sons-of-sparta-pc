#pragma once

#include <string>
#include <string_view>

namespace sos {

// The names of the files the release flow publishes; the updater asks for exactly these.
inline constexpr std::string_view kReleaseAssetPrefix = "SonsOfSpartaPC-v";
inline constexpr std::string_view kWindowsPackageSuffix = "-windows-x64.zip";
inline constexpr std::string_view kLinuxPackageSuffix = "-x86_64.AppImage";
inline constexpr std::string_view kChecksumSuffix = ".sha256";

inline std::string PackageName(std::string_view core_version, std::string_view suffix) {
    return std::string(kReleaseAssetPrefix) + std::string(core_version) + std::string(suffix);
}

inline std::string ChecksumName(std::string_view core_version, std::string_view suffix) {
    return PackageName(core_version, suffix) + std::string(kChecksumSuffix);
}

} // namespace sos

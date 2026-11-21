#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sos {

struct PrivateFacts {
    std::vector<std::string> user_names;
    std::vector<std::string> machine_names;
    std::vector<std::string> profile_folders;
    std::vector<std::string> install_folders;
};

PrivateFacts GatherPrivateFacts(const std::vector<std::filesystem::path>& install_folders);

std::string ScrubText(std::string_view text, const PrivateFacts& facts);

} // namespace sos

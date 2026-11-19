#include "settings.h"

namespace sos {

std::filesystem::path SettingsPath(const std::filesystem::path& user_dir) {
    return user_dir / "sons_of_sparta.ini";
}

void ConfigureSettings(trg::TextFileSettings& settings) {
    settings.true_value = "1";
    settings.false_value = "0";
}

} // namespace sos

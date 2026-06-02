#pragma once
#include <string>
#include "../ui/DebriefUI.hpp"

namespace debrief {

class ConfigManager {
public:
    static void load_config(UIState& state, const std::string& path = "debrief_config.yaml");
    static void save_config(const UIState& state, const std::string& path = "debrief_config.yaml");
};

} // namespace debrief

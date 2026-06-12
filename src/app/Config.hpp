#pragma once
#include <string>
#include "../ui/AfterActionUI.hpp"

namespace afteraction {

class ConfigManager {
public:
    static void load_config(UIState& state, const std::string& path = "afteraction_config.yaml");
    static void save_config(const UIState& state, const std::string& path = "afteraction_config.yaml");
};

} // namespace afteraction

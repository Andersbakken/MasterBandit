#pragma once

#include "Config.h"
#include <climits>
#include <optional>
#include <string>

struct TerminalOptions {
    std::string shell;
    std::string user;
    std::string font;               // empty = auto-detect
    float fontSize = 16.0f;
    float boldStrength = 0.04f;
    std::optional<int> scrollbackLines; // nullopt = infinite
    TabBarConfig tabBar;
    std::vector<BindingConfig> keybindings;
    std::string dividerColor = "#3d3d3d";
    int dividerWidth = 1;

    // Returns the tier1 capacity to pass to Document: INT_MAX when infinite.
    int resolvedScrollback() const
    {
        return scrollbackLines.value_or(INT_MAX);
    }
};

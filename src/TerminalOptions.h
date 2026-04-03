#pragma once

#include <climits>
#include <optional>
#include <string>

struct TerminalOptions {
    std::string shell;
    std::string user;
    std::string font;               // empty = auto-detect
    float fontSize = 16.0f;
    std::optional<int> scrollbackLines; // nullopt = infinite

    // Returns the tier1 capacity to pass to Document: INT_MAX when infinite.
    int resolvedScrollback() const
    {
        return scrollbackLines.value_or(INT_MAX);
    }
};

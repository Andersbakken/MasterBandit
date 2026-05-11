#pragma once

#include "Config.h"
#include <climits>
#include <optional>
#include <string>

struct TerminalOptions
{
    std::string shell;
    std::string command; // if set, exec this instead of shell (for pager etc.)
    std::string cwd;     // if set, chdir before exec
    std::string user;
    std::string font; // empty = auto-detect
    float fontSize     = 16.0f;
    float boldStrength = 0.04f;
    std::optional<int> scrollbackLines; // nullopt = infinite
    TabBarConfig tabBar;
    std::vector<BindingConfig> keybindings;
    std::vector<MouseBindingConfig> mousebindings;
    std::string dividerColor     = "#3d3d3d";
    int dividerWidth             = 1;
    std::string inactivePaneTint = "#000000";
    float inactivePaneTintAlpha  = 0.3f;
    std::string activePaneTint   = "#000000";
    float activePaneTintAlpha    = 0.0f;
    std::string replacementChar  = "\xEF\xBF\xBD"; // U+FFFD
    PaddingConfig padding;
    CursorConfig cursor;
    ColorScheme colors;
    std::string shellIntegration = "auto"; // "auto" | "off"
    // Absolute path to the shell-integration asset root (contains zsh/.zshenv,
    // future bash/, fish/, etc.). Set by main.cpp from Resources::path() so the
    // terminal lib doesn't need to depend on resource resolution. Empty
    // disables injection regardless of `shellIntegration` value.
    std::string shellIntegrationDir;
    // Per-feature opt-out tokens (e.g. "no-cwd"). Joined with spaces and
    // exported as MB_SHELL_INTEGRATION pre-exec; the integration script
    // reads that env var. Users configure via TOML, never the env var.
    std::vector<std::string> shellIntegrationFeatures;

    // Returns the tier1 capacity to pass to Document: INT_MAX when infinite.
    int resolvedScrollback() const
    {
        return scrollbackLines.value_or(INT_MAX);
    }
};

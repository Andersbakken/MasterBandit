#pragma once

#include <glaze/core/common.hpp>
#include <string>
#include <vector>

struct BindingConfig {
    std::vector<std::string> keys;
    std::string              action;
    std::vector<std::string> args;

    struct glaze {
        using T = BindingConfig;
        static constexpr auto value = glz::object(
            "keys",   &T::keys,
            "action", &T::action,
            "args",   &T::args
        );
    };
};

struct TabBarColors {
    std::string background  = "#1a1b26";
    std::string active_bg   = "#7aa2f7";
    std::string active_fg   = "#1a1b26";
    std::string inactive_bg = "#24283b";
    std::string inactive_fg = "#565f89";

    struct glaze {
        using T = TabBarColors;
        static constexpr auto value = glz::object(
            "background",  &T::background,
            "active_bg",   &T::active_bg,
            "active_fg",   &T::active_fg,
            "inactive_bg", &T::inactive_bg,
            "inactive_fg", &T::inactive_fg
        );
    };
};

struct TabBarConfig {
    std::string style    = "powerline";   // "powerline" | "hidden"
    std::string position = "bottom";      // "top" | "bottom"
    std::string font;                     // empty = same as terminal font
    float       font_size = 0.0f;         // 0 = same as terminal font_size
    int         max_title_length = 30;    // 0 = no limit
    TabBarColors colors;

    struct glaze {
        using T = TabBarConfig;
        static constexpr auto value = glz::object(
            "style",            &T::style,
            "position",         &T::position,
            "font",             &T::font,
            "font_size",        &T::font_size,
            "max_title_length", &T::max_title_length,
            "colors",           &T::colors
        );
    };
};

struct Config {
    std::string font;
    float font_size = 16.0f;
    float bold_strength = 0.04f;
    int scrollback_lines = -1; // -1 = infinite
    TabBarConfig tab_bar;
    std::vector<BindingConfig> keybindings;
    std::string divider_color = "#3d3d3d";
    int   divider_width = 1;
    std::string inactive_pane_tint = "#000000";
    float inactive_pane_tint_alpha = 0.3f;
    std::string active_pane_tint = "#000000";
    float active_pane_tint_alpha = 0.0f;

    struct glaze {
        using T = Config;
        static constexpr auto value = glz::object(
            "font", &T::font,
            "font_size", &T::font_size,
            "bold_strength", &T::bold_strength,
            "scrollback_lines", &T::scrollback_lines,
            "tab_bar", &T::tab_bar,
            "keybinding", &T::keybindings,
            "divider_color", &T::divider_color,
            "divider_width", &T::divider_width,
            "inactive_pane_tint", &T::inactive_pane_tint,
            "inactive_pane_tint_alpha", &T::inactive_pane_tint_alpha,
            "active_pane_tint", &T::active_pane_tint,
            "active_pane_tint_alpha", &T::active_pane_tint_alpha
        );
    };
};

Config loadConfig();

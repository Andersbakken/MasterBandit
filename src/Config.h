#pragma once

#include <glaze/core/common.hpp>
#include <string>

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
    TabBarColors colors;

    struct glaze {
        using T = TabBarConfig;
        static constexpr auto value = glz::object(
            "style",     &T::style,
            "position",  &T::position,
            "font",      &T::font,
            "font_size", &T::font_size,
            "colors",    &T::colors
        );
    };
};

struct Config {
    std::string font;
    float font_size = 16.0f;
    float bold_strength = 0.04f;
    int scrollback_lines = -1; // -1 = infinite
    TabBarConfig tab_bar;

    struct glaze {
        using T = Config;
        static constexpr auto value = glz::object(
            "font", &T::font,
            "font_size", &T::font_size,
            "bold_strength", &T::bold_strength,
            "scrollback_lines", &T::scrollback_lines,
            "tab_bar", &T::tab_bar
        );
    };
};

Config loadConfig();

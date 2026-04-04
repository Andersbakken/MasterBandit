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
    std::string style    = "auto";         // "auto" | "visible" | "hidden"
    std::string position = "bottom";      // "top" | "bottom"
    std::string font;                     // empty = same as terminal font
    float       font_size = 0.0f;         // 0 = same as terminal font_size
    int         max_title_length = 30;    // 0 = no limit
    bool        progress_icon = true;    // show nerd font progress icon in tab
    bool        progress_bar  = true;    // show progress bar line at top of pane
    std::string progress_color = "#0099ff"; // progress bar color
    float       progress_height = 3.0f;    // progress bar height in points
    TabBarColors colors;

    struct glaze {
        using T = TabBarConfig;
        static constexpr auto value = glz::object(
            "style",            &T::style,
            "position",         &T::position,
            "font",             &T::font,
            "font_size",        &T::font_size,
            "max_title_length", &T::max_title_length,
            "progress_icon",    &T::progress_icon,
            "progress_bar",     &T::progress_bar,
            "progress_color",   &T::progress_color,
            "progress_height",  &T::progress_height,
            "colors",           &T::colors
        );
    };
};

struct ColorScheme {
    std::string foreground  = "#dddddd";
    std::string background  = "#000000";
    std::string cursor      = "#cccccc";

    // ANSI 16-color palette (0-7 normal, 8-15 bright)
    std::string color0  = "#000000"; // black
    std::string color1  = "#cc0403"; // red
    std::string color2  = "#19cb00"; // green
    std::string color3  = "#cecb00"; // yellow
    std::string color4  = "#0d73cc"; // blue
    std::string color5  = "#cb1ed1"; // magenta
    std::string color6  = "#0dcdcd"; // cyan
    std::string color7  = "#dddddd"; // white
    std::string color8  = "#767676"; // bright black
    std::string color9  = "#f2201f"; // bright red
    std::string color10 = "#23fd00"; // bright green
    std::string color11 = "#fffd00"; // bright yellow
    std::string color12 = "#1a8fff"; // bright blue
    std::string color13 = "#fd28ff"; // bright magenta
    std::string color14 = "#14ffff"; // bright cyan
    std::string color15 = "#ffffff"; // bright white

    struct glaze {
        using T = ColorScheme;
        static constexpr auto value = glz::object(
            "foreground",  &T::foreground,
            "background",  &T::background,
            "cursor",      &T::cursor,
            "color0",  &T::color0,  "color1",  &T::color1,
            "color2",  &T::color2,  "color3",  &T::color3,
            "color4",  &T::color4,  "color5",  &T::color5,
            "color6",  &T::color6,  "color7",  &T::color7,
            "color8",  &T::color8,  "color9",  &T::color9,
            "color10", &T::color10, "color11", &T::color11,
            "color12", &T::color12, "color13", &T::color13,
            "color14", &T::color14, "color15", &T::color15
        );
    };
};

struct PaddingConfig {
    int left   = 0;
    int top    = 6;  // room for progress bar
    int right  = 0;
    int bottom = 0;

    struct glaze {
        using T = PaddingConfig;
        static constexpr auto value = glz::object(
            "left",   &T::left,
            "top",    &T::top,
            "right",  &T::right,
            "bottom", &T::bottom
        );
    };
};

struct Config {
    std::string font;
    float font_size = 16.0f;
    float bold_strength = 0.04f;
    int scrollback_lines = -1; // -1 = infinite
    PaddingConfig padding;
    ColorScheme colors;
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
            "padding", &T::padding,
            "colors", &T::colors,
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

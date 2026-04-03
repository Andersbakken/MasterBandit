#pragma once

#include <glaze/core/common.hpp>
#include <string>

struct Config {
    std::string font;
    float font_size = 16.0f;
    int scrollback_lines = -1; // -1 = infinite

    struct glaze {
        using T = Config;
        static constexpr auto value = glz::object(
            "font", &T::font,
            "font_size", &T::font_size,
            "scrollback_lines", &T::scrollback_lines
        );
    };
};

Config loadConfig();

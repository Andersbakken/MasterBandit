#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace base64 {

inline std::vector<uint8_t> decode(std::string_view input)
{
    static constexpr auto table = []() {
        std::array<uint8_t, 256> t{};
        const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (uint8_t i = 0; i < 64; ++i)
            t[static_cast<uint8_t>(chars[i])] = i;
        return t;
    }();

    std::vector<uint8_t> out;
    out.reserve(input.size() * 3 / 4);
    uint32_t accum = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        accum = (accum << 6) | table[static_cast<uint8_t>(c)];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

inline std::string encode(const uint8_t* data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? table[n & 0x3F] : '=';
    }
    return out;
}

} // namespace base64

namespace color {

// Parse "#RRGGBB" hex string into r, g, b bytes. Returns false on invalid input.
inline bool parseHex(const std::string& hex, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (hex.size() != 7 || hex[0] != '#') return false;
    r = static_cast<uint8_t>(std::stoul(hex.substr(1, 2), nullptr, 16));
    g = static_cast<uint8_t>(std::stoul(hex.substr(3, 2), nullptr, 16));
    b = static_cast<uint8_t>(std::stoul(hex.substr(5, 2), nullptr, 16));
    return true;
}

// Parse "#RRGGBB" to packed uint32: R | G<<8 | B<<16 | 0xFF<<24 (matches CellAttrs::packFgAsU32)
inline uint32_t parseHexRGBA(const std::string& hex, uint32_t def = 0)
{
    uint8_t r, g, b;
    if (!parseHex(hex, r, g, b)) return def;
    return r | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | 0xFF000000u;
}

} // namespace color

namespace unicode {

// Returns true for Unicode general category Zs (space separators)
// and ASCII/C1 whitespace controls. Does not depend on locale.
inline bool isSpace(char32_t cp)
{
    switch (cp) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x0020: case 0x00A0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004:
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: case 0x2009:
    case 0x200A: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

} // namespace unicode

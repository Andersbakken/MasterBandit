#pragma once

#include <cstdint>
#include <string>

namespace utf8 {

inline int encode(char32_t cp, char* out) {
    if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

inline char32_t decode(const char* s, int len, int& bytesConsumed) {
    auto u = [](char c) -> uint8_t { return static_cast<uint8_t>(c); };
    if (len <= 0) { bytesConsumed = 0; return 0; }
    uint8_t b0 = u(s[0]);
    if (b0 < 0x80) { bytesConsumed = 1; return b0; }
    if ((b0 & 0xE0) == 0xC0 && len >= 2) {
        bytesConsumed = 2;
        return ((b0 & 0x1F) << 6) | (u(s[1]) & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0 && len >= 3) {
        bytesConsumed = 3;
        return ((b0 & 0x0F) << 12) | ((u(s[1]) & 0x3F) << 6) | (u(s[2]) & 0x3F);
    }
    if ((b0 & 0xF8) == 0xF0 && len >= 4) {
        bytesConsumed = 4;
        return ((b0 & 0x07) << 18) | ((u(s[1]) & 0x3F) << 12) |
               ((u(s[2]) & 0x3F) << 6) | (u(s[3]) & 0x3F);
    }
    bytesConsumed = 1;
    return 0xFFFD;
}

inline std::string fromCodepoint(char32_t cp) {
    char buf[4];
    int n = encode(cp, buf);
    return std::string(buf, n);
}

} // namespace utf8

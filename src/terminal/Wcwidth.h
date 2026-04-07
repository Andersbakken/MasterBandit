#pragma once

#include <cstdint>

// Returns the display width of a Unicode codepoint:
//   0 for combining/zero-width characters
//   2 for East Asian wide/fullwidth characters
//   1 for everything else (including box-drawing, Latin, etc.)
//  -1 for non-printable control characters
inline int wcwidth(char32_t cp) {
    // C0/C1 control characters (except NUL)
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return -1;

    // Combining characters (zero width)
    // Unicode General Category Mn, Mc, Me and certain special chars
    if ((cp >= 0x0300 && cp <= 0x036f) ||   // Combining Diacriticals
        (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic combining
        (cp >= 0x0591 && cp <= 0x05bd) ||   // Hebrew combining
        cp == 0x05bf ||
        (cp >= 0x05c1 && cp <= 0x05c2) ||
        (cp >= 0x05c4 && cp <= 0x05c5) ||
        cp == 0x05c7 ||
        (cp >= 0x0600 && cp <= 0x0605) ||   // Arabic formatting
        (cp >= 0x0610 && cp <= 0x061a) ||
        (cp >= 0x064b && cp <= 0x065f) ||   // Arabic combining
        cp == 0x0670 ||
        (cp >= 0x06d6 && cp <= 0x06dd) ||
        (cp >= 0x06df && cp <= 0x06e4) ||
        (cp >= 0x06e7 && cp <= 0x06e8) ||
        (cp >= 0x06ea && cp <= 0x06ed) ||
        cp == 0x070f ||
        (cp >= 0x0711 && cp <= 0x0711) ||
        (cp >= 0x0730 && cp <= 0x074a) ||
        (cp >= 0x07a6 && cp <= 0x07b0) ||
        (cp >= 0x07eb && cp <= 0x07f3) ||
        (cp >= 0x0816 && cp <= 0x0819) ||
        (cp >= 0x081b && cp <= 0x0823) ||
        (cp >= 0x0825 && cp <= 0x0827) ||
        (cp >= 0x0829 && cp <= 0x082d) ||
        (cp >= 0x0859 && cp <= 0x085b) ||
        (cp >= 0x08d4 && cp <= 0x08e1) ||
        (cp >= 0x08e3 && cp <= 0x0902) ||
        (cp >= 0x093a && cp <= 0x093a) ||
        cp == 0x093c ||
        (cp >= 0x0941 && cp <= 0x0948) ||
        cp == 0x094d ||
        (cp >= 0x0951 && cp <= 0x0957) ||
        (cp >= 0x0962 && cp <= 0x0963) ||
        cp == 0x0981 ||
        cp == 0x09bc ||
        (cp >= 0x09c1 && cp <= 0x09c4) ||
        cp == 0x09cd ||
        (cp >= 0x09e2 && cp <= 0x09e3) ||
        (cp >= 0x0a01 && cp <= 0x0a02) ||
        cp == 0x0a3c ||
        (cp >= 0x0a41 && cp <= 0x0a42) ||
        (cp >= 0x0a47 && cp <= 0x0a48) ||
        (cp >= 0x0a4b && cp <= 0x0a4d) ||
        cp == 0x0a51 ||
        (cp >= 0x0a70 && cp <= 0x0a71) ||
        cp == 0x0a75 ||
        (cp >= 0x0a81 && cp <= 0x0a82) ||
        cp == 0x0abc ||
        (cp >= 0x0ac1 && cp <= 0x0ac5) ||
        (cp >= 0x0ac7 && cp <= 0x0ac8) ||
        cp == 0x0acd ||
        (cp >= 0x0ae2 && cp <= 0x0ae3) ||
        (cp >= 0x0b01 && cp <= 0x0b01) ||
        cp == 0x0b3c ||
        cp == 0x0b3f ||
        (cp >= 0x0b41 && cp <= 0x0b44) ||
        cp == 0x0b4d ||
        cp == 0x0b56 ||
        (cp >= 0x0b62 && cp <= 0x0b63) ||
        cp == 0x0b82 ||
        cp == 0x0bc0 ||
        cp == 0x0bcd ||
        cp == 0x0c00 ||
        cp == 0x0c3e ||
        (cp >= 0x0c40 && cp <= 0x0c40) ||
        (cp >= 0x0c46 && cp <= 0x0c48) ||
        (cp >= 0x0c4a && cp <= 0x0c4d) ||
        (cp >= 0x0c55 && cp <= 0x0c56) ||
        (cp >= 0x0c62 && cp <= 0x0c63) ||
        cp == 0x0c81 ||
        cp == 0x0cbc ||
        cp == 0x0cbf ||
        cp == 0x0cc6 ||
        (cp >= 0x0ccc && cp <= 0x0ccd) ||
        (cp >= 0x0ce2 && cp <= 0x0ce3) ||
        (cp >= 0x0d01 && cp <= 0x0d01) ||
        (cp >= 0x0d41 && cp <= 0x0d44) ||
        cp == 0x0d4d ||
        (cp >= 0x0d62 && cp <= 0x0d63) ||
        cp == 0x0dca ||
        (cp >= 0x0dd2 && cp <= 0x0dd4) ||
        cp == 0x0dd6 ||
        cp == 0x0e31 ||
        (cp >= 0x0e34 && cp <= 0x0e3a) ||
        (cp >= 0x0e47 && cp <= 0x0e4e) ||
        cp == 0x0eb1 ||
        (cp >= 0x0eb4 && cp <= 0x0eb9) ||
        (cp >= 0x0ebb && cp <= 0x0ebc) ||
        (cp >= 0x0ec8 && cp <= 0x0ecd) ||
        (cp >= 0x0f18 && cp <= 0x0f19) ||
        cp == 0x0f35 ||
        cp == 0x0f37 ||
        cp == 0x0f39 ||
        (cp >= 0x0f71 && cp <= 0x0f7e) ||
        (cp >= 0x0f80 && cp <= 0x0f84) ||
        (cp >= 0x0f86 && cp <= 0x0f87) ||
        (cp >= 0x0f8d && cp <= 0x0f97) ||
        (cp >= 0x0f99 && cp <= 0x0fbc) ||
        cp == 0x0fc6 ||
        (cp >= 0x102d && cp <= 0x1030) ||
        (cp >= 0x1032 && cp <= 0x1037) ||
        (cp >= 0x1039 && cp <= 0x103a) ||
        (cp >= 0x103d && cp <= 0x103e) ||
        (cp >= 0x1058 && cp <= 0x1059) ||
        (cp >= 0x105e && cp <= 0x1060) ||
        (cp >= 0x1071 && cp <= 0x1074) ||
        cp == 0x1082 ||
        (cp >= 0x1085 && cp <= 0x1086) ||
        cp == 0x108d ||
        cp == 0x109d ||
        (cp >= 0x1160 && cp <= 0x11ff) ||   // Hangul Jungseong/Jongseong
        (cp >= 0x135d && cp <= 0x135f) ||
        (cp >= 0x1712 && cp <= 0x1714) ||
        (cp >= 0x1732 && cp <= 0x1734) ||
        (cp >= 0x1752 && cp <= 0x1753) ||
        (cp >= 0x1772 && cp <= 0x1773) ||
        (cp >= 0x17b4 && cp <= 0x17b5) ||
        (cp >= 0x17b7 && cp <= 0x17bd) ||
        cp == 0x17c6 ||
        (cp >= 0x17c9 && cp <= 0x17d3) ||
        cp == 0x17dd ||
        (cp >= 0x180b && cp <= 0x180e) ||
        (cp >= 0x1885 && cp <= 0x1886) ||
        cp == 0x18a9 ||
        (cp >= 0x1920 && cp <= 0x1922) ||
        (cp >= 0x1927 && cp <= 0x1928) ||
        cp == 0x1932 ||
        (cp >= 0x1939 && cp <= 0x193b) ||
        (cp >= 0x1a17 && cp <= 0x1a18) ||
        cp == 0x1a1b ||
        cp == 0x1a56 ||
        (cp >= 0x1a58 && cp <= 0x1a5e) ||
        cp == 0x1a60 ||
        cp == 0x1a62 ||
        (cp >= 0x1a65 && cp <= 0x1a6c) ||
        (cp >= 0x1a73 && cp <= 0x1a7c) ||
        cp == 0x1a7f ||
        (cp >= 0x1ab0 && cp <= 0x1abe) ||
        (cp >= 0x1b00 && cp <= 0x1b03) ||
        cp == 0x1b34 ||
        (cp >= 0x1b36 && cp <= 0x1b3a) ||
        cp == 0x1b3c ||
        cp == 0x1b42 ||
        (cp >= 0x1b6b && cp <= 0x1b73) ||
        (cp >= 0x1b80 && cp <= 0x1b81) ||
        (cp >= 0x1ba2 && cp <= 0x1ba5) ||
        (cp >= 0x1ba8 && cp <= 0x1ba9) ||
        (cp >= 0x1bab && cp <= 0x1bad) ||
        cp == 0x1be6 ||
        (cp >= 0x1be8 && cp <= 0x1be9) ||
        cp == 0x1bed ||
        (cp >= 0x1bef && cp <= 0x1bf1) ||
        (cp >= 0x1c2c && cp <= 0x1c33) ||
        (cp >= 0x1c36 && cp <= 0x1c37) ||
        (cp >= 0x1cd0 && cp <= 0x1cd2) ||
        (cp >= 0x1cd4 && cp <= 0x1ce0) ||
        (cp >= 0x1ce2 && cp <= 0x1ce8) ||
        cp == 0x1ced ||
        cp == 0x1cf4 ||
        (cp >= 0x1cf8 && cp <= 0x1cf9) ||
        (cp >= 0x1dc0 && cp <= 0x1df5) ||
        (cp >= 0x1dfb && cp <= 0x1dff) ||
        (cp >= 0x200b && cp <= 0x200f) ||   // Zero-width space/joiners
        (cp >= 0x202a && cp <= 0x202e) ||   // Bidi formatting
        (cp >= 0x2060 && cp <= 0x2064) ||   // Word joiner etc
        (cp >= 0x2066 && cp <= 0x206f) ||   // Bidi formatting
        (cp >= 0x20d0 && cp <= 0x20f0) ||   // Combining for symbols
        (cp >= 0xfe00 && cp <= 0xfe0f) ||   // Variation selectors
        (cp >= 0xfe20 && cp <= 0xfe2f) ||   // Combining half marks
        cp == 0xfeff ||                      // BOM / ZWNBSP
        (cp >= 0xfff9 && cp <= 0xfffb) ||   // Interlinear annotation
        (cp >= 0x1d167 && cp <= 0x1d169) ||
        (cp >= 0x1d173 && cp <= 0x1d182) ||
        (cp >= 0x1d185 && cp <= 0x1d18b) ||
        (cp >= 0x1d1aa && cp <= 0x1d1ad) ||
        (cp >= 0xe0001 && cp <= 0xe0001) ||
        (cp >= 0xe0020 && cp <= 0xe007f) ||
        (cp >= 0xe0100 && cp <= 0xe01ef))   // Variation selectors supplement
    {
        return 0;
    }

    // East Asian Wide and Fullwidth characters
    if ((cp >= 0x1100 && cp <= 0x115f) ||   // Hangul Jamo
        cp == 0x2329 || cp == 0x232a ||      // Angle brackets
        (cp >= 0x2e80 && cp <= 0x303e) ||   // CJK Radicals..CJK Symbols
        (cp >= 0x3041 && cp <= 0x33bf) ||   // Hiragana..CJK Compatibility
        (cp >= 0x3400 && cp <= 0x4dbf) ||   // CJK Unified Extension A
        (cp >= 0x4e00 && cp <= 0xa4cf) ||   // CJK Unified..Yi Radicals
        (cp >= 0xa960 && cp <= 0xa97c) ||   // Hangul Jamo Extended-A
        (cp >= 0xac00 && cp <= 0xd7a3) ||   // Hangul Syllables
        (cp >= 0xf900 && cp <= 0xfaff) ||   // CJK Compatibility Ideographs
        (cp >= 0xfe10 && cp <= 0xfe19) ||   // Vertical forms
        (cp >= 0xfe30 && cp <= 0xfe6f) ||   // CJK Compatibility Forms
        (cp >= 0xff01 && cp <= 0xff60) ||   // Fullwidth Forms
        (cp >= 0xffe0 && cp <= 0xffe6) ||   // Fullwidth Signs
        (cp >= 0x1f004 && cp <= 0x1f004) || // Mahjong tile
        (cp >= 0x1f0cf && cp <= 0x1f0cf) || // Playing card
        (cp >= 0x1f18e && cp <= 0x1f18e) || // Negative squared AB
        (cp >= 0x1f191 && cp <= 0x1f19a) || // Squared CL..VS
        (cp >= 0x1f200 && cp <= 0x1f202) || // Enclosed ideographs
        (cp >= 0x1f210 && cp <= 0x1f23b) ||
        (cp >= 0x1f240 && cp <= 0x1f248) ||
        (cp >= 0x1f250 && cp <= 0x1f251) ||
        // Widened in Unicode 9 (matches WezTerm's WIDENED_TABLE, BMP subset)
        cp == 0x231a || cp == 0x231b ||      // Watch, hourglass
        (cp >= 0x23e9 && cp <= 0x23ec) ||   // Fast arrows
        cp == 0x23f0 || cp == 0x23f3 ||      // Alarm clock, hourglass
        cp == 0x25fd || cp == 0x25fe ||      // Medium-small squares
        cp == 0x2614 || cp == 0x2615 ||      // Umbrella with rain, hot beverage
        (cp >= 0x2648 && cp <= 0x2653) ||   // Zodiac signs
        cp == 0x267f ||                      // Wheelchair
        cp == 0x2693 ||                      // Anchor
        cp == 0x26a1 ||                      // Lightning bolt
        cp == 0x26aa || cp == 0x26ab ||      // White/black circles
        cp == 0x26bd || cp == 0x26be ||      // Soccer ball, baseball
        cp == 0x26c4 || cp == 0x26c5 ||      // Snowman, sun behind cloud
        cp == 0x26ce || cp == 0x26d4 ||      // Ophiuchus, no entry
        cp == 0x26ea ||                      // Church
        cp == 0x26f2 || cp == 0x26f3 ||      // Fountain, golf
        cp == 0x26f5 || cp == 0x26fa ||      // Sailboat, tent
        cp == 0x26fd ||                      // Fuel pump
        cp == 0x2705 ||                      // White heavy check mark
        cp == 0x270a || cp == 0x270b ||      // Raised fist/hand
        cp == 0x2728 ||                      // Sparkles
        cp == 0x274c || cp == 0x274e ||      // Cross mark
        (cp >= 0x2753 && cp <= 0x2755) ||   // Question marks
        cp == 0x2757 ||                      // Exclamation mark
        (cp >= 0x2795 && cp <= 0x2797) ||   // Plus/minus/division
        cp == 0x27b0 || cp == 0x27bf ||      // Curly loops
        cp == 0x2b1b || cp == 0x2b1c ||      // Large squares
        cp == 0x2b50 || cp == 0x2b55 ||      // Star, hollow red circle
        (cp >= 0x1f300 && cp <= 0x1f64f) || // Misc Symbols..Emoticons
        (cp >= 0x1f680 && cp <= 0x1f6ff) || // Transport/Map Symbols
        (cp >= 0x1f900 && cp <= 0x1f9ff) || // Supplemental Symbols
        (cp >= 0x1fa00 && cp <= 0x1fa6f) || // Chess Symbols
        (cp >= 0x1fa70 && cp <= 0x1faff) || // Symbols Extended-A
        (cp >= 0x20000 && cp <= 0x2fffd) || // CJK Extension B+
        (cp >= 0x30000 && cp <= 0x3fffd))   // CJK Extension G+
    {
        return 2;
    }

    return 1;
}

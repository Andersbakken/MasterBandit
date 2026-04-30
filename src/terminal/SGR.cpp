#include "TerminalEmulator.h"
#include <spdlog/spdlog.h>
#include <assert.h>
#include <vector>
#include <cstdlib>

void TerminalEmulator::processSGR(const char* buf, int len)
{
    assert(len >= 1);
    assert(buf[0] == CSI);
    assert(buf[len - 1] == 'm');

    // Parse semicolon-delimited parameters from buf[1..len-2]
    // e.g. "[0;31m" -> params = {0, 31}
    // Colon sub-params (e.g. "4:3" or "58:2::255:100:100") are stored in the
    // subs[] array.  subCount == 0 means no colon subparams were present.
    static constexpr int MaxSubs = 6; // enough for 58:2:CS:R:G:B
    struct SGRParam {
        int value;
        int subs[MaxSubs];
        int subCount;
    };
    std::vector<SGRParam> params;
    {
        const char* start = buf + 1;
        const char* end = buf + len - 1;
        if (start == end) {
            params.push_back({0, {}, 0});
        } else {
            while (start < end) {
                char* next;
                long val = strtol(start, &next, 10);
                SGRParam p;
                p.value = static_cast<int>(val);
                p.subCount = 0;
                while (next < end && *next == ':' && p.subCount < MaxSubs) {
                    start = next + 1;
                    if (start < end && (*start == ':' || *start == ';' || *start == 'm')) {
                        // Empty sub-param (e.g. the colorspace id in "58:2::R:G:B")
                        p.subs[p.subCount++] = -1;
                        next = const_cast<char*>(start);
                    } else {
                        p.subs[p.subCount++] = static_cast<int>(strtol(start, &next, 10));
                    }
                }
                params.push_back(p);
                if (next < end && *next == ';') {
                    start = next + 1;
                } else {
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < params.size(); ++i) {
        int p = params[i].value;

        switch (p) {
        case 0: // Reset
            mState->currentAttrs.reset();
            mState->currentUnderlineColor = 0;
            break;
        case 1: mState->currentAttrs.setBold(true); break;
        case 2: mState->currentAttrs.setDim(true); break;
        case 3: mState->currentAttrs.setItalic(true); break;
        case 4: { // Underline (with optional style sub-param)
            int style = params[i].subCount > 0 ? params[i].subs[0] : -1;
            if (style == 0) {
                mState->currentAttrs.setUnderline(false);
                mState->currentAttrs.setUnderlineStyle(0);
            } else {
                mState->currentAttrs.setUnderline(true);
                switch (style) {
                case 1: case -1: mState->currentAttrs.setUnderlineStyle(0); break; // straight (default)
                case 2: mState->currentAttrs.setUnderlineStyle(1); break; // double
                case 3: mState->currentAttrs.setUnderlineStyle(2); break; // curly
                case 4: mState->currentAttrs.setUnderlineStyle(3); break; // dotted
                case 5: mState->currentAttrs.setUnderlineStyle(3); break; // dashed (share with dotted)
                default: mState->currentAttrs.setUnderlineStyle(0); break;
                }
            }
            break;
        }
        case 5: mState->currentAttrs.setBlink(true); break;
        case 7: mState->currentAttrs.setInverse(true); break;
        case 8: mState->currentAttrs.setInvisible(true); break;
        case 9: mState->currentAttrs.setStrikethrough(true); break;

        case 21: // doubly underlined or bold off (varies)
        case 22: mState->currentAttrs.setBold(false); mState->currentAttrs.setDim(false); break;
        case 23: mState->currentAttrs.setItalic(false); break;
        case 24: mState->currentAttrs.setUnderline(false); mState->currentAttrs.setUnderlineStyle(0); break;
        case 25: mState->currentAttrs.setBlink(false); break;
        case 27: mState->currentAttrs.setInverse(false); break;
        case 28: mState->currentAttrs.setInvisible(false); break;
        case 29: mState->currentAttrs.setStrikethrough(false); break;

        // Foreground standard colors (30-37)
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37: {
            int idx = p - 30;
            mState->currentAttrs.setFg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mState->currentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        case 38: // Extended foreground
            if (params[i].subCount >= 2 && params[i].subs[0] == 5) {
                // Colon form: 38:5:IDX
                uint8_t r, g, b;
                color256ToRGB(params[i].subs[1], r, g, b);
                mState->currentAttrs.setFg(r, g, b);
                mState->currentAttrs.setFgMode(CellAttrs::RGB);
            } else if (params[i].subCount >= 4 && params[i].subs[0] == 2) {
                // Colon form: 38:2:CS:R:G:B or 38:2:R:G:B
                // If subCount >= 5, subs[1] is colorspace (ignored), R=subs[2..4]
                // If subCount == 4, no colorspace, R=subs[1..3]
                int off = params[i].subCount >= 5 ? 2 : 1;
                mState->currentAttrs.setFg(
                    static_cast<uint8_t>(params[i].subs[off] & 0xFF),
                    static_cast<uint8_t>(params[i].subs[off + 1] & 0xFF),
                    static_cast<uint8_t>(params[i].subs[off + 2] & 0xFF));
                mState->currentAttrs.setFgMode(CellAttrs::RGB);
            } else if (i + 1 < params.size()) {
                // Semicolon form: 38;5;IDX or 38;2;R;G;B
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mState->currentAttrs.setFg(r, g, b);
                    mState->currentAttrs.setFgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mState->currentAttrs.setFg(
                        static_cast<uint8_t>(params[i + 2].value),
                        static_cast<uint8_t>(params[i + 3].value),
                        static_cast<uint8_t>(params[i + 4].value));
                    mState->currentAttrs.setFgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 39: // Default foreground
            mState->currentAttrs.setFgMode(CellAttrs::Default);
            break;

        // Background standard colors (40-47)
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47: {
            int idx = p - 40;
            mState->currentAttrs.setBg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mState->currentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        case 48: // Extended background
            if (params[i].subCount >= 2 && params[i].subs[0] == 5) {
                // Colon form: 48:5:IDX
                uint8_t r, g, b;
                color256ToRGB(params[i].subs[1], r, g, b);
                mState->currentAttrs.setBg(r, g, b);
                mState->currentAttrs.setBgMode(CellAttrs::RGB);
            } else if (params[i].subCount >= 4 && params[i].subs[0] == 2) {
                // Colon form: 48:2:CS:R:G:B or 48:2:R:G:B
                int off = params[i].subCount >= 5 ? 2 : 1;
                mState->currentAttrs.setBg(
                    static_cast<uint8_t>(params[i].subs[off] & 0xFF),
                    static_cast<uint8_t>(params[i].subs[off + 1] & 0xFF),
                    static_cast<uint8_t>(params[i].subs[off + 2] & 0xFF));
                mState->currentAttrs.setBgMode(CellAttrs::RGB);
            } else if (i + 1 < params.size()) {
                // Semicolon form: 48;5;IDX or 48;2;R;G;B
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mState->currentAttrs.setBg(r, g, b);
                    mState->currentAttrs.setBgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mState->currentAttrs.setBg(
                        static_cast<uint8_t>(params[i + 2].value),
                        static_cast<uint8_t>(params[i + 3].value),
                        static_cast<uint8_t>(params[i + 4].value));
                    mState->currentAttrs.setBgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 49: // Default background
            mState->currentAttrs.setBgMode(CellAttrs::Default);
            break;

        case 58: // Underline color
            if (params[i].subCount >= 2 && params[i].subs[0] == 5) {
                // Colon form: 58:5:IDX
                uint8_t r, g, b;
                color256ToRGB(params[i].subs[1], r, g, b);
                mState->currentUnderlineColor = static_cast<uint32_t>(r)
                    | (static_cast<uint32_t>(g) << 8)
                    | (static_cast<uint32_t>(b) << 16)
                    | 0xFF000000u;
            } else if (params[i].subCount >= 4 && params[i].subs[0] == 2) {
                // Colon form: 58:2:CS:R:G:B or 58:2:R:G:B
                int off = params[i].subCount >= 5 ? 2 : 1;
                mState->currentUnderlineColor = static_cast<uint32_t>(params[i].subs[off] & 0xFF)
                    | (static_cast<uint32_t>(params[i].subs[off + 1] & 0xFF) << 8)
                    | (static_cast<uint32_t>(params[i].subs[off + 2] & 0xFF) << 16)
                    | 0xFF000000u;
            } else if (i + 1 < params.size()) {
                // Semicolon form: 58;5;IDX or 58;2;R;G;B
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mState->currentUnderlineColor = static_cast<uint32_t>(r)
                        | (static_cast<uint32_t>(g) << 8)
                        | (static_cast<uint32_t>(b) << 16)
                        | 0xFF000000u;
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mState->currentUnderlineColor = static_cast<uint32_t>(params[i + 2].value & 0xFF)
                        | (static_cast<uint32_t>(params[i + 3].value & 0xFF) << 8)
                        | (static_cast<uint32_t>(params[i + 4].value & 0xFF) << 16)
                        | 0xFF000000u;
                    i += 4;
                }
            }
            break;

        case 59: // Reset underline color
            mState->currentUnderlineColor = 0;
            break;

        // Bright foreground (90-97)
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97: {
            int idx = p - 90 + 8;
            mState->currentAttrs.setFg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mState->currentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        // Bright background (100-107)
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107: {
            int idx = p - 100 + 8;
            mState->currentAttrs.setBg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mState->currentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        default:
            spdlog::debug("Unhandled SGR param {}", p);
            break;
        }
    }
}

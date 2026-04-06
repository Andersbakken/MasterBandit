#include "TerminalEmulator.h"
#include <spdlog/spdlog.h>
#include <assert.h>
#include <vector>
#include <cstdlib>

void TerminalEmulator::processSGR()
{
    assert(mEscapeBuffer[0] == CSI);
    assert(mEscapeBuffer[mEscapeIndex - 1] == 'm');

    // Parse semicolon-delimited parameters from mEscapeBuffer[1..mEscapeIndex-2]
    // e.g. "[0;31m" -> params = {0, 31}
    // Colon sub-params (e.g. "4:3") are encoded as negative: -4003
    // meaning param 4 with sub-param 3.
    struct SGRParam { int value; int subparam; }; // subparam = -1 if none
    std::vector<SGRParam> params;
    {
        const char* start = mEscapeBuffer + 1;
        const char* end = mEscapeBuffer + mEscapeIndex - 1;
        if (start == end) {
            params.push_back({0, -1});
        } else {
            while (start < end) {
                char* next;
                long val = strtol(start, &next, 10);
                int sub = -1;
                if (next < end && *next == ':') {
                    // Colon sub-parameter
                    start = next + 1;
                    sub = static_cast<int>(strtol(start, &next, 10));
                }
                params.push_back({static_cast<int>(val), sub});
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
            mCurrentAttrs.reset();
            mCurrentUnderlineColor = 0;
            break;
        case 1: mCurrentAttrs.setBold(true); break;
        case 2: mCurrentAttrs.setDim(true); break;
        case 3: mCurrentAttrs.setItalic(true); break;
        case 4: // Underline (with optional style sub-param)
            if (params[i].subparam == 0) {
                mCurrentAttrs.setUnderline(false);
                mCurrentAttrs.setUnderlineStyle(0);
            } else {
                mCurrentAttrs.setUnderline(true);
                switch (params[i].subparam) {
                case 1: case -1: mCurrentAttrs.setUnderlineStyle(0); break; // straight (default)
                case 2: mCurrentAttrs.setUnderlineStyle(1); break; // double
                case 3: mCurrentAttrs.setUnderlineStyle(2); break; // curly
                case 4: mCurrentAttrs.setUnderlineStyle(3); break; // dotted
                case 5: mCurrentAttrs.setUnderlineStyle(3); break; // dashed (share with dotted)
                default: mCurrentAttrs.setUnderlineStyle(0); break;
                }
            }
            break;
        case 5: mCurrentAttrs.setBlink(true); break;
        case 7: mCurrentAttrs.setInverse(true); break;
        case 8: mCurrentAttrs.setInvisible(true); break;
        case 9: mCurrentAttrs.setStrikethrough(true); break;

        case 21: // doubly underlined or bold off (varies)
        case 22: mCurrentAttrs.setBold(false); mCurrentAttrs.setDim(false); break;
        case 23: mCurrentAttrs.setItalic(false); break;
        case 24: mCurrentAttrs.setUnderline(false); mCurrentAttrs.setUnderlineStyle(0); break;
        case 25: mCurrentAttrs.setBlink(false); break;
        case 27: mCurrentAttrs.setInverse(false); break;
        case 28: mCurrentAttrs.setInvisible(false); break;
        case 29: mCurrentAttrs.setStrikethrough(false); break;

        // Foreground standard colors (30-37)
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37: {
            int idx = p - 30;
            mCurrentAttrs.setFg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mCurrentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        case 38: // Extended foreground
            if (i + 1 < params.size()) {
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mCurrentAttrs.setFg(r, g, b);
                    mCurrentAttrs.setFgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mCurrentAttrs.setFg(
                        static_cast<uint8_t>(params[i + 2].value),
                        static_cast<uint8_t>(params[i + 3].value),
                        static_cast<uint8_t>(params[i + 4].value));
                    mCurrentAttrs.setFgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 39: // Default foreground
            mCurrentAttrs.setFgMode(CellAttrs::Default);
            break;

        // Background standard colors (40-47)
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47: {
            int idx = p - 40;
            mCurrentAttrs.setBg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mCurrentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        case 48: // Extended background
            if (i + 1 < params.size()) {
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mCurrentAttrs.setBg(r, g, b);
                    mCurrentAttrs.setBgMode(CellAttrs::RGB);
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mCurrentAttrs.setBg(
                        static_cast<uint8_t>(params[i + 2].value),
                        static_cast<uint8_t>(params[i + 3].value),
                        static_cast<uint8_t>(params[i + 4].value));
                    mCurrentAttrs.setBgMode(CellAttrs::RGB);
                    i += 4;
                }
            }
            break;

        case 49: // Default background
            mCurrentAttrs.setBgMode(CellAttrs::Default);
            break;

        case 58: // Underline color
            if (i + 1 < params.size()) {
                if (params[i + 1].value == 5 && i + 2 < params.size()) {
                    uint8_t r, g, b;
                    color256ToRGB(params[i + 2].value, r, g, b);
                    mCurrentUnderlineColor = static_cast<uint32_t>(r)
                        | (static_cast<uint32_t>(g) << 8)
                        | (static_cast<uint32_t>(b) << 16)
                        | 0xFF000000u;
                    i += 2;
                } else if (params[i + 1].value == 2 && i + 4 < params.size()) {
                    mCurrentUnderlineColor = static_cast<uint32_t>(params[i + 2].value & 0xFF)
                        | (static_cast<uint32_t>(params[i + 3].value & 0xFF) << 8)
                        | (static_cast<uint32_t>(params[i + 4].value & 0xFF) << 16)
                        | 0xFF000000u;
                    i += 4;
                }
            }
            break;

        case 59: // Reset underline color
            mCurrentUnderlineColor = 0;
            break;

        // Bright foreground (90-97)
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97: {
            int idx = p - 90 + 8;
            mCurrentAttrs.setFg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mCurrentAttrs.setFgMode(CellAttrs::RGB);
            break;
        }

        // Bright background (100-107)
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107: {
            int idx = p - 100 + 8;
            mCurrentAttrs.setBg(m16ColorPalette[idx][0], m16ColorPalette[idx][1], m16ColorPalette[idx][2]);
            mCurrentAttrs.setBgMode(CellAttrs::RGB);
            break;
        }

        default:
            spdlog::debug("Unhandled SGR param {}", p);
            break;
        }
    }
}

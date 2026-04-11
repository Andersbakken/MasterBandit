#include "TerminalEmulator.h"
#include <spdlog/spdlog.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// ---------------------------------------------------------------------------
// XTGETTCAP capability table
// Values use the same escape notation as terminfo sources (\E = ESC, ^X = ctrl).
// We only advertise capabilities that this terminal actually implements.
// ---------------------------------------------------------------------------

static const std::unordered_map<std::string, std::string>& xtgettcapTable()
{
    // clang-format off
    static const std::unordered_map<std::string, std::string> table = {
        // Terminal identity
        { "TN",       "MasterBandit" },
        { "name",     "MasterBandit" },

        // MasterBandit-specific capabilities
        { "mb-query-popup",   "" },  // supports OSC 58237 popup pane API
        { "mb-query-applet",  "" },  // supports OSC 58237 JS applet loading

        // Boolean caps (returned as empty string = present)
        { "Tc",       "" },   // true-color (tmux extension)
        { "Su",       "" },   // styled/colored underlines
        { "fullkbd",  "" },   // kitty full keyboard protocol
        { "XF",       "" },   // focus-in/out events
        { "am",       "" },   // auto right-margin
        { "km",       "" },   // has meta key

        // Synchronized output (tmux/kitty extension)
        { "Sync",     "\x1bP=%p1%ds\x1b\\" },

        // Bracketed paste
        { "BE",       "\x1b[?2004h" },
        { "BD",       "\x1b[?2004l" },
        { "PS",       "\x1b[200~"   },
        { "PE",       "\x1b[201~"   },

        // Focus events
        { "fe",       "\x1b[?1004h" },
        { "fd",       "\x1b[?1004l" },

        // XTVERSION / DA2
        { "XR",       "\x1b[>0q" },
        { "RV",       "\x1b[>c"  },

        // Cursor visibility / style
        { "civis",    "\x1b[?25l"       },
        { "cnorm",    "\x1b[?12h\x1b[?25h" },
        { "cvvis",    "\x1b[?12;25h"    },
        { "Ss",       "\x1b[%p1%d q"   },
        { "Se",       "\x1b[2 q"       },

        // Alternate screen
        { "smcup",    "\x1b[?1049h" },
        { "rmcup",    "\x1b[?1049l" },

        // Application keypad mode
        { "smkx",     "\x1b[?1h" },
        { "rmkx",     "\x1b[?1l" },

        // Cursor movement
        { "cup",      "\x1b[%i%p1%d;%p2%dH" },
        { "hpa",      "\x1b[%i%p1%dG"        },
        { "vpa",      "\x1b[%i%p1%dd"        },
        { "cuu",      "\x1b[%p1%dA"          },
        { "cud",      "\x1b[%p1%dB"          },
        { "cuf",      "\x1b[%p1%dC"          },
        { "cub",      "\x1b[%p1%dD"          },
        { "cuu1",     "\x1b[A"               },
        { "cud1",     "\n"                   },
        { "cuf1",     "\x1b[C"               },
        { "cub1",     "\x08"                 },

        // Erase
        { "clear",    "\x1b[H\x1b[2J" },
        { "ed",       "\x1b[J"        },
        { "el",       "\x1b[K"        },
        { "el1",      "\x1b[1K"       },
        { "ech",      "\x1b[%p1%dX"  },

        // Scroll region / scroll
        { "csr",      "\x1b[%i%p1%d;%p2%dr" },
        { "ind",      "\n"                    },
        { "ri",       "\x1bM"                },
        { "indn",     "\x1b[%p1%dS"          },
        { "rin",      "\x1b[%p1%dT"          },

        // Insert / delete
        { "il",       "\x1b[%p1%dL" },
        { "il1",      "\x1b[L"      },
        { "dl",       "\x1b[%p1%dM" },
        { "dl1",      "\x1b[M"      },
        { "dch",      "\x1b[%p1%dP" },
        { "dch1",     "\x1b[P"      },
        { "ich",      "\x1b[%p1%d@" },

        // REP
        { "rep",      "%p1%c\x1b[%p2%{1}%-%db" },

        // Save / restore cursor
        { "sc",       "\x1b""7" },
        { "rc",       "\x1b""8" },

        // SGR
        { "bold",     "\x1b[1m"  },
        { "dim",      "\x1b[2m"  },
        { "sitm",     "\x1b[3m"  },
        { "ritm",     "\x1b[23m" },
        { "smul",     "\x1b[4m"  },
        { "rmul",     "\x1b[24m" },
        { "Smulx",    "\x1b[4:%p1%dm" },
        { "rev",      "\x1b[7m"  },
        { "smso",     "\x1b[7m"  },
        { "rmso",     "\x1b[27m" },
        { "blink",    "\x1b[5m"  },
        { "smxx",     "\x1b[9m"  },
        { "rmxx",     "\x1b[29m" },
        { "sgr0",     "\x1b[m"   },
        { "op",       "\x1b[39;49m" },

        // Underline color (kitty extension)
        { "Setulc",   "\x1b[58:2:%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm" },

        // 256-color + RGB
        { "setaf",    "\x1b[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m" },
        { "setab",    "\x1b[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m" },
        { "setrgbf",  "\x1b[38:2:%p1%d:%p2%d:%p3%dm" },
        { "setrgbb",  "\x1b[48:2:%p1%d:%p2%d:%p3%dm" },

        // Misc
        { "bel",      "\x07" },
        { "cr",       "\r"   },
        { "ht",       "\t"   },
        { "hts",      "\x1bH" },
        { "tbc",      "\x1b[3g" },

        // CPR / DA queries (u6-u9 convention)
        { "u6",       "\x1b[%i%d;%dR" },
        { "u7",       "\x1b[6n"       },
        { "u8",       "\x1b[?%[;0123456789]c" },
        { "u9",       "\x1b[c"        },

        // Numeric caps
        { "colors",   "256" },
        { "it",       "8"   },
    };
    // clang-format on
    return table;
}

// Decode a hex string into bytes. Returns false on invalid input.
static bool hexDecode(std::string_view hex, std::string& out)
{
    if (hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto hexNibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

// Hex-encode bytes.
static std::string hexEncode(std::string_view in)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        out += digits[c >> 4];
        out += digits[c & 0xf];
    }
    return out;
}

void TerminalEmulator::processDCS()
{
    std::string_view seq(mStringSequence);

    // XTGETTCAP: DCS + q <hex-names> ST  (names separated by ';')
    if (seq.size() >= 2 && seq[0] == '+' && seq[1] == 'q') {
        std::string_view queries = seq.substr(2);
        const auto& table = xtgettcapTable();

        auto handleOne = [&](std::string_view hexName) {
            std::string name;
            if (!hexDecode(hexName, name)) {
                spdlog::warn("XTGETTCAP: invalid hex in query '{}'", std::string(hexName));
                std::string resp = "\x1bP0+r";
                resp += hexName;
                resp += "\x1b\\";
                writeToOutput(resp.c_str(), resp.size());
                return;
            }

            std::string resp = "\x1bP";
            auto it = table.find(name);
            if (it != table.end()) {
                resp += "1+r";
                resp += hexName;
                if (!it->second.empty()) {
                    resp += '=';
                    resp += hexEncode(it->second);
                }
            } else if (mCallbacks.customTcapLookup) {
                auto custom = mCallbacks.customTcapLookup(name);
                if (custom.has_value()) {
                    resp += "1+r";
                    resp += hexName;
                    if (!custom->empty()) {
                        resp += '=';
                        resp += hexEncode(*custom);
                    }
                } else {
                    spdlog::debug("XTGETTCAP: unknown cap '{}'", name);
                    resp += "0+r";
                    resp += hexName;
                }
            } else {
                spdlog::debug("XTGETTCAP: unknown cap '{}'", name);
                resp += "0+r";
                resp += hexName;
            }
            resp += "\x1b\\";
            writeToOutput(resp.c_str(), resp.size());
        };

        size_t pos = 0;
        while (pos <= queries.size()) {
            size_t semi = queries.find(';', pos);
            if (semi == std::string_view::npos) {
                if (pos < queries.size())
                    handleOne(queries.substr(pos));
                break;
            }
            handleOne(queries.substr(pos, semi - pos));
            pos = semi + 1;
        }
        return;
    }

    // DECRQSS: DCS $ q <subparam> ST — query current terminal state
    if (seq.size() >= 2 && seq[0] == '$' && seq[1] == 'q') {
        std::string_view sub = seq.substr(2);
        if (sub == " q") {
            // Cursor shape query (DECSCUSR)
            // CursorBlock=0 maps to Ps=1 (blinking block); other values already match DECSCUSR
            int ps = static_cast<int>(mCursorShape);
            if (ps == 0) ps = 1;
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1bP1$r%d q\x1b\\", ps);
            writeToOutput(resp, len);
        } else if (sub == "m") {
            // SGR state query
            std::string params = buildCurrentSGR();
            std::string resp = "\x1bP1$r" + params + "m\x1b\\";
            writeToOutput(resp.c_str(), resp.size());
        } else if (sub == "r") {
            // Scroll margins query (DECSTBM)
            // mScrollTop is 0-indexed; mScrollBottom is exclusive upper bound
            char resp[64];
            int len = snprintf(resp, sizeof(resp), "\x1bP1$r%d;%dr\x1b\\",
                mScrollTop + 1, mScrollBottom);
            writeToOutput(resp, len);
        } else {
            // Unknown subparam
            const char* resp = "\x1bP0$r\x1b\\";
            writeToOutput(resp, strlen(resp));
        }
        return;
    }

    spdlog::debug("Ignoring DCS sequence: {}", std::string(seq.substr(0, std::min(seq.size(), size_t(40)))));
}

// Serialize current SGR pen state to a semicolon-separated parameter string.
// Returns "0" if no attributes are active (reset state).
std::string TerminalEmulator::buildCurrentSGR() const
{
    std::string p;
    auto add = [&](int n) {
        if (!p.empty()) p += ';';
        p += std::to_string(n);
    };

    // Text attributes
    if (mCurrentAttrs.bold())          add(1);
    if (mCurrentAttrs.dim())           add(2);
    if (mCurrentAttrs.italic())        add(3);
    if (mCurrentAttrs.underline())     add(4);
    if (mCurrentAttrs.blink())         add(5);
    if (mCurrentAttrs.inverse())       add(7);
    if (mCurrentAttrs.invisible())     add(8);
    if (mCurrentAttrs.strikethrough()) add(9);

    // Foreground color
    switch (mCurrentAttrs.fgMode()) {
    case CellAttrs::Default: break;
    case CellAttrs::Indexed: {
        int idx = mCurrentAttrs.fgR();
        if (idx < 8)       add(30 + idx);
        else if (idx < 16) add(90 + (idx - 8));
        else               { add(38); add(5); add(idx); }
        break;
    }
    case CellAttrs::RGB:
        add(38); add(2);
        add(mCurrentAttrs.fgR()); add(mCurrentAttrs.fgG()); add(mCurrentAttrs.fgB());
        break;
    }

    // Background color
    switch (mCurrentAttrs.bgMode()) {
    case CellAttrs::Default: break;
    case CellAttrs::Indexed: {
        int idx = mCurrentAttrs.bgR();
        if (idx < 8)       add(40 + idx);
        else if (idx < 16) add(100 + (idx - 8));
        else               { add(48); add(5); add(idx); }
        break;
    }
    case CellAttrs::RGB:
        add(48); add(2);
        add(mCurrentAttrs.bgR()); add(mCurrentAttrs.bgG()); add(mCurrentAttrs.bgB());
        break;
    }

    return p.empty() ? "0" : p;
}

#include "TerminalEmulator.h"
#include "Terminfo.h"
#include <algorithm>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// XTGETTCAP capability table
// Values are stored in terminfo source notation (\E = ESC, ^X = control char,
// ^? = DEL) and transmitted as-is over the wire — apps decode source format
// before passing to tparm(). Only advertise caps the terminal actually
// implements (advertising a sequence MB doesn't accept will mislead clients).
// ---------------------------------------------------------------------------

static const std::unordered_map<std::string, std::string> &xtgettcapTable()
{
    // clang-format off
    static const std::unordered_map<std::string, std::string> table = {
        // Terminal identity
        { "TN",       "MasterBandit" },
        { "name",     "MasterBandit" },

        // MasterBandit-specific capabilities (mb-query-* mirrors kitty-query-*).
        // Boolean caps signal "feature present" so applets/shells can probe
        // the full feature set in a single XTGETTCAP round-trip.
        { "mb-query-popup",            "" },  // OSC 58237 popup pane API
        { "mb-query-applet",           "" },  // OSC 58237 JS applet loading
        { "mb-query-hyperlinks",       "" },  // OSC 8 hyperlinks
        { "mb-query-cwd",              "" },  // OSC 7 working directory
        { "mb-query-title-stack",      "" },  // XTWINOPS 22/23 title push/pop
        { "mb-query-kitty-graphics",   "" },  // kitty graphics protocol
        { "mb-query-kitty-keyboard",   "" },  // kitty keyboard protocol
        { "mb-query-color-scheme",     "" },  // mode 2031 color-scheme notify
        { "mb-query-decstr",           "" },  // CSI ! p soft reset
        { "mb-query-osc133",           "" },  // OSC 133 shell integration
        { "mb-query-osc52",            "" },  // OSC 52 clipboard
        { "mb-query-sync",             "" },  // mode 2026 sync output

        // Boolean caps
        { "Tc",       "" },   // true-color (tmux extension)
        { "Su",       "" },   // styled/colored underlines
        { "Sync",     "" },   // sync output (boolean — mode 2026 only; DCS form not implemented)
        { "fullkbd",  "" },   // kitty full keyboard protocol
        { "XF",       "" },   // focus-in/out events
        { "am",       "" },   // auto right-margin
        { "bce",      "" },   // back-color erase (erase ops fill with current SGR bg)
        { "bw",       "" },   // backspace wraps from col 0 to last col of prev line
        { "ccc",      "" },   // can change colors (OSC 4 set)
        { "km",       "" },   // has meta key
        { "mc5i",     "" },   // printer will not echo on screen
        { "mir",      "" },   // safe to move while in insert mode
        { "msgr",     "" },   // safe to move while in standout mode
        { "npc",      "" },   // pad character does not exist
        { "xenl",     "" },   // newline glitch (cursor stays at end-of-line)
        { "hs",       "" },   // has status line (window title)

        // Bracketed paste
        { "BE",       "\\E[?2004h" },
        { "BD",       "\\E[?2004l" },
        { "PS",       "\\E[200~"   },
        { "PE",       "\\E[201~"   },

        // Focus events
        { "fe",       "\\E[?1004h" },
        { "fd",       "\\E[?1004l" },

        // Status line / title (OSC 2 with BEL terminator)
        { "tsl",      "\\E]2;" },
        { "fsl",      "^G"     },

        // XTVERSION / DA2
        { "XR",       "\\E[>0q" },
        { "RV",       "\\E[>c"  },

        // Cursor visibility / style
        { "civis",    "\\E[?25l"            },
        { "cnorm",    "\\E[?12h\\E[?25h"    },
        { "cvvis",    "\\E[?12;25h"         },
        { "Ss",       "\\E[%p1%d q"         },
        { "Se",       "\\E[2 q"             },

        // Alternate screen
        { "smcup",    "\\E[?1049h" },
        { "rmcup",    "\\E[?1049l" },

        // Application cursor keys mode (DECCKM)
        { "smkx",     "\\E[?1h" },
        { "rmkx",     "\\E[?1l" },

        // Cursor movement
        { "cup",      "\\E[%i%p1%d;%p2%dH" },
        { "hpa",      "\\E[%i%p1%dG"       },
        { "vpa",      "\\E[%i%p1%dd"       },
        { "cuu",      "\\E[%p1%dA"         },
        { "cud",      "\\E[%p1%dB"         },
        { "cuf",      "\\E[%p1%dC"         },
        { "cub",      "\\E[%p1%dD"         },
        { "cuu1",     "\\E[A"              },
        { "cud1",     "^J"                 },
        { "cuf1",     "\\E[C"              },
        { "cub1",     "^H"                 },

        // Erase
        { "clear",    "\\E[H\\E[2J" },
        { "ed",       "\\E[J"       },
        { "el",       "\\E[K"       },
        { "el1",      "\\E[1K"      },
        { "ech",      "\\E[%p1%dX"  },

        // Scroll region / scroll
        { "csr",      "\\E[%i%p1%d;%p2%dr" },
        { "ind",      "^J"                 },
        { "ri",       "\\EM"               },
        { "indn",     "\\E[%p1%dS"         },
        { "rin",      "\\E[%p1%dT"         },

        // Insert / delete
        { "il",       "\\E[%p1%dL" },
        { "il1",      "\\E[L"      },
        { "dl",       "\\E[%p1%dM" },
        { "dl1",      "\\E[M"      },
        { "dch",      "\\E[%p1%dP" },
        { "dch1",     "\\E[P"      },
        { "ich",      "\\E[%p1%d@" },

        // REP
        { "rep",      "%p1%c\\E[%p2%{1}%-%db" },

        // Save / restore cursor
        { "sc",       "\\E7" },
        { "rc",       "\\E8" },

        // SGR
        { "bold",     "\\E[1m"        },
        { "dim",      "\\E[2m"        },
        { "sitm",     "\\E[3m"        },
        { "ritm",     "\\E[23m"       },
        { "smul",     "\\E[4m"        },
        { "rmul",     "\\E[24m"       },
        { "Smulx",    "\\E[4:%p1%dm"  },
        { "rev",      "\\E[7m"        },
        { "smso",     "\\E[7m"        },
        { "rmso",     "\\E[27m"       },
        { "blink",    "\\E[5m"        },
        { "smxx",     "\\E[9m"        },
        { "rmxx",     "\\E[29m"       },
        { "sgr0",     "\\E[m"         },
        { "op",       "\\E[39;49m"    },

        // Underline color (kitty extension)
        { "Setulc",   "\\E[58:2:%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm" },

        // 256-color + RGB
        { "setaf",    "\\E[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m" },
        { "setab",    "\\E[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d%e48;5;%p1%d%;m" },
        { "setrgbf",  "\\E[38:2:%p1%d:%p2%d:%p3%dm" },
        { "setrgbb",  "\\E[48:2:%p1%d:%p2%d:%p3%dm" },

        // Reset
        // is2: soft init (DECSTR — preserves cursor pos, screen content, alt-screen)
        // rs1: terminate any in-flight OSC, then full reset (kitty convention)
        // rs2: full reset (RIS — TerminalEmulator.cpp:1616-1646)
        { "is2",      "\\E[!p"        },
        { "rs1",      "\\E]\\E\\\\\\Ec" },
        { "rs2",      "\\Ec"          },

        // Misc
        { "bel",      "^G"     },
        { "cr",       "^M"     },
        { "ht",       "^I"     },
        { "hts",      "\\EH"   },
        { "tbc",      "\\E[3g" },
        { "cbt",      "\\E[Z"  },  // back-tab — TerminalEmulator.cpp:2409

        // Keyboard input — values are what MB sends to the application when
        // the corresponding key is pressed. Legacy mode only; modifier-encoded
        // forms (kRIT/kf13+/etc.) are not advertised because Keyboard.cpp's
        // legacy path emits unmodified sequences only — modifier encoding kicks
        // in solely under kitty keyboard protocol (mKittyFlags != 0).
        { "kbs",      "^?"      },  // backspace → DEL (Keyboard.cpp:62)
        { "kcuu1",    "\\E[A"   },
        { "kcud1",    "\\E[B"   },
        { "kcuf1",    "\\E[C"   },
        { "kcub1",    "\\E[D"   },
        { "khome",    "\\E[H"   },
        { "kend",     "\\E[F"   },
        { "kpp",      "\\E[5~"  },
        { "knp",      "\\E[6~"  },
        { "kich1",    "\\E[2~"  },
        { "kdch1",    "\\E[3~"  },
        { "kf1",      "\\EOP"   },
        { "kf2",      "\\EOQ"   },
        { "kf3",      "\\EOR"   },
        { "kf4",      "\\EOS"   },
        { "kf5",      "\\E[15~" },
        { "kf6",      "\\E[17~" },
        { "kf7",      "\\E[18~" },
        { "kf8",      "\\E[19~" },
        { "kf9",      "\\E[20~" },
        { "kf10",     "\\E[21~" },
        { "kf11",     "\\E[23~" },
        // kf12 omitted: Keyboard.cpp:39-41 swallows F12 (debug binding).

        // Shift-modified navigation (legacy modifier-encoded forms — Keyboard.cpp).
        { "kcbt",     "\\E[Z"     },  // back-tab (shift-Tab)
        { "kRIT",     "\\E[1;2C"  },  // shift-right
        { "kLFT",     "\\E[1;2D"  },  // shift-left
        { "kUP",      "\\E[1;2A"  },  // shift-up
        { "kDN",      "\\E[1;2B"  },  // shift-down
        { "kHOM",     "\\E[1;2H"  },  // shift-home
        { "kEND",     "\\E[1;2F"  },  // shift-end
        { "kPRV",     "\\E[5;2~"  },  // shift-pageup
        { "kNXT",     "\\E[6;2~"  },  // shift-pagedown
        { "kIC",      "\\E[2;2~"  },  // shift-insert
        { "kDC",      "\\E[3;2~"  },  // shift-delete

        // Shift-modified function keys (kf13–kf24).
        { "kf13",     "\\E[1;2P"  },
        { "kf14",     "\\E[1;2Q"  },
        { "kf15",     "\\E[1;2R"  },
        { "kf16",     "\\E[1;2S"  },
        { "kf17",     "\\E[15;2~" },
        { "kf18",     "\\E[17;2~" },
        { "kf19",     "\\E[18;2~" },
        { "kf20",     "\\E[19;2~" },
        { "kf21",     "\\E[20;2~" },
        { "kf22",     "\\E[21;2~" },
        { "kf23",     "\\E[23;2~" },
        // kf24 (shift-F12) omitted — F12 swallowed before legacy path.

        // CPR / DA queries (u6-u9 convention)
        { "u6",       "\\E[%i%d;%dR" },
        { "u7",       "\\E[6n"       },
        { "u8",       "\\E[?%[;0123456789]c" },
        { "u9",       "\\E[c"        },

        // Numeric caps
        { "colors",   "256"   },
        { "pairs",    "32767" },
        { "it",       "8"     },
    };
    // clang-format on
    return table;
}

// Decode a hex string into bytes. Returns false on invalid input.
static bool hexDecode(std::string_view hex, std::string &out)
{
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto hexNibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
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

void TerminalEmulator::processDCS(std::string_view seq)
{
    // XTGETTCAP: DCS + q <hex-names> ST  (names separated by ';')
    if (seq.size() >= 2 && seq[0] == '+' && seq[1] == 'q') {
        std::string_view queries = seq.substr(2);
        const auto &table        = xtgettcapTable();

        auto handleOne = [&](std::string_view hexName)
        {
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
            auto it          = table.find(name);
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
                if (pos < queries.size()) {
                    handleOne(queries.substr(pos));
                }
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
            int ps = static_cast<int>(mState->cursorShape);
            if (ps == 0) {
                ps = 1;
            }
            char resp[32];
            int len = snprintf(resp, sizeof(resp), "\x1bP1$r%d q\x1b\\", ps);
            writeToOutput(resp, len);
        } else if (sub == "m") {
            // SGR state query
            std::string params = buildCurrentSGR();
            std::string resp   = "\x1bP1$r" + params + "m\x1b\\";
            writeToOutput(resp.c_str(), resp.size());
        } else if (sub == "r") {
            // Scroll margins query (DECSTBM)
            // mState->scrollTop is 0-indexed; mState->scrollBottom is exclusive upper bound
            char resp[64];
            int len = snprintf(resp, sizeof(resp), "\x1bP1$r%d;%dr\x1b\\", mState->scrollTop + 1, mState->scrollBottom);
            writeToOutput(resp, len);
        } else {
            // Unknown subparam
            const char *resp = "\x1bP0$r\x1b\\";
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
    auto add = [&](int n)
    {
        if (!p.empty()) {
            p += ';';
        }
        p += std::to_string(n);
    };

    // Text attributes
    if (mState->currentAttrs.bold()) {
        add(1);
    }
    if (mState->currentAttrs.dim()) {
        add(2);
    }
    if (mState->currentAttrs.italic()) {
        add(3);
    }
    if (mState->currentAttrs.underline()) {
        add(4);
    }
    if (mState->currentAttrs.blink()) {
        add(5);
    }
    if (mState->currentAttrs.inverse()) {
        add(7);
    }
    if (mState->currentAttrs.invisible()) {
        add(8);
    }
    if (mState->currentAttrs.strikethrough()) {
        add(9);
    }

    // Foreground color — we always store RGB, never Indexed (SGR parse resolves at ingest).
    switch (mState->currentAttrs.fgMode()) {
        case CellAttrs::Default: break;
        case CellAttrs::RGB:
            add(38);
            add(2);
            add(mState->currentAttrs.fgR());
            add(mState->currentAttrs.fgG());
            add(mState->currentAttrs.fgB());
            break;
    }

    // Background color
    switch (mState->currentAttrs.bgMode()) {
        case CellAttrs::Default: break;
        case CellAttrs::RGB:
            add(48);
            add(2);
            add(mState->currentAttrs.bgR());
            add(mState->currentAttrs.bgG());
            add(mState->currentAttrs.bgB());
            break;
    }

    return p.empty() ? "0" : p;
}

// ---------------------------------------------------------------------------
// Terminfo source emitter — single source of truth shared with XTGETTCAP.
// ---------------------------------------------------------------------------

std::string emitTerminfoSource()
{
    const auto &table = xtgettcapTable();

    // Heuristic typing: empty → boolean; all-digit → numeric; else → string.
    // The numeric case currently matches `colors`, `pairs`, `it`. If new entries
    // ever store a numeric-looking string, prefer to give it a non-numeric
    // representation or extend this typing rule.
    std::vector<std::string> bools;
    std::vector<std::pair<std::string, std::string>> nums;
    std::vector<std::pair<std::string, std::string>> strings;

    // tic rejects cap names containing characters outside [A-Za-z0-9]. The
    // mb-query-* extensions live in XTGETTCAP only — apps probe them via
    // DCS+q, not via terminfo lookup, so silently skipping here is correct.
    auto isTerminfoLegalName = [](const std::string &k)
    {
        return std::all_of(k.begin(), k.end(), [](char c)
                           { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    };

    for (const auto &[k, v] : table) {
        if (k == "TN" || k == "name") {
            continue; // promoted into the header line
        }
        if (!isTerminfoLegalName(k)) {
            continue; // XTGETTCAP-only extension (e.g. mb-query-*)
        }
        if (v.empty()) {
            bools.push_back(k);
        } else if (std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            nums.emplace_back(k, v);
        } else {
            strings.emplace_back(k, v);
        }
    }
    std::sort(bools.begin(), bools.end());
    std::sort(nums.begin(), nums.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::sort(strings.begin(), strings.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    // Terminfo source escape: comma is the cap separator and must be backslash-
    // escaped if it appears inside a value. Caret is *not* escaped — `^X` is the
    // canonical control-character notation (`^G` = BEL, `^M` = CR, etc.) and
    // values in our table already use that form. Literal-caret would need `\^`,
    // but no current value contains one; revisit if that changes.
    auto escape = [](const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ',') {
                out += '\\';
            }
            out += c;
        }
        return out;
    };

    std::string r;
    r += "# MasterBandit terminfo entry — generated from xtgettcapTable() in\n";
    r += "# src/terminal/DCS.cpp. Do not edit by hand; regenerate with `mb --emit-terminfo`.\n";
    r += "#\n";
    r += "# Install:  mb --emit-terminfo | tic -x -o ~/.terminfo -\n";
    r += "# Activate: export TERM=xterm-mb\n";
    r += "#\n";
    r += "# The -x flag is mandatory — without it tic silently drops every extension\n";
    r += "# capability (mb-query-*, Tc, Su, Smulx, Setulc, BE/BD, fe/fd, kRIT/kf13+, …).\n";
    r += "\n";
    r += "xterm-mb|mb|MasterBandit terminal,\n";

    for (const auto &k : bools) {
        r += '\t';
        r += k;
        r += ",\n";
    }
    for (const auto &[k, v] : nums) {
        r += '\t';
        r += k;
        r += '#';
        r += v;
        r += ",\n";
    }
    for (const auto &[k, v] : strings) {
        r += '\t';
        r += k;
        r += '=';
        r += escape(v);
        r += ",\n";
    }

    return r;
}

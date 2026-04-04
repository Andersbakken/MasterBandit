#pragma once
#include "TerminalEmulator.h"
#include <string>

// Lightweight wrapper around TerminalEmulator for use in unit tests.
// No PTY, no GPU — just feed bytes in and inspect the cell grid.
struct TestTerminal {
    TerminalEmulator term;

    TestTerminal(int cols = 80, int rows = 24)
        : term(TerminalCallbacks{})
    {
        term.resize(cols, rows);
    }

    void feed(const std::string& s)
    {
        term.injectData(s.data(), s.size());
    }

    // Feed a printf-style escape sequence. ESC = \x1b.
    void esc(const std::string& s) { feed("\x1b" + s); }
    void csi(const std::string& s) { feed("\x1b[" + s); }

    const Cell& cell(int col, int row) const
    {
        return term.grid().cell(col, row);
    }

    char32_t wc(int col, int row) const
    {
        return term.grid().cell(col, row).wc;
    }

    const CellAttrs& attrs(int col, int row) const
    {
        return term.grid().cell(col, row).attrs;
    }

    // Returns the visible text of a row, trimmed of trailing spaces/nulls.
    std::string rowText(int row) const
    {
        std::string result;
        for (int col = 0; col < term.width(); ++col) {
            char32_t cp = term.grid().cell(col, row).wc;
            if (cp == 0) cp = ' ';
            if (cp < 0x80) {
                result += static_cast<char>(cp);
            } else if (cp < 0x800) {
                result += static_cast<char>(0xC0 | (cp >> 6));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                result += static_cast<char>(0xE0 | (cp >> 12));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                result += static_cast<char>(0xF0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
        auto end = result.find_last_not_of(' ');
        return end == std::string::npos ? "" : result.substr(0, end + 1);
    }
};

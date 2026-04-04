#include <doctest/doctest.h>
#include "TestTerminal.h"

// ── RIS - full reset ──────────────────────────────────────────────────────────

TEST_CASE("RIS resets cursor and clears screen")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("5;10H");
    t.esc("c");  // RIS
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    CHECK(t.rowText(0) == "");
}

TEST_CASE("RIS resets SGR attributes")
{
    TestTerminal t;
    t.csi("1m");   // bold
    t.esc("c");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).bold());
}

// ── cursor visibility ─────────────────────────────────────────────────────────

TEST_CASE("DECTCEM hide cursor (mode 25 reset)")
{
    TestTerminal t;
    CHECK(t.term.cursorVisible());
    t.csi("?25l");
    CHECK_FALSE(t.term.cursorVisible());
}

TEST_CASE("DECTCEM show cursor (mode 25 set)")
{
    TestTerminal t;
    t.csi("?25l");
    t.csi("?25h");
    CHECK(t.term.cursorVisible());
}

// ── alt screen (mode 1049) ────────────────────────────────────────────────────

TEST_CASE("alt screen is clean on entry")
{
    TestTerminal t;
    t.feed("Main content");
    t.csi("?1049h");
    CHECK(t.rowText(0) == "");
}

TEST_CASE("alt screen does not affect main screen content")
{
    TestTerminal t;
    t.feed("Main content");
    t.csi("?1049h");
    t.feed("Alt content");
    t.csi("?1049l");
    CHECK(t.rowText(0) == "Main content");
}

TEST_CASE("alt screen restores cursor position on exit")
{
    TestTerminal t;
    t.csi("5;10H");  // move cursor on main screen
    t.csi("?1049h"); // save cursor, enter alt
    t.csi("1;1H");   // move on alt screen
    t.csi("?1049l"); // restore cursor, exit alt
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

// ── mouse modes ───────────────────────────────────────────────────────────────

TEST_CASE("mouse mode 1000 set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.mouseReportingActive());
    t.csi("?1000h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1000l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

TEST_CASE("mouse mode 1002 set/reset")
{
    TestTerminal t;
    t.csi("?1002h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1002l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

TEST_CASE("mouse mode 1003 set/reset")
{
    TestTerminal t;
    t.csi("?1003h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1003l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

// ── bracketed paste (mode 2004) ───────────────────────────────────────────────

TEST_CASE("bracketed paste mode set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.bracketedPaste());
    t.csi("?2004h");
    CHECK(t.term.bracketedPaste());
    t.csi("?2004l");
    CHECK_FALSE(t.term.bracketedPaste());
}

// ── synchronized output (mode 2026) ──────────────────────────────────────────

TEST_CASE("synchronized output mode set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.syncOutputActive());
    t.csi("?2026h");
    CHECK(t.term.syncOutputActive());
    t.csi("?2026l");
    CHECK_FALSE(t.term.syncOutputActive());
}

// ── Device Attributes ─────────────────────────────────────────────────────────

TEST_CASE("primary DA responds to CSI c")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("c");
    CHECK_FALSE(t.term.capturedOutput.empty());
    // Response starts with ESC [ ?
    CHECK(t.term.capturedOutput.substr(0, 3) == "\x1b[?");
}

TEST_CASE("secondary DA responds to CSI > c")
{
    TestTerminal t;
    t.clearOutput();
    t.feed("\x1b[>c");
    CHECK_FALSE(t.term.capturedOutput.empty());
    CHECK(t.term.capturedOutput.substr(0, 3) == "\x1b[>");
}

TEST_CASE("XTVERSION responds to CSI > q")
{
    TestTerminal t;
    t.clearOutput();
    t.feed("\x1b[>q");
    CHECK_FALSE(t.term.capturedOutput.empty());
    // DCS response: ESC P > | ...
    CHECK(t.term.capturedOutput.find("MasterBandit") != std::string::npos);
}

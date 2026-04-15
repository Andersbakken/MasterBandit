#include <doctest/doctest.h>
#include "TestTerminal.h"

// ── default tab stops (every 8 columns) ───────────────────────────────────────

TEST_CASE("\\t advances to next 8-column boundary by default")
{
    TestTerminal t;
    t.feed("\t");
    CHECK(t.term.cursorX() == 8);
    t.feed("\t");
    CHECK(t.term.cursorX() == 16);
    t.feed("\t");
    CHECK(t.term.cursorX() == 24);
}

TEST_CASE("\\t from mid-cell jumps to next boundary, not +8")
{
    TestTerminal t;
    t.feed("hello\t"); // cursor at 5, jumps to 8
    CHECK(t.term.cursorX() == 8);
}

TEST_CASE("\\t at last stop clamps to right margin")
{
    TestTerminal t(80, 24);
    // Walk to past column 72, the last default stop before 80.
    t.csi("74G"); // move cursor to column 74 (1-based) → cursorX=73
    t.feed("\t");
    CHECK(t.term.cursorX() == 79); // right margin
}

// ── HTS (ESC H) / TBC (CSI g) ─────────────────────────────────────────────────

TEST_CASE("HTS sets a tab stop at the current column")
{
    TestTerminal t;
    t.csi("4G");     // column 4 (1-based) → x=3
    t.esc("H");      // set tab stop here
    t.csi("1G");     // back to column 1
    t.feed("\t");
    CHECK(t.term.cursorX() == 3);
}

TEST_CASE("TBC Ps=0 clears the stop at current column")
{
    TestTerminal t;
    // column 8 is a default stop. Move there and clear it.
    t.csi("9G");     // x=8
    t.csi("0g");     // clear stop here
    t.csi("1G");
    t.feed("\t");
    CHECK(t.term.cursorX() == 16); // jumps past the cleared stop
}

TEST_CASE("TBC Ps=3 clears all stops")
{
    TestTerminal t(80, 24);
    t.csi("3g");
    t.csi("1G");
    t.feed("\t");
    CHECK(t.term.cursorX() == 79); // no stops left → right margin
}

TEST_CASE("TBC default parameter (missing) is Ps=0")
{
    TestTerminal t;
    t.csi("9G");
    t.csi("g");      // no parameter → Ps=0
    t.csi("1G");
    t.feed("\t");
    CHECK(t.term.cursorX() == 16);
}

// ── CHT (CSI I) / CBT (CSI Z) ─────────────────────────────────────────────────

TEST_CASE("CHT moves forward N tab stops")
{
    TestTerminal t;
    t.csi("3I");     // forward 3 stops from x=0 → 8, 16, 24
    CHECK(t.term.cursorX() == 24);
}

TEST_CASE("CHT default is 1 stop")
{
    TestTerminal t;
    t.csi("I");
    CHECK(t.term.cursorX() == 8);
}

TEST_CASE("CBT moves backward N tab stops")
{
    TestTerminal t;
    t.csi("25G");    // x=24
    t.csi("2Z");     // back 2 stops → 16, then 8
    CHECK(t.term.cursorX() == 8);
}

TEST_CASE("CBT at column 0 stays at 0")
{
    TestTerminal t;
    t.csi("Z");
    CHECK(t.term.cursorX() == 0);
}

// ── RIS re-seeds defaults ─────────────────────────────────────────────────────

TEST_CASE("RIS restores default tab stops")
{
    TestTerminal t;
    t.csi("3g");     // wipe all
    t.esc("c");      // RIS
    t.feed("\t");
    CHECK(t.term.cursorX() == 8); // default stop restored
}

// ── resize grows stops but preserves existing ones ────────────────────────────

TEST_CASE("growing terminal adds default stops in new columns")
{
    TestTerminal t(40, 10);
    t.term.resize(80, 10);
    t.csi("73G");    // x=72, a default stop
    t.esc("H");      // ensure it's set
    t.csi("1G");
    // Walk through stops up to 72.
    for (int expected : {8, 16, 24, 32, 40, 48, 56, 64, 72}) {
        t.feed("\t");
        CHECK(t.term.cursorX() == expected);
    }
}

TEST_CASE("resize does not resurrect cleared default stops in preserved range")
{
    TestTerminal t(40, 10);
    t.csi("9G");
    t.csi("0g");     // clear the stop at column 8
    t.term.resize(80, 10); // grow
    t.csi("1G");
    t.feed("\t");
    CHECK(t.term.cursorX() == 16); // stop at 8 stays cleared
}

// ── DECSTR (CSI ! p) ──────────────────────────────────────────────────────────

TEST_CASE("DECSTR resets SGR and cursor visibility, keeps screen content")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("1m");              // bold
    t.csi("?25l");            // hide cursor
    t.csi("!p");              // DECSTR
    t.feed("X");
    // Post-DECSTR text writes with default attrs.
    int xCol = 5; // "Hello" ends at col 4, so "X" lands at col 5
    CHECK_FALSE(t.attrs(xCol, 0).bold());
    CHECK(t.term.cursorVisible());
    CHECK(t.rowText(0).substr(0, 5) == "Hello"); // screen content kept
}

TEST_CASE("DECSTR resets tab stops to defaults")
{
    TestTerminal t;
    t.csi("3g");              // wipe all tab stops
    t.csi("!p");              // DECSTR
    t.csi("1G");
    t.feed("\t");
    CHECK(t.term.cursorX() == 8);
}

TEST_CASE("DECSTR preserves mouse/paste/focus modes")
{
    TestTerminal t;
    t.csi("?1000h");          // mouse reporting
    t.csi("?2004h");          // bracketed paste
    t.csi("?1004h");          // focus reporting
    t.csi("!p");              // DECSTR
    CHECK(t.term.bracketedPaste());
    CHECK(t.term.mouseReportingActive());
}

TEST_CASE("DECSTR preserves cursor position")
{
    TestTerminal t;
    t.csi("10;20H");          // row 10 col 20
    t.csi("!p");
    CHECK(t.term.cursorX() == 19);
    CHECK(t.term.cursorY() == 9);
}


// ── DECCOLM (private mode 3) ──────────────────────────────────────────────────

TEST_CASE("DECCOLM set clears screen and homes cursor")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("5;10H");
    t.csi("?3h");             // DECCOLM set
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    CHECK(t.rowText(0) == "");
}

TEST_CASE("DECCOLM reset clears screen and homes cursor")
{
    TestTerminal t;
    t.feed("World");
    t.csi("5;10H");
    t.csi("?3l");             // DECCOLM reset
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    CHECK(t.rowText(0) == "");
}


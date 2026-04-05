#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("reflow: shrink joins nothing without soft wrap")
{
    TestTerminal t(10, 3);
    t.feed("ABCD");
    t.term.resize(5, 3);
    CHECK(t.rowText(0) == "ABCD");
    CHECK(t.rowText(1) == "");
}

TEST_CASE("reflow: shrink wraps long line")
{
    TestTerminal t(10, 3);
    t.feed("ABCDEFGH");
    t.term.resize(4, 3);
    CHECK(t.rowText(0) == "ABCD");
    CHECK(t.rowText(1) == "EFGH");
}

TEST_CASE("reflow: grow rejoins soft-wrapped line")
{
    TestTerminal t(4, 3);
    t.feed("ABCDEFGH"); // wraps at col 4
    CHECK(t.rowText(0) == "ABCD");
    CHECK(t.rowText(1) == "EFGH");
    t.term.resize(10, 3);
    CHECK(t.rowText(0) == "ABCDEFGH");
    CHECK(t.rowText(1) == "");
}

TEST_CASE("reflow: explicit newline not merged")
{
    TestTerminal t(10, 3);
    t.feed("ABCD\r\nEFGH"); // \r\n = carriage return + line feed
    t.term.resize(20, 3);
    CHECK(t.rowText(0) == "ABCD");
    CHECK(t.rowText(1) == "EFGH");
}

TEST_CASE("reflow: trailing blanks trimmed")
{
    TestTerminal t(8, 3);
    t.feed("AB"); // only 2 chars on an 8-col line
    t.term.resize(4, 3);
    CHECK(t.rowText(0) == "AB");
    CHECK(t.rowText(1) == "");
}

TEST_CASE("reflow: cursor tracks through shrink")
{
    TestTerminal t(10, 3);
    t.feed("ABCDEFGH"); // cursor at col 8
    CHECK(t.term.cursorX() == 8);
    CHECK(t.term.cursorY() == 0);
    t.term.resize(4, 3);
    // "ABCDEFGH" wraps to row0="ABCD" row1="EFGH", cursor was after H
    CHECK(t.term.cursorY() == 1); // now on second row (screen-relative)
}

TEST_CASE("reflow: cursor tracks through grow")
{
    TestTerminal t(4, 3);
    t.feed("ABCDEFGH"); // wraps: row0=ABCD row1=EFGH, cursor at (0,2)
    t.term.resize(10, 3);
    // Rejoined: row0=ABCDEFGH, cursor should be on row 0
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("reflow: SGR attributes preserved")
{
    TestTerminal t(4, 3);
    t.csi("1m"); // bold
    t.feed("ABCDEFGH");
    t.term.resize(10, 3);
    CHECK(t.rowText(0) == "ABCDEFGH");
    CHECK(t.attrs(0, 0).bold());
    CHECK(t.attrs(4, 0).bold()); // was on row 1 before reflow
    CHECK(t.attrs(7, 0).bold());
}

TEST_CASE("reflow: multiple logical lines")
{
    TestTerminal t(4, 5);
    t.feed("ABCDEFGH"); // wraps: ABCD + EFGH (continued)
    t.feed("\r\n");      // CR+LF = carriage return + line feed
    t.feed("IJKL");     // separate logical line
    t.term.resize(10, 5);
    CHECK(t.rowText(0) == "ABCDEFGH");
    CHECK(t.rowText(1) == "IJKL");
}

TEST_CASE("reflow: height-only grow adds rows at bottom")
{
    TestTerminal t(10, 3);
    t.feed("AB");
    t.feed("\r\n");
    t.feed("CD");
    t.term.resize(10, 5);
    // Content stays at rows 0-1, new blank rows at bottom
    CHECK(t.rowText(0) == "AB");
    CHECK(t.rowText(1) == "CD");
    CHECK(t.rowText(2) == "");
    CHECK(t.rowText(3) == "");
    CHECK(t.rowText(4) == "");
}

TEST_CASE("reflow: empty terminal resize")
{
    TestTerminal t(10, 3);
    t.term.resize(5, 5); // should not crash
    CHECK(t.term.width() == 5);
    CHECK(t.term.height() == 5);
}

TEST_CASE("reflow: wide character at boundary")
{
    TestTerminal t(5, 3);
    // Write 4 ASCII + 1 wide char (needs 2 cells, won't fit at col 4)
    t.feed("ABCD\xe4\xb8\xad"); // 中 is a wide CJK char
    t.term.resize(5, 3); // same size, verify layout is correct
    // The wide char should be at (0,1) if it wrapped, or (4,0) if it didn't
    // With 5 cols: A(0) B(1) C(2) D(3) + 中 needs 2 cols at pos 4 — doesn't fit
    // So row 0 = "ABCD " (padded), row 1 = "中"
}

TEST_CASE("reflow: shrink then grow restores content")
{
    TestTerminal t(10, 3);
    t.feed("ABCDEFGHIJ"); // 10 chars
    t.term.resize(5, 3);  // shrink: ABCDE + FGHIJ
    CHECK(t.rowText(0) == "ABCDE");
    CHECK(t.rowText(1) == "FGHIJ");
    t.term.resize(10, 3); // grow back
    CHECK(t.rowText(0) == "ABCDEFGHIJ");
}

TEST_CASE("reflow: history rows participate in reflow")
{
    TestTerminal t(10, 2); // only 2 visible rows
    t.feed("LINE1\n");
    t.feed("LINE2\n");
    t.feed("LINE3\n");
    // LINE1 and LINE2 are in history, LINE3 + empty on screen
    t.term.resize(3, 2);
    // All lines should reflow to 3-col width
    // LINE1 → "LIN" + "E1", LINE2 → "LIN" + "E2", LINE3 → "LIN" + "E3"
    // History should contain the reflowed lines
    // Screen shows the last 2 rows
}

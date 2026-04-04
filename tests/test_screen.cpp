#include <doctest/doctest.h>
#include "TestTerminal.h"

// ── ED - erase in display ─────────────────────────────────────────────────────

TEST_CASE("ED 0 - erase from cursor to end of screen")
{
    TestTerminal t(10, 3);
    t.feed("AAAA\r\nBBBB\r\nCCCC");
    t.csi("2;3H");  // row 2, col 3 (1-based) → (1, 2) 0-indexed... wait: row=2,col=3 → y=1,x=2
    t.csi("0J");
    // row 0 should be intact
    CHECK(t.rowText(0) == "AAAA");
    // row 1: cols 0-1 intact ("BB"), col 2 onward erased
    CHECK(t.wc(0, 1) == U'B');
    CHECK(t.wc(1, 1) == U'B');
    CHECK(t.wc(2, 1) == 0);
    // row 2 fully erased
    CHECK(t.rowText(2) == "");
}

TEST_CASE("ED 1 - erase from beginning to cursor")
{
    TestTerminal t(10, 3);
    t.feed("AAAA\r\nBBBB\r\nCCCC");
    t.csi("2;3H");  // row 2, col 3 → y=1, x=2
    t.csi("1J");
    // row 0 fully erased
    CHECK(t.rowText(0) == "");
    // row 1: cols 0-2 erased, col 3 onward intact
    CHECK(t.wc(0, 1) == 0);
    CHECK(t.wc(1, 1) == 0);
    CHECK(t.wc(2, 1) == 0);
    CHECK(t.wc(3, 1) == U'B');
    // row 2 intact
    CHECK(t.rowText(2) == "CCCC");
}

TEST_CASE("ED 2 - erase entire screen")
{
    TestTerminal t(10, 3);
    t.feed("AAAA\r\nBBBB\r\nCCCC");
    t.csi("2J");
    CHECK(t.rowText(0) == "");
    CHECK(t.rowText(1) == "");
    CHECK(t.rowText(2) == "");
}

// ── EL - erase in line ────────────────────────────────────────────────────────

TEST_CASE("EL 1 - erase from beginning of line to cursor")
{
    TestTerminal t;
    t.feed("Hello World");
    t.csi("6G");   // col 6 (1-based) → col index 5
    t.csi("1K");
    // cols 0-5 erased, cols 6-10 intact
    CHECK(t.wc(0, 0) == 0);
    CHECK(t.wc(5, 0) == 0);
    CHECK(t.wc(6, 0) == U'W');
    CHECK(t.wc(7, 0) == U'o');
}

// ── SU / SD - scroll up / down ────────────────────────────────────────────────

TEST_CASE("SU - scroll up")
{
    TestTerminal t(10, 3);
    t.feed("AAA\r\nBBB\r\nCCC");
    t.csi("1S");  // scroll up 1
    CHECK(t.rowText(0) == "BBB");
    CHECK(t.rowText(1) == "CCC");
    CHECK(t.rowText(2) == "");
}

TEST_CASE("SD - scroll down")
{
    TestTerminal t(10, 3);
    t.feed("AAA\r\nBBB\r\nCCC");
    t.csi("1T");  // scroll down 1
    CHECK(t.rowText(0) == "");
    CHECK(t.rowText(1) == "AAA");
    CHECK(t.rowText(2) == "BBB");
}

// ── DECSTBM - scroll region ───────────────────────────────────────────────────

TEST_CASE("DECSTBM constrains scroll to region")
{
    TestTerminal t(10, 4);
    t.feed("AAA\r\nBBB\r\nCCC\r\nDDD");
    t.csi("2;3r");  // scroll region rows 2-3 (1-based)
    t.csi("2;1H");  // move into region
    t.csi("1S");    // scroll up 1 within region
    // row 0 (outside region) unchanged
    CHECK(t.rowText(0) == "AAA");
    // row 1 (was BBB) now contains CCC
    CHECK(t.rowText(1) == "CCC");
    // row 2 (bottom of region) now blank
    CHECK(t.rowText(2) == "");
    // row 3 (outside region) unchanged
    CHECK(t.rowText(3) == "DDD");
}

TEST_CASE("DECSTBM moves cursor to top of region on set")
{
    TestTerminal t;
    t.csi("5;10r");
    CHECK(t.term.cursorY() == 4);  // top of region (0-indexed)
    CHECK(t.term.cursorX() == 0);
}

// ── CNL / CPL ────────────────────────────────────────────────────────────────

TEST_CASE("CNL - cursor next line")
{
    TestTerminal t;
    t.csi("3;5H");  // row 3, col 5 → y=2, x=4
    t.csi("2E");    // down 2 lines, col 0
    CHECK(t.term.cursorY() == 4);
    CHECK(t.term.cursorX() == 0);
}

TEST_CASE("CPL - cursor previous line")
{
    TestTerminal t;
    t.csi("5;5H");  // row 5, col 5 → y=4, x=4
    t.csi("2F");    // up 2 lines, col 0
    CHECK(t.term.cursorY() == 2);
    CHECK(t.term.cursorX() == 0);
}

// ── VPA - vertical position absolute ─────────────────────────────────────────

TEST_CASE("VPA - vertical position absolute")
{
    TestTerminal t;
    t.csi("5d");   // row 5 (1-based) → y=4
    CHECK(t.term.cursorY() == 4);
    // column unchanged (should still be 0)
    CHECK(t.term.cursorX() == 0);
}

// ── DCH - delete characters ───────────────────────────────────────────────────

TEST_CASE("DCH - delete characters at cursor")
{
    TestTerminal t;
    t.feed("ABCDE");
    t.csi("2G");   // col 2 (1-based) → col index 1
    t.csi("2P");   // delete 2 chars (B, C)
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == U'D');
    CHECK(t.wc(2, 0) == U'E');
    CHECK(t.wc(3, 0) == 0);
}

// ── ICH - insert characters ───────────────────────────────────────────────────

TEST_CASE("ICH - insert blank characters at cursor")
{
    TestTerminal t;
    t.feed("ABCDE");
    t.csi("2G");   // col index 1
    t.csi("2@");   // insert 2 blanks
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == 0);
    CHECK(t.wc(2, 0) == 0);
    CHECK(t.wc(3, 0) == U'B');
    CHECK(t.wc(4, 0) == U'C');
}

// ── IL / DL - insert / delete lines ──────────────────────────────────────────

TEST_CASE("IL - insert lines")
{
    TestTerminal t(10, 4);
    t.feed("AAA\r\nBBB\r\nCCC");
    t.csi("2;1H");  // row 2, col 1 → y=1
    t.csi("1L");    // insert 1 blank line
    CHECK(t.rowText(0) == "AAA");
    CHECK(t.rowText(1) == "");
    CHECK(t.rowText(2) == "BBB");
    CHECK(t.rowText(3) == "CCC");
}

TEST_CASE("DL - delete lines")
{
    TestTerminal t(10, 4);
    t.feed("AAA\r\nBBB\r\nCCC\r\nDDD");
    t.csi("2;1H");  // y=1
    t.csi("1M");    // delete 1 line
    CHECK(t.rowText(0) == "AAA");
    CHECK(t.rowText(1) == "CCC");
    CHECK(t.rowText(2) == "DDD");
    CHECK(t.rowText(3) == "");
}

// ── ECH - erase characters ────────────────────────────────────────────────────

TEST_CASE("ECH - erase characters at cursor (no shift)")
{
    TestTerminal t;
    t.feed("ABCDE");
    t.csi("2G");   // col index 1
    t.csi("3X");   // erase 3 chars (B, C, D)
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == 0);
    CHECK(t.wc(2, 0) == 0);
    CHECK(t.wc(3, 0) == 0);
    CHECK(t.wc(4, 0) == U'E');
    // cursor does not move
    CHECK(t.term.cursorX() == 1);
}

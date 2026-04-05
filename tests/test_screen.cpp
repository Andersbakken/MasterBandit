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

// ── DCH/ICH extras shifting ──────────────────────────────────────────────────

TEST_CASE("DCH shifts underline color extras left")
{
    TestTerminal t(10, 1);
    // Write "ABCDE" with underline color on C (col 2) and E (col 4)
    t.feed("AB");
    t.csi("4:1m");             // underline on
    t.csi("58;2;255;0;0m");   // underline color = red (RGB)
    t.feed("C");
    t.csi("59m");              // underline color off
    t.csi("24m");              // underline off
    t.feed("D");
    t.csi("4:1m");
    t.csi("58;2;0;255;0m");   // underline color = green
    t.feed("E");
    t.csi("24m");

    // Verify extras are at col 2 and 4
    auto* ex2 = t.term.grid().getExtra(2, 0);
    auto* ex4 = t.term.grid().getExtra(4, 0);
    REQUIRE(ex2 != nullptr);
    REQUIRE(ex4 != nullptr);
    CHECK(ex2->underlineColor != 0);
    CHECK(ex4->underlineColor != 0);

    // Delete 1 char at col 1 (B) — C shifts from 2→1, E shifts from 4→3
    t.csi("2G");   // cursor to col 1 (1-based)
    t.csi("1P");   // delete 1 char

    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == U'C');
    CHECK(t.wc(2, 0) == U'D');
    CHECK(t.wc(3, 0) == U'E');

    // Extras should have shifted: col 2→1, col 4→3
    auto* shifted1 = t.term.grid().getExtra(1, 0);
    auto* shifted3 = t.term.grid().getExtra(3, 0);
    CHECK(shifted1 != nullptr);
    CHECK(shifted3 != nullptr);
    // Old positions should be gone
    CHECK(t.term.grid().getExtra(2, 0) == nullptr);
    CHECK(t.term.grid().getExtra(4, 0) == nullptr);
}

TEST_CASE("ICH shifts underline color extras right")
{
    TestTerminal t(10, 1);
    // Write "ABCDE" with underline color on B (col 1) and D (col 3)
    t.feed("A");
    t.csi("4:1m");
    t.csi("58;2;255;0;0m");
    t.feed("B");
    t.csi("59m");
    t.csi("24m");
    t.feed("C");
    t.csi("4:1m");
    t.csi("58;2;0;255;0m");
    t.feed("D");
    t.csi("24m");
    t.feed("E");

    // Verify extras at col 1 and 3
    REQUIRE(t.term.grid().getExtra(1, 0) != nullptr);
    REQUIRE(t.term.grid().getExtra(3, 0) != nullptr);

    // Insert 2 blanks at col 1 — B shifts from 1→3, D shifts from 3→5
    t.csi("2G");   // cursor to col 1
    t.csi("2@");   // insert 2 blanks

    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == 0);
    CHECK(t.wc(2, 0) == 0);
    CHECK(t.wc(3, 0) == U'B');
    CHECK(t.wc(4, 0) == U'C');

    // Extras should have shifted: col 1→3, col 3→5
    CHECK(t.term.grid().getExtra(3, 0) != nullptr);
    CHECK(t.term.grid().getExtra(5, 0) != nullptr);
    // Old positions should be gone
    CHECK(t.term.grid().getExtra(1, 0) == nullptr);
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

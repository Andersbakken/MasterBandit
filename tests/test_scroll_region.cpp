#include <doctest/doctest.h>
#include "TestTerminal.h"

// Edge-case tests for IL/DL/scroll-region interaction with the
// visible-grid / scrollback split.
//
// Architectural invariant (per resize refactor decisions): IL/DL operate
// on the visible grid only. They never write into scrollback or stitch
// logical lines across the scrollback/visible boundary. The visible grid
// is fixed-width, so soft-wrap chains that physically span multiple
// visible rows are mutated row-by-row exactly as before.

TEST_CASE("scroll region: DECSTBM IL inside region preserves rows above" * doctest::test_suite("scrollregion"))
{
    TestTerminal t(20, 6);
    t.feed("AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF");
    // Set scroll region rows 3..5 (1-indexed: 3 to 5 = screen rows 2..4).
    t.csi("3;5r");
    // Move cursor into region, IL 1: insert a blank row at cursor, push
    // bottom of region out.
    t.csi("4;1H");      // cursor at row 4 (= screen row 3)
    t.csi("L");          // IL 1
    // Rows 0..1 (above region) untouched.
    CHECK(t.rowText(0) == "AAAA");
    CHECK(t.rowText(1) == "BBBB");
    // Row 2: still CCCC (top of region, unchanged).
    CHECK(t.rowText(2) == "CCCC");
    // Row 3 (cursor): was DDDD, now blank (newly inserted row).
    CHECK(t.rowText(3) == "");
    // Row 4: shifted down from row 3 = DDDD.
    CHECK(t.rowText(4) == "DDDD");
    // Row 5 (below region): unchanged = FFFF.
    CHECK(t.rowText(5) == "FFFF");
}

TEST_CASE("scroll region: IL on a soft-wrapped chain only mutates visible row" * doctest::test_suite("scrollregion"))
{
    // Verify IL doesn't accidentally stitch into scrollback or break
    // soft-wrap accounting. Set up a soft-wrapped chain inside a scroll
    // region and IL one row.
    TestTerminal t(4, 5);
    t.feed("ABCDEFGH");  // 8 chars autowraps: row0=ABCD row1=EFGH (continued)
    // Verify pre-state.
    CHECK(t.rowText(0) == "ABCD");
    CHECK(t.rowText(1) == "EFGH");
    // Set scroll region rows 1..3 (screen rows 0..2).
    t.csi("1;3r");
    t.csi("2;1H");       // cursor at row 2 (screen row 1)
    t.csi("L");           // IL 1: insert blank above row 1 within region
    CHECK(t.rowText(0) == "ABCD");  // top of region unchanged
    CHECK(t.rowText(1) == "");      // newly inserted blank
    CHECK(t.rowText(2) == "EFGH");  // shifted down
    // Region scrolled the original row 2 (blank) out, row 3 still blank.
    CHECK(t.rowText(3) == "");
}

TEST_CASE("scroll region: DL within region drops the right row" * doctest::test_suite("scrollregion"))
{
    TestTerminal t(20, 6);
    t.feed("AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF");
    t.csi("3;5r");      // region rows 3..5 (screen 2..4)
    t.csi("4;1H");      // cursor at screen row 3 (DDDD)
    t.csi("M");          // DL 1
    // Rows above region untouched.
    CHECK(t.rowText(0) == "AAAA");
    CHECK(t.rowText(1) == "BBBB");
    CHECK(t.rowText(2) == "CCCC");
    // DDDD removed; EEEE shifts up to row 3; row 4 (bottom of region) blank.
    CHECK(t.rowText(3) == "EEEE");
    CHECK(t.rowText(4) == "");
    // Below region untouched.
    CHECK(t.rowText(5) == "FFFF");
}

TEST_CASE("scroll region: full-region scroll feeds top row to scrollback" * doctest::test_suite("scrollregion"))
{
    // The default scroll region is full-screen; LF at bottom scrolls
    // top into scrollback.
    TestTerminal t(10, 3);
    t.term.resetScrollback(100);
    t.feed("XXX\r\nYYY\r\nZZZ");  // visible: XXX, YYY, ZZZ
    t.feed("\r\n");                 // LF at bottom scrolls XXX out
    CHECK(t.term.document().scrollbackLogicalLines() == 1);
    CHECK(t.term.document().historyRow(0)[0].wc == U'X');
}

TEST_CASE("scroll region: partial-region scroll does NOT feed scrollback" * doctest::test_suite("scrollregion"))
{
    // A scroll inside a non-top-anchored region must NOT push into
    // scrollback (only full-region scrolling at the top counts).
    TestTerminal t(10, 5);
    t.term.resetScrollback(100);
    t.feed("XXX\r\nYYY\r\nZZZ\r\nWWW\r\nQQQ");
    int before = t.term.document().scrollbackLogicalLines();
    // Scroll region rows 2..4 (screen 1..3).
    t.csi("2;4r");
    t.csi("4;1H");      // cursor at bottom of region
    t.feed("\n");         // LF at bottom of region: in-region scroll only
    int after = t.term.document().scrollbackLogicalLines();
    CHECK(before == after);
}

TEST_CASE("ICH/DCH bounds stay within physical row" * doctest::test_suite("scrollregion"))
{
    // ICH/DCH operate within a single physical row; soft-wrap chain
    // continuation doesn't leak shifted cells across the row boundary.
    TestTerminal t(4, 3);
    t.feed("ABCDEFGH");  // row 0: ABCD row 1: EFGH (continued)
    t.csi("1;1H");       // cursor at (0, 0)
    t.csi("2@");          // ICH 2: insert 2 blank cells at cursor
    // Row 0: 2 blanks + AB (cells C and D shift off the row, NOT into row 1).
    CHECK(t.rowText(0) == "  AB");
    // Row 1 unchanged.
    CHECK(t.rowText(1) == "EFGH");
}

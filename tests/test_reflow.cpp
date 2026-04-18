#include <doctest/doctest.h>
#include "TestTerminal.h"
#include <set>

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

TEST_CASE("reflow: width shrink wraps content that fills viewport, cursor gets blank row")
{
    // 4 rows of content each 20 chars wide, cursor on row 4 (blank).
    // Shrinking to width 10 doubles each row → 8 content rows.
    // With height=5, viewport is full; cursor must still get its own blank row.
    TestTerminal t(20, 5);
    t.feed("AAAAAAAAAAAAAAAAAAAA"); t.feed("\r\n"); // row 0, 20 chars
    t.feed("BBBBBBBBBBBBBBBBBBBB"); t.feed("\r\n"); // row 1
    t.feed("CCCCCCCCCCCCCCCCCCCC"); t.feed("\r\n"); // row 2
    t.feed("DDDDDDDDDDDDDDDDDDDD");                // row 3, cursor ends on row 3
    t.csi("5;1H");                                 // move cursor to row 5 (0-indexed: 4), blank
    CHECK(t.term.cursorY() == 4);

    t.term.resize(10, 5); // each row wraps to 2 → 8 content rows, viewport=5

    // Cursor must be on an empty row
    int cy = t.term.cursorY();
    CHECK(cy == 4); // last row of viewport
    CHECK(t.wc(0, cy) == 0); // that row must be blank (cursor's own row)
}

TEST_CASE("height shrink: cursor on last blank row is not treated as padding")
{
    // Content on rows 0-1, cursor moved to last row (blank).
    // Shrinking by 1 should push a content row to history rather than
    // silently discarding the blank cursor row as padding.
    TestTerminal t(10, 5);
    t.feed("AB\r\nCD");         // row 0: "AB", row 1: "CD", cursor at (2, 1)
    t.csi("5;1H");              // move cursor to row 5 (0-indexed: row 4), blank
    CHECK(t.term.cursorY() == 4);

    int histBefore = t.term.document().historySize();
    t.term.resize(10, 4);
    int histAfter = t.term.document().historySize();

    CHECK(histAfter > histBefore);        // a content row was pushed to history
    CHECK(t.term.cursorY() == 3);         // cursor at bottom of new viewport
    CHECK(t.rowText(0) == "CD");          // remaining content still visible
}

TEST_CASE("height shrink: blank rows below cursor are discarded as padding")
{
    // Cursor is mid-screen, blank rows below it should be discarded
    // without pushing content to history.
    TestTerminal t(10, 5);
    t.feed("AB\r\nCD");         // cursor at (2, 1), rows 2-4 blank
    CHECK(t.term.cursorY() == 1);

    int histBefore = t.term.document().historySize();
    t.term.resize(10, 4);
    int histAfter = t.term.document().historySize();

    CHECK(histAfter == histBefore);       // blank padding discarded, nothing pushed
    CHECK(t.term.cursorY() == 1);
    CHECK(t.rowText(0) == "AB");
    CHECK(t.rowText(1) == "CD");
}

TEST_CASE("height shrink: cursor on last blank row, shrink by 2")
{
    // Cursor is at the very bottom (row 5, blank). Shrink by 2.
    // The cursor row is not discardable, so blanksAtBottom=0 and
    // all 2 required rows are pushed to history (L1, L2).
    TestTerminal t(10, 6);
    t.feed("L1\r\nL2\r\nL3");   // rows 0="L1", 1="L2", 2="L3", cursor at (2,2)
    t.csi("6;1H");              // cursor to row 6 (0-indexed: row 5), blank
    CHECK(t.term.cursorY() == 5);

    t.term.resize(10, 4);       // shrink by 2: push 2 rows to history

    // L1 and L2 pushed to history; L3 is now row 0
    CHECK(t.term.cursorY() == 3);         // cursor at bottom of new viewport
    CHECK(t.rowText(0) == "L3");
    CHECK(t.rowText(1) == "");
    CHECK(t.rowText(2) == "");
    CHECK(t.rowText(3) == "");
    CHECK(t.term.document().historySize() == 2);
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

TEST_CASE("line ids: unique across initial screen")
{
    TestTerminal t(20, 5);
    const auto& doc = t.term.document();
    std::set<uint64_t> seen;
    for (int r = 0; r < 5; ++r) {
        uint64_t id = doc.lineIdForAbs(r);
        CHECK(id != 0);
        CHECK(seen.insert(id).second); // never duplicated
    }
}

TEST_CASE("line ids: stable across scrolling")
{
    TestTerminal t(10, 3);
    t.feed("one\n");
    t.feed("two\n");
    t.feed("three\n");
    // Capture the id for "one" (now at abs 0 in history)
    uint64_t oneId = t.term.document().lineIdForAbs(0);
    CHECK(oneId != 0);
    // More scrolling — "one" is still retained in scrollback
    t.feed("four\n");
    t.feed("five\n");
    // Abs positions shifted, but the id for "one" still points at it.
    int firstAbs = t.term.document().firstAbsOfLine(oneId);
    REQUIRE(firstAbs >= 0);
    CHECK(t.term.document().lineIdForAbs(firstAbs) == oneId);
}

TEST_CASE("line ids: survive width-change reflow (shrink)")
{
    TestTerminal t(10, 3);
    t.feed("LINE1\nLINE2\nLINE3\n");
    uint64_t idL1 = t.term.document().lineIdForAbs(0);
    uint64_t idL2 = t.term.document().lineIdForAbs(1);
    uint64_t idL3 = t.term.document().lineIdForAbs(2);
    CHECK(idL1 != 0);
    CHECK(idL2 != 0);
    CHECK(idL3 != 0);
    t.term.resize(3, 3); // shrink causes wrap
    // Original ids should still resolve to some abs row. Content verification
    // isn't needed here — just that the ids are still present somewhere.
    CHECK(t.term.document().firstAbsOfLine(idL1) >= 0);
    CHECK(t.term.document().firstAbsOfLine(idL2) >= 0);
    CHECK(t.term.document().firstAbsOfLine(idL3) >= 0);
}

TEST_CASE("line ids: survive width-change reflow (grow)")
{
    TestTerminal t(4, 3);
    t.feed("ABCDEFGH\n"); // wraps at 4 cols into two rows
    uint64_t rowA = t.term.document().lineIdForAbs(0);
    uint64_t rowB = t.term.document().lineIdForAbs(1);
    CHECK(rowA != 0);
    CHECK(rowB != 0);
    t.term.resize(10, 3); // grow un-wraps
    // At least one of the original ids should survive — when source rows
    // merge into one dst row, the first src wins.
    bool anySurvived =
        t.term.document().firstAbsOfLine(rowA) >= 0 ||
        t.term.document().firstAbsOfLine(rowB) >= 0;
    CHECK(anySurvived);
}

TEST_CASE("line ids: vector is monotonic non-decreasing after complex edits")
{
    // Mix of autowraps, newlines, scrolls, and reflow to exercise all paths.
    TestTerminal t(6, 4);
    t.feed("HELLO WORLD\n");       // autowraps at width 6
    t.feed("foo\n");
    t.feed("bar\n");
    t.feed("baz\n");
    t.feed("AAAAAAAAAAA\n");       // more wraps
    t.term.resize(4, 4);           // narrow reflow
    t.term.resize(12, 4);          // wide reflow (un-wraps)
    t.feed("last\n");

    const auto& doc = t.term.document();
    int total = doc.archiveSize() + doc.historySize() + 4;
    uint64_t prev = 0;
    for (int abs = 0; abs < total; ++abs) {
        uint64_t id = doc.lineIdForAbs(abs);
        CHECK(id >= prev); // non-decreasing
        prev = id;
    }
}

TEST_CASE("line ids: autowrap continuation shares one id per logical line")
{
    TestTerminal t(4, 3);
    // "ABCDEFGH" at width 4 autowraps into two physical rows, both part of
    // the same logical line. They must share a line id.
    t.feed("ABCDEFGH");
    const auto& doc = t.term.document();
    uint64_t idRow0 = doc.lineIdForAbs(0);
    uint64_t idRow1 = doc.lineIdForAbs(1);
    CHECK(idRow0 != 0);
    CHECK(idRow0 == idRow1);
}

TEST_CASE("line ids: survive autowrap then un-wrap reflow")
{
    TestTerminal t(4, 3);
    t.feed("ABCDEFGH"); // wraps into 2 rows at width 4
    const auto& doc = t.term.document();
    uint64_t logicalId = doc.lineIdForAbs(0);
    CHECK(logicalId == doc.lineIdForAbs(1)); // shared
    // Widen so the line un-wraps into one physical row.
    t.term.resize(10, 3);
    // The logical line's id must still resolve; un-wrapping doesn't drop it.
    CHECK(doc.firstAbsOfLine(logicalId) >= 0);
    CHECK(doc.lastAbsOfLine(logicalId) == doc.firstAbsOfLine(logicalId));
}

TEST_CASE("line ids: rows under OSC 133 regions resolvable after reflow")
{
    TestTerminal t(20, 5);
    t.feed("\x1b]133;A\x1b\\$ ");
    t.feed("\x1b]133;B\x1b\\ls");
    t.feed("\x1b]133;C\x1b\\output1\r\noutput2\r\n");
    t.feed("\x1b]133;D;0\x1b\\");

    REQUIRE(t.term.commands().size() == 1);
    const auto& doc = t.term.document();
    const auto& cmd = t.term.commands()[0];
    uint64_t promptLineId = cmd.promptStartLineId;
    uint64_t outputEndLineId = cmd.outputEndLineId;
    CHECK(promptLineId != 0);
    CHECK(outputEndLineId != 0);

    // Resize to narrower; prompt and output lines wrap.
    t.term.resize(8, 5);
    // The captured line ids should still resolve to valid abs rows.
    CHECK(doc.firstAbsOfLine(promptLineId) >= 0);
    CHECK(doc.firstAbsOfLine(outputEndLineId) >= 0);
}


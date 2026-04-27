#include <doctest/doctest.h>
#include "TestTerminal.h"

static MouseEvent makeMouseEvent(int col, int row, Button button = LeftButton,
                                 bool xRightHalf = false)
{
    MouseEvent ev;
    ev.x = col;
    ev.y = row;
    ev.globalX = col;
    ev.globalY = row;
    ev.xRightHalf = xRightHalf;
    ev.button  = button;
    ev.buttons = button;
    ev.modifiers = 0;
    return ev;
}

TEST_CASE("click alone does not create a selection")
{
    TestTerminal t;
    t.feed("Hello");

    auto press   = makeMouseEvent(0, 0);
    auto release = makeMouseEvent(0, 0);

    t.term.mousePressEvent(&press);
    t.term.mouseReleaseEvent(&release);

    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("click clears an existing selection")
{
    TestTerminal t;
    t.feed("Hello");

    // Drag to create a selection
    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(3, 0);
    auto rel   = makeMouseEvent(3, 0);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    REQUIRE(t.term.hasSelection());

    // Plain click should clear it
    auto press2 = makeMouseEvent(0, 0);
    auto rel2   = makeMouseEvent(0, 0);
    t.term.mousePressEvent(&press2);
    t.term.mouseReleaseEvent(&rel2);
    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("drag creates a selection")
{
    TestTerminal t;
    t.feed("Hello World");

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 0);
    auto rel   = makeMouseEvent(4, 0);

    t.term.mousePressEvent(&press);
    CHECK_FALSE(t.term.hasSelection()); // not yet — mouse hasn't moved
    t.term.mouseMoveEvent(&move);
    CHECK(t.term.hasSelection());       // now active
    t.term.mouseReleaseEvent(&rel);
    CHECK(t.term.hasSelection());       // finalized
    CHECK_FALSE(t.term.selectedText().empty());
}

TEST_CASE("selection text matches dragged range")
{
    TestTerminal t(20, 5);
    t.feed("Hello");

    // Drag from left edge of col 0 to right half of col 4 — the right-half
    // bit advances the trailing boundary past col 4 so 'o' is included.
    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);

    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "Hello");
}

TEST_CASE("selection survives width-change reflow")
{
    TestTerminal t(20, 5);
    t.feed("Hello");

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    REQUIRE(t.term.hasSelection());
    REQUIRE(t.term.selectedText() == "Hello");

    t.term.resize(10, 5);
    CHECK(t.term.hasSelection());
    CHECK(t.term.selectedText() == "Hello");

    t.term.resize(40, 5);
    CHECK(t.term.hasSelection());
    CHECK(t.term.selectedText() == "Hello");
}

TEST_CASE("multi-row selection of a wrapped line preserves text across reflow")
{
    // Regression: rowOffset-based storage lost text when the wrap point
    // moved. Switching to logical cell offset (iTerm2's approach) means
    // the same N cells of the logical line stay selected regardless of
    // how they're wrapped at any given width.
    TestTerminal t(10, 5);
    // 30 chars autowrap to 3 rows on a 10-col terminal:
    //   row 0: "AAAAAAAAAA"  (cells 0..9)
    //   row 1: "BBBBBBBBBB"  (cells 10..19)
    //   row 2: "CCCCCCCCCC"  (cells 20..29)
    t.feed("AAAAAAAAAABBBBBBBBBBCCCCCCCCCC");

    // Select all of row 0 + first 5 cols of row 1: cells 0..14. Right-half
    // of col 4 advances the trailing boundary past col 4.
    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    REQUIRE(t.term.selectedText() == "AAAAAAAAAABBBBB");

    // Resize wider so the line un-wraps onto one visual row. Post-fix the
    // same 15 logical cells remain selected (now at row 0 cols 0..14).
    t.term.resize(40, 5);
    CHECK(t.term.selectedText() == "AAAAAAAAAABBBBB");

    // Resize narrower so the line wraps to even more rows. Same cells.
    t.term.resize(5, 5);
    CHECK(t.term.selectedText() == "AAAAAAAAAABBBBB");
}

TEST_CASE("selection survives height-change reflow (regression)")
{
    TestTerminal t(20, 5);
    t.feed("Hello");

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 0, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    REQUIRE(t.term.selectedText() == "Hello");

    t.term.resize(20, 10);
    CHECK(t.term.selectedText() == "Hello");
}

TEST_CASE("selection on inner row of a wrapped logical line points at that row")
{
    // Regression: end anchor previously stored only `(lineId, col)` and
    // resolved via lastAbsOfLine, so dragging onto an inner visual row of a
    // wrapped logical line jumped the selection to the line's last row.
    // Storing rowOffset fixes it.
    TestTerminal t(10, 5);
    // 25 chars autowrap to 3 visual rows on a 10-col terminal, all sharing
    // one line id (rows 0, 1, 2 below):
    //   row 0: "AAAAAAAAAA"
    //   row 1: "BBBBBBBBBB"
    //   row 2: "CCCCC"
    t.feed("AAAAAAAAAABBBBBBBBBBCCCCC");

    // Click row 0 col 0, drag to row 1 col 4 right-half (inner row of
    // wrapped line). Right-half advances the trailing boundary past col 4.
    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    REQUIRE(t.term.hasSelection());

    // Selection text should be "AAAAAAAAAA" + "BBBBB" — first row plus
    // first 5 cols of second row. Pre-fix it would have included the third
    // row's "CCCCC" too because end resolved to the line's LAST visual row.
    CHECK(t.term.selectedText() == "AAAAAAAAAABBBBB");
}

TEST_CASE("backward drag from left edge of cell excludes that cell")
{
    // wezterm/iTerm2/Terminal.app behavior: starting a drag at the left
    // edge of a cell and dragging upward/backward should NOT include that
    // anchor cell. It only becomes part of the selection when the click
    // started past the cell midpoint.
    TestTerminal t(20, 5);
    t.feed("AAAAA\r\nBBBBB");

    // Press at far-left of (row 1, col 3) — left half of the cell.
    auto press = makeMouseEvent(3, 1, LeftButton, /*xRightHalf=*/false);
    // Drag backward/up to (row 0, col 1).
    auto move  = makeMouseEvent(1, 0, LeftButton, /*xRightHalf=*/false);
    auto rel   = makeMouseEvent(1, 0, LeftButton, /*xRightHalf=*/false);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    // Cell (1, 3) — the anchor cell — must not be in the selection.
    // Selection should cover (0, 1)..(0, last) and continue through
    // (1, 0)..(1, 2). The text on row 1 should stop before the 4th 'B'.
    std::string sel = t.term.selectedText();
    REQUIRE_FALSE(sel.empty());
    // Row 1 contribution: "BBB" (cols 0..2), not "BBBB".
    CHECK(sel.find("BBBB") == std::string::npos);
    CHECK(sel.find("BBB") != std::string::npos);
}

TEST_CASE("backward drag from right half of cell includes that cell")
{
    // Mirror of the above: starting in the right half snaps the anchor
    // boundary to the cell's right edge, so the cell is included.
    TestTerminal t(20, 5);
    t.feed("AAAAA\r\nBBBBB");

    auto press = makeMouseEvent(3, 1, LeftButton, /*xRightHalf=*/true);
    auto move  = makeMouseEvent(1, 0, LeftButton, /*xRightHalf=*/false);
    auto rel   = makeMouseEvent(1, 0, LeftButton, /*xRightHalf=*/false);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);
    std::string sel = t.term.selectedText();
    REQUIRE_FALSE(sel.empty());
    // Row 1 contribution: "BBBB" (cols 0..3), including the anchor cell.
    CHECK(sel.find("BBBB") != std::string::npos);
}

// (Eviction-past-archive-cap drop is exercised by hasSelection() /
// resolveSelection() returning empty, but a unit test for it requires
// flooding past `maxArchiveRows` (100 000 by default), which is too slow.
// Add a knob on resetScrollback for the archive cap and re-introduce the
// test if the behavior ever needs lockdown.)

#include "TestTerminal.h"
#include <doctest/doctest.h>

static MouseEvent makeMouseEvent(int col, int row, Button button = LeftButton,
                                 bool xRightHalf = false)
{
    MouseEvent ev;
    ev.x          = col;
    ev.y          = row;
    ev.globalX    = col;
    ev.globalY    = row;
    ev.xRightHalf = xRightHalf;
    ev.button     = button;
    ev.buttons    = button;
    ev.modifiers  = 0;
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
    CHECK(t.term.hasSelection()); // now active
    t.term.mouseReleaseEvent(&rel);
    CHECK(t.term.hasSelection()); // finalized
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

TEST_CASE("autowrapped line copies as one logical line (no embedded newline)")
{
    // Autowrap at 10 cols produces three soft-wrapped rows; selecting all of
    // them must copy a single string with no '\n', because the wrap is a
    // visual artifact, not part of the source text.
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123"); // 30 chars → rows 0..2

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(9, 2, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(9, 2, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    const std::string sel = t.term.selectedText();
    CHECK(sel == "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");
    CHECK(sel.find('\n') == std::string::npos);
}

TEST_CASE("explicit CR/LF separated lines preserve newlines on copy")
{
    // Counterpart to the autowrap case: when the source emits real line
    // breaks (\r\n), continued_ stays false on each row and copy must
    // preserve them as '\n'.
    TestTerminal t(20, 5);
    t.feed("First\r\nSecond\r\nThird");

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 2, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 2, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "First\nSecond\nThird");
}

TEST_CASE("autowrap row that exactly fills the line still joins on copy")
{
    // Edge case: line of length == cols does NOT autowrap on its own (cursor
    // sits at the right edge with wrapPending), but if more content follows
    // the next char triggers the wrap. The first row's continued_ should
    // still be set so a select-all returns one joined string.
    TestTerminal t(5, 5);
    t.feed("HELLOWORLD"); // exactly fills row 0, wraps to row 1

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(4, 1, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "HELLOWORLD");
}

TEST_CASE("trailing space on a soft-wrapped row survives copy")
{
    // A space in the last column of a wrapped row is real content of the
    // logical line, not padding; trailing-space trimming must skip it.
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHI JKL"); // row 0 = "ABCDEFGHI ", wraps; row 1 = "JKL"

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(2, 1, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(2, 1, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "ABCDEFGHI JKL");
}

TEST_CASE("trailing spaces on a non-wrapped row are still trimmed on copy")
{
    // Counterpart: selecting past the end of a short, hard-broken line must
    // not pick up the blank padding cells.
    TestTerminal t(10, 5);
    t.feed("AB\r\nCD");

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(9, 0, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(9, 0, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "AB");
}

// (Eviction-past-archive-cap drop is exercised by hasSelection() /
// resolveSelection() returning empty, but a unit test for it requires
// flooding past `maxArchiveRows` (100 000 by default), which is too slow.
// Add a knob on resetScrollback for the archive cap and re-introduce the
// test if the behavior ever needs lockdown.)

TEST_CASE("triple-click selects the whole line")
{
    TestTerminal t(20, 5);
    t.feed("first line\r\nsecond line\r\nthird line");

    t.term.startLineSelection(/*absRow=*/1);
    auto rel = makeMouseEvent(0, 0);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "second line");
}

TEST_CASE("triple-click + drag down extends selection by full lines")
{
    TestTerminal t(20, 5);
    t.feed("first line\r\nsecond line\r\nthird line\r\nfourth line");

    t.term.startLineSelection(/*absRow=*/1);
    auto move = makeMouseEvent(/*col=*/3, /*row=*/2);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(3, 2);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "second line\nthird line");
}

TEST_CASE("triple-click + drag up extends selection by full lines")
{
    TestTerminal t(20, 5);
    t.feed("first line\r\nsecond line\r\nthird line\r\nfourth line");

    t.term.startLineSelection(/*absRow=*/2);
    auto move = makeMouseEvent(/*col=*/5, /*row=*/0);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(5, 0);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "first line\nsecond line\nthird line");
}

TEST_CASE("triple-click + drag into same line keeps the original line")
{
    TestTerminal t(20, 5);
    t.feed("first line\r\nsecond line\r\nthird line");

    t.term.startLineSelection(/*absRow=*/1);
    auto move = makeMouseEvent(/*col=*/0, /*row=*/1);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(0, 1);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "second line");
}

TEST_CASE("triple-click + drag finalizes and copies to clipboard")
{
    TestTerminal t(20, 5);
    t.feed("first line\r\nsecond line\r\nthird line");

    t.term.startLineSelection(/*absRow=*/0);
    auto move = makeMouseEvent(/*col=*/0, /*row=*/1);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(0, 1);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.capturedClipboard == "first line\nsecond line");
    CHECK(t.capturedPrimary == "first line\nsecond line");
}

TEST_CASE("double-click selects the word under cursor")
{
    TestTerminal t(40, 5);
    t.feed("hello world foo");

    t.term.startWordSelection(/*col=*/2, /*absRow=*/0);
    auto rel = makeMouseEvent(2, 0);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "hello");
}

TEST_CASE("double-click + drag right extends selection to cover later words")
{
    TestTerminal t(40, 5);
    t.feed("hello world foo");

    t.term.startWordSelection(/*col=*/2, /*absRow=*/0);
    auto move = makeMouseEvent(/*col=*/13, /*row=*/0, LeftButton, /*xRightHalf=*/true);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(13, 0, LeftButton, /*xRightHalf=*/true);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "hello world foo");
}

TEST_CASE("double-click + drag left extends selection to cover earlier words")
{
    TestTerminal t(40, 5);
    t.feed("hello world foo");

    t.term.startWordSelection(/*col=*/13, /*absRow=*/0);
    auto move = makeMouseEvent(/*col=*/2, /*row=*/0);
    t.term.mouseMoveEvent(&move);
    auto rel = makeMouseEvent(2, 0);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "hello world foo");
}

TEST_CASE("double-click on a soft-wrapped word selects the whole word")
{
    // width=10. The word "abcdefghij" (10 chars) starts at col 4 of row 0,
    // so it occupies cols 4..9 of row 0 and cols 0..3 of row 1 — the wrap
    // splits a single logical word across two physical rows.
    TestTerminal t(10, 5);
    t.feed("foo abcdefghij klm");

    // Click in the first half of the wrapped word (row 0, col 5 = 'b').
    t.term.startWordSelection(/*col=*/5, /*absRow=*/0);
    auto rel = makeMouseEvent(5, 0);
    t.term.mouseReleaseEvent(&rel);
    CHECK(t.term.selectedText() == "abcdefghij");

    // And from the second half (row 1, col 1 = 'h').
    t.term.startWordSelection(/*col=*/1, /*absRow=*/1);
    auto rel2 = makeMouseEvent(1, 1);
    t.term.mouseReleaseEvent(&rel2);
    CHECK(t.term.selectedText() == "abcdefghij");
}

TEST_CASE("triple-click on a wrapped line selects the whole logical line")
{
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");

    t.term.startLineSelection(/*absRow=*/1);
    auto rel = makeMouseEvent(0, 0);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");
}

TEST_CASE("first visual row of a wrapped line in scrollback is selectable")
{
    // Repro: a wrapped logical line that's been scrolled back into history.
    // Clicking on its FIRST visual row and dragging within it (or onto it)
    // must produce a selection that highlights that row.
    TestTerminal t(10, 5);
    // 50 chars wrap to 5 rows on a 10-col terminal, then we push them all
    // into scrollback with extra lines, then scroll back to bring the
    // wrapped line back into view.
    t.feed("AAAAAAAAAABBBBBBBBBBCCCCCCCCCCDDDDDDDDDDEEEEEEEEEE\r\n");
    for (int i = 0; i < 5; ++i) {
        t.feed("filler\r\n");
    }
    int history = t.term.document().historySize();
    REQUIRE(history >= 5);
    t.term.scrollViewport(history);

    // The wrapped line's first visual row is at absRow 0 (it was pushed
    // into scrollback first). Find it explicitly so the test doesn't
    // depend on exact scrollback geometry.
    int firstWrappedAbs = -1;
    for (int abs = 0; abs < history + t.term.height(); ++abs) {
        uint64_t id = t.term.document().lineIdForAbs(abs);
        if (id != 0 && t.term.document().firstAbsOfLine(id) == abs) {
            const Cell *row = (abs < t.term.document().historySize())
                ? t.term.document().historyRow(abs)
                : t.term.grid().row(abs - t.term.document().historySize());
            if (row && row[0].wc == U'A') {
                firstWrappedAbs = abs;
                break;
            }
        }
    }
    REQUIRE(firstWrappedAbs >= 0);

    int viewportOff = t.term.viewportOffset();
    int viewRow     = firstWrappedAbs - (t.term.document().historySize() - viewportOff);
    REQUIRE(viewRow >= 0);
    REQUIRE(viewRow < t.term.height());

    // Click at col 2 of the first visual row, drag to col 5 within same row.
    auto press = makeMouseEvent(2, viewRow);
    auto move  = makeMouseEvent(5, viewRow, LeftButton, /*xRightHalf=*/true);
    auto rel   = makeMouseEvent(5, viewRow, LeftButton, /*xRightHalf=*/true);
    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.hasSelection());
    CHECK(t.term.selectedText() == "AAAA");
}

// Helper: enter alt screen, paint some text, and start a selection on a
// given alt-screen row. Returns the absRow used.
static int paintAltAndSelect(TestTerminal &t, int altRow, int colStart, int colEnd)
{
    t.feed("\x1b[?1049h");
    // Position cursor at row 1 (1-based) col 1, paint a known string.
    t.feed("\x1b[1;1H");
    t.feed("hello world");
    t.feed("\x1b[2;1H");
    t.feed("second row");
    t.feed("\x1b[3;1H");
    t.feed("third row");

    int absRow = t.term.document().historySize() + altRow;
    // Start + extend selection to cover [colStart..colEnd] on `altRow`.
    auto press = makeMouseEvent(colStart, altRow);
    t.term.startSelection(colStart, absRow);
    auto move   = makeMouseEvent(colEnd, altRow, LeftButton, /*xRightHalf=*/true);
    int absMove = t.term.document().historySize() + altRow;
    t.term.updateSelection(colEnd, absMove, /*xRightHalf=*/true);
    t.term.finalizeSelection();
    return absRow;
}

TEST_CASE("alt screen: writing to a selected row clears the selection (kitty parity)")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/0, /*colStart=*/0, /*colEnd=*/4);
    REQUIRE(t.term.hasSelection());
    REQUIRE(t.term.selectedText() == "hello");

    // Write into row 0 — must clear the selection.
    t.feed("\x1b[1;1H");
    t.feed("X");
    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("alt screen: writing to a different row leaves the selection intact")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/0, /*colStart=*/0, /*colEnd=*/4);
    REQUIRE(t.term.hasSelection());

    // Write into row 2 (well outside the row-0 selection) — selection survives.
    t.feed("\x1b[3;1H");
    t.feed("Y");
    CHECK(t.term.hasSelection());
    CHECK(t.term.selectedText() == "hello");
}

TEST_CASE("alt screen: EL on the selected row clears the selection")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/1, /*colStart=*/0, /*colEnd=*/5);
    REQUIRE(t.term.hasSelection());

    // Move cursor to row 2 (1-based) and EL 2 — wipes the entire row.
    t.feed("\x1b[2;1H\x1b[2K");
    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("alt screen: EL on a different row leaves the selection intact")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/1, /*colStart=*/0, /*colEnd=*/5);
    REQUIRE(t.term.hasSelection());

    // EL on row 3 — selection on row 1 survives.
    t.feed("\x1b[3;1H\x1b[2K");
    CHECK(t.term.hasSelection());
}

TEST_CASE("alt screen: ED 2 (clear screen) wipes the selection")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/1, /*colStart=*/0, /*colEnd=*/5);
    REQUIRE(t.term.hasSelection());

    t.feed("\x1b[2J");
    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("alt screen: scroll within the scroll region clears an intersecting selection")
{
    TestTerminal t(40, 5);
    paintAltAndSelect(t, /*altRow=*/1, /*colStart=*/0, /*colEnd=*/5);
    REQUIRE(t.term.hasSelection());

    // Force a scroll by writing past the bottom row.
    t.feed("\x1b[5;1H\n");
    CHECK_FALSE(t.term.hasSelection());
}

TEST_CASE("main screen: writing to selected row leaves the selection intact")
{
    // Main-screen behavior is unchanged — line-id anchoring keeps the
    // selection bound to the original logical line even when the visible
    // grid row is overwritten.
    TestTerminal t(40, 5);
    t.feed("hello\r\nworld\r\nthird\r\n");
    int absRow = 0; // first scrollback row holds "hello"

    t.term.startSelection(0, absRow);
    t.term.updateSelection(4, absRow, /*xRightHalf=*/true);
    t.term.finalizeSelection();
    REQUIRE(t.term.hasSelection());
    REQUIRE(t.term.selectedText() == "hello");

    // Overwrite the current cursor row (which is row 3 of the visible
    // grid, with "hello" already in scrollback). Selection is on the
    // scrollback line; this write does not affect it.
    t.feed("XYZ");
    CHECK(t.term.hasSelection());
}

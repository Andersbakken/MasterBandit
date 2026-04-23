#include <doctest/doctest.h>
#include "TestTerminal.h"

// Push enough lines to get content into scrollback history.
// With a 5-row terminal, writing 10 lines pushes 5 into history.
static void fillScrollback(TestTerminal& t, int extraLines)
{
    int rows = t.term.height();
    for (int i = 0; i < rows + extraLines; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
}

TEST_CASE("scrollback history accumulates on overflow")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 3);
    CHECK(t.term.document().historySize() >= 3);
}

TEST_CASE("scrollViewport changes viewportOffset")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int history = t.term.document().historySize();
    REQUIRE(history >= 1);

    t.term.scrollViewport(1);
    CHECK(t.term.viewportOffset() == 1);
}

TEST_CASE("scrollViewport clamps to history size")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 3);
    int history = t.term.document().historySize();

    t.term.scrollViewport(history + 100);
    CHECK(t.term.viewportOffset() == history);
}

TEST_CASE("scrollViewport clamps at zero going negative")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 3);
    t.term.scrollViewport(2);
    t.term.scrollViewport(-100);
    CHECK(t.term.viewportOffset() == 0);
}

TEST_CASE("resetViewport returns to live view")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    t.term.scrollViewport(3);
    REQUIRE(t.term.viewportOffset() == 3);
    t.term.resetViewport();
    CHECK(t.term.viewportOffset() == 0);
}

TEST_CASE("scrollByPixels: integer rows behave like scrollViewport")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int cellH = 20;
    t.term.scrollByPixels(cellH * 2, cellH); // two full cells up
    CHECK(t.term.viewportOffset() == 2);
    CHECK(t.term.topPixelSubY() == 0);
}

TEST_CASE("scrollByPixels: sub-cell advances only topPixelSubY, clamps to [0, cellH)")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int cellH = 20;
    t.term.scrollByPixels(5, cellH);
    CHECK(t.term.viewportOffset() == 0);
    CHECK(t.term.topPixelSubY() == 5);

    // Another 18 pixels rolls 3 px over into a whole row.
    t.term.scrollByPixels(18, cellH);
    CHECK(t.term.viewportOffset() == 1);
    CHECK(t.term.topPixelSubY() == 3);
}

TEST_CASE("scrollByPixels: clamps at top (can't scroll past live)")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int cellH = 20;
    t.term.scrollByPixels(-9999, cellH);
    CHECK(t.term.viewportOffset() == 0);
    CHECK(t.term.topPixelSubY() == 0);
}

TEST_CASE("scrollByPixels: clamps at oldest history")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int cellH = 20;
    int histSize = t.term.document().historySize();
    t.term.scrollByPixels(999999, cellH);
    CHECK(t.term.viewportOffset() == histSize);
    CHECK(t.term.topPixelSubY() == 0);
}

TEST_CASE("topLineId stays stable while content streams in (user scrolled back)")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    t.term.scrollViewport(2);
    uint64_t pinnedLineId = t.term.topLineId();
    int pinnedOffset = t.term.viewportOffset();
    REQUIRE(pinnedOffset == 2);

    // Three more lines stream in. User stays pinned to the same CONTENT —
    // topLineId unchanged, viewportOffset grows by 3 because historySize
    // grew by 3 with a stable abs anchor below it.
    t.feed("newA\r\nnewB\r\nnewC\r\n");
    CHECK(t.term.topLineId() == pinnedLineId);
    CHECK(t.term.viewportOffset() == pinnedOffset + 3);
}

TEST_CASE("topLineId advances automatically while live-tailing")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    REQUIRE(t.term.viewportOffset() == 0);
    uint64_t liveBefore = t.term.topLineId();

    // One more line appends into history. In live mode the anchor must
    // advance; otherwise the viewport drifts backward one row per line.
    t.feed("newD\r\n");
    CHECK(t.term.viewportOffset() == 0);
    CHECK(t.term.topLineId() != liveBefore);
}

TEST_CASE("resetViewport restores live after scrollByPixels")
{
    TestTerminal t(20, 5);
    fillScrollback(t, 5);
    int cellH = 20;
    t.term.scrollByPixels(cellH + 7, cellH);
    REQUIRE(t.term.viewportOffset() == 1);
    REQUIRE(t.term.topPixelSubY() == 7);
    t.term.resetViewport();
    CHECK(t.term.viewportOffset() == 0);
    CHECK(t.term.topPixelSubY() == 0);
}

TEST_CASE("viewportRow with offset returns history content")
{
    TestTerminal t(20, 5);
    // Write lines so we know content: "line0" through "line7"
    for (int i = 0; i < 8; ++i)
        t.feed("line" + std::to_string(i) + "\r\n");

    // With 5-row terminal and 8 lines written, 3 lines are in history.
    // Scroll 1 line into history.
    t.term.scrollViewport(1);
    std::vector<Cell> row(t.term.width());
    REQUIRE(t.term.copyViewportRow(0, row));
    // The first cell of that row should be 'l' (start of "lineX")
    CHECK(row[0].wc == U'l');
}

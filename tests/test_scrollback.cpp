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

TEST_CASE("viewportRow with offset returns history content")
{
    TestTerminal t(20, 5);
    // Write lines so we know content: "line0" through "line7"
    for (int i = 0; i < 8; ++i)
        t.feed("line" + std::to_string(i) + "\r\n");

    // With 5-row terminal and 8 lines written, 3 lines are in history.
    // Scroll 1 line into history.
    t.term.scrollViewport(1);
    const Cell* row = t.term.viewportRow(0);
    REQUIRE(row != nullptr);
    // The first cell of that row should be 'l' (start of "lineX")
    CHECK(row[0].wc == U'l');
}

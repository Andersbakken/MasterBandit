#include <doctest/doctest.h>
#include "Terminal.h"

// Spec tests for Terminal::isCellCoveredByPopup, the helper the renderer uses
// to decide whether the main pane's cursor should be hidden. A popup that does
// NOT overlap the cursor cell should leave the cursor visible; a popup that
// DOES overlap should hide it.

// Helper: create a Terminal with popup children at given cell rects.
// We use createPopup with dummy PlatformCallbacks (headless, no PTY).
struct TestTerminalWithPopups {
    std::unique_ptr<Terminal> term;

    TestTerminalWithPopups() {
        PlatformCallbacks pcbs;
        TerminalCallbacks cbs;
        term = std::make_unique<Terminal>(std::move(pcbs), std::move(cbs));
        TerminalOptions opts;
        opts.scrollbackLines = 0;
        term->initHeadless(opts);
        term->resize(80, 24);
    }

    void addPopupRect(int x, int y, int w, int h) {
        PlatformCallbacks pcbs;
        term->createPopup("popup_" + std::to_string(x) + "_" + std::to_string(y),
                          x, y, w, h, std::move(pcbs));
    }
};

TEST_CASE("Terminal::isCellCoveredByPopup: no popups → never covered")
{
    TestTerminalWithPopups tp;
    CHECK_FALSE(tp.term->isCellCoveredByPopup(0, 0));
    CHECK_FALSE(tp.term->isCellCoveredByPopup(10, 5));
    CHECK_FALSE(tp.term->isCellCoveredByPopup(100, 100));
}

TEST_CASE("Terminal::isCellCoveredByPopup: cell inside popup rect")
{
    TestTerminalWithPopups tp;
    tp.addPopupRect(10, 5, 20, 8);  // x=[10,30), y=[5,13)

    // Interior points.
    CHECK(tp.term->isCellCoveredByPopup(10, 5));      // top-left inclusive
    CHECK(tp.term->isCellCoveredByPopup(20, 9));      // center
    CHECK(tp.term->isCellCoveredByPopup(29, 12));     // bottom-right inclusive (exclusive bounds)
}

TEST_CASE("Terminal::isCellCoveredByPopup: cells outside popup rect")
{
    TestTerminalWithPopups tp;
    tp.addPopupRect(10, 5, 20, 8);  // x=[10,30), y=[5,13)

    // Above.
    CHECK_FALSE(tp.term->isCellCoveredByPopup(15, 4));
    // Below.
    CHECK_FALSE(tp.term->isCellCoveredByPopup(15, 13));
    // Left.
    CHECK_FALSE(tp.term->isCellCoveredByPopup(9, 8));
    // Right (x=30 is exclusive).
    CHECK_FALSE(tp.term->isCellCoveredByPopup(30, 8));
    // Far away.
    CHECK_FALSE(tp.term->isCellCoveredByPopup(0, 0));
    CHECK_FALSE(tp.term->isCellCoveredByPopup(100, 100));
}

TEST_CASE("Terminal::isCellCoveredByPopup: multiple popups — covered by any")
{
    TestTerminalWithPopups tp;
    tp.addPopupRect(0, 0, 10, 5);    // top-left box
    tp.addPopupRect(40, 20, 15, 10); // bottom-right box

    CHECK(tp.term->isCellCoveredByPopup(5, 2));        // inside first
    CHECK(tp.term->isCellCoveredByPopup(50, 25));      // inside second
    CHECK_FALSE(tp.term->isCellCoveredByPopup(20, 10)); // in the gap between them
    CHECK_FALSE(tp.term->isCellCoveredByPopup(10, 0));  // exactly on first box's right edge (exclusive)
}

TEST_CASE("Terminal::isCellCoveredByPopup: rect edges are half-open [x, x+w)")
{
    TestTerminalWithPopups tp;
    tp.addPopupRect(5, 5, 3, 3);  // x=[5,8), y=[5,8)

    CHECK(tp.term->isCellCoveredByPopup(5, 5));       // inclusive corner
    CHECK(tp.term->isCellCoveredByPopup(7, 7));       // last inclusive cell
    CHECK_FALSE(tp.term->isCellCoveredByPopup(8, 5)); // just past right edge
    CHECK_FALSE(tp.term->isCellCoveredByPopup(5, 8)); // just past bottom edge
    CHECK_FALSE(tp.term->isCellCoveredByPopup(8, 8)); // past both
}

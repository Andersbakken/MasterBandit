#include <doctest/doctest.h>
#include "Pane.h"

// Spec tests for Pane::isCellCoveredByPopup, the helper the renderer uses to
// decide whether the main pane's cursor should be hidden. A popup that does NOT
// overlap the cursor cell should leave the cursor visible; a popup that DOES
// overlap should hide it.

// Build a PopupPane with rect coords. `terminal` stays null — these tests
// exercise geometry only.
static PopupPane makePopup(int x, int y, int w, int h)
{
    PopupPane p;
    p.id = "test";
    p.cellX = x; p.cellY = y; p.cellW = w; p.cellH = h;
    return p;
}

// Helper: create a pane with the given popup rects. We poke mPopups directly
// via a custom subclass to avoid createPopup's PlatformCallbacks machinery.
struct TestPane : public Pane {
    TestPane() : Pane(0) {}
    void addPopupRect(int x, int y, int w, int h) {
        auto& v = const_cast<std::vector<PopupPane>&>(popups());
        v.push_back(makePopup(x, y, w, h));
    }
};

TEST_CASE("Pane::isCellCoveredByPopup: no popups → never covered")
{
    TestPane p;
    CHECK_FALSE(p.isCellCoveredByPopup(0, 0));
    CHECK_FALSE(p.isCellCoveredByPopup(10, 5));
    CHECK_FALSE(p.isCellCoveredByPopup(100, 100));
}

TEST_CASE("Pane::isCellCoveredByPopup: cell inside popup rect")
{
    TestPane p;
    p.addPopupRect(10, 5, 20, 8);  // x=[10,30), y=[5,13)

    // Interior points.
    CHECK(p.isCellCoveredByPopup(10, 5));      // top-left inclusive
    CHECK(p.isCellCoveredByPopup(20, 9));      // center
    CHECK(p.isCellCoveredByPopup(29, 12));     // bottom-right inclusive (exclusive bounds)
}

TEST_CASE("Pane::isCellCoveredByPopup: cells outside popup rect")
{
    TestPane p;
    p.addPopupRect(10, 5, 20, 8);  // x=[10,30), y=[5,13)

    // Above.
    CHECK_FALSE(p.isCellCoveredByPopup(15, 4));
    // Below.
    CHECK_FALSE(p.isCellCoveredByPopup(15, 13));
    // Left.
    CHECK_FALSE(p.isCellCoveredByPopup(9, 8));
    // Right (x=30 is exclusive).
    CHECK_FALSE(p.isCellCoveredByPopup(30, 8));
    // Far away.
    CHECK_FALSE(p.isCellCoveredByPopup(0, 0));
    CHECK_FALSE(p.isCellCoveredByPopup(100, 100));
}

TEST_CASE("Pane::isCellCoveredByPopup: multiple popups — covered by any")
{
    TestPane p;
    p.addPopupRect(0, 0, 10, 5);    // top-left box
    p.addPopupRect(40, 20, 15, 10); // bottom-right box

    CHECK(p.isCellCoveredByPopup(5, 2));        // inside first
    CHECK(p.isCellCoveredByPopup(50, 25));      // inside second
    CHECK_FALSE(p.isCellCoveredByPopup(20, 10)); // in the gap between them
    CHECK_FALSE(p.isCellCoveredByPopup(10, 0));  // exactly on first box's right edge (exclusive)
}

TEST_CASE("Pane::isCellCoveredByPopup: rect edges are half-open [x, x+w)")
{
    TestPane p;
    p.addPopupRect(5, 5, 3, 3);  // x=[5,8), y=[5,8)

    CHECK(p.isCellCoveredByPopup(5, 5));       // inclusive corner
    CHECK(p.isCellCoveredByPopup(7, 7));       // last inclusive cell
    CHECK_FALSE(p.isCellCoveredByPopup(8, 5)); // just past right edge
    CHECK_FALSE(p.isCellCoveredByPopup(5, 8)); // just past bottom edge
    CHECK_FALSE(p.isCellCoveredByPopup(8, 8)); // past both
}

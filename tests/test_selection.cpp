#include <doctest/doctest.h>
#include "TestTerminal.h"

static MouseEvent makeMouseEvent(int col, int row, Button button = LeftButton)
{
    MouseEvent ev;
    ev.x = col;
    ev.y = row;
    ev.globalX = col;
    ev.globalY = row;
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

    auto press = makeMouseEvent(0, 0);
    auto move  = makeMouseEvent(4, 0);
    auto rel   = makeMouseEvent(4, 0);

    t.term.mousePressEvent(&press);
    t.term.mouseMoveEvent(&move);
    t.term.mouseReleaseEvent(&rel);

    CHECK(t.term.selectedText() == "Hello");
}

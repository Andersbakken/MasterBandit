#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("CUP - cursor position")
{
    TestTerminal t;
    t.csi("5;10H"); // row 5, col 10 (1-based)
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

TEST_CASE("CUP - clamps to grid")
{
    TestTerminal t(80, 24);
    t.csi("999;999H");
    CHECK(t.term.cursorX() == 79);
    CHECK(t.term.cursorY() == 23);
}

TEST_CASE("CUU - cursor up")
{
    TestTerminal t;
    t.csi("5;5H");
    t.csi("2A"); // up 2
    CHECK(t.term.cursorY() == 2);
    CHECK(t.term.cursorX() == 4);
}

TEST_CASE("CUD - cursor down")
{
    TestTerminal t;
    t.csi("2A"); // no-op at row 0 (clamps)
    t.csi("3B"); // down 3
    CHECK(t.term.cursorY() == 3);
}

TEST_CASE("CUF - cursor forward")
{
    TestTerminal t;
    t.csi("5C"); // right 5
    CHECK(t.term.cursorX() == 5);
}

TEST_CASE("CUB - cursor back")
{
    TestTerminal t;
    t.csi("10;10H");
    t.csi("3D"); // left 3
    CHECK(t.term.cursorX() == 6);
}

TEST_CASE("CHA - cursor horizontal absolute")
{
    TestTerminal t;
    t.csi("10G"); // col 10 (1-based)
    CHECK(t.term.cursorX() == 9);
}

TEST_CASE("save and restore cursor")
{
    TestTerminal t;
    t.csi("5;10H");
    t.esc("7");   // DECSC save
    t.csi("1;1H");
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    t.esc("8");   // DECRC restore
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

TEST_CASE("text written at cursor position")
{
    TestTerminal t;
    t.csi("3;5H"); // row 3, col 5 (1-based) → (4, 2) 0-indexed
    t.feed("Hi");
    CHECK(t.wc(4, 2) == U'H');
    CHECK(t.wc(5, 2) == U'i');
}

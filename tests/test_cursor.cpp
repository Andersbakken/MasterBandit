#include "TestTerminal.h"
#include <doctest/doctest.h>

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

TEST_CASE("CUP - zero param defaults to 1")
{
    // ECMA-48 / xterm: a parameter value of 0 is equivalent to 1. mpv's
    // kitty VO hits this when edge-positioning a frame at row/col 0.
    TestTerminal t(80, 24);
    t.csi("5;10H");
    t.csi("0;0H");
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi("0H"); // single-param 0
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi("1;0H"); // row 1, col 0
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi("0;5H"); // row 0, col 5 → (4, 0)
    CHECK(t.term.cursorX() == 4);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("CUP - extra params after the second are ignored")
{
    TestTerminal t(80, 24);
    t.csi("3;7;9H"); // third param ignored
    CHECK(t.term.cursorX() == 6);
    CHECK(t.term.cursorY() == 2);

    t.csi("5;10;1;2;3H"); // many extras, still row 5 col 10
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

TEST_CASE("CUP - empty params keep defaults")
{
    TestTerminal t(80, 24);
    t.csi("5;10H");
    t.csi("H"); // bare CSI H → (1,1)
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi(";H"); // empty row, empty col → (1,1)
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi(";5H"); // empty row, col 5 → (1, 5)
    CHECK(t.term.cursorX() == 4);
    CHECK(t.term.cursorY() == 0);

    t.csi("5;10H");
    t.csi("7;H"); // row 7, empty col → (7, 1)
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 6);
}

TEST_CASE("HVP - same semantics as CUP")
{
    TestTerminal t(80, 24);
    t.csi("4;8f"); // HVP, row 4, col 8
    CHECK(t.term.cursorX() == 7);
    CHECK(t.term.cursorY() == 3);

    t.csi("0;0f"); // 0 → 1
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
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
    t.esc("7"); // DECSC save
    t.csi("1;1H");
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    t.esc("8"); // DECRC restore
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

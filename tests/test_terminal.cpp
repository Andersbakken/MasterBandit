#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("basic text output")
{
    TestTerminal t;
    t.feed("Hello");
    CHECK(t.wc(0, 0) == U'H');
    CHECK(t.wc(1, 0) == U'e');
    CHECK(t.wc(2, 0) == U'l');
    CHECK(t.wc(3, 0) == U'l');
    CHECK(t.wc(4, 0) == U'o');
    CHECK(t.term.cursorX() == 5);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("rowText helper")
{
    TestTerminal t;
    t.feed("Hello");
    CHECK(t.rowText(0) == "Hello");
}

TEST_CASE("carriage return and line feed")
{
    TestTerminal t;
    t.feed("AB\r\nCD");
    CHECK(t.rowText(0) == "AB");
    CHECK(t.wc(0, 1) == U'C');
    CHECK(t.wc(1, 1) == U'D');
    CHECK(t.term.cursorX() == 2);
    CHECK(t.term.cursorY() == 1);
}

TEST_CASE("line wrap")
{
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJKL"); // 12 chars into a 10-wide terminal
    CHECK(t.rowText(0) == "ABCDEFGHIJ");
    CHECK(t.wc(0, 1) == U'K');
    CHECK(t.wc(1, 1) == U'L');
}

TEST_CASE("erase in line - to end")
{
    TestTerminal t;
    t.feed("Hello World");
    t.csi("5G");   // move to col 5 (1-based) → col index 4
    t.csi("0K");   // erase to end of line
    CHECK(t.rowText(0) == "Hell");
}

TEST_CASE("erase in line - whole line")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("2K");
    CHECK(t.rowText(0) == "");
}

TEST_CASE("UTF-8 multibyte text")
{
    TestTerminal t;
    t.feed("caf\xC3\xA9"); // "café"
    CHECK(t.wc(0, 0) == U'c');
    CHECK(t.wc(1, 0) == U'a');
    CHECK(t.wc(2, 0) == U'f');
    CHECK(t.wc(3, 0) == U'\u00e9'); // é
}

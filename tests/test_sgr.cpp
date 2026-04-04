#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("SGR reset")
{
    TestTerminal t;
    t.csi("1m");   // bold
    t.csi("0m");   // reset
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).bold());
}

TEST_CASE("SGR bold")
{
    TestTerminal t;
    t.csi("1m");
    t.feed("A");
    CHECK(t.attrs(0, 0).bold());
}

TEST_CASE("SGR italic")
{
    TestTerminal t;
    t.csi("3m");
    t.feed("A");
    CHECK(t.attrs(0, 0).italic());
}

TEST_CASE("SGR underline")
{
    TestTerminal t;
    t.csi("4m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
}

TEST_CASE("SGR fg 256-color")
{
    TestTerminal t;
    t.csi("38;5;196m"); // bright red from 256-color cube
    t.feed("A");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    // color 196 = #ff0000
    CHECK(t.attrs(0, 0).fgR() == 0xff);
    CHECK(t.attrs(0, 0).fgG() == 0x00);
    CHECK(t.attrs(0, 0).fgB() == 0x00);
}

TEST_CASE("SGR fg truecolor")
{
    TestTerminal t;
    t.csi("38;2;255;128;0m"); // orange
    t.feed("A");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).fgR() == 255);
    CHECK(t.attrs(0, 0).fgG() == 128);
    CHECK(t.attrs(0, 0).fgB() == 0);
}

TEST_CASE("SGR bg truecolor")
{
    TestTerminal t;
    t.csi("48;2;0;128;255m"); // blue background
    t.feed("A");
    CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).bgR() == 0);
    CHECK(t.attrs(0, 0).bgG() == 128);
    CHECK(t.attrs(0, 0).bgB() == 255);
}

TEST_CASE("SGR bg 256-color")
{
    TestTerminal t;
    t.csi("48;5;21m"); // blue from 256-color cube
    t.feed("A");
    CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
    // color 21 = #0000ff
    CHECK(t.attrs(0, 0).bgR() == 0x00);
    CHECK(t.attrs(0, 0).bgG() == 0x00);
    CHECK(t.attrs(0, 0).bgB() == 0xff);
}

TEST_CASE("SGR attributes persist across cells")
{
    TestTerminal t;
    t.csi("1m");   // bold
    t.feed("AB");
    CHECK(t.attrs(0, 0).bold());
    CHECK(t.attrs(1, 0).bold());
}

TEST_CASE("SGR attributes reset mid-line")
{
    TestTerminal t;
    t.csi("1m");
    t.feed("A");
    t.csi("0m");
    t.feed("B");
    CHECK(t.attrs(0, 0).bold());
    CHECK_FALSE(t.attrs(1, 0).bold());
}

TEST_CASE("SGR fg default restores default mode")
{
    TestTerminal t;
    t.csi("38;2;255;0;0m");
    t.feed("A");
    t.csi("39m"); // default fg
    t.feed("B");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    CHECK(t.attrs(1, 0).fgMode() == CellAttrs::Default);
}

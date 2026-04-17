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
    CHECK(t.attrs(0, 0).underlineStyle() == 0); // straight
}

TEST_CASE("SGR 4:3 curly underline")
{
    TestTerminal t;
    t.csi("4:3m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    CHECK(t.attrs(0, 0).underlineStyle() == 2); // curly
}

TEST_CASE("SGR 4:2 double underline")
{
    TestTerminal t;
    t.csi("4:2m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    CHECK(t.attrs(0, 0).underlineStyle() == 1); // double
}

TEST_CASE("SGR 4:4 dotted underline")
{
    TestTerminal t;
    t.csi("4:4m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    CHECK(t.attrs(0, 0).underlineStyle() == 3); // dotted
}

TEST_CASE("SGR 4:0 removes underline")
{
    TestTerminal t;
    t.csi("4:3m");
    t.csi("4:0m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).underline());
}

TEST_CASE("SGR 24 removes underline and style")
{
    TestTerminal t;
    t.csi("4:3m");
    t.csi("24m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).underline());
    CHECK(t.attrs(0, 0).underlineStyle() == 0);
}

TEST_CASE("SGR 58 sets underline color")
{
    TestTerminal t;
    t.csi("4m");              // underline on
    t.csi("58;2;255;0;128m"); // underline color: red+half-blue
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK(ex->underlineColor != 0);
}

TEST_CASE("SGR 59 resets underline color")
{
    TestTerminal t;
    t.csi("4m");
    t.csi("58;2;255;0;0m");
    t.csi("59m");
    t.feed("A");
    // underlineColor should be 0 (use fg), or no CellExtra
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    CHECK((ex == nullptr || ex->underlineColor == 0));
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

// --- Strikethrough ---

TEST_CASE("SGR 9 strikethrough")
{
    TestTerminal t;
    t.csi("9m");
    t.feed("A");
    CHECK(t.attrs(0, 0).strikethrough());
}

TEST_CASE("SGR 29 removes strikethrough")
{
    TestTerminal t;
    t.csi("9m");
    t.csi("29m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).strikethrough());
}

// --- Dim/faint ---

TEST_CASE("SGR 2 dim")
{
    TestTerminal t;
    t.csi("2m");
    t.feed("A");
    CHECK(t.attrs(0, 0).dim());
}

TEST_CASE("SGR 22 removes dim")
{
    TestTerminal t;
    t.csi("2m");
    t.csi("22m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).dim());
}

// --- Colon-separated color forms ---

TEST_CASE("SGR 38:2::R:G:B colon truecolor fg with empty colorspace")
{
    TestTerminal t;
    t.csi("38:2::200:100:50m");
    t.feed("A");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).fgR() == 200);
    CHECK(t.attrs(0, 0).fgG() == 100);
    CHECK(t.attrs(0, 0).fgB() == 50);
}

TEST_CASE("SGR 38:2:R:G:B colon truecolor fg without colorspace")
{
    TestTerminal t;
    t.csi("38:2:200:100:50m");
    t.feed("A");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).fgR() == 200);
    CHECK(t.attrs(0, 0).fgG() == 100);
    CHECK(t.attrs(0, 0).fgB() == 50);
}

TEST_CASE("SGR 38:5:IDX colon 256-color fg")
{
    TestTerminal t;
    t.csi("38:5:196m");
    t.feed("A");
    CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).fgR() == 0xff);
    CHECK(t.attrs(0, 0).fgG() == 0x00);
    CHECK(t.attrs(0, 0).fgB() == 0x00);
}

TEST_CASE("SGR 48:2::R:G:B colon truecolor bg with empty colorspace")
{
    TestTerminal t;
    t.csi("48:2::0:128:255m");
    t.feed("A");
    CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).bgR() == 0);
    CHECK(t.attrs(0, 0).bgG() == 128);
    CHECK(t.attrs(0, 0).bgB() == 255);
}

TEST_CASE("SGR 48:5:IDX colon 256-color bg")
{
    TestTerminal t;
    t.csi("48:5:21m");
    t.feed("A");
    CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
    CHECK(t.attrs(0, 0).bgR() == 0x00);
    CHECK(t.attrs(0, 0).bgG() == 0x00);
    CHECK(t.attrs(0, 0).bgB() == 0xff);
}

TEST_CASE("SGR 58:2::R:G:B colon underline color with empty colorspace")
{
    TestTerminal t;
    t.csi("4m");
    t.csi("58:2::255:100:100m");
    t.feed("A");
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK((ex->underlineColor & 0xFF) == 255);
    CHECK(((ex->underlineColor >> 8) & 0xFF) == 100);
    CHECK(((ex->underlineColor >> 16) & 0xFF) == 100);
}

TEST_CASE("SGR 58:2:R:G:B colon underline color without colorspace")
{
    TestTerminal t;
    t.csi("4m");
    t.csi("58:2:255:100:100m");
    t.feed("A");
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK((ex->underlineColor & 0xFF) == 255);
    CHECK(((ex->underlineColor >> 8) & 0xFF) == 100);
    CHECK(((ex->underlineColor >> 16) & 0xFF) == 100);
}

TEST_CASE("SGR 58:5:IDX colon 256-color underline")
{
    TestTerminal t;
    t.csi("4m");
    t.csi("58:5:214m");
    t.feed("A");
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK(ex->underlineColor != 0);
}

TEST_CASE("SGR 4:3 + 58:2::R:G:B curly underline with colon color")
{
    TestTerminal t;
    t.csi("4:3m");
    t.csi("58:2::255:80:80m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    CHECK(t.attrs(0, 0).underlineStyle() == 2); // curly
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK((ex->underlineColor & 0xFF) == 255);
    CHECK(((ex->underlineColor >> 8) & 0xFF) == 80);
    CHECK(((ex->underlineColor >> 16) & 0xFF) == 80);
}

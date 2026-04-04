#include <doctest/doctest.h>
#include "TestTerminal.h"
#include "Config.h"
#include "Utils.h"

// Helper: get default palette color as r,g,b
static void defaultPaletteColor(int idx, uint8_t& r, uint8_t& g, uint8_t& b) {
    ColorScheme cs;
    const std::string* colors[] = {
        &cs.color0, &cs.color1, &cs.color2, &cs.color3,
        &cs.color4, &cs.color5, &cs.color6, &cs.color7,
        &cs.color8, &cs.color9, &cs.color10, &cs.color11,
        &cs.color12, &cs.color13, &cs.color14, &cs.color15
    };
    color::parseHex(*colors[idx], r, g, b);
}

// ── dim, blink, inverse, invisible, strikethrough ────────────────────────────

TEST_CASE("SGR dim")
{
    TestTerminal t;
    t.csi("2m");
    t.feed("A");
    CHECK(t.attrs(0, 0).dim());
}

TEST_CASE("SGR blink")
{
    TestTerminal t;
    t.csi("5m");
    t.feed("A");
    CHECK(t.attrs(0, 0).blink());
}

TEST_CASE("SGR inverse")
{
    TestTerminal t;
    t.csi("7m");
    t.feed("A");
    CHECK(t.attrs(0, 0).inverse());
}

TEST_CASE("SGR invisible")
{
    TestTerminal t;
    t.csi("8m");
    t.feed("A");
    CHECK(t.attrs(0, 0).invisible());
}

TEST_CASE("SGR strikethrough")
{
    TestTerminal t;
    t.csi("9m");
    t.feed("A");
    CHECK(t.attrs(0, 0).strikethrough());
}

// ── turn-off codes ───────────────────────────────────────────────────────────

TEST_CASE("SGR turn off bold and dim (22)")
{
    TestTerminal t;
    t.csi("1m");
    t.csi("22m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).bold());
    CHECK_FALSE(t.attrs(0, 0).dim());
}

TEST_CASE("SGR turn off italic (23)")
{
    TestTerminal t;
    t.csi("3m");
    t.csi("23m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).italic());
}

TEST_CASE("SGR turn off underline (24)")
{
    TestTerminal t;
    t.csi("4m");
    t.csi("24m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).underline());
}

TEST_CASE("SGR turn off blink (25)")
{
    TestTerminal t;
    t.csi("5m");
    t.csi("25m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).blink());
}

TEST_CASE("SGR turn off inverse (27)")
{
    TestTerminal t;
    t.csi("7m");
    t.csi("27m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).inverse());
}

TEST_CASE("SGR turn off invisible (28)")
{
    TestTerminal t;
    t.csi("8m");
    t.csi("28m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).invisible());
}

TEST_CASE("SGR turn off strikethrough (29)")
{
    TestTerminal t;
    t.csi("9m");
    t.csi("29m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).strikethrough());
}

// ── 16-color standard foreground (30-37) ─────────────────────────────────────

TEST_CASE("SGR fg standard colors 30-37")
{
    for (int i = 0; i < 8; ++i) {
        TestTerminal t;
        t.csi(std::to_string(30 + i) + "m");
        t.feed("A");
        CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
        { uint8_t er, eg, eb; defaultPaletteColor(i, er, eg, eb);
        CHECK(t.attrs(0, 0).fgR() == er);
        CHECK(t.attrs(0, 0).fgG() == eg);
        CHECK(t.attrs(0, 0).fgB() == eb); }
    }
}

// ── 16-color standard background (40-47) ─────────────────────────────────────

TEST_CASE("SGR bg standard colors 40-47")
{
    for (int i = 0; i < 8; ++i) {
        TestTerminal t;
        t.csi(std::to_string(40 + i) + "m");
        t.feed("A");
        CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
        { uint8_t er, eg, eb; defaultPaletteColor(i, er, eg, eb);
        CHECK(t.attrs(0, 0).bgR() == er);
        CHECK(t.attrs(0, 0).bgG() == eg);
        CHECK(t.attrs(0, 0).bgB() == eb); }
    }
}

// ── bright foreground (90-97) ────────────────────────────────────────────────

TEST_CASE("SGR fg bright colors 90-97")
{
    for (int i = 0; i < 8; ++i) {
        TestTerminal t;
        t.csi(std::to_string(90 + i) + "m");
        t.feed("A");
        CHECK(t.attrs(0, 0).fgMode() == CellAttrs::RGB);
        { uint8_t er, eg, eb; defaultPaletteColor(8 + i, er, eg, eb);
        CHECK(t.attrs(0, 0).fgR() == er);
        CHECK(t.attrs(0, 0).fgG() == eg);
        CHECK(t.attrs(0, 0).fgB() == eb); }
    }
}

// ── bright background (100-107) ──────────────────────────────────────────────

TEST_CASE("SGR bg bright colors 100-107")
{
    for (int i = 0; i < 8; ++i) {
        TestTerminal t;
        t.csi(std::to_string(100 + i) + "m");
        t.feed("A");
        CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
        { uint8_t er, eg, eb; defaultPaletteColor(8 + i, er, eg, eb);
        CHECK(t.attrs(0, 0).bgR() == er);
        CHECK(t.attrs(0, 0).bgG() == eg);
        CHECK(t.attrs(0, 0).bgB() == eb); }
    }
}

// ── default background restore ───────────────────────────────────────────────

TEST_CASE("SGR bg default restores default mode (49)")
{
    TestTerminal t;
    t.csi("48;2;0;255;0m");
    t.feed("A");
    t.csi("49m");
    t.feed("B");
    CHECK(t.attrs(0, 0).bgMode() == CellAttrs::RGB);
    CHECK(t.attrs(1, 0).bgMode() == CellAttrs::Default);
}

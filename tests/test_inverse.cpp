#include <doctest/doctest.h>
#include "TestTerminal.h"
#include <algorithm>

// Simulates what resolveRow does for fg/bg with inverse applied.
static std::pair<uint32_t, uint32_t> resolvedColors(const CellAttrs& a)
{
    uint32_t fg = a.packFgAsU32();
    uint32_t bg = a.packBgAsU32();
    if (a.inverse()) std::swap(fg, bg);
    return {fg, bg};
}

TEST_CASE("SGR inverse swaps fg and bg colors")
{
    TestTerminal t;
    t.csi("38;2;255;0;0m");   // fg = red
    t.csi("48;2;0;0;255m");   // bg = blue
    t.csi("7m");               // inverse on
    t.feed("A");

    auto [fg, bg] = resolvedColors(t.attrs(0, 0));

    // After inverse: fg should be what was bg (blue) and vice-versa
    // packFgAsU32 / packBgAsU32 pack as 0xAARRGGBB with R in low byte...
    // actually RGBA: R in bits 0-7, so blue = 0xFF0000FF
    // bg (blue 0,0,255): packed as (0) | (0<<8) | (255<<16) | 0xFF000000 = 0xFF0000FF...
    // wait: packBgAsU32 = R<<0 | G<<8 | B<<16 | 0xFF000000
    // blue (R=0,G=0,B=255): 0x00 | 0x00<<8 | 0xFF<<16 | 0xFF000000 = 0xFFFF0000
    // red (R=255,G=0,B=0): 0xFF | 0x00<<8 | 0x00<<16 | 0xFF000000 = 0xFF0000FF

    // After inverse:
    // fg (was bg=blue) = 0xFFFF0000
    // bg (was fg=red)  = 0xFF0000FF
    CHECK(fg == 0xFFFF0000u); // blue
    CHECK(bg == 0xFF0000FFu); // red
}

TEST_CASE("SGR inverse with default colors uses swapped defaults")
{
    TestTerminal t;
    t.csi("7m");
    t.feed("A");

    CHECK(t.attrs(0, 0).inverse());

    auto [fg, bg] = resolvedColors(t.attrs(0, 0));
    // Default fg packed = 0xFFFFFFFF, default bg packed = 0.
    // After swap: fg = 0 (was bg default), bg = 0xFFFFFFFF (was fg default).
    CHECK(fg == 0u);
    CHECK(bg == 0xFFFFFFFFu);
}

TEST_CASE("SGR inverse off restores original colors")
{
    TestTerminal t;
    t.csi("38;2;255;0;0m");   // fg = red
    t.csi("48;2;0;0;255m");   // bg = blue
    t.csi("7m");               // inverse on
    t.csi("27m");              // inverse off
    t.feed("A");

    CHECK_FALSE(t.attrs(0, 0).inverse());

    auto [fg, bg] = resolvedColors(t.attrs(0, 0));
    CHECK(fg == 0xFF0000FFu); // red (R=255 in low byte)
    CHECK(bg == 0xFFFF0000u); // blue (B=255 in bits 16-23)
}

TEST_CASE("SGR inverse persists across cells")
{
    TestTerminal t;
    t.csi("7m");
    t.feed("AB");
    CHECK(t.attrs(0, 0).inverse());
    CHECK(t.attrs(1, 0).inverse());
}

TEST_CASE("SGR reset clears inverse")
{
    TestTerminal t;
    t.csi("7m");
    t.csi("0m");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).inverse());
}

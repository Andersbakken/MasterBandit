#include "TestTerminal.h"
#include <doctest/doctest.h>

// Default palette color 0 from Config.h: #3a3a3a
// Default fg: #dddddd, bg: #000000, cursor: #cccccc

// ---------------------------------------------------------------------------
// OSC 4: query individual palette entries
// ---------------------------------------------------------------------------

TEST_CASE("OSC 4 query index 0 returns config default")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("4;0;?");
    // #3a3a3a → rgb:3a3a/3a3a/3a3a (8-bit scaled to 16-bit: 0x3a * 257 = 0x3a3a)
    CHECK(t.output() == "\x1b]4;0;rgb:3a3a/3a3a/3a3a\x1b\\");
}

TEST_CASE("OSC 4 query index 1 returns config default red")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("4;1;?");
    // #cc0403
    CHECK(t.output() == "\x1b]4;1;rgb:cccc/0404/0303\x1b\\");
}

TEST_CASE("OSC 4 set index 0 and query roundtrip")
{
    TestTerminal t;
    t.osc("4;0;#ff0000");
    t.clearOutput();
    t.osc("4;0;?");
    CHECK(t.output() == "\x1b]4;0;rgb:ffff/0000/0000\x1b\\");
}

TEST_CASE("OSC 4 set with rgb: format and query")
{
    TestTerminal t;
    t.osc("4;5;rgb:aa/bb/cc");
    t.clearOutput();
    t.osc("4;5;?");
    CHECK(t.output() == "\x1b]4;5;rgb:aaaa/bbbb/cccc\x1b\\");
}

TEST_CASE("OSC 4 multiple pairs in one sequence")
{
    TestTerminal t;
    t.osc("4;0;#ff0000;1;#00ff00");
    t.clearOutput();
    t.osc("4;0;?");
    CHECK(t.output() == "\x1b]4;0;rgb:ffff/0000/0000\x1b\\");
    t.clearOutput();
    t.osc("4;1;?");
    CHECK(t.output() == "\x1b]4;1;rgb:0000/ffff/0000\x1b\\");
}

TEST_CASE("OSC 4 query index 200 returns computed cube color before any override")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("4;200;?");
    // idx=200: 200-16=184; bi=184%6=4→55+4*40=215, gi=(184/6)%6=0→0, ri=184/36=5→55+5*40=255
    // rgb(255, 0, 215)
    CHECK(t.output() == "\x1b]4;200;rgb:ffff/0000/d7d7\x1b\\");
}

TEST_CASE("OSC 4 set index 200 overrides computed color")
{
    TestTerminal t;
    t.osc("4;200;#123456");
    t.clearOutput();
    t.osc("4;200;?");
    CHECK(t.output() == "\x1b]4;200;rgb:1212/3434/5656\x1b\\");
}

// ---------------------------------------------------------------------------
// OSC 104: reset palette entries
// ---------------------------------------------------------------------------

TEST_CASE("OSC 104 with no args resets all palette entries")
{
    TestTerminal t;
    // Override index 0 and index 200
    t.osc("4;0;#ff0000");
    t.osc("4;200;#123456");
    // Reset all
    t.osc("104");
    t.clearOutput();
    t.osc("4;0;?");
    // Should be back to config default #3a3a3a
    CHECK(t.output() == "\x1b]4;0;rgb:3a3a/3a3a/3a3a\x1b\\");
    t.clearOutput();
    t.osc("4;200;?");
    // Should be back to computed cube color
    CHECK(t.output() == "\x1b]4;200;rgb:ffff/0000/d7d7\x1b\\");
}

TEST_CASE("OSC 104 with specific index resets only that entry")
{
    TestTerminal t;
    t.osc("4;0;#ff0000");
    t.osc("4;1;#00ff00");
    t.osc("104;0");
    t.clearOutput();
    t.osc("4;0;?");
    // Index 0 reset to default
    CHECK(t.output() == "\x1b]4;0;rgb:3a3a/3a3a/3a3a\x1b\\");
    t.clearOutput();
    t.osc("4;1;?");
    // Index 1 still overridden
    CHECK(t.output() == "\x1b]4;1;rgb:0000/ffff/0000\x1b\\");
}

TEST_CASE("OSC 104 reset index 200 reverts to computed color")
{
    TestTerminal t;
    t.osc("4;200;#aabbcc");
    t.osc("104;200");
    t.clearOutput();
    t.osc("4;200;?");
    CHECK(t.output() == "\x1b]4;200;rgb:ffff/0000/d7d7\x1b\\");
}

// ---------------------------------------------------------------------------
// OSC 110/111/112: reset default fg/bg/cursor to config defaults
// ---------------------------------------------------------------------------

TEST_CASE("OSC 110 resets fg to config default after OSC 10 change")
{
    TestTerminal t;
    t.osc("10;#ff8800");
    // Verify it changed
    t.clearOutput();
    t.osc("10;?");
    CHECK(t.output() == "\x1b]10;rgb:ffff/8888/0000\x1b\\");
    // Now reset
    t.osc("110;");
    t.clearOutput();
    t.osc("10;?");
    // Default fg is #dddddd
    CHECK(t.output() == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\");
}

TEST_CASE("OSC 111 resets bg to config default")
{
    TestTerminal t;
    t.osc("11;#336699");
    t.osc("111;");
    t.clearOutput();
    t.osc("11;?");
    // Default bg is #000000
    CHECK(t.output() == "\x1b]11;rgb:0000/0000/0000\x1b\\");
}

TEST_CASE("OSC 112 resets cursor color to config default")
{
    TestTerminal t;
    t.osc("12;#ff0000");
    t.osc("112;");
    t.clearOutput();
    t.osc("12;?");
    // Default cursor is #cccccc
    CHECK(t.output() == "\x1b]12;rgb:cccc/cccc/cccc\x1b\\");
}

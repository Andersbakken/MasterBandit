#include "TestTerminal.h"
#include <doctest/doctest.h>

// ---------------------------------------------------------------------------
// DECRQSS: DCS $ q <subparam> ST
// Responses: DCS 1 $ r <result> ST (found) or DCS 0 $ r ST (unknown)
// ---------------------------------------------------------------------------

// Cursor shape queries (DCS $ q SP q)

TEST_CASE("DECRQSS cursor shape default is blinking block (1)")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("$q q");
    CHECK(t.output() == "\x1bP1$r1 q\x1b\\");
}

TEST_CASE("DECRQSS cursor shape after CSI 2 SP q (steady block)")
{
    TestTerminal t;
    t.csi("2 q");
    t.clearOutput();
    t.dcs("$q q");
    CHECK(t.output() == "\x1bP1$r2 q\x1b\\");
}

TEST_CASE("DECRQSS cursor shape after CSI 3 SP q (blinking underline)")
{
    TestTerminal t;
    t.csi("3 q");
    t.clearOutput();
    t.dcs("$q q");
    CHECK(t.output() == "\x1bP1$r3 q\x1b\\");
}

TEST_CASE("DECRQSS cursor shape after CSI 6 SP q (steady bar)")
{
    TestTerminal t;
    t.csi("6 q");
    t.clearOutput();
    t.dcs("$q q");
    CHECK(t.output() == "\x1bP1$r6 q\x1b\\");
}

// SGR state queries (DCS $ q m)

TEST_CASE("DECRQSS SGR with no attributes returns 0")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r0m\x1b\\");
}

TEST_CASE("DECRQSS SGR after bold (CSI 1m)")
{
    TestTerminal t;
    t.csi("1m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r1m\x1b\\");
}

TEST_CASE("DECRQSS SGR after bold + italic (CSI 1;3m)")
{
    TestTerminal t;
    t.csi("1;3m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r1;3m\x1b\\");
}

TEST_CASE("DECRQSS SGR after dim + inverse + strikethrough")
{
    TestTerminal t;
    t.csi("2;7;9m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r2;7;9m\x1b\\");
}

TEST_CASE("DECRQSS SGR with truecolor fg (CSI 38;2;255;0;0m)")
{
    TestTerminal t;
    t.csi("38;2;255;0;0m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r38;2;255;0;0m\x1b\\");
}

TEST_CASE("DECRQSS SGR with 256-color bg (CSI 48;5;200m) serializes as RGB")
{
    // SGR converts 256-color indices to RGB at storage time.
    // Index 200: cube value = rgb(255,0,215)
    TestTerminal t;
    t.csi("48;5;200m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r48;2;255;0;215m\x1b\\");
}

TEST_CASE("DECRQSS SGR with ANSI named fg color (CSI 31m) serializes as RGB")
{
    // SGR converts ANSI named colors to RGB at storage time using the palette.
    // Config default for color 1 (red) is #cc0403
    TestTerminal t;
    t.csi("31m");
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r38;2;204;4;3m\x1b\\");
}

TEST_CASE("DECRQSS SGR reset to 0 after CSI 0m clears attrs")
{
    TestTerminal t;
    t.csi("1;3m");
    t.csi("0m"); // reset
    t.clearOutput();
    t.dcs("$qm");
    CHECK(t.output() == "\x1bP1$r0m\x1b\\");
}

// Scroll margins queries (DCS $ q r)

TEST_CASE("DECRQSS scroll margins default (full terminal 80x24)")
{
    TestTerminal t; // default 80x24
    t.clearOutput();
    t.dcs("$qr");
    CHECK(t.output() == "\x1bP1$r1;24r\x1b\\");
}

TEST_CASE("DECRQSS scroll margins after CSI 5;20 r")
{
    TestTerminal t;
    t.csi("5;20r");
    t.clearOutput();
    t.dcs("$qr");
    CHECK(t.output() == "\x1bP1$r5;20r\x1b\\");
}

TEST_CASE("DECRQSS scroll margins reset after CSI r (no args = full terminal)")
{
    TestTerminal t;
    t.csi("5;20r");
    t.csi("r"); // reset to full terminal
    t.clearOutput();
    t.dcs("$qr");
    CHECK(t.output() == "\x1bP1$r1;24r\x1b\\");
}

// Unknown subparam

TEST_CASE("DECRQSS unknown subparam returns 0 status")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("$qx");
    CHECK(t.output() == "\x1bP0$r\x1b\\");
}

TEST_CASE("DECRQSS empty subparam returns 0 status")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("$q");
    CHECK(t.output() == "\x1bP0$r\x1b\\");
}

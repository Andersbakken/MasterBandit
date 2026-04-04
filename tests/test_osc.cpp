#include <doctest/doctest.h>
#include "TestTerminal.h"

TEST_CASE("OSC 0 sets window title")
{
    TestTerminal t;
    t.osc("0;My Window Title");
    CHECK(t.capturedTitle == "My Window Title");
}

TEST_CASE("OSC 2 sets window title")
{
    TestTerminal t;
    t.osc("2;Hello Terminal");
    CHECK(t.capturedTitle == "Hello Terminal");
}

TEST_CASE("OSC 1 sets icon name")
{
    TestTerminal t;
    t.osc("1;myicon");
    CHECK(t.capturedIcon == "myicon");
}

TEST_CASE("OSC title with ESC-backslash terminator")
{
    TestTerminal t;
    // ST-terminated (ESC \) instead of BEL
    t.feed("\x1b]2;ST Title\x1b\\");
    CHECK(t.capturedTitle == "ST Title");
}

TEST_CASE("OSC title updates on successive calls")
{
    TestTerminal t;
    t.osc("2;First");
    CHECK(t.capturedTitle == "First");
    t.osc("2;Second");
    CHECK(t.capturedTitle == "Second");
}

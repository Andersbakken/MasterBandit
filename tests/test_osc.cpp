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

// ── OSC 7 (CWD) ─────────────────────────────────────────────────────────────

TEST_CASE("OSC 7 extracts path from file URL")
{
    TestTerminal t;
    t.osc("7;file://localhost/home/user/project");
    CHECK(t.capturedCWD == "/home/user/project");
}

TEST_CASE("OSC 7 handles missing hostname")
{
    TestTerminal t;
    t.osc("7;file:///tmp");
    CHECK(t.capturedCWD == "/tmp");
}

// ── OSC 8 (Hyperlinks) ──────────────────────────────────────────────────────

TEST_CASE("OSC 8 sets hyperlink on cells")
{
    TestTerminal t;
    t.osc("8;;https://example.com");
    t.feed("click");
    t.osc("8;;");

    // Cells 0-4 should have hyperlink extras
    const CellExtra* ex = t.term.grid().getExtra(0, 0);
    REQUIRE(ex != nullptr);
    CHECK(ex->hyperlinkId != 0);

    const std::string* uri = t.term.hyperlinkURI(ex->hyperlinkId);
    REQUIRE(uri != nullptr);
    CHECK(*uri == "https://example.com");

    // Cell 5 should not have a hyperlink
    const CellExtra* ex5 = t.term.grid().getExtra(5, 0);
    CHECK((ex5 == nullptr || ex5->hyperlinkId == 0));
}

TEST_CASE("OSC 8 clears active hyperlink")
{
    TestTerminal t;
    t.osc("8;;https://example.com");
    t.feed("A");
    t.osc("8;;");
    t.feed("B");

    const CellExtra* exA = t.term.grid().getExtra(0, 0);
    REQUIRE(exA != nullptr);
    CHECK(exA->hyperlinkId != 0);

    const CellExtra* exB = t.term.grid().getExtra(1, 0);
    CHECK((exB == nullptr || exB->hyperlinkId == 0));
}

TEST_CASE("OSC 8 with id= reuses same hyperlink entry")
{
    TestTerminal t;
    t.osc("8;id=link1;https://example.com");
    t.feed("A");
    t.osc("8;;");
    t.feed(" ");
    t.osc("8;id=link1;https://example.com");
    t.feed("B");
    t.osc("8;;");

    const CellExtra* exA = t.term.grid().getExtra(0, 0);
    const CellExtra* exB = t.term.grid().getExtra(2, 0);
    REQUIRE(exA != nullptr);
    REQUIRE(exB != nullptr);
    CHECK(exA->hyperlinkId == exB->hyperlinkId);
}

// ── OSC 99 (Desktop Notifications) ──────────────────────────────────────────

TEST_CASE("OSC 99 fires notification on d=1")
{
    TestTerminal t;
    t.osc("99;d=0:p=title;Hello World");
    CHECK(t.capturedNotifyTitle.empty()); // not fired yet
    t.osc("99;d=1:p=body;This is the body");
    CHECK(t.capturedNotifyTitle == "Hello World");
    CHECK(t.capturedNotifyBody == "This is the body");
}

TEST_CASE("OSC 99 does not fire without d=1")
{
    TestTerminal t;
    t.osc("99;d=0:p=title;Test");
    CHECK(t.capturedNotifyTitle.empty());
}

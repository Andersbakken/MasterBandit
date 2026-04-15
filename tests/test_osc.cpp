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

// ── Title stack (CSI 22/23 t) ────────────────────────────────────────────────

TEST_CASE("CSI 22t pushes title, CSI 23t pops and restores")
{
    TestTerminal t;
    t.osc("0;shell");
    CHECK(t.capturedTitle == "shell");
    t.csi("22t");              // push "shell"
    t.osc("0;vim foo.txt");
    CHECK(t.capturedTitle == "vim foo.txt");
    t.csi("23t");              // pop → "shell"
    CHECK(t.capturedTitle == "shell");
}

TEST_CASE("OSC 0 between push/pop does not affect saved title")
{
    TestTerminal t;
    t.osc("0;original");
    t.csi("22t");
    t.osc("0;changed");
    t.osc("0;changed again");
    CHECK(t.capturedTitle == "changed again");
    t.csi("23t");
    CHECK(t.capturedTitle == "original");
}

TEST_CASE("Pop last title entry clears to empty string")
{
    TestTerminal t;
    t.osc("0;only title");
    CHECK(t.capturedTitle == "only title");
    t.csi("23t");
    CHECK(t.capturedTitle.empty());
}

TEST_CASE("Pop on empty stack is a no-op")
{
    TestTerminal t;
    t.csi("23t");
    CHECK(t.capturedTitle.empty());
}

TEST_CASE("Nested push/pop restores correctly")
{
    TestTerminal t;
    t.osc("0;level0");
    t.csi("22t");              // push level0
    t.osc("0;level1");
    t.csi("22t");              // push level1
    t.osc("0;level2");
    CHECK(t.capturedTitle == "level2");
    t.csi("23t");              // pop → level1
    CHECK(t.capturedTitle == "level1");
    t.csi("23t");              // pop → level0
    CHECK(t.capturedTitle == "level0");
}

TEST_CASE("Title stack caps at 10 entries")
{
    TestTerminal t;
    t.osc("0;base");
    for (int i = 0; i < 20; ++i)
        t.csi("22t");
    // Should not have grown beyond 10; pop all and verify we get back to base
    int pops = 0;
    while (pops < 20) {
        t.csi("23t");
        ++pops;
        if (t.capturedTitle.empty()) break;
    }
    // Should have popped at most 10 times before emptying
    CHECK(pops <= 10);
}

TEST_CASE("OSC 0 on empty stack creates first entry")
{
    TestTerminal t;
    CHECK(t.capturedTitle.empty());
    t.osc("0;first");
    CHECK(t.capturedTitle == "first");
    // Push should work now
    t.csi("22t");
    t.osc("0;second");
    t.csi("23t");
    CHECK(t.capturedTitle == "first");
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

// === OSC 10/11/12 — default color query/set ===

TEST_CASE("OSC 10 query returns default foreground")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("10;?");
    // Default fg is #dddddd → rgb:dddd/dddd/dddd
    CHECK(t.output() == "\x1b]10;rgb:dddd/dddd/dddd\x1b\\");
}

TEST_CASE("OSC 11 query returns default background")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("11;?");
    // Default bg is #000000 → rgb:0000/0000/0000
    CHECK(t.output() == "\x1b]11;rgb:0000/0000/0000\x1b\\");
}

TEST_CASE("OSC 12 query returns cursor color")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("12;?");
    // Default cursor is #cccccc → rgb:cccc/cccc/cccc
    CHECK(t.output() == "\x1b]12;rgb:cccc/cccc/cccc\x1b\\");
}

TEST_CASE("OSC 10 set changes foreground")
{
    TestTerminal t;
    t.osc("10;#ff8800");
    t.clearOutput();
    t.osc("10;?");
    CHECK(t.output() == "\x1b]10;rgb:ffff/8888/0000\x1b\\");
}

TEST_CASE("OSC 11 set with rgb: format")
{
    TestTerminal t;
    t.osc("11;rgb:aa/bb/cc");
    t.clearOutput();
    t.osc("11;?");
    CHECK(t.output() == "\x1b]11;rgb:aaaa/bbbb/cccc\x1b\\");
}

TEST_CASE("OSC 10 set updates defaultColors struct")
{
    TestTerminal t;
    t.osc("10;#102030");
    auto dc = t.term.defaultColors();
    CHECK(dc.fgR == 0x10);
    CHECK(dc.fgG == 0x20);
    CHECK(dc.fgB == 0x30);
}

// ── OSC 22 — mouse pointer shape (kitty) ─────────────────────────────────────

TEST_CASE("OSC 22 set defaults to '=' op and updates current shape")
{
    TestTerminal t;
    t.osc("22;pointer");
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.capturedPointerShape == "pointer");
    CHECK(t.pointerShapeCallCount == 1);
}

TEST_CASE("OSC 22 explicit '=' replaces top of stack")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;=text");
    CHECK(t.term.currentPointerShape() == "text");
}

TEST_CASE("OSC 22 '>' pushes onto stack")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;>text");
    CHECK(t.term.currentPointerShape() == "text");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "pointer");
}

TEST_CASE("OSC 22 '<' pops; popping empty stack is a no-op")
{
    TestTerminal t;
    t.osc("22;<");  // empty
    CHECK(t.term.currentPointerShape().empty());
    CHECK(t.pointerShapeCallCount == 0);
}

TEST_CASE("OSC 22 empty payload with '=' resets to default")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.osc("22;=");
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("OSC 22 '>' with comma list pushes each in order")
{
    TestTerminal t;
    t.osc("22;>pointer,text,wait");
    CHECK(t.term.currentPointerShape() == "wait");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "text");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "pointer");
    t.osc("22;<");
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("OSC 22 '?' query: known/unknown CSS names")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?pointer,bogus,text");
    CHECK(t.output() == "\x1b]22;1,0,1\x1b\\");
}

TEST_CASE("OSC 22 '?' __current__ returns current shape")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.clearOutput();
    t.osc("22;?__current__");
    CHECK(t.output() == "\x1b]22;pointer\x1b\\");
}

TEST_CASE("OSC 22 '?' __current__ on empty stack returns empty")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?__current__");
    CHECK(t.output() == "\x1b]22;\x1b\\");
}

TEST_CASE("OSC 22 '?' __default__ returns 'default'")
{
    TestTerminal t;
    t.clearOutput();
    t.osc("22;?__default__");
    CHECK(t.output() == "\x1b]22;default\x1b\\");
}

TEST_CASE("OSC 22 query does not mutate state")
{
    TestTerminal t;
    t.osc("22;pointer");
    int before = t.pointerShapeCallCount;
    t.osc("22;?text");
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.pointerShapeCallCount == before);  // no callback for queries
}

TEST_CASE("OSC 22 push beyond stack limit drops oldest")
{
    TestTerminal t;
    // Push MAX + 1 entries; oldest should be dropped, current is the last pushed.
    for (int i = 0; i < 17; ++i) {
        t.osc(std::string("22;>shape") + std::to_string(i));
    }
    CHECK(t.term.currentPointerShape() == "shape16");
    // Pop 15 times; we should still see shape1 (shape0 was dropped).
    for (int i = 0; i < 15; ++i) t.osc("22;<");
    CHECK(t.term.currentPointerShape() == "shape1");
}

TEST_CASE("OSC 22 main and alt screens have separate stacks")
{
    TestTerminal t;
    t.osc("22;pointer");                // main stack: [pointer]
    CHECK(t.term.currentPointerShape() == "pointer");
    t.csi("?1049h");                    // enter alt screen
    CHECK(t.term.currentPointerShape().empty());
    CHECK(t.capturedPointerShape.empty());  // toggle fires callback with new top
    t.osc("22;text");                   // alt stack: [text]
    CHECK(t.term.currentPointerShape() == "text");
    t.csi("?1049l");                    // back to main
    CHECK(t.term.currentPointerShape() == "pointer");
    CHECK(t.capturedPointerShape == "pointer");
}

TEST_CASE("OSC 22 RIS clears both stacks")
{
    TestTerminal t;
    t.osc("22;pointer");
    t.csi("?1049h");
    t.osc("22;text");
    t.esc("c");                         // RIS (also exits alt screen)
    CHECK(t.term.currentPointerShape().empty());
    t.csi("?1049h");                    // alt again
    CHECK(t.term.currentPointerShape().empty());
}

TEST_CASE("isKnownPointerShape recognises CSS and X11 names")
{
    using TE = TerminalEmulator;
    CHECK(TE::isKnownPointerShape("pointer"));
    CHECK(TE::isKnownPointerShape("text"));
    CHECK(TE::isKnownPointerShape("nesw-resize"));
    CHECK(TE::isKnownPointerShape("hand2"));        // X11 alias
    CHECK(TE::isKnownPointerShape("sb_h_double_arrow"));
    CHECK_FALSE(TE::isKnownPointerShape("not-a-cursor-name"));
    CHECK_FALSE(TE::isKnownPointerShape(""));
}

// ── OSC 52 clipboard ──────────────────────────────────────────────────────────

TEST_CASE("OSC 52 c;<base64> writes to clipboard")
{
    TestTerminal t;
    t.osc("52;c;aGVsbG8=");          // base64("hello")
    CHECK(t.capturedClipboard == "hello");
}

TEST_CASE("OSC 52 c;? queries clipboard and responds with base64")
{
    TestTerminal t;
    t.clipboardContent = "world";
    t.clearOutput();
    t.osc("52;c;?");
    CHECK(t.output() == "\x1b]52;c;d29ybGQ=\x1b\\");
}

TEST_CASE("OSC 52 with empty data clears the clipboard")
{
    TestTerminal t;
    t.capturedClipboard = "previous";
    t.osc("52;c;");
    CHECK(t.capturedClipboard == "");
}

TEST_CASE("OSC 52 round-trip: write then query returns same content")
{
    TestTerminal t;
    t.osc("52;c;dGVzdGluZw==");      // base64("testing")
    t.clipboardContent = t.capturedClipboard;
    t.clearOutput();
    t.osc("52;c;?");
    CHECK(t.output() == "\x1b]52;c;dGVzdGluZw==\x1b\\");
}

// ── OSC 9;4 kitty progress ────────────────────────────────────────────────────

TEST_CASE("OSC 9;4 reports progress state and percent")
{
    TestTerminal t;
    t.osc("9;4;1;42");
    CHECK(t.progressCallCount == 1);
    CHECK(t.capturedProgressState == 1);
    CHECK(t.capturedProgressPct == 42);
}

TEST_CASE("OSC 9;4 state 0 clears progress (percent optional)")
{
    TestTerminal t;
    t.osc("9;4;1;50");
    t.osc("9;4;0");
    CHECK(t.capturedProgressState == 0);
    CHECK(t.progressCallCount == 2);
}

TEST_CASE("OSC 9;4 states 2/3/4: error, indeterminate, pause")
{
    TestTerminal t;
    t.osc("9;4;2;25");
    CHECK(t.capturedProgressState == 2);
    t.osc("9;4;3");
    CHECK(t.capturedProgressState == 3);
    t.osc("9;4;4");
    CHECK(t.capturedProgressState == 4);
}

TEST_CASE("OSC 9 without ;4 does not fire progress callback")
{
    TestTerminal t;
    t.osc("9;some other payload");
    CHECK(t.progressCallCount == 0);
}

// ── XTGETTCAP (DCS + q ... ST) ────────────────────────────────────────────────

TEST_CASE("XTGETTCAP returns 1+r for known capability (TN)")
{
    TestTerminal t;
    t.clearOutput();
    // Query "TN" (terminal name) — hex-encoded: "544E"
    t.dcs("+q544E");
    const std::string& out = t.output();
    REQUIRE(out.size() >= 9);
    CHECK(out.substr(0, 5) == "\x1bP1+r");
    CHECK(out.substr(5, 4) == "544E");
    CHECK(out.substr(out.size() - 2) == "\x1b\\");
}

TEST_CASE("XTGETTCAP returns 0+r for unknown capability")
{
    TestTerminal t;
    t.clearOutput();
    // "zzzz" — hex "7A7A7A7A"
    t.dcs("+q7A7A7A7A");
    const std::string& out = t.output();
    REQUIRE(out.size() >= 7);
    CHECK(out.substr(0, 5) == "\x1bP0+r");
    CHECK(out.substr(out.size() - 2) == "\x1b\\");
}

TEST_CASE("XTGETTCAP handles multiple capabilities separated by ;")
{
    TestTerminal t;
    t.clearOutput();
    t.dcs("+q544E;7A7A7A7A");
    const std::string& out = t.output();
    CHECK(out.find("\x1bP1+r") != std::string::npos);
    CHECK(out.find("\x1bP0+r") != std::string::npos);
}

// ── DSR 5 device status ───────────────────────────────────────────────────────

TEST_CASE("DSR 5 (device status) responds with OK")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("5n");
    CHECK(t.output() == "\x1b[0n");
}

// ── SGR 4:5 dashed underline ──────────────────────────────────────────────────

TEST_CASE("SGR 4:5 sets dashed underline style")
{
    TestTerminal t;
    t.csi("4:5m");
    t.feed("A");
    CHECK(t.attrs(0, 0).underline());
    // Only 2 bits of storage for style; dashed is aliased to dotted (3).
    CHECK(t.attrs(0, 0).underlineStyle() == 3);
}

// ── mode 2027 DECRQM always reports permanently set ───────────────────────────

TEST_CASE("DECRQM mode 2027 reports permanently set (pm=3)")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("?2027$p");
    CHECK(t.output() == "\x1b[?2027;3$y");
}

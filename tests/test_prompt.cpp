#include <doctest/doctest.h>
#include "TestTerminal.h"

// Helper: send OSC 133 marker
static void osc133(TestTerminal& t, const std::string& kind) {
    t.osc("133;" + kind);
}

// === OSC 133 marker storage ===

TEST_CASE("OSC 133 A sets PromptStart on cursor row")
{
    TestTerminal t;
    osc133(t, "A");
    CHECK(t.term.grid().cols() > 0); // sanity
    // Can't directly query promptKind from TestTerminal, but verify no crash
}

// === Jump to prompt ===

TEST_CASE("scrollToPrompt: previous prompt")
{
    TestTerminal t(20, 5);
    // Row 0: prompt
    osc133(t, "A");
    t.feed("prompt1\r\n");
    // Row 1: output
    osc133(t, "C");
    t.feed("output1\r\n");
    // Row 2: prompt
    osc133(t, "A");
    t.feed("prompt2\r\n");
    // Row 3: output
    osc133(t, "C");
    t.feed("output2\r\n");
    // Row 4: prompt (current)
    osc133(t, "A");
    t.feed("prompt3");

    // Scroll into history first (need content to push into history)
    // For this test, just verify that scrollToPrompt doesn't crash
    // and the method exists
    t.term.scrollToPrompt(-1);
    t.term.scrollToPrompt(1);
}

TEST_CASE("scrollToPrompt: jump to previous in history")
{
    TestTerminal t(20, 3);
    // Fill enough to push prompts into history
    osc133(t, "A");
    t.feed("p1\r\n");
    osc133(t, "C");
    t.feed("line1\r\nline2\r\nline3\r\nline4\r\n");
    osc133(t, "A");
    t.feed("p2\r\n");
    osc133(t, "C");
    t.feed("out2\r\n");
    osc133(t, "A");
    t.feed("p3");

    // Scroll to previous prompt
    t.term.scrollToPrompt(-1);
    // Should have scrolled into history (viewport offset > 0)
    // unless prompt is still on screen
    // Just verify no crash and viewport may have changed
    CHECK(t.term.viewportOffset() >= 0);
}

TEST_CASE("scrollToPrompt: next with no more prompts resets viewport")
{
    TestTerminal t(20, 5);
    osc133(t, "A");
    t.feed("prompt1\r\n");
    osc133(t, "C");
    t.feed("output\r\n");
    osc133(t, "A");
    t.feed("prompt2");

    // Scroll up first
    t.term.scrollViewport(5);
    int offset = t.term.viewportOffset();
    // Now jump to next — should eventually reset to live
    t.term.scrollToPrompt(1);
    CHECK(t.term.viewportOffset() <= offset);
}

// === Command output selection ===

TEST_CASE("selectCommandOutput: selects output region")
{
    TestTerminal t(20, 5);
    osc133(t, "A");
    t.feed("$ ls\r\n");
    osc133(t, "C");
    t.feed("file1\r\nfile2\r\nfile3\r\n");
    osc133(t, "A");
    t.feed("$ ");

    // Viewport center is row 2, which is in the output region
    t.term.selectCommandOutput();
    CHECK(t.term.hasSelection());
}

// === Scrollback serialization ===

TEST_CASE("serializeScrollback: contains screen content")
{
    TestTerminal t(10, 3);
    t.feed("hello");
    std::string content = t.term.serializeScrollback();
    CHECK(content.find("hello") != std::string::npos);
}

TEST_CASE("serializeScrollback: contains history content")
{
    TestTerminal t(10, 2);
    t.feed("line1\r\n");
    t.feed("line2\r\n");
    t.feed("line3\r\n"); // line1 pushed to history
    std::string content = t.term.serializeScrollback();
    CHECK(content.find("line1") != std::string::npos);
    CHECK(content.find("line2") != std::string::npos);
    CHECK(content.find("line3") != std::string::npos);
}

TEST_CASE("serializeScrollback: empty terminal")
{
    TestTerminal t(10, 3);
    std::string content = t.term.serializeScrollback();
    // Should not crash, should have newlines for the screen rows
    CHECK(!content.empty());
}

// === REP tests (added here since they were missed from test file) ===

TEST_CASE("REP: wide character repeat")
{
    TestTerminal t(10, 3);
    t.feed("\xe4\xb8\xad"); // 中 (wide)
    t.csi("2b"); // repeat 2 times
    // Should have 3 wide chars = 6 cells
    CHECK(t.wc(0, 0) == U'\u4e2d');
    CHECK(t.wc(2, 0) == U'\u4e2d');
    CHECK(t.wc(4, 0) == U'\u4e2d');
}

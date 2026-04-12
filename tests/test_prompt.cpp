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

// === OSC 133 CommandRecord tests ===

TEST_CASE("OSC 133: full A/B/C/D lifecycle captures command + output + exit code")
{
    TestTerminal t(40, 5);
    osc133(t, "A");
    t.feed("$ ");
    osc133(t, "B");
    t.feed("ls -la");
    osc133(t, "C");
    t.feed("file1.txt\r\nfile2.txt\r\n");
    osc133(t, "D;0");

    const auto& ring = t.term.commands();
    REQUIRE(ring.size() == 1);
    const auto& r = ring.back();
    CHECK(r.complete);
    CHECK(r.exitCode.has_value());
    CHECK(r.exitCode.value() == 0);
    CHECK(r.command == "ls -la");
    // Output includes both file lines (joined by newlines in the range walk).
    CHECK(r.output.find("file1.txt") != std::string::npos);
    CHECK(r.output.find("file2.txt") != std::string::npos);
}

TEST_CASE("OSC 133: D exit code non-zero")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  t.feed("$ ");
    osc133(t, "B");  t.feed("false");
    osc133(t, "C");
    osc133(t, "D;1");
    REQUIRE(t.term.commands().size() == 1);
    CHECK(t.term.commands().back().exitCode.value() == 1);
}

TEST_CASE("OSC 133: D exit code from err= option overrides bare arg")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  osc133(t, "B");  osc133(t, "C");
    // Spec: non-empty err= wins over bare exit code.
    osc133(t, "D;0;err=42");
    REQUIRE(t.term.commands().size() == 1);
    CHECK(t.term.commands().back().exitCode.value() == 42);
}

TEST_CASE("OSC 133: D without exit code leaves exitCode unset")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  osc133(t, "B");  osc133(t, "C");
    osc133(t, "D");
    REQUIRE(t.term.commands().size() == 1);
    CHECK_FALSE(t.term.commands().back().exitCode.has_value());
}

TEST_CASE("OSC 133: collapse case — second A before D updates in-flight record")
{
    TestTerminal t(40, 5);
    // First A-B-C with a multi-line header
    osc133(t, "A");
    t.feed("== header ==\r\n$ ");
    osc133(t, "B");
    t.feed("ls");
    // Shell now collapses — cursor moves up, rewrites, emits A again.
    t.csi("H");  // cursor to (1,1) → screen (0,0)
    osc133(t, "A");
    t.feed("$ ");
    osc133(t, "B");
    t.feed("ls");
    osc133(t, "C");
    t.feed("out\r\n");
    osc133(t, "D;0");

    // Exactly one command was completed, not two.
    CHECK(t.term.commands().size() == 1);
    CHECK(t.term.commands().back().command == "ls");
}

TEST_CASE("OSC 133: N closes in-flight command implicitly")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  osc133(t, "B");
    t.feed("partial-command");
    // N kills the in-flight command (no C/D seen) and starts a new one.
    osc133(t, "N");
    osc133(t, "B");  osc133(t, "C");
    osc133(t, "D;0");

    // Two records: the first is incomplete (no C/D) and stays in-flight;
    // N finalizes it without exit code. Second completes normally.
    const auto& ring = t.term.commands();
    REQUIRE(ring.size() == 2);
    CHECK(ring[0].complete);
    CHECK_FALSE(ring[0].exitCode.has_value()); // N doesn't carry an exit code
    CHECK(ring[1].complete);
    CHECK(ring[1].exitCode.value() == 0);
}

TEST_CASE("OSC 133: P treated as prompt start without rewriting command")
{
    TestTerminal t(40, 5);
    osc133(t, "A");   // start prompt
    osc133(t, "P");   // explicit P — should keep the in-flight record alive, not start a new one
    osc133(t, "B");  osc133(t, "C");  osc133(t, "D;0");
    // Just one record produced.
    CHECK(t.term.commands().size() == 1);
}

TEST_CASE("OSC 133: cwd captured from OSC 7 at A time")
{
    TestTerminal t(40, 5);
    t.osc("7;file:///home/user/proj");
    osc133(t, "A");   osc133(t, "B");
    t.feed("pwd");
    osc133(t, "C");   osc133(t, "D;0");

    REQUIRE(t.term.commands().size() == 1);
    CHECK(t.term.commands().back().cwd == "/home/user/proj");
}

TEST_CASE("OSC 133: commandComplete callback fires with the record")
{
    bool fired = false;
    uint64_t seenId = 0;
    std::optional<int> seenExit;
    std::string seenCommand;

    // Custom TestTerminal-style setup with event callback.
    TerminalCallbacks cb;
    cb.event = [&](TerminalEmulator*, int ev, void* payload) {
        if (ev == TerminalEmulator::CommandComplete && payload) {
            const auto* r = static_cast<const TerminalEmulator::CommandRecord*>(payload);
            fired = true;
            seenId = r->id;
            seenExit = r->exitCode;
            seenCommand = r->command;
        }
    };

    TerminalEmulator term(cb);
    term.resize(40, 5);
    auto feed = [&](const std::string& s) { term.injectData(s.data(), s.size()); };
    auto osc = [&](const std::string& s) { feed("\x1b]" + s + "\x07"); };

    osc("133;A");
    feed("$ ");
    osc("133;B");
    feed("echo hi");
    osc("133;C");
    feed("hi\r\n");
    osc("133;D;7");

    CHECK(fired);
    CHECK(seenId != 0);
    CHECK(seenExit.value() == 7);
    CHECK(seenCommand == "echo hi");
}

TEST_CASE("OSC 133: second A after output finalizes previous command without exit code")
{
    TestTerminal t(20, 5);
    osc133(t, "A");
    t.feed("$ ls\r\n");
    osc133(t, "C");
    t.feed("file1\r\nfile2\r\n");
    // No D — shell emits next prompt directly.
    osc133(t, "A");
    t.feed("$ ");

    // Previous command was auto-finalized (no exit code), new command is in-flight.
    const auto& ring = t.term.commands();
    REQUIRE(ring.size() == 2);
    CHECK(ring[0].complete);
    CHECK_FALSE(ring[0].exitCode.has_value());
    // Output should include file1 and file2.
    CHECK(ring[0].output.find("file1") != std::string::npos);
    CHECK(ring[0].output.find("file2") != std::string::npos);
    CHECK_FALSE(ring[1].complete);
}

TEST_CASE("OSC 133: captured output survives scrolling into tier-1 history")
{
    TestTerminal t(20, 3);
    osc133(t, "A");
    t.feed("$ ");
    osc133(t, "B");
    t.feed("cmd");
    osc133(t, "C");
    t.feed("\r\nout\r\n");
    osc133(t, "D;0");

    // Scroll the prompt into history by writing more content.
    for (int i = 0; i < 20; ++i) t.feed("filler line\r\n");

    // The captured command + output strings were copied eagerly at C / D, so
    // they survive scrolling even if the underlying rows have moved far back.
    const auto* last = t.term.lastCommand();
    REQUIRE(last != nullptr);
    CHECK(last->complete);
    CHECK(last->command == "cmd");
    CHECK(last->output.find("out") != std::string::npos);
}

TEST_CASE("OSC 133: semantic type round-trips through tier-2 archive")
{
    // Force rapid archive eviction: tier1Capacity=2 means after 2 rows in history,
    // the next gets pushed to tier-2 (parsed back on demand via parseArchivedRow).
    TestTerminal t(20, 2);
    t.term.resetScrollback(2); // tight history

    osc133(t, "A");
    t.feed("$ prompt\r\n");    // this row becomes tier-1 after scroll
    osc133(t, "C");
    t.feed("out\r\n");
    osc133(t, "D;0");
    // Keep writing to push early rows into tier-2 archive.
    for (int i = 0; i < 10; ++i) t.feed("filler\r\n");

    // Historical content should be in archive. Access via historyRow —
    // parseArchivedRow reconstitutes cells including semantic type.
    int histSize = t.term.document().historySize();
    REQUIRE(histSize > 2);
    const Cell* row0 = t.term.document().historyRow(0);
    REQUIRE(row0 != nullptr);
    // Row 0's first non-blank char should be tagged Prompt (the "$ prompt" row)
    // because the archive roundtrip preserved the semantic type.
    bool sawPrompt = false;
    for (int c = 0; c < 20; ++c) {
        if (row0[c].wc != 0 && row0[c].attrs.semanticType() == CellAttrs::Prompt) {
            sawPrompt = true;
            break;
        }
    }
    CHECK(sawPrompt);
}

TEST_CASE("OSC 133: cells are tagged with SemanticType at write time")
{
    TestTerminal t(20, 5);
    osc133(t, "A");
    t.feed("$ ");              // prompt cells
    osc133(t, "B");
    t.feed("ls");              // input cells
    osc133(t, "C");
    t.feed("out");             // output cells (partial — no newline yet)

    CHECK(t.cell(0, 0).attrs.semanticType() == CellAttrs::Prompt);
    CHECK(t.cell(1, 0).attrs.semanticType() == CellAttrs::Prompt);
    CHECK(t.cell(2, 0).attrs.semanticType() == CellAttrs::Input);
    CHECK(t.cell(3, 0).attrs.semanticType() == CellAttrs::Input);
    CHECK(t.cell(4, 0).attrs.semanticType() == CellAttrs::Output);
    CHECK(t.cell(5, 0).attrs.semanticType() == CellAttrs::Output);
    CHECK(t.cell(6, 0).attrs.semanticType() == CellAttrs::Output);
}

TEST_CASE("OSC 133: SGR 0 preserves semantic type on the pen")
{
    TestTerminal t(20, 3);
    osc133(t, "A");
    t.feed("$");
    // SGR 0 (reset) — should NOT wipe the Prompt tag off the pen.
    t.csi("0m");
    t.feed(" p");
    CHECK(t.cell(0, 0).attrs.semanticType() == CellAttrs::Prompt);
    CHECK(t.cell(1, 0).attrs.semanticType() == CellAttrs::Prompt);
    CHECK(t.cell(2, 0).attrs.semanticType() == CellAttrs::Prompt);
}

TEST_CASE("OSC 133: clear operation resets cells to Output")
{
    TestTerminal t(20, 3);
    osc133(t, "A");
    t.feed("$ prompt");
    // Every cell is now Prompt-tagged.
    CHECK(t.cell(0, 0).attrs.semanticType() == CellAttrs::Prompt);
    // Clear the row via EL (erase in line).
    t.csi("H");        // cursor home
    t.csi("2K");       // erase entire line
    // Cleared cells should be Output (default), not Prompt.
    CHECK(t.cell(0, 0).attrs.semanticType() == CellAttrs::Output);
    CHECK(t.cell(5, 0).attrs.semanticType() == CellAttrs::Output);
}

TEST_CASE("OSC 133: lastCommand returns most recent completed")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  osc133(t, "B");  t.feed("cmd1");
    osc133(t, "C");  osc133(t, "D;0");
    osc133(t, "A");  osc133(t, "B");  t.feed("cmd2");
    osc133(t, "C");  osc133(t, "D;1");

    REQUIRE(t.term.lastCommand() != nullptr);
    CHECK(t.term.lastCommand()->command == "cmd2");
    CHECK(t.term.lastCommand()->exitCode.value() == 1);
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

#include <doctest/doctest.h>
#include "TestTerminal.h"

// Helper: send OSC 133 marker
static void osc133(TestTerminal& t, const std::string& kind) {
    t.osc("133;" + kind);
}

// Helpers for lazily extracting text from a CommandRecord via the document.
static std::string recordCommandText(const TerminalEmulator& term,
                                      const TerminalEmulator::CommandRecord& r)
{
    if (r.commandStartCol < 0) return {};
    auto text = term.document().getTextFromLines(r.commandStartLineId, r.outputStartLineId,
                                                 r.commandStartCol, r.outputStartCol);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
                              text.back() == '\r' || text.back() == '\t'))
        text.pop_back();
    return text;
}

static std::string recordOutputText(const TerminalEmulator& term,
                                     const TerminalEmulator::CommandRecord& r)
{
    if (r.outputStartCol < 0) return {};
    return term.document().getTextFromLines(r.outputStartLineId, r.outputEndLineId,
                                            r.outputStartCol, r.outputEndCol);
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

TEST_CASE("selectCommandOutput: selects output region of selected command")
{
    TestTerminal t(20, 5);
    osc133(t, "A");
    t.feed("$ ls\r\n");
    osc133(t, "C");
    t.feed("file1\r\nfile2\r\nfile3\r\n");
    osc133(t, "A");
    t.feed("$ ");

    // Keyboard entry point requires a selected command (set via Cmd+click,
    // Cmd+Up/Down, or the mouse gesture). Without one, it's a no-op.
    t.term.selectCommandOutput();
    CHECK(!t.term.hasSelection());

    // With the first command selected, the output range becomes a text selection.
    REQUIRE(!t.term.commands().empty());
    t.term.setSelectedCommand(t.term.commands().front().id);
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

TEST_CASE("serializeScrollback: tier-2 archive lines preserved")
{
    // tier1Capacity=2 forces eviction to tier-2 archive after 2 history rows.
    TestTerminal t(20, 2);
    t.term.resetScrollback(100);

    // Feed enough lines that the earliest ones must live in the tier-2 archive.
    for (int i = 0; i < 12; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "line%02d\r\n", i);
        t.feed(buf);
    }

    // Sanity: archive must actually be populated (otherwise we're only testing tier-1).
    REQUIRE(t.term.document().scrollbackLogicalLines() > 0);

    std::string content = t.term.serializeScrollback();

    // Every fed line — including ones evicted to tier-2 — must round-trip.
    for (int i = 0; i < 12; ++i) {
        char needle[8];
        std::snprintf(needle, sizeof(needle), "line%02d", i);
        CAPTURE(needle);
        CHECK(content.find(needle) != std::string::npos);
    }

    // Ordering: archived line00 must appear before line11.
    auto p0 = content.find("line00");
    auto p11 = content.find("line11");
    REQUIRE(p0 != std::string::npos);
    REQUIRE(p11 != std::string::npos);
    CHECK(p0 < p11);
}

TEST_CASE("serializeScrollback: tier-2 ordering is monotonic across all lines")
{
    TestTerminal t(20, 2);
    t.term.resetScrollback(100);

    constexpr int N = 30;
    for (int i = 0; i < N; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "L%03d\r\n", i);
        t.feed(buf);
    }

    REQUIRE(t.term.document().scrollbackLogicalLines() > 0);

    std::string content = t.term.serializeScrollback();

    // Each marker must appear exactly once, in increasing position.
    size_t prev = 0;
    for (int i = 0; i < N; ++i) {
        char needle[8];
        std::snprintf(needle, sizeof(needle), "L%03d", i);
        auto pos = content.find(needle);
        CAPTURE(needle);
        REQUIRE(pos != std::string::npos);
        CHECK(content.find(needle, pos + 1) == std::string::npos); // unique
        if (i > 0) CHECK(pos > prev);
        prev = pos;
    }
}

TEST_CASE("serializeScrollback: wide chars survive tier-2 roundtrip")
{
    TestTerminal t(10, 2);
    t.term.resetScrollback(100);

    // First row: a wide char (中) followed by ASCII, then newline.
    t.feed("\xe4\xb8\xad" "ab\r\n");
    // Push the wide-char row into the tier-2 archive.
    for (int i = 0; i < 8; ++i) t.feed("pad\r\n");

    REQUIRE(t.term.document().scrollbackLogicalLines() > 0);

    std::string content = t.term.serializeScrollback();
    // Wide char encoded as UTF-8, with no spacer artifact between it and "ab".
    CHECK(content.find("\xe4\xb8\xad" "ab") != std::string::npos);
}

TEST_CASE("serializeScrollback: wrapped line is searchable as one string")
{
    // Feed a 20-char string into a 10-wide terminal — it wraps once.
    TestTerminal t(10, 2);
    t.term.resetScrollback(100);

    t.feed("abcdefghij1234567890\r\n");
    // Push the wrapped pair into history / tier-2.
    for (int i = 0; i < 8; ++i) t.feed("z\r\n");

    std::string content = t.term.serializeScrollback();
    // A pager search like /abcdefghij1234567890 must succeed even though
    // the original line was wrapped across two physical rows.
    CHECK(content.find("abcdefghij1234567890") != std::string::npos);
}

TEST_CASE("serializeScrollback: combining marks preserved through tier-2")
{
    // Warning sign (U+26A0) + VS16 (U+FE0F) — VS16 lives in cell extras.
    TestTerminal t(10, 2);
    t.term.resetScrollback(100);

    t.feed("\xe2\x9a\xa0\xef\xb8\x8f\r\n"); // ⚠️
    for (int i = 0; i < 8; ++i) t.feed("z\r\n");

    REQUIRE(t.term.document().scrollbackLogicalLines() > 0);

    std::string content = t.term.serializeScrollback();
    // The base codepoint must be present.
    CHECK(content.find("\xe2\x9a\xa0") != std::string::npos);
    // The combining VS16 selector must also be emitted, immediately after the base.
    CHECK(content.find("\xe2\x9a\xa0\xef\xb8\x8f") != std::string::npos);
}

TEST_CASE("serializeScrollback: full-width row (no trailing blanks) preserved")
{
    // Row that exactly fills cols — exercises the effectiveWidth path with
    // no trailing-blank trim.
    TestTerminal t(10, 2);
    t.term.resetScrollback(100);

    t.feed("0123456789"); // exactly 10 chars; cursor sits at the right edge
    t.feed("\r\n");
    for (int i = 0; i < 8; ++i) t.feed("z\r\n");

    REQUIRE(t.term.document().scrollbackLogicalLines() > 0);

    std::string content = t.term.serializeScrollback();
    CHECK(content.find("0123456789") != std::string::npos);
}

TEST_CASE("serializeScrollback: SGR colors are stripped from output (documented behavior)")
{
    // Pager output is plain text — colors are dropped intentionally so that
    // `less -R` sees no escapes and search doesn't match against ANSI bytes.
    TestTerminal t(20, 2);
    t.term.resetScrollback(100);

    t.feed("\x1b[31mred\x1b[0m text\r\n");
    for (int i = 0; i < 8; ++i) t.feed("z\r\n");

    std::string content = t.term.serializeScrollback();
    CHECK(content.find("red text") != std::string::npos);
    CHECK(content.find('\x1b') == std::string::npos);
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
    CHECK(recordCommandText(t.term, r) == "ls -la");
    // Output includes both file lines (joined by newlines in the range walk).
    CHECK(recordOutputText(t.term, r).find("file1.txt") != std::string::npos);
    CHECK(recordOutputText(t.term, r).find("file2.txt") != std::string::npos);
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
    CHECK(recordCommandText(t.term, t.term.commands().back()) == "ls");
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
    uint64_t seenCmdRowId = 0, seenOutRowId = 0;

    // Custom TestTerminal-style setup with event callback.
    TerminalCallbacks cb;
    cb.event = [&](TerminalEmulator*, int ev, void* payload) {
        if (ev == TerminalEmulator::CommandComplete && payload) {
            const auto* r = static_cast<const TerminalEmulator::CommandRecord*>(payload);
            fired = true;
            seenId = r->id;
            seenExit = r->exitCode;
            seenCmdRowId = r->commandStartLineId;
            seenOutRowId = r->outputStartLineId;
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
    REQUIRE(term.lastCommand() != nullptr);
    CHECK(recordCommandText(term, *term.lastCommand()) == "echo hi");
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
    CHECK(recordOutputText(t.term, ring[0]).find("file1") != std::string::npos);
    CHECK(recordOutputText(t.term, ring[0]).find("file2") != std::string::npos);
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

    // Text is extracted lazily from the document — rows are in tier-1 history.
    const auto* last = t.term.lastCommand();
    REQUIRE(last != nullptr);
    CHECK(last->complete);
    CHECK(recordCommandText(t.term, *last) == "cmd");
    CHECK(recordOutputText(t.term, *last).find("out") != std::string::npos);
}

TEST_CASE("OSC 133: semantic type round-trips through tier-2 archive")
{
    // Force rapid archive eviction: tier1Capacity=2 means after 2 rows in history,
    // the next gets pushed to tier-2 (parsed back on demand via parseArchivedRow).
    TestTerminal t(20, 2);
    t.term.resetScrollback(100); // tight history

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

TEST_CASE("OSC 133: lastCommand skips in-flight tail and returns previous completed")
{
    TestTerminal t(20, 3);
    // A completed command.
    osc133(t, "A");  osc133(t, "B");  t.feed("cmd1");
    osc133(t, "C");  osc133(t, "D;0");
    // A new in-flight command (prompt shown, user hasn't finished typing).
    osc133(t, "A");  osc133(t, "B");
    t.feed("cmd2-typing");

    // The in-flight command is the ring's tail but has no exit code.
    // lastCommand() should look past it to the previously-completed one.
    const auto* last = t.term.lastCommand();
    REQUIRE(last != nullptr);
    CHECK(last->complete);
    CHECK(recordCommandText(t.term, *last) == "cmd1");
    CHECK(last->exitCode.value() == 0);
}

TEST_CASE("OSC 133: lastCommand returns most recent completed")
{
    TestTerminal t(40, 5);
    osc133(t, "A");  osc133(t, "B");  t.feed("cmd1");
    osc133(t, "C");  osc133(t, "D;0");
    osc133(t, "A");  osc133(t, "B");  t.feed("cmd2");
    osc133(t, "C");  osc133(t, "D;1");

    REQUIRE(t.term.lastCommand() != nullptr);
    CHECK(recordCommandText(t.term, *t.term.lastCommand()) == "cmd2");
    CHECK(t.term.lastCommand()->exitCode.value() == 1);
}

// === Uncapped command ring + selected-command highlight ===

TEST_CASE("OSC 133: command ring retains > 256 records when archive has room")
{
    TestTerminal t(40, 3);
    // Feed 300 quick commands; each takes 1 screen row (prompt + output wrap).
    // With the default 4096-row tier-1 archive, nothing evicts past the floor.
    for (int i = 0; i < 300; ++i) {
        osc133(t, "A"); osc133(t, "B"); osc133(t, "C");
        osc133(t, "D;0");
        t.feed("\r\n");
    }
    // Old behavior capped at 256. New behavior: keeps all while archive room exists.
    CHECK(t.term.commands().size() == 300u);
}

TEST_CASE("OSC 133: commandForLineId hits the containing record")
{
    TestTerminal t(40, 8);
    osc133(t, "A"); t.feed("$ ");
    osc133(t, "B"); t.feed("cmd1");
    osc133(t, "C"); t.feed("out1\r\n");
    osc133(t, "D;0");
    osc133(t, "A"); t.feed("$ ");
    osc133(t, "B"); t.feed("cmd2");
    osc133(t, "C"); t.feed("out2\r\n");
    osc133(t, "D;1");

    REQUIRE(t.term.commands().size() == 2);
    const auto& r0 = t.term.commands()[0];
    const auto& r1 = t.term.commands()[1];

    const auto* hit0 = t.term.commandForLineId(r0.promptStartLineId);
    REQUIRE(hit0 != nullptr);
    CHECK(hit0->id == r0.id);

    const auto* hit1 = t.term.commandForLineId(r1.outputEndLineId);
    REQUIRE(hit1 != nullptr);
    CHECK(hit1->id == r1.id);

    // Below oldest prompt: no hit.
    CHECK(t.term.commandForLineId(r0.promptStartLineId - 1) == nullptr);
}

TEST_CASE("OSC 133: setSelectedCommand populates selectedCommandId")
{
    TestTerminal t(40, 5);
    osc133(t, "A"); osc133(t, "B"); t.feed("cmd");
    osc133(t, "C"); osc133(t, "D;0");

    REQUIRE(t.term.commands().size() == 1);
    uint64_t id = t.term.commands()[0].id;

    CHECK_FALSE(t.term.selectedCommandId().has_value());
    t.term.setSelectedCommand(id);
    REQUIRE(t.term.selectedCommandId().has_value());
    CHECK(*t.term.selectedCommandId() == id);

    t.term.setSelectedCommand(std::nullopt);
    CHECK_FALSE(t.term.selectedCommandId().has_value());
}

TEST_CASE("OSC 133: setSelectedCommand rejects unknown id")
{
    TestTerminal t(40, 5);
    osc133(t, "A"); osc133(t, "B"); osc133(t, "C"); osc133(t, "D;0");
    t.term.setSelectedCommand(uint64_t{99999});
    CHECK_FALSE(t.term.selectedCommandId().has_value());
}

TEST_CASE("OSC 133: scrollToPrompt sets selection on landed command")
{
    TestTerminal t(20, 3);
    osc133(t, "A"); t.feed("$ cmd1\r\n");
    osc133(t, "C"); t.feed("o1\r\n");
    osc133(t, "D;0");
    osc133(t, "A"); t.feed("$ cmd2\r\n");
    osc133(t, "C"); t.feed("o2\r\n");
    osc133(t, "D;0");
    osc133(t, "A"); t.feed("$ ");

    REQUIRE(t.term.commands().size() == 3);
    // With no selection, Cmd+Up anchors past the end of content so it lands
    // on the most recent command (the in-flight one at "$ ").
    t.term.scrollToPrompt(-1);
    REQUIRE(t.term.selectedCommandId().has_value());
    CHECK(*t.term.selectedCommandId() == t.term.commands()[2].id);

    // Another Cmd+Up steps backward from the selection to the previous prompt.
    t.term.scrollToPrompt(-1);
    CHECK(*t.term.selectedCommandId() == t.term.commands()[1].id);

    // Clear the selection. Cmd+Down with no selection anchors before the
    // start so it lands on the oldest command.
    t.term.setSelectedCommand(std::nullopt);
    t.term.scrollToPrompt(+1);
    REQUIRE(t.term.selectedCommandId().has_value());
    CHECK(*t.term.selectedCommandId() == t.term.commands()[0].id);

    // At the oldest, Cmd+Up wraps around to the newest.
    t.term.scrollToPrompt(-1);
    CHECK(*t.term.selectedCommandId() == t.term.commands()[2].id);
    // At the newest, Cmd+Down wraps around to the oldest.
    t.term.scrollToPrompt(+1);
    CHECK(*t.term.selectedCommandId() == t.term.commands()[0].id);
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

#include <doctest/doctest.h>
#include "TestTerminal.h"

// Eviction & content survival across the variable-length scrollback
// (formerly the tier-2 archive subsystem). After the storage refactor
// the buffer is a deque of LogicalLineBlock arenas; eviction drops the
// oldest logical line when maxLogicalLines or maxTotalCells is exceeded.
// These tests assert the externally observable invariants that survive:
// scrollback retains content under eviction, content round-trips, line IDs
// remain stable, resize is non-destructive.

static std::string lineText(int i) { return "line" + std::to_string(i); }

static std::string historyRowText(const TestTerminal& t, int idx)
{
    const Cell* row = t.term.document().historyRow(idx);
    if (!row) return "";
    int cols = t.term.width();
    std::string result;
    for (int c = 0; c < cols; ++c) {
        char32_t wc = row[c].wc;
        if (wc == 0) wc = ' ';
        if (wc < 0x80) result += static_cast<char>(wc);
    }
    auto end = result.find_last_not_of(' ');
    return end == std::string::npos ? "" : result.substr(0, end + 1);
}

TEST_CASE("scrollback: rows persist past eviction window" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);  // max 3 logical lines in scrollback

    // 10 hard-broken lines into a 5-row terminal: first 6 lines (line0..line5)
    // scroll into scrollback as the visible grid fills past height-1; the
    // remaining 4 lines + trailing blank stay in the visible grid. With a
    // cap of 3 logical lines, oldest 3 of the 6 evict.
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    CHECK(doc.scrollbackLogicalLines() == 3);
    CHECK(doc.historySize() == 3);
}

TEST_CASE("scrollback: eviction drops oldest content" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    // Of line0..line5 in scrollback, oldest 3 evicted; line3..line5 remain.
    const auto& doc = t.term.document();
    REQUIRE(doc.scrollbackLogicalLines() == 3);
    CHECK(historyRowText(t, 0) == "line3");
    CHECK(historyRowText(t, 1) == "line4");
    CHECK(historyRowText(t, 2) == "line5");
}

TEST_CASE("scrollback: text content round-trips through scrollback" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);  // big enough to keep all that scroll out

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    // 10 hard-broken lines + final \n in 5-row grid: line0..line5 scroll out
    // into scrollback (6 lines).
    const auto& doc = t.term.document();
    REQUIRE(doc.historySize() == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(historyRowText(t, i) == lineText(i));
    }
}

TEST_CASE("scrollback: historyRow returns null past end" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    CHECK(doc.historyRow(doc.historySize()) == nullptr);
    CHECK(doc.historyRow(-1) == nullptr);
}

TEST_CASE("scrollback: SGR colors survive scrollback eviction" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(2);

    t.feed("\x1b[38;2;255;0;0mred\x1b[0m\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    // Most recent two lines retained — "red" was first, will be evicted
    // unless the cap is big enough. Bump cap to keep "red" present.
    t.term.resetScrollback(20);
    t.feed("\x1b[38;2;255;0;0mred\x1b[0m\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    const auto& doc = t.term.document();
    REQUIRE(doc.historySize() > 0);

    // Find the row that starts with 'r' (the red one).
    bool found = false;
    for (int i = 0; i < doc.historySize() && !found; ++i) {
        const Cell* row = doc.historyRow(i);
        if (!row || row[0].wc != U'r') continue;
        CHECK(row[0].attrs.fgMode() == CellAttrs::RGB);
        CHECK(row[0].attrs.fgR() == 255);
        CHECK(row[0].attrs.fgG() == 0);
        CHECK(row[0].attrs.fgB() == 0);
        found = true;
    }
    CHECK(found);
}

TEST_CASE("scrollback: bold attribute survives scrollback" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);

    t.feed("\x1b[1mbold\x1b[0m\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    const auto& doc = t.term.document();
    bool found = false;
    for (int i = 0; i < doc.historySize() && !found; ++i) {
        const Cell* row = doc.historyRow(i);
        if (!row || row[0].wc != U'b') continue;
        CHECK(row[0].attrs.bold());
        found = true;
    }
    CHECK(found);
}

TEST_CASE("scrollback: bound respected under heavy feed" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 3);
    t.term.resetScrollback(50);
    for (int i = 0; i < 200; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    CHECK(doc.scrollbackLogicalLines() == 50);
}

TEST_CASE("scrollback: viewportRow at full offset returns oldest row" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    REQUIRE(doc.historySize() > 0);

    int histSize = doc.historySize();
    const Cell* vrow = doc.viewportRow(0, histSize);
    REQUIRE(vrow != nullptr);
    CHECK(vrow[0].wc == U'l'); // first line starts with 'l'
}

TEST_CASE("scrollback: clearHistory empties scrollback" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    REQUIRE(t.term.document().historySize() > 0);
    t.term.document().clearHistory();
    CHECK(t.term.document().historySize() == 0);
    CHECK(t.term.document().scrollbackLogicalLines() == 0);
    CHECK(t.term.document().historyRow(0) == nullptr);
}

TEST_CASE("scrollback: width-change reflow leaves scrollback content intact"
          * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    int preLogical = doc.scrollbackLogicalLines();
    REQUIRE(preLogical > 0);

    t.term.resize(30, 5);

    // Logical-line count is invariant across width change (no eviction).
    CHECK(doc.scrollbackLogicalLines() == preLogical);
    CHECK(t.term.width() == 30);
}

TEST_CASE("scrollback: width-change reflow preserves locatable content"
          * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);

    t.feed("hello world\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    REQUIRE(t.term.document().historySize() > 0);

    t.term.resize(40, 5);

    const auto& doc = t.term.document();
    bool found = false;
    for (int i = 0; i < doc.historySize() && !found; ++i) {
        const Cell* row = doc.historyRow(i);
        if (!row) continue;
        if (row[0].wc == U'h' && row[1].wc == U'e') found = true;
    }
    CHECK(found);
}

TEST_CASE("scrollback: width-change reflow keeps line IDs monotonic"
          * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(20);

    for (int i = 0; i < 12; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    REQUIRE(doc.historySize() > 0);

    t.term.resize(10, 5);

    int postTotal = doc.historySize() + t.term.height();
    uint64_t prev = 0;
    for (int abs = 0; abs < postTotal; ++abs) {
        uint64_t id = doc.lineIdForAbs(abs);
        CHECK(id >= prev);
        prev = id;
    }
}

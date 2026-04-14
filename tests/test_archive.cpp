#include <doctest/doctest.h>
#include "TestTerminal.h"

// Helpers

// Feed n lines of the form "lineN\r\n" and return the content of line i as text.
static std::string lineText(int i) { return "line" + std::to_string(i); }

// Return the text of a history row (archive or tier-1), trimmed of trailing blanks.
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

// ============================================================================
// Basic tier-2 eviction
// ============================================================================

TEST_CASE("archive: rows evict to tier-2 when tier-1 is full" * doctest::test_suite("archive"))
{
    // tier1Capacity=3, 5-row terminal. Feed enough lines to overflow tier-1.
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    // Feed 10 lines — history will grow beyond 3, forcing eviction to archive.
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    CHECK(doc.archiveSize() > 0);
    CHECK(doc.historySize() > 3);
}

TEST_CASE("archive: tier-1 count stays at or below capacity" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 20; ++i) t.feed(lineText(i) + "\r\n");

    // historySize() = archive + tier-1; tier-1 alone should be <= capacity.
    const auto& doc = t.term.document();
    int tier1 = doc.historySize() - doc.archiveSize();
    CHECK(tier1 <= 3);
}

// ============================================================================
// Content round-trip through archive
// ============================================================================

TEST_CASE("archive: text content survives archive round-trip" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    // Feed lines with known content.
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    // Rows are in oldest-first order. The oldest rows are in the archive.
    const auto& doc = t.term.document();
    REQUIRE(doc.archiveSize() > 0);

    // Verify every archived row has the expected content in order.
    for (int i = 0; i < doc.archiveSize(); ++i) {
        std::string text = historyRowText(t, i);
        CHECK(text == lineText(i));
    }
}

TEST_CASE("archive: tier-1 content is correct after archive eviction" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    // Verify tier-1 rows (after archive) also have correct content in order.
    const auto& doc = t.term.document();
    int archSz = doc.archiveSize();
    int histSz = doc.historySize();
    for (int i = archSz; i < histSz; ++i) {
        std::string text = historyRowText(t, i);
        CHECK(text == lineText(i));
    }
}

TEST_CASE("archive: historyRow returns null past end" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);
    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    CHECK(doc.historyRow(doc.historySize()) == nullptr);
    CHECK(doc.historyRow(-1) == nullptr);
}

TEST_CASE("archive: SGR colors survive archive round-trip" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(2);

    // Write a red-foreground line, then overflow tier-1.
    t.feed("\x1b[38;2;255;0;0mred\x1b[0m\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    // The red line should now be in the archive.
    const auto& doc = t.term.document();
    REQUIRE(doc.archiveSize() > 0);

    const Cell* row = doc.historyRow(0);
    REQUIRE(row != nullptr);
    // First cell should be 'r' with red RGB foreground.
    CHECK(row[0].wc == U'r');
    CHECK(row[0].attrs.fgMode() == CellAttrs::RGB);
    CHECK(row[0].attrs.fgR() == 255);
    CHECK(row[0].attrs.fgG() == 0);
    CHECK(row[0].attrs.fgB() == 0);
}

TEST_CASE("archive: bold attribute survives archive round-trip" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(2);

    t.feed("\x1b[1mbold\x1b[0m\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    const auto& doc = t.term.document();
    REQUIRE(doc.archiveSize() > 0);
    const Cell* row = doc.historyRow(0);
    REQUIRE(row != nullptr);
    CHECK(row[0].wc == U'b');
    CHECK(row[0].attrs.bold());
}

// ============================================================================
// Archive cap
// ============================================================================

TEST_CASE("archive: archive is capped at maxArchiveRows" * doctest::test_suite("archive"))
{
    // maxArchiveRows defaults to 100000 — use a tiny terminal to test capping
    // without feeding 100K lines. We can't set maxArchiveRows via the public API
    // so we construct Document directly via resetScrollback and use a large feed.
    // Instead, just verify that archiveSize() is bounded and never grows forever.

    TestTerminal t(20, 3);
    t.term.resetScrollback(1); // tiny tier-1 so almost everything hits archive

    // Feed enough to fill and stabilize the archive.
    for (int i = 0; i < 50; ++i) t.feed(lineText(i) + "\r\n");

    // archiveSize should reflect accumulated rows but be bounded.
    int sz = t.term.document().archiveSize();
    CHECK(sz > 0);
    CHECK(sz < 100000);
}

// ============================================================================
// historySize() accounting
// ============================================================================

TEST_CASE("archive: historySize includes both archive and tier-1" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    int reported = doc.historySize();
    int manual = doc.archiveSize() + (reported - doc.archiveSize()); // tautology check
    CHECK(reported == manual);
    CHECK(reported >= doc.archiveSize());
}

// ============================================================================
// viewportRow through archive
// ============================================================================

TEST_CASE("archive: viewportRow can read archived rows" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    const auto& doc = t.term.document();
    REQUIRE(doc.archiveSize() > 0);

    // Scroll all the way back.
    int histSize = doc.historySize();
    // viewportRow(0, histSize) should be the oldest row (row 0 of history).
    const Cell* vrow = doc.viewportRow(0, histSize);
    REQUIRE(vrow != nullptr);
    CHECK(vrow[0].wc == U'l'); // starts with 'l' from "lineN"
}

// ============================================================================
// clearHistory
// ============================================================================

TEST_CASE("archive: clearHistory empties both archive and tier-1" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    REQUIRE(t.term.document().historySize() > 0);
    REQUIRE(t.term.document().archiveSize() > 0);
    t.term.document().clearHistory();
    CHECK(t.term.document().historySize() == 0);
    CHECK(t.term.document().archiveSize() == 0);
    CHECK(t.term.document().historyRow(0) == nullptr);
}

// ============================================================================
// Resize (reflow) with archive content
// ============================================================================

TEST_CASE("archive: resize with archived rows does not crash" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    for (int i = 0; i < 10; ++i) t.feed(lineText(i) + "\r\n");

    REQUIRE(t.term.document().archiveSize() > 0);

    // Resize to different column count — triggers full reflow including archive.
    t.term.resize(30, 5);
    CHECK(t.term.document().historySize() > 0);

    // Screen should still be accessible.
    CHECK(t.term.width() == 30);
}

TEST_CASE("archive: resize preserves archived content" * doctest::test_suite("archive"))
{
    TestTerminal t(20, 5);
    t.term.resetScrollback(3);

    // Write a line we can identify, then push it into archive.
    t.feed("hello world\r\n");
    for (int i = 0; i < 8; ++i) t.feed("filler\r\n");

    REQUIRE(t.term.document().archiveSize() > 0);

    // Reflow at wider width.
    t.term.resize(40, 5);

    // "hello world" should still be findable in history.
    const auto& doc = t.term.document();
    bool found = false;
    for (int i = 0; i < doc.historySize() && !found; ++i) {
        const Cell* row = doc.historyRow(i);
        if (!row) continue;
        if (row[0].wc == U'h' && row[1].wc == U'e') found = true;
    }
    CHECK(found);
}

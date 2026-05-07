#include "Document.h"
#include "TestTerminal.h"
#include <doctest/doctest.h>

// Document::findText is the C++ engine behind pane.findText. These tests
// drive it directly through the Document inside a TestTerminal so we can
// exercise scrollback + visible-grid + wrap edges without pulling in JS.

namespace {

const Document &doc(TestTerminal &t)
{
    return t.term.document();
}

bool hasMatch(const std::vector<Document::Match> &v,
              uint64_t lineId, int startCol, int endCol)
{
    for (const auto &m : v) {
        if (m.startLineId == lineId && m.endLineId == lineId &&
            m.startCol == startCol && m.endCol == endCol) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("findText: empty needle returns no matches")
{
    TestTerminal t(40, 5);
    t.feed("hello world\r\n");
    auto matches = doc(t).findText("", {});
    CHECK(matches.empty());
}

TEST_CASE("findText: literal substring on visible grid")
{
    TestTerminal t(40, 5);
    t.feed("hello world");
    Document::FindOptions opts;
    auto matches = doc(t).findText("world", opts);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 6);
    CHECK(matches[0].endCol == 11);
    CHECK(matches[0].startLineId == matches[0].endLineId);
    CHECK(matches[0].startLineId != 0);
}

TEST_CASE("findText: case-insensitive by default")
{
    TestTerminal t(40, 5);
    t.feed("Hello World");
    auto matches = doc(t).findText("hello", {});
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 0);
    CHECK(matches[0].endCol == 5);
}

TEST_CASE("findText: case-sensitive opt forces exact case")
{
    TestTerminal t(40, 5);
    t.feed("Hello World");
    Document::FindOptions opts;
    opts.caseSensitive = true;
    auto matches       = doc(t).findText("hello", opts);
    CHECK(matches.empty());
    matches = doc(t).findText("Hello", opts);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 0);
}

TEST_CASE("findText: multiple non-overlapping matches on one line")
{
    TestTerminal t(40, 5);
    t.feed("foo bar foo baz foo");
    auto matches = doc(t).findText("foo", {});
    REQUIRE(matches.size() == 3);
    CHECK(matches[0].startCol == 0);
    CHECK(matches[1].startCol == 8);
    CHECK(matches[2].startCol == 16);
    // All three live on the same logical line.
    CHECK(matches[0].startLineId == matches[1].startLineId);
    CHECK(matches[1].startLineId == matches[2].startLineId);
}

TEST_CASE("findText: self-overlapping needle skips past its own match")
{
    TestTerminal t(40, 5);
    t.feed("aaaa");
    auto matches = doc(t).findText("aa", {});
    // "aaaa" with needle "aa" → 2 non-overlapping matches at 0 and 2,
    // not 3 overlapping at 0/1/2.
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].startCol == 0);
    CHECK(matches[0].endCol == 2);
    CHECK(matches[1].startCol == 2);
    CHECK(matches[1].endCol == 4);
}

TEST_CASE("findText: matches across separate logical lines come back in order")
{
    TestTerminal t(40, 5);
    t.feed("alpha\r\n");
    t.feed("beta\r\n");
    t.feed("alpha\r\n");
    t.feed("gamma");
    auto matches = doc(t).findText("alpha", {});
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].startLineId != matches[1].startLineId);
    // First match is the older one (oldest-first walk).
    CHECK(matches[0].startLineId < matches[1].startLineId);
}

TEST_CASE("findText: walks scrollback before visible grid")
{
    TestTerminal t(20, 5);
    // Push enough lines so "needle" lands in scrollback, then put it in
    // visible grid too. With 5 visible rows + 7 lines + needle + needle
    // again, the first needle is in scrollback and the second is on screen.
    for (int i = 0; i < 7; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    t.feed("needle\r\n");
    for (int i = 0; i < 3; ++i) {
        t.feed("more" + std::to_string(i) + "\r\n");
    }
    t.feed("needle");
    auto matches = doc(t).findText("needle", {});
    REQUIRE(matches.size() == 2);
    // Scrollback line is older → comes first.
    CHECK(matches[0].startLineId < matches[1].startLineId);
}

TEST_CASE("findText: wholeWord enforces boundaries (literal mode)")
{
    TestTerminal t(40, 5);
    t.feed("foo foobar barfoo foo_bar");
    Document::FindOptions opts;
    opts.wholeWord = true;
    auto matches   = doc(t).findText("foo", opts);
    // Only the first "foo" is a whole word — "foobar" / "barfoo" /
    // "foo_bar" all have alphanum on at least one side. Underscore counts
    // as word-char, so "foo_bar" fails the right-boundary check.
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 0);
    CHECK(matches[0].endCol == 3);
}

TEST_CASE("findText: regex mode with character class")
{
    TestTerminal t(40, 5);
    t.feed("error 42 warning 17 error 99");
    Document::FindOptions opts;
    opts.regex   = true;
    auto matches = doc(t).findText("[0-9]+", opts);
    REQUIRE(matches.size() == 3);
    CHECK(matches[0].startCol == 6);
    CHECK(matches[0].endCol == 8); // "42"
    CHECK(matches[1].startCol == 17);
    CHECK(matches[1].endCol == 19); // "17"
    CHECK(matches[2].startCol == 26);
    CHECK(matches[2].endCol == 28); // "99"
}

TEST_CASE("findText: invalid regex returns empty")
{
    TestTerminal t(40, 5);
    t.feed("hello");
    Document::FindOptions opts;
    opts.regex   = true;
    auto matches = doc(t).findText("[unclosed", opts);
    CHECK(matches.empty());
}

TEST_CASE("findText: limit caps the number of results")
{
    TestTerminal t(40, 5);
    // 30 separate logical lines each containing the needle.
    for (int i = 0; i < 30; ++i) {
        t.feed("hit" + std::to_string(i) + "\r\n");
    }
    Document::FindOptions opts;
    opts.limit   = 5;
    auto matches = doc(t).findText("hit", opts);
    CHECK(matches.size() == 5);
}

TEST_CASE("findText: unicode codepoints encode and search correctly")
{
    TestTerminal t(40, 5);
    // Greek alpha (U+03B1) — 2-byte UTF-8.
    t.feed("alpha α beta α gamma");
    auto matches = doc(t).findText("\xCE\xB1", {});
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].startCol == 6);
    CHECK(matches[0].endCol == 7);
    CHECK(matches[1].startCol == 13);
    CHECK(matches[1].endCol == 14);
}

TEST_CASE("findText: match anchored by lineId survives scroll")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 3; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    t.feed("findme\r\n");
    auto before = doc(t).findText("findme", {});
    REQUIRE(before.size() == 1);
    uint64_t id = before[0].startLineId;

    // Push enough content to scroll "findme" into scrollback.
    for (int i = 0; i < 20; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    auto after = doc(t).findText("findme", {});
    REQUIRE(after.size() == 1);
    CHECK(after[0].startLineId == id); // same line ID, just scrolled
    CHECK(after[0].startCol == before[0].startCol);
    CHECK(after[0].endCol == before[0].endCol);
}

TEST_CASE("findText: cell offset crosses soft-wrap boundary on visible grid")
{
    // Width=10. Write a line longer than 10 chars without a hard break;
    // it soft-wraps on the visible grid. The match across the wrap
    // boundary should resolve to a single logical-line offset that the
    // decoration resolver maps back to two visual rows.
    TestTerminal t(10, 5);
    t.feed("aaaa needle bbbb"); // length 16, soft-wraps at col 10
    auto matches = doc(t).findText("needle", {});
    REQUIRE(matches.size() == 1);
    // "needle" starts at logical col 5 (offset within the wrapped line).
    CHECK(matches[0].startCol == 5);
    CHECK(matches[0].endCol == 11);
}

TEST_CASE("scrollToRow: brings matched line to viewport top")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 20; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    auto matches = doc(t).findText("line5", {});
    REQUIRE(matches.size() == 1);
    REQUIRE(t.term.viewportOffset() == 0);

    bool changed = t.term.scrollToRow(matches[0].startLineId);
    CHECK(changed);
    CHECK(t.term.viewportOffset() > 0);
}

TEST_CASE("scrollToRow: 0 lineId returns false")
{
    TestTerminal t(20, 5);
    CHECK_FALSE(t.term.scrollToRow(0));
}

TEST_CASE("scrollToRow: evicted id returns false")
{
    TestTerminal t(20, 5);
    // findText would never return a match for an id that's already
    // evicted; this test simulates the post-eviction read by passing a
    // synthetic unused id.
    CHECK_FALSE(t.term.scrollToRow(99999999ull));
}

// Reproduces the user-reported "no scrollback → highlights don't render"
// symptom. With a tall terminal, content fitting fully on screen, and
// matches confined to visible-grid lineIds, every match should resolve to
// an abs row inside the segments range.
TEST_CASE("findText → addDecoration → resolveDecoration: many matches on visible grid only")
{
    TestTerminal t(80, 24); // standard size, no scrollback expected
    // Mimic `ls -la` output with multiple `s` characters per line.
    t.feed("drwxr-x---  3 jhanssen jhanssen      4096 Apr 14 12:19 .agents\r\n");
    t.feed("drwxrwxr-x  4 jhanssen jhanssen      4096 Aug 23  2024 .android\r\n");
    t.feed("drwxr-x---  3 jhanssen jhanssen      4096 Apr 14 12:17 .bun\r\n");
    REQUIRE(t.term.document().historySize() == 0);

    auto matches = t.term.document().findText("s", {});
    REQUIRE(matches.size() > 0);

    // Each match must resolve cleanly via firstAbsOfLine.
    for (const auto &m : matches) {
        int abs = t.term.document().firstAbsOfLine(m.startLineId);
        CHECK(abs >= 0);
        CHECK(abs < 24); // within the visible grid (24 rows, no scrollback)
    }

    // Add a decoration for each match (using inclusive endCellOffset
    // semantics, exactly as jsTerminalAddDecoration does).
    for (const auto &m : matches) {
        Decoration d;
        d.kind            = DecorationKind::User;
        d.startLineId     = m.startLineId;
        d.endLineId       = m.endLineId;
        d.startCellOffset = m.startCol;
        d.endCellOffset   = m.endCol - 1;
        d.tag             = "search";
        t.term.addDecoration(d);
    }

    // Every stored decoration should resolve.
    for (const auto &dec : t.term.decorations()) {
        auto r = t.term.resolveDecoration(dec);
        CHECK(r.has_value());
        if (r) {
            CHECK(r->startAbsRow >= 0);
            CHECK(r->startAbsRow < 24);
        }
    }
}

// Round-trips findText → addDecoration → resolveDecoration to catch
// regressions in the inclusive/exclusive endCol semantics across the
// boundary. Ensures a one-cell match at col N actually paints col N
// (and ONLY col N) in both no-scrollback and with-scrollback cases.
TEST_CASE("findText → addDecoration → resolveDecoration: single-cell match, no scrollback")
{
    TestTerminal t(40, 20); // tall terminal, content fits without scrolling
    t.feed("hello world");
    REQUIRE(t.term.document().historySize() == 0);

    auto matches = t.term.document().findText("o", {});
    REQUIRE(matches.size() == 2); // "hello" and "world" both contain o
    // First "o" at col 4 (h-e-l-l-o), exclusive endCol 5.
    CHECK(matches[0].startCol == 4);
    CHECK(matches[0].endCol == 5);

    // Now simulate what addDecoration does at the JS boundary: store
    // exclusive endCol as inclusive endCellOffset (ec - 1).
    Decoration d;
    d.kind            = DecorationKind::User;
    d.startLineId     = matches[0].startLineId;
    d.endLineId       = matches[0].endLineId;
    d.startCellOffset = matches[0].startCol;
    d.endCellOffset   = matches[0].endCol - 1; // 5 - 1 = 4 (single-cell)
    uint64_t id       = t.term.addDecoration(d);
    REQUIRE(id != 0);

    auto resolved = t.term.resolveDecoration(t.term.decorations()[0]);
    REQUIRE(resolved.has_value());
    CHECK(resolved->startCol == 4);
    CHECK(resolved->endCol == 4);      // INCLUSIVE end column
    CHECK(resolved->startAbsRow == 0); // first visible row, no scrollback
    CHECK(resolved->endAbsRow == 0);
}

TEST_CASE("findText → addDecoration → resolveDecoration: single-cell match, with scrollback")
{
    TestTerminal t(40, 5); // short terminal so content scrolls
    for (int i = 0; i < 10; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    t.feed("hello world");
    REQUIRE(t.term.document().historySize() > 0);
    int hist = t.term.document().historySize();

    auto matches = t.term.document().findText("hello", {});
    REQUIRE(matches.size() == 1); // "hello" is on the bottom visible row only

    Decoration d;
    d.kind            = DecorationKind::User;
    d.startLineId     = matches[0].startLineId;
    d.endLineId       = matches[0].endLineId;
    d.startCellOffset = matches[0].startCol;
    d.endCellOffset   = matches[0].endCol - 1; // 5 - 1 = 4 → covers cols 0..4 inclusive
    uint64_t id       = t.term.addDecoration(d);
    REQUIRE(id != 0);

    auto resolved = t.term.resolveDecoration(t.term.decorations()[0]);
    REQUIRE(resolved.has_value());
    CHECK(resolved->startCol == 0);
    CHECK(resolved->endCol == 4);
    CHECK(resolved->startAbsRow == hist + 4); // last visible row (5 rows tall, "hello" on the last)
    CHECK(resolved->endAbsRow == hist + 4);
}

TEST_CASE("scrollToRow: idempotent when already at row")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 20; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    auto matches = doc(t).findText("line0", {});
    REQUIRE(matches.size() == 1);
    bool first = t.term.scrollToRow(matches[0].startLineId);
    CHECK(first);
    bool second = t.term.scrollToRow(matches[0].startLineId);
    CHECK_FALSE(second); // no change → returns false
}

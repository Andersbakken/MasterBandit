#include "Document.h"
#include "TestTerminal.h"
#include <doctest/doctest.h>

// Document::findText is the C++ engine behind pane.findText. These tests
// drive it directly through the Document inside a TestTerminal so we can
// exercise scrollback + visible-grid + wrap edges without pulling in JS.
//
// findText is a cursor API: it walks newest → oldest from an optional
// anchor, bounded by a match cap and a per-call line budget, and returns
// a resume anchor for the next slice. Matches come back grouped by line,
// newest line first, ascending column within a line.

namespace {

const Document &doc(TestTerminal &t)
{
    return t.term.document();
}

// Single-slice convenience: run one findText call and return its matches.
std::vector<Document::Match> findAll(TestTerminal &t, std::string_view needle,
                                     Document::FindOptions opts = {})
{
    return doc(t).findText(needle, opts).matches;
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
    auto res = doc(t).findText("", {});
    CHECK(res.matches.empty());
    CHECK(res.resumeLineId == 0);
    CHECK(res.linesSearched == 0);
}

TEST_CASE("findText: literal substring on visible grid")
{
    TestTerminal t(40, 5);
    t.feed("hello world");
    auto matches = findAll(t, "world");
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
    auto matches = findAll(t, "hello");
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
    auto matches       = findAll(t, "hello", opts);
    CHECK(matches.empty());
    matches = findAll(t, "Hello", opts);
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 0);
}

TEST_CASE("findText: multiple non-overlapping matches on one line, ascending cols")
{
    TestTerminal t(40, 5);
    t.feed("foo bar foo baz foo");
    auto matches = findAll(t, "foo");
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
    auto matches = findAll(t, "aa");
    // "aaaa" with needle "aa" → 2 non-overlapping matches at 0 and 2,
    // not 3 overlapping at 0/1/2.
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].startCol == 0);
    CHECK(matches[0].endCol == 2);
    CHECK(matches[1].startCol == 2);
    CHECK(matches[1].endCol == 4);
}

TEST_CASE("findText: matches across separate logical lines come back newest first")
{
    TestTerminal t(40, 5);
    t.feed("alpha\r\n");
    t.feed("beta\r\n");
    t.feed("alpha\r\n");
    t.feed("gamma");
    auto matches = findAll(t, "alpha");
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].startLineId != matches[1].startLineId);
    // First match is the newer one (newest-first walk).
    CHECK(matches[0].startLineId > matches[1].startLineId);
}

TEST_CASE("findText: walks visible grid before scrollback")
{
    TestTerminal t(20, 5);
    // Push enough lines so the first "needle" lands in scrollback, then
    // put it in the visible grid too.
    for (int i = 0; i < 7; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    t.feed("needle\r\n");
    for (int i = 0; i < 3; ++i) {
        t.feed("more" + std::to_string(i) + "\r\n");
    }
    t.feed("needle");
    auto matches = findAll(t, "needle");
    REQUIRE(matches.size() == 2);
    // Visible-grid line is newer → comes first.
    CHECK(matches[0].startLineId > matches[1].startLineId);
}

TEST_CASE("findText: wholeWord enforces boundaries (literal mode)")
{
    TestTerminal t(40, 5);
    t.feed("foo foobar barfoo foo_bar");
    Document::FindOptions opts;
    opts.wholeWord = true;
    auto matches   = findAll(t, "foo", opts);
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
    auto matches = findAll(t, "[0-9]+", opts);
    REQUIRE(matches.size() == 3);
    CHECK(matches[0].startCol == 6);
    CHECK(matches[0].endCol == 8); // "42"
    CHECK(matches[1].startCol == 17);
    CHECK(matches[1].endCol == 19); // "17"
    CHECK(matches[2].startCol == 26);
    CHECK(matches[2].endCol == 28); // "99"
}

TEST_CASE("findText: invalid regex returns empty complete result")
{
    TestTerminal t(40, 5);
    t.feed("hello");
    Document::FindOptions opts;
    opts.regex = true;
    auto res   = doc(t).findText("[unclosed", opts);
    CHECK(res.matches.empty());
    CHECK(res.resumeLineId == 0);
}

TEST_CASE("findText: limit keeps the newest matches and reports a resume anchor")
{
    TestTerminal t(40, 5);
    // 30 separate logical lines each containing the needle.
    for (int i = 0; i < 30; ++i) {
        t.feed("hit" + std::to_string(i) + "\r\n");
    }
    Document::FindOptions opts;
    opts.limit = 5;
    auto res   = doc(t).findText("hit", opts);
    REQUIRE(res.matches.size() == 5);
    // Newest-first walk: the truncated-away matches are the OLDEST ones.
    CHECK(res.matches[0].startLineId > res.matches[4].startLineId);
    CHECK(res.resumeLineId != 0);

    // Resuming picks up the remaining 25 and completes.
    opts.limit      = 0;
    opts.fromLineId = res.resumeLineId;
    auto rest       = doc(t).findText("hit", opts);
    CHECK(rest.matches.size() == 25);
    CHECK(rest.resumeLineId == 0);
}

TEST_CASE("findText: limit never splits a line's matches")
{
    TestTerminal t(40, 5);
    t.feed("foo foo foo\r\n"); // older line, 3 matches
    t.feed("foo foo\r\n");     // newer line, 2 matches
    Document::FindOptions opts;
    opts.limit = 1;
    auto res   = doc(t).findText("foo", opts);
    // The newest matching line has 2 matches; both are returned even
    // though the cap was hit after the first.
    REQUIRE(res.matches.size() == 2);
    CHECK(res.matches[0].startLineId == res.matches[1].startLineId);
    REQUIRE(res.resumeLineId != 0);

    opts.fromLineId = res.resumeLineId;
    auto rest       = doc(t).findText("foo", opts);
    REQUIRE(rest.matches.size() == 3);
    CHECK(rest.matches[0].startLineId < res.matches[0].startLineId);
}

TEST_CASE("findText: maxLines bounds per-call work; resumed slices cover everything once")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 30; ++i) {
        t.feed("hit" + std::to_string(i) + "\r\n");
    }
    Document::FindOptions opts;
    opts.limit       = 0;
    opts.maxLines    = 4;
    int totalMatches = 0;
    int rounds       = 0;
    uint64_t from    = 0;
    while (true) {
        opts.fromLineId = from;
        auto res        = doc(t).findText("hit", opts);
        CHECK(res.linesSearched <= 4);
        totalMatches += static_cast<int>(res.matches.size());
        ++rounds;
        REQUIRE(rounds < 100);
        if (res.resumeLineId == 0) {
            break;
        }
        from = res.resumeLineId;
    }
    CHECK(totalMatches == 30);
    CHECK(rounds > 1);
}

TEST_CASE("findText: maxLines makes a miss cheap")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 30; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    Document::FindOptions opts;
    opts.maxLines = 3;
    auto res      = doc(t).findText("zzqx", opts);
    CHECK(res.matches.empty());
    CHECK(res.linesSearched == 3);
    CHECK(res.resumeLineId != 0); // more document remains
}

TEST_CASE("findText: chunked pagination equals one uncapped call")
{
    TestTerminal t(30, 5);
    for (int i = 0; i < 25; ++i) {
        t.feed("word" + std::to_string(i % 3) + " tail\r\n");
    }
    Document::FindOptions all;
    all.limit  = 0;
    auto whole = doc(t).findText("word", all).matches;
    REQUIRE(!whole.empty());

    std::vector<Document::Match> paged;
    Document::FindOptions opts;
    opts.limit    = 3;
    opts.maxLines = 4;
    uint64_t from = 0;
    int rounds    = 0;
    while (true) {
        opts.fromLineId = from;
        auto res        = doc(t).findText("word", opts);
        paged.insert(paged.end(), res.matches.begin(), res.matches.end());
        ++rounds;
        REQUIRE(rounds < 100);
        if (res.resumeLineId == 0) {
            break;
        }
        from = res.resumeLineId;
    }
    REQUIRE(paged.size() == whole.size());
    for (size_t i = 0; i < whole.size(); ++i) {
        CHECK(paged[i].startLineId == whole[i].startLineId);
        CHECK(paged[i].startCol == whole[i].startCol);
        CHECK(paged[i].endCol == whole[i].endCol);
    }
}

TEST_CASE("findText: fromLineId anchor is inclusive and skips newer lines")
{
    TestTerminal t(40, 5);
    t.feed("mark A\r\n");
    t.feed("mark B\r\n");
    t.feed("mark C");
    auto all = findAll(t, "mark");
    REQUIRE(all.size() == 3); // newest first: C, B, A

    Document::FindOptions opts;
    opts.fromLineId = all[1].startLineId; // anchor at B
    auto res        = doc(t).findText("mark", opts);
    REQUIRE(res.matches.size() == 2);
    CHECK(res.matches[0].startLineId == all[1].startLineId); // B itself
    CHECK(res.matches[1].startLineId == all[2].startLineId); // then A
    CHECK(res.resumeLineId == 0);
}

TEST_CASE("findText: unknown fromLineId reports completion")
{
    TestTerminal t(40, 5);
    t.feed("hello world\r\n");
    Document::FindOptions opts;
    opts.fromLineId = 99999999ull;
    auto res        = doc(t).findText("hello", opts);
    CHECK(res.matches.empty());
    CHECK(res.resumeLineId == 0);
    CHECK(res.linesSearched == 0);
}

TEST_CASE("findText: unicode codepoints encode and search correctly")
{
    TestTerminal t(40, 5);
    // Greek alpha (U+03B1) — 2-byte UTF-8.
    t.feed("alpha α beta α gamma");
    auto matches = findAll(t, "\xCE\xB1");
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
    auto before = findAll(t, "findme");
    REQUIRE(before.size() == 1);
    uint64_t id = before[0].startLineId;

    // Push enough content to scroll "findme" into scrollback.
    for (int i = 0; i < 20; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    auto after = findAll(t, "findme");
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
    auto matches = findAll(t, "needle");
    REQUIRE(matches.size() == 1);
    // "needle" starts at logical col 5 (offset within the wrapped line).
    CHECK(matches[0].startCol == 5);
    CHECK(matches[0].endCol == 11);
}

TEST_CASE("findText: match crossing a scrollback block boundary is found")
{
    // A logical line longer than LogicalLineBlock::kCellCapacity (682) is
    // split across consecutive blocks that share one lineId. The needle is
    // placed at cells 676..682 so it straddles the first split point when
    // the line opens a fresh block (40-col rows → 17 rows = 680 cells fit
    // in block 0). If capacities change the needle no longer straddles,
    // but the match itself must be found at col 676 either way.
    TestTerminal t(40, 5);
    std::string line(676, 'a');
    line += "NEEDLE";
    line.append(2000 - line.size(), 'b');
    t.feed(line);
    t.feed("\r\n");
    for (int i = 0; i < 8; ++i) {
        t.feed("filler" + std::to_string(i) + "\r\n");
    }
    REQUIRE(doc(t).historySize() > 0);

    auto matches = findAll(t, "NEEDLE");
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].startCol == 676);
    CHECK(matches[0].endCol == 682);

    // And the offsets must resolve through the decoration path.
    int firstAbs = doc(t).firstAbsOfLine(matches[0].startLineId);
    CHECK(firstAbs >= 0);
}

TEST_CASE("scrollToRow: brings matched line to viewport top")
{
    TestTerminal t(20, 5);
    for (int i = 0; i < 20; ++i) {
        t.feed("line" + std::to_string(i) + "\r\n");
    }
    auto matches = findAll(t, "line5");
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

    auto matches = findAll(t, "s");
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

    auto matches = findAll(t, "o");
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

    auto matches = findAll(t, "hello");
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
    auto matches = findAll(t, "line0");
    REQUIRE(matches.size() == 1);
    bool first = t.term.scrollToRow(matches[0].startLineId);
    CHECK(first);
    bool second = t.term.scrollToRow(matches[0].startLineId);
    CHECK_FALSE(second); // no change → returns false
}

namespace {

// Build an Add op with a one-line, one-cell range at (lineId, col).
DecorationBatchOp addOp(uint64_t lineId, int col, std::string tag)
{
    DecorationBatchOp op;
    op.kind                 = DecorationBatchOp::Kind::Add;
    op.spec.kind            = DecorationKind::User;
    op.spec.startLineId     = lineId;
    op.spec.endLineId       = lineId;
    op.spec.startCellOffset = col;
    op.spec.endCellOffset   = col;
    op.spec.tag             = std::move(tag);
    return op;
}

DecorationBatchOp clearOp(std::string tag)
{
    DecorationBatchOp op;
    op.kind     = DecorationBatchOp::Kind::Clear;
    op.clearTag = std::move(tag);
    return op;
}

} // namespace

TEST_CASE("applyDecorationBatch: empty batch is a no-op")
{
    TestTerminal t(40, 5);
    t.feed("hello world");
    auto m = findAll(t, "o");
    REQUIRE(m.size() >= 1);

    auto ids = t.term.applyDecorationBatch({});
    CHECK(ids.empty());
    CHECK(t.term.decorations().empty());
}

TEST_CASE("applyDecorationBatch: returns ids in queue order, ids match stored decorations")
{
    TestTerminal t(40, 5);
    t.feed("aaaaa");
    auto m = findAll(t, "a");
    REQUIRE(m.size() >= 3);

    std::vector<DecorationBatchOp> ops;
    ops.push_back(addOp(m[0].startLineId, m[0].startCol, "search"));
    ops.push_back(addOp(m[1].startLineId, m[1].startCol, "search"));
    ops.push_back(addOp(m[2].startLineId, m[2].startCol, "current-match"));

    auto ids = t.term.applyDecorationBatch(std::move(ops));
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] != 0);
    CHECK(ids[1] != ids[0]);
    CHECK(ids[2] != ids[1]);

    const auto &decs = t.term.decorations();
    REQUIRE(decs.size() == 3);
    // Insertion order preserved.
    CHECK(decs[0].id == ids[0]);
    CHECK(decs[1].id == ids[1]);
    CHECK(decs[2].id == ids[2]);
    CHECK(decs[2].tag == "current-match");
}

TEST_CASE("applyDecorationBatch: clear-then-add atomically replaces a tag set")
{
    TestTerminal t(40, 5);
    t.feed("xxxxx");
    auto m = findAll(t, "x");
    REQUIRE(m.size() >= 5);

    // Pre-populate with 3 "search" decorations.
    {
        std::vector<DecorationBatchOp> seed;
        for (int i = 0; i < 3; ++i) {
            seed.push_back(addOp(m[i].startLineId, m[i].startCol, "search"));
        }
        t.term.applyDecorationBatch(std::move(seed));
    }
    REQUIRE(t.term.decorations().size() == 3);

    // Now clear "search" + add 2 fresh "search" decorations + 1 "other"
    // in a single batch — the prior 3 should be gone and the new 3 visible.
    std::vector<DecorationBatchOp> batch;
    batch.push_back(clearOp("search"));
    batch.push_back(addOp(m[3].startLineId, m[3].startCol, "search"));
    batch.push_back(addOp(m[4].startLineId, m[4].startCol, "search"));
    batch.push_back(addOp(m[0].startLineId, m[0].startCol, "other"));
    auto ids = t.term.applyDecorationBatch(std::move(batch));
    CHECK(ids.size() == 3); // 3 Add ops

    const auto &decs = t.term.decorations();
    REQUIRE(decs.size() == 3);
    int searchCount = 0, otherCount = 0;
    for (const auto &d : decs) {
        if (d.tag == "search") {
            ++searchCount;
        } else if (d.tag == "other") {
            ++otherCount;
        }
    }
    CHECK(searchCount == 2);
    CHECK(otherCount == 1);
}

TEST_CASE("applyDecorationBatch: clear is tag-scoped")
{
    TestTerminal t(40, 5);
    t.feed("aaaaa");
    auto m = findAll(t, "a");
    REQUIRE(m.size() >= 3);

    std::vector<DecorationBatchOp> seed;
    seed.push_back(addOp(m[0].startLineId, m[0].startCol, "search"));
    seed.push_back(addOp(m[1].startLineId, m[1].startCol, "search"));
    seed.push_back(addOp(m[2].startLineId, m[2].startCol, "current-match"));
    t.term.applyDecorationBatch(std::move(seed));
    REQUIRE(t.term.decorations().size() == 3);

    // Clearing "search" leaves "current-match" intact.
    std::vector<DecorationBatchOp> batch;
    batch.push_back(clearOp("search"));
    t.term.applyDecorationBatch(std::move(batch));

    const auto &decs = t.term.decorations();
    REQUIRE(decs.size() == 1);
    CHECK(decs[0].tag == "current-match");
}

TEST_CASE("applyDecorationBatch: empty clearTag clears all User decorations")
{
    TestTerminal t(40, 5);
    t.feed("aaaaa");
    auto m = findAll(t, "a");
    REQUIRE(m.size() >= 3);

    std::vector<DecorationBatchOp> seed;
    seed.push_back(addOp(m[0].startLineId, m[0].startCol, "search"));
    seed.push_back(addOp(m[1].startLineId, m[1].startCol, "current-match"));
    seed.push_back(addOp(m[2].startLineId, m[2].startCol, "other"));
    t.term.applyDecorationBatch(std::move(seed));
    REQUIRE(t.term.decorations().size() == 3);

    std::vector<DecorationBatchOp> batch;
    batch.push_back(clearOp("")); // empty tag = clear all User
    t.term.applyDecorationBatch(std::move(batch));

    CHECK(t.term.decorations().empty());
}

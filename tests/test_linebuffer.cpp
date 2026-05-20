#include "LineBuffer.h"
#include <doctest/doctest.h>
#include <string>

namespace {

// Build a Cell with a single ASCII codepoint and default attrs.
Cell c(char ch)
{
    Cell cell;
    cell.wc = static_cast<char32_t>(ch);
    return cell;
}

// Build a row of cells from a string.
std::vector<Cell> row(const std::string &s)
{
    std::vector<Cell> r;
    r.reserve(s.size());
    for (char ch : s) {
        r.push_back(c(ch));
    }
    return r;
}

// Pull the cells of a wrapped row out as a string. Trailing nulls are spaces.
std::string wrappedRowText(const LineBuffer &lb, int wrappedRow, int width)
{
    int len       = 0;
    const Cell *p = lb.wrappedRowCells(wrappedRow, width, &len);
    if (!p) {
        return {};
    }
    std::string out;
    for (int i = 0; i < len; ++i) {
        char32_t cp = p[i].wc;
        out += (cp == 0) ? ' ' : static_cast<char>(cp);
    }
    return out;
}

} // namespace

TEST_CASE("LineBuffer: empty state")
{
    LineBuffer lb;
    CHECK(lb.totalLogicalLines() == 0);
    CHECK(lb.totalCells() == 0);
    CHECK(lb.numWrappedRows(80) == 0);
    LineBuffer::WrappedLineRef ref;
    CHECK_FALSE(lb.wrappedRowAt(0, 80, &ref));
}

TEST_CASE("LineBuffer: append a single hard line")
{
    LineBuffer lb;
    auto r = row("hello world");
    lb.appendHardLine(r.data(), static_cast<int>(r.size()), /*lineId*/ 1, /*flags*/ 0, nullptr);
    CHECK(lb.totalLogicalLines() == 1);
    CHECK(lb.totalCells() == 11);
    CHECK(lb.numWrappedRows(80) == 1);
    CHECK(wrappedRowText(lb, 0, 80) == "hello world");
    CHECK(lb.lineIdAtLogicalIndex(0) == 1);
    CHECK(lb.logicalIndexOfLineId(1) == 0);
}

TEST_CASE("LineBuffer: wrap a long line at a narrow width")
{
    LineBuffer lb;
    auto r = row("ABCDEFGHIJ");
    lb.appendHardLine(r.data(), 10, 1, 0, nullptr);
    CHECK(lb.numWrappedRows(4) == 3);
    CHECK(wrappedRowText(lb, 0, 4) == "ABCD");
    CHECK(wrappedRowText(lb, 1, 4) == "EFGH");
    CHECK(wrappedRowText(lb, 2, 4) == "IJ");
}

TEST_CASE("LineBuffer: wrap cache MRU per block")
{
    LineBuffer lb;
    auto r = row("ABCDEFGHIJ");
    lb.appendHardLine(r.data(), 10, 1, 0, nullptr);
    // First call populates cache at width=4.
    CHECK(lb.numWrappedRows(4) == 3);
    // Different width recomputes.
    CHECK(lb.numWrappedRows(5) == 2);
    // Cache only holds MRU; back to 4 recomputes but result is still 3.
    CHECK(lb.numWrappedRows(4) == 3);
}

TEST_CASE("LineBuffer: extend a partial line via soft EOL")
{
    LineBuffer lb;
    // Simulate a soft-wrapped scroll-out: first row is partial.
    auto p1 = row("ABCDEF");
    lb.appendLine(p1.data(), 6, LineMeta::EolSoft, /*partial*/ true, /*extendsLast*/ false,
                  /*lineId*/ 1,
                  0,
                  nullptr);
    CHECK(lb.totalLogicalLines() == 1);
    CHECK(lb.lastLineIsPartial());

    // Second row continues the same logical line.
    auto p2 = row("GHIJ");
    lb.appendLine(p2.data(), 4, LineMeta::EolHard, /*partial*/ false, /*extendsLast*/ true,
                  /*lineId*/ 0 /*ignored*/,
                  0,
                  nullptr);
    CHECK(lb.totalLogicalLines() == 1);
    CHECK_FALSE(lb.lastLineIsPartial());

    CHECK(lb.numWrappedRows(4) == 3);
    CHECK(wrappedRowText(lb, 0, 4) == "ABCD");
    CHECK(wrappedRowText(lb, 1, 4) == "EFGH");
    CHECK(wrappedRowText(lb, 2, 4) == "IJ");

    // Same content, different display width.
    CHECK(lb.numWrappedRows(10) == 1);
    CHECK(wrappedRowText(lb, 0, 10) == "ABCDEFGHIJ");
}

TEST_CASE("LineBuffer: hard-line boundaries are independent")
{
    LineBuffer lb;
    auto r1 = row("hello");
    lb.appendHardLine(r1.data(), 5, 1, 0, nullptr);
    auto r2 = row("world");
    lb.appendHardLine(r2.data(), 5, 2, 0, nullptr);
    CHECK(lb.totalLogicalLines() == 2);
    CHECK(lb.numWrappedRows(80) == 2);
    CHECK(wrappedRowText(lb, 0, 80) == "hello");
    CHECK(wrappedRowText(lb, 1, 80) == "world");
}

TEST_CASE("LineBuffer: max logical lines eviction")
{
    LineBuffer lb(3, /*maxCells*/ 0);
    for (int i = 1; i <= 5; ++i) {
        std::string s(1, static_cast<char>('A' + i - 1));
        auto r = row(s);
        lb.appendHardLine(r.data(), 1, /*lineId*/ static_cast<uint64_t>(i), 0, nullptr);
    }
    CHECK(lb.totalLogicalLines() == 3);
    // Oldest 2 evicted; lines C, D, E remain (ids 3, 4, 5).
    CHECK(lb.lineIdAtLogicalIndex(0) == 3);
    CHECK(lb.lineIdAtLogicalIndex(1) == 4);
    CHECK(lb.lineIdAtLogicalIndex(2) == 5);
    CHECK(lb.logicalIndexOfLineId(1) == -1);
    CHECK(lb.logicalIndexOfLineId(2) == -1);
    CHECK(lb.logicalIndexOfLineId(3) == 0);
    CHECK(wrappedRowText(lb, 0, 80) == "C");
    CHECK(wrappedRowText(lb, 2, 80) == "E");
}

TEST_CASE("LineBuffer: max total cells eviction (backstop)")
{
    LineBuffer lb(/*maxLines*/ 100000, /*maxCells*/ 15);
    auto r5 = row("AAAAA");
    for (int i = 1; i <= 4; ++i) {
        lb.appendHardLine(r5.data(), 5, static_cast<uint64_t>(i), 0, nullptr);
    }
    // 4 lines × 5 cells = 20 cells; max is 15. Oldest line drops to bring us to 15.
    CHECK(lb.totalCells() == 15);
    CHECK(lb.totalLogicalLines() == 3);
    CHECK(lb.lineIdAtLogicalIndex(0) == 2);
}

TEST_CASE("LineBuffer: eviction callback fires once per line")
{
    LineBuffer lb(2, 0);
    std::vector<uint64_t> evicted;
    lb.setOnLineIdEvicted([&](uint64_t id)
                          {
                              evicted.push_back(id);
                          });
    for (int i = 1; i <= 5; ++i) {
        auto r = row("X");
        lb.appendHardLine(r.data(), 1, static_cast<uint64_t>(i), 0, nullptr);
    }
    // 5 lines appended, max=2 → 3 lines evicted (ids 1, 2, 3).
    CHECK(evicted.size() == 3);
    CHECK(evicted[0] == 1);
    CHECK(evicted[1] == 2);
    CHECK(evicted[2] == 3);
}

TEST_CASE("LineBuffer: popLastLine returns full content")
{
    LineBuffer lb;
    auto r1 = row("first");
    lb.appendHardLine(r1.data(), 5, 1, 0, nullptr);
    auto r2 = row("second");
    lb.appendHardLine(r2.data(), 6, 2, 0, nullptr);
    auto popped = lb.popLastLine();
    REQUIRE(popped.ok);
    CHECK(popped.lineId == 2);
    CHECK(popped.cells.size() == 6);
    CHECK(popped.cells[0].wc == 's');
    CHECK(popped.eol == LineMeta::EolHard);
    CHECK_FALSE(popped.wasPartial);
    CHECK(lb.totalLogicalLines() == 1);
    CHECK(lb.totalCells() == 5);
}

TEST_CASE("LineBuffer: popLastLine on partial line preserves partial flag")
{
    LineBuffer lb;
    auto r = row("partial");
    lb.appendLine(r.data(), 7, LineMeta::EolSoft, /*partial*/ true, false, 1, 0, nullptr);
    auto popped = lb.popLastLine();
    REQUIRE(popped.ok);
    CHECK(popped.wasPartial);
    CHECK(popped.eol == LineMeta::EolSoft);
}

TEST_CASE("LineBuffer: empty popLastLine")
{
    LineBuffer lb;
    auto p = lb.popLastLine();
    CHECK_FALSE(p.ok);
}

TEST_CASE("LineBuffer: textInRange joins lines with newlines")
{
    LineBuffer lb;
    auto r1 = row("hello");
    auto r2 = row("world");
    auto r3 = row("foo");
    lb.appendHardLine(r1.data(), 5, 1, 0, nullptr);
    lb.appendHardLine(r2.data(), 5, 2, 0, nullptr);
    lb.appendHardLine(r3.data(), 3, 3, 0, nullptr);
    CHECK(lb.textInRange(0, 2) == "hello\nworld\nfoo");
    CHECK(lb.textInRange(1, 1) == "world");
    CHECK(lb.textInRange(0, 1, /*startCol*/ 2) == "llo\nworld");
    CHECK(lb.textInRange(0, 2, /*startCol*/ 0, /*endCol*/ 3) == "hello\nworld\nfoo");
    CHECK(lb.textInRange(0, 2, 0, 4) == "hello\nworld\nfoo");
}

TEST_CASE("LineBuffer: blocks roll over when one fills")
{
    LineBuffer lb;
    // Append enough cells to span multiple blocks.
    auto r = row(std::string(100, 'X'));
    for (int i = 1; i <= 20; ++i) {
        lb.appendHardLine(r.data(), 100, static_cast<uint64_t>(i), 0, nullptr);
    }
    // 20×100 = 2000 cells; block capacity is 682, so ~3 blocks.
    CHECK(lb.blockCount() >= 3);
    CHECK(lb.totalLogicalLines() == 20);
    CHECK(lb.totalCells() == 2000);
    // Line IDs are still resolvable across blocks.
    for (int i = 0; i < 20; ++i) {
        CHECK(lb.lineIdAtLogicalIndex(i) == static_cast<uint64_t>(i + 1));
    }
}

TEST_CASE("LineBuffer: extras carried with line and remapped on extension")
{
    LineBuffer lb;
    // First row of soft-wrapped line: extra at col 2.
    std::unordered_map<int, CellExtra> extrasA;
    extrasA[2].hyperlinkId = 99;
    auto r1                = row("AAAA");
    lb.appendLine(r1.data(), 4, LineMeta::EolSoft, /*partial*/ true, false, 1, 0, &extrasA);

    // Second row continues, extra at col 1 (= col 5 in the joined line).
    std::unordered_map<int, CellExtra> extrasB;
    extrasB[1].hyperlinkId = 77;
    auto r2                = row("BBBB");
    lb.appendLine(r2.data(), 4, LineMeta::EolHard, /*partial*/ false, /*extendsLast*/ true, 0, 0, &extrasB);

    CHECK(lb.totalLogicalLines() == 1);
    const auto &m = lb.block(0).meta(0);
    REQUIRE(m.extras.count(2));
    CHECK(m.extras.at(2).hyperlinkId == 99);
    REQUIRE(m.extras.count(5)); // 4 (length of first row) + 1
    CHECK(m.extras.at(5).hyperlinkId == 77);
}

TEST_CASE("LineBuffer: invalidateWrapCaches clears MRU")
{
    LineBuffer lb;
    auto r = row("ABCDEFGH");
    lb.appendHardLine(r.data(), 8, 1, 0, nullptr);
    CHECK(lb.numWrappedRows(4) == 2); // populates cache
    lb.invalidateWrapCaches();
    CHECK(lb.numWrappedRows(4) == 2); // still correct
}

TEST_CASE("LineBuffer: lineId monotonic resolution after partial eviction")
{
    LineBuffer lb(3, 0);
    for (int i = 1; i <= 10; ++i) {
        auto r = row("X");
        lb.appendHardLine(r.data(), 1, static_cast<uint64_t>(i), 0, nullptr);
    }
    CHECK(lb.totalLogicalLines() == 3);
    CHECK(lb.logicalIndexOfLineId(7) == -1); // very old, evicted
    CHECK(lb.logicalIndexOfLineId(8) == 0);
    CHECK(lb.logicalIndexOfLineId(9) == 1);
    CHECK(lb.logicalIndexOfLineId(10) == 2);
    CHECK(lb.logicalIndexOfLineId(11) == -1); // doesn't exist
}

TEST_CASE("LineBuffer: clear")
{
    LineBuffer lb;
    auto r = row("hello");
    lb.appendHardLine(r.data(), 5, 1, 0, nullptr);
    lb.appendHardLine(r.data(), 5, 2, 0, nullptr);
    lb.clear();
    CHECK(lb.totalLogicalLines() == 0);
    CHECK(lb.totalCells() == 0);
    CHECK(lb.blockCount() == 0);
}

// Oracle: fresh recompute via invalidateWrapCaches + numWrappedRows.
static int freshNumWrappedRows(LineBuffer &lb, int width)
{
    lb.invalidateWrapCaches();
    return lb.numWrappedRows(width);
}

TEST_CASE("LineBuffer: incremental sum cache matches fresh recompute (warm cache, no eviction)")
{
    LineBuffer lb;
    const int width = 13;
    // Warm cache with the empty state.
    CHECK(lb.numWrappedRows(width) == 0);

    // Append lines of varying lengths (forcing multi-row wraps).
    const char *src = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 5000; ++i) {
        std::string s;
        int len = 1 + (i * 7) % 64; // 1..64
        for (int k = 0; k < len; ++k) {
            s += src[(i + k) % 36];
        }
        auto r       = row(s);
        bool partial = (i % 5) == 0;
        auto eol     = partial ? LineMeta::EolSoft : LineMeta::EolHard;
        lb.appendLine(r.data(), static_cast<int>(r.size()), eol, partial,
                      /*extendsLast*/ false,
                      static_cast<uint64_t>(i + 1),
                      0,
                      nullptr);
        // Touch the cache periodically — this is what real callers do during
        // rendering and what makes the optimization actually fire.
        if ((i % 17) == 0) {
            (void)lb.numWrappedRows(width);
        }
    }

    int cached = lb.numWrappedRows(width);
    int fresh  = freshNumWrappedRows(lb, width);
    CHECK(cached == fresh);
}

TEST_CASE("LineBuffer: incremental sum cache matches fresh recompute (with extendsLast)")
{
    LineBuffer lb;
    const int width = 11;
    CHECK(lb.numWrappedRows(width) == 0);

    // Alternate "new partial line" / "extend it" / "seal with hard line".
    uint64_t id = 1;
    for (int batch = 0; batch < 1000; ++batch) {
        // Start partial.
        auto r1 = row("aaa");
        lb.appendLine(r1.data(), 3, LineMeta::EolSoft, /*partial*/ true,
                      /*extendsLast*/ false,
                      id,
                      0,
                      nullptr);
        // Touch cache.
        (void)lb.numWrappedRows(width);

        // Extend a few times.
        for (int k = 0; k < 4; ++k) {
            auto rk = row("bbbbbb"); // 6 cells
            lb.appendLine(rk.data(), 6, LineMeta::EolSoft, /*partial*/ true,
                          /*extendsLast*/ true,
                          id,
                          0,
                          nullptr);
            if ((k & 1) == 0) {
                (void)lb.numWrappedRows(width);
            }
        }

        // Seal with a hard EOL extension.
        auto rs = row("Z");
        lb.appendLine(rs.data(), 1, LineMeta::EolHard, /*partial*/ false,
                      /*extendsLast*/ true,
                      id,
                      0,
                      nullptr);
        (void)lb.numWrappedRows(width);

        ++id;
    }

    int cached = lb.numWrappedRows(width);
    int fresh  = freshNumWrappedRows(lb, width);
    CHECK(cached == fresh);
}

TEST_CASE("LineBuffer: incremental sum cache matches fresh recompute (with eviction)")
{
    // Tight scrollback that forces evictions on most appends.
    LineBuffer lb(/*maxLogicalLines*/ 50, /*maxTotalCells*/ 0);
    const int width = 7;
    CHECK(lb.numWrappedRows(width) == 0);

    const char *src = "abcdefghij";
    for (int i = 0; i < 5000; ++i) {
        std::string s;
        int len = 1 + (i * 3) % 30;
        for (int k = 0; k < len; ++k) {
            s += src[(i + k) % 10];
        }
        auto r = row(s);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(i + 1), 0, nullptr);
        if ((i % 11) == 0) {
            (void)lb.numWrappedRows(width);
        }
    }

    int cached = lb.numWrappedRows(width);
    int fresh  = freshNumWrappedRows(lb, width);
    CHECK(cached == fresh);
}

TEST_CASE("LineBuffer: incremental sum cache matches fresh recompute (mixed append + popLastLine)")
{
    // popLastLine invalidates the sum cache fully. Re-appending afterward
    // must rebuild correctly.
    LineBuffer lb;
    const int width = 9;
    (void)lb.numWrappedRows(width);

    uint64_t id = 1;
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 10; ++i) {
            std::string s(1 + ((round + i) % 25), 'X');
            auto r = row(s);
            lb.appendHardLine(r.data(), static_cast<int>(r.size()), id++, 0, nullptr);
            if ((i & 3) == 0) {
                (void)lb.numWrappedRows(width);
            }
        }
        // Pop a couple — invalidates cache.
        lb.popLastLine();
        lb.popLastLine();
        // Re-warm cache, then keep going.
        (void)lb.numWrappedRows(width);
    }

    int cached = lb.numWrappedRows(width);
    int fresh  = freshNumWrappedRows(lb, width);
    CHECK(cached == fresh);
}

TEST_CASE("LineBuffer: width switch then incremental append")
{
    LineBuffer lb;
    for (int i = 0; i < 100; ++i) {
        std::string s(1 + (i % 20), 'X');
        auto r = row(s);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(i + 1), 0, nullptr);
    }
    // Warm at width 10.
    int w10_before = lb.numWrappedRows(10);
    // Now ask at width 5 (different) — internally rebuilds cache at width 5.
    int w5_before  = lb.numWrappedRows(5);
    // Append more. The internal cache is at width 5. The block-level wrap
    // cache for the back block may still hold width 10's value until the
    // append invalidates it.
    for (int i = 0; i < 50; ++i) {
        std::string s(3 + (i % 11), 'Y');
        auto r = row(s);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(1000 + i), 0, nullptr);
        if ((i & 1) == 0) {
            (void)lb.numWrappedRows(5);
        }
    }
    int w5_cached = lb.numWrappedRows(5);
    int w5_fresh  = freshNumWrappedRows(lb, 5);
    CHECK(w5_cached == w5_fresh);
    int w10_cached = lb.numWrappedRows(10);
    int w10_fresh  = freshNumWrappedRows(lb, 10);
    CHECK(w10_cached == w10_fresh);
    (void)w10_before;
    (void)w5_before;
}

TEST_CASE("LineBuffer: cache survives back-to-back appends that cross block boundaries")
{
    // Sized to push hard against LogicalLineBlock::kCellCapacity (682) so the
    // append path frequently seals a block and opens a new one.
    LineBuffer lb;
    const int width = 17;
    (void)lb.numWrappedRows(width);

    std::string big(200, 'q');
    for (int i = 0; i < 2000; ++i) {
        auto r = row(big);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(i + 1), 0, nullptr);
        if ((i & 7) == 0) {
            (void)lb.numWrappedRows(width);
        }
    }
    int cached = lb.numWrappedRows(width);
    int fresh  = freshNumWrappedRows(lb, width);
    CHECK(cached == fresh);
}

// ── resolveLogicalIndex: O(log N_blocks) via cachedBlockEndLogical_ ──────────

namespace {
// Reference implementation: the old O(N_blocks) linear scan. Lets tests
// compare the new binary-search impl against ground truth at every index.
bool resolveLogicalIndexLinear(const LineBuffer &lb, int idx, int *blockIdx, int *lineInBlock)
{
    if (idx < 0) {
        return false;
    }
    int rem = idx;
    for (int bi = 0; bi < lb.blockCount(); ++bi) {
        const int n = lb.block(bi).numLines();
        if (rem < n) {
            *blockIdx    = bi;
            *lineInBlock = rem;
            return true;
        }
        rem -= n;
    }
    return false;
}
} // namespace

TEST_CASE("LineBuffer: resolveLogicalIndex matches linear scan across mutation patterns")
{
    LineBuffer lb;
    uint64_t id = 1;

    // Build up to several blocks of mixed line lengths so the prefix array
    // has nontrivial breakpoints.
    for (int batch = 0; batch < 200; ++batch) {
        for (int j = 0; j < 5; ++j) {
            std::string s(1 + ((batch + j) % 50), 'X');
            auto r = row(s);
            lb.appendHardLine(r.data(), static_cast<int>(r.size()), id++, 0, nullptr);
        }
        // Pop one occasionally — exercises truncate path in afterBackBlockMutation.
        if ((batch % 7) == 0) {
            lb.popLastLine();
        }
        // Touch caches with mixed accesses.
        (void)lb.numWrappedRows(11);
        (void)lb.numWrappedRows(31);
    }

    REQUIRE(lb.totalLogicalLines() > 0);
    const int total = lb.totalLogicalLines();
    // Spot-check every index.
    for (int idx = 0; idx < total; ++idx) {
        int b1 = -1, l1 = -1, b2 = -1, l2 = -1;
        bool ok1 = resolveLogicalIndexLinear(lb, idx, &b1, &l1);
        bool ok2 = lb.resolveLogicalIndex(idx, &b2, &l2);
        REQUIRE(ok1 == ok2);
        if (ok1) {
            CHECK_MESSAGE(b1 == b2, "block mismatch at idx=", idx);
            CHECK_MESSAGE(l1 == l2, "line-in-block mismatch at idx=", idx);
        }
    }
    // Out-of-range.
    int b = -1, l = -1;
    CHECK_FALSE(lb.resolveLogicalIndex(total, &b, &l));
    CHECK_FALSE(lb.resolveLogicalIndex(total + 1000, &b, &l));
    CHECK_FALSE(lb.resolveLogicalIndex(-1, &b, &l));
}

TEST_CASE("LineBuffer: resolveLogicalIndex correct after eviction")
{
    // Tight scrollback forces eviction. After eviction the logical cache
    // must be rebuilt and continue to match the linear scan.
    LineBuffer lb(/*maxLogicalLines*/ 80, /*maxTotalCells*/ 0);
    for (int i = 0; i < 5000; ++i) {
        std::string s(1 + (i * 3) % 28, 'A');
        auto r = row(s);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(i + 1), 0, nullptr);
        // Force the logical cache warm by calling resolveLogicalIndex.
        if ((i % 13) == 0 && lb.totalLogicalLines() > 0) {
            int b, l;
            (void)lb.resolveLogicalIndex(0, &b, &l);
            (void)lb.resolveLogicalIndex(lb.totalLogicalLines() - 1, &b, &l);
        }
    }
    // After all that churn, every index should still resolve identically.
    const int total = lb.totalLogicalLines();
    for (int idx = 0; idx < total; ++idx) {
        int b1, l1, b2, l2;
        bool ok1 = resolveLogicalIndexLinear(lb, idx, &b1, &l1);
        bool ok2 = lb.resolveLogicalIndex(idx, &b2, &l2);
        REQUIRE(ok1 == ok2);
        REQUIRE(ok1);
        CHECK(b1 == b2);
        CHECK(l1 == l2);
    }
}

TEST_CASE("LineBuffer: findLine on a line that spans multiple blocks points at the first block")
{
    // Regression: a logical line too long for one block is split across
    // consecutive blocks (each new continuation gets the same lineId).
    // The line-id index must point at the FIRST such block so firstAbsOfLine
    // returns the line's actual first row, not the start of the last
    // continuation.
    LineBuffer lb;
    const uint64_t id = 42;
    // Seed the buffer with a few short lines so the multi-block line isn't
    // the first thing in scrollback. Without this, firstAbsOfLine would
    // happen to return 0 either way.
    for (int i = 0; i < 3; ++i) {
        auto r = row("xx");
        lb.appendHardLine(r.data(), static_cast<int>(r.size()),
                          static_cast<uint64_t>(i + 1), 0, nullptr);
    }
    // Open the long line as a partial soft-wrapped seed.
    auto seed = row("abcdefghij");
    lb.appendLine(seed.data(), static_cast<int>(seed.size()),
                  LineMeta::EolSoft, /*partial*/ true, /*extendsLast*/ false,
                  id, 0, nullptr);
    // Extend it with enough data to force at least one block split. The
    // extension path tolerates up to 2*kCellCapacity (1364) before rejecting,
    // so multiple smaller extensions guarantee a seal-and-restart somewhere.
    std::string chunk(500, 'a');
    auto ext = row(chunk);
    for (int i = 0; i < 6; ++i) {
        lb.appendLine(ext.data(), static_cast<int>(ext.size()),
                      LineMeta::EolSoft, /*partial*/ true, /*extendsLast*/ true,
                      id, 0, nullptr);
    }
    // Seal with a final non-partial extension.
    lb.appendLine(ext.data(), static_cast<int>(ext.size()),
                  LineMeta::EolHard, /*partial*/ false, /*extendsLast*/ true,
                  id, 0, nullptr);

    // Sanity: the line really did span more than one block.
    int blocksWithId = 0;
    for (int bi = 0; bi < lb.blockCount(); ++bi) {
        const auto &b = lb.block(bi);
        for (int li = 0; li < b.numLines(); ++li) {
            if (b.lineId(li) == id) {
                ++blocksWithId;
                break;
            }
        }
    }
    REQUIRE(blocksWithId >= 2);

    auto loc = lb.findLine(id);
    REQUIRE(loc.has_value());
    // Walk back from loc and confirm no earlier block also holds this id;
    // if found, that's the block findLine should have returned.
    for (int bi = 0; bi < loc->blockIdx; ++bi) {
        const auto &b = lb.block(bi);
        for (int li = 0; li < b.numLines(); ++li) {
            CHECK(b.lineId(li) != id);
        }
    }
    // And the indexed block must indeed contain the id.
    const auto &b = lb.block(loc->blockIdx);
    CHECK(b.lineId(loc->externalLineIdx) == id);
}

TEST_CASE("LineBuffer: incremental sum cache stays valid across width queries interleaved with appends")
{
    // Drives the selection-drag scenario: render thread asks numWrappedRows
    // at the current width while the parse worker keeps appending blocks.
    // Each append should perform an incremental extend, not a full rebuild.
    // We can't measure that directly from the public API, but we CAN verify
    // the cache stays correct.
    LineBuffer lb;
    const int width = 19;
    std::string base = "hello world this is a moderately long line to force wrapping";
    for (int i = 0; i < 3000; ++i) {
        auto r = row(base);
        lb.appendHardLine(r.data(), static_cast<int>(r.size()), static_cast<uint64_t>(i + 1), 0, nullptr);
        // Hammer the cache.
        if ((i & 1) == 0) {
            int cached = lb.numWrappedRows(width);
            int fresh  = freshNumWrappedRows(lb, width);
            CHECK_MESSAGE(cached == fresh, "diverged at i=", i);
        }
    }
}

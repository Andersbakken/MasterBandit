#include <doctest/doctest.h>
#include "LineBuffer.h"
#include <string>

namespace {

// Build a Cell with a single ASCII codepoint and default attrs.
Cell c(char ch) { Cell cell; cell.wc = static_cast<char32_t>(ch); return cell; }

// Build a row of cells from a string.
std::vector<Cell> row(const std::string& s) {
    std::vector<Cell> r;
    r.reserve(s.size());
    for (char ch : s) r.push_back(c(ch));
    return r;
}

// Pull the cells of a wrapped row out as a string. Trailing nulls are spaces.
std::string wrappedRowText(const LineBuffer& lb, int wrappedRow, int width) {
    int len = 0;
    const Cell* p = lb.wrappedRowCells(wrappedRow, width, &len);
    if (!p) return {};
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
    lb.appendHardLine(r.data(), static_cast<int>(r.size()), /*lineId*/1, /*flags*/0, nullptr);
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
    lb.appendLine(p1.data(), 6, LineMeta::EolSoft, /*partial*/true, /*extendsLast*/false,
                  /*lineId*/1, 0, nullptr);
    CHECK(lb.totalLogicalLines() == 1);
    CHECK(lb.lastLineIsPartial());

    // Second row continues the same logical line.
    auto p2 = row("GHIJ");
    lb.appendLine(p2.data(), 4, LineMeta::EolHard, /*partial*/false, /*extendsLast*/true,
                  /*lineId*/0 /*ignored*/, 0, nullptr);
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
    LineBuffer lb(3, /*maxCells*/0);
    for (int i = 1; i <= 5; ++i) {
        std::string s(1, static_cast<char>('A' + i - 1));
        auto r = row(s);
        lb.appendHardLine(r.data(), 1, /*lineId*/static_cast<uint64_t>(i), 0, nullptr);
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
    LineBuffer lb(/*maxLines*/100000, /*maxCells*/15);
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
    lb.setOnLineIdEvicted([&](uint64_t id) { evicted.push_back(id); });
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
    lb.appendLine(r.data(), 7, LineMeta::EolSoft, /*partial*/true, false, 1, 0, nullptr);
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
    CHECK(lb.textInRange(0, 1, /*startCol*/2) == "llo\nworld");
    CHECK(lb.textInRange(0, 2, /*startCol*/0, /*endCol*/3) == "hello\nworld\nfoo");
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
    auto r1 = row("AAAA");
    lb.appendLine(r1.data(), 4, LineMeta::EolSoft, /*partial*/true, false, 1, 0, &extrasA);

    // Second row continues, extra at col 1 (= col 5 in the joined line).
    std::unordered_map<int, CellExtra> extrasB;
    extrasB[1].hyperlinkId = 77;
    auto r2 = row("BBBB");
    lb.appendLine(r2.data(), 4, LineMeta::EolHard, /*partial*/false, /*extendsLast*/true,
                  0, 0, &extrasB);

    CHECK(lb.totalLogicalLines() == 1);
    const auto& m = lb.block(0).meta(0);
    REQUIRE(m.extras.count(2));
    CHECK(m.extras.at(2).hyperlinkId == 99);
    REQUIRE(m.extras.count(5));   // 4 (length of first row) + 1
    CHECK(m.extras.at(5).hyperlinkId == 77);
}

TEST_CASE("LineBuffer: invalidateWrapCaches clears MRU")
{
    LineBuffer lb;
    auto r = row("ABCDEFGH");
    lb.appendHardLine(r.data(), 8, 1, 0, nullptr);
    CHECK(lb.numWrappedRows(4) == 2);  // populates cache
    lb.invalidateWrapCaches();
    CHECK(lb.numWrappedRows(4) == 2);  // still correct
}

TEST_CASE("LineBuffer: lineId monotonic resolution after partial eviction")
{
    LineBuffer lb(3, 0);
    for (int i = 1; i <= 10; ++i) {
        auto r = row("X");
        lb.appendHardLine(r.data(), 1, static_cast<uint64_t>(i), 0, nullptr);
    }
    CHECK(lb.totalLogicalLines() == 3);
    CHECK(lb.logicalIndexOfLineId(7) == -1);  // very old, evicted
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

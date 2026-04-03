#pragma once

#include "CellGrid.h"
#include <deque>
#include <string>
#include <vector>

class ScrollbackBuffer {
public:
    ScrollbackBuffer(int cols = 0, int tier1Capacity = 4096,
                     int maxArchiveRows = 100000);

    int cols() const { return cols_; }
    int historySize() const;       // tier1 + tier2 total
    int tier1Size() const { return tier1Size_; }

    // idx 0 = oldest, historySize()-1 = most recent
    const Cell* historyRow(int idx) const;

    void pushHistory(const Cell* row, int numCols,
                     std::unordered_map<int, CellExtra>&& extras = {});
    void resize(int newCols);
    void clearHistory();

    // Get extras for a history row (idx 0 = oldest)
    const std::unordered_map<int, CellExtra>* historyExtras(int idx) const;

private:
    int cols_;
    int tier1Capacity_;            // power-of-2
    int tier1Size_ = 0;
    int ringHead_ = 0;             // next write slot
    std::vector<Cell> ring_;       // tier1Capacity_ * cols_
    std::vector<std::unordered_map<int, CellExtra>> ringExtras_; // parallel to ring slots

    int ringMask() const { return tier1Capacity_ - 1; }
    // Convert tier1 logical index (0=oldest) to physical ring slot
    int tier1ToPhysical(int tier1Idx) const;

    // --- Tier 2: compressed archive ---
    struct ArchivedRow {
        std::string data;          // UTF-8 + inline SGR escapes
    };
    std::deque<ArchivedRow> archive_;
    int maxArchiveRows_;

    static std::string serializeRow(const Cell* cells, int cols);
    mutable std::vector<Cell> parseBuffer_;
    void parseArchivedRow(const ArchivedRow& row) const;

    static int roundUpPow2(int v);
};

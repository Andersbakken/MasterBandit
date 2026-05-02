#pragma once

#include "IGrid.h"
#include <memory>
#include <unordered_map>
#include <vector>

class CellGrid : public IGrid {
public:
    CellGrid();
    CellGrid(int cols, int rows);

    void resize(int cols, int rows);

    int cols() const override { return cols_; }
    int rows() const override { return rows_; }

    Cell& cell(int col, int row) override { return rowVec_[row]->cells[col]; }
    const Cell& cell(int col, int row) const override { return rowVec_[row]->cells[col]; }
    Cell* row(int row) override { return rowVec_[row]->cells.data(); }
    const Cell* row(int row) const override { return rowVec_[row]->cells.data(); }

    void markRowDirty(int row) override;
    void markAllDirty() override;
    void clearDirty(int row) override;
    void clearAllDirty() override;
    bool isRowDirty(int row) const override;
    bool anyDirty() const override;

    void clearRow(int row) override;
    void clearRow(int row, int startCol, int endCol) override;
    void scrollUp(int top, int bottom, int n) override;
    void scrollDown(int top, int bottom, int n) override;
    void deleteChars(int row, int col, int count) override;
    void insertChars(int row, int col, int count) override;

    const CellExtra* getExtra(int col, int row) const override;
    CellExtra& ensureExtra(int col, int row) override;
    void clearExtra(int col, int row) override;
    void clearRowExtras(int row) override;

private:
    struct Row {
        std::vector<Cell> cells;
        // Lazy: nullptr means "no extras on this row", which is the common
        // case. Allocated only when ensureExtra fires. Move/destroy of an
        // empty row is then a single pointer copy instead of allocating
        // and freeing libc++ unordered_map bucket arrays on every scroll.
        std::unique_ptr<std::unordered_map<int, CellExtra>> extras;
        bool dirty = true;
    };

    static std::unique_ptr<Row> makeRow(int cols);
    void clearRowInternal(Row& r, int startCol, int endCol);

    int cols_ = 0, rows_ = 0;
    std::vector<std::unique_ptr<Row>> rowVec_;
    bool allDirty_ = true;
};

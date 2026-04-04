#pragma once

#include "IGrid.h"
#include <vector>

class CellGrid : public IGrid {
public:
    CellGrid();
    CellGrid(int cols, int rows);

    void resize(int cols, int rows);

    int cols() const override { return cols_; }
    int rows() const override { return rows_; }

    Cell& cell(int col, int row) override { return cells_[row * cols_ + col]; }
    const Cell& cell(int col, int row) const override { return cells_[row * cols_ + col]; }
    Cell* row(int row) override { return &cells_[row * cols_]; }
    const Cell* row(int row) const override { return &cells_[row * cols_]; }

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
    int cols_ = 0, rows_ = 0;
    std::vector<Cell> cells_;
    std::vector<bool> dirty_;
    bool allDirty_ = true;
    std::vector<std::unordered_map<int, CellExtra>> extras_;
};

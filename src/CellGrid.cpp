#include "CellGrid.h"
#include <algorithm>
#include <cassert>
#include <cstring>

CellGrid::CellGrid() = default;

CellGrid::CellGrid(int cols, int rows)
    : cols_(cols), rows_(rows)
    , cells_(static_cast<size_t>(cols) * rows)
    , dirty_(rows, true)
    , allDirty_(true)
    , extras_(rows)
{
}

void CellGrid::resize(int cols, int rows)
{
    if (cols == cols_ && rows == rows_) return;

    std::vector<Cell> newCells(static_cast<size_t>(cols) * rows);

    // Copy existing content
    int copyRows = std::min(rows, rows_);
    int copyCols = std::min(cols, cols_);
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            newCells[r * cols + c] = cells_[r * cols_ + c];
        }
    }

    int oldRows = rows_;
    cols_ = cols;
    rows_ = rows;
    cells_ = std::move(newCells);
    dirty_.assign(rows, true);
    allDirty_ = true;
    extras_.resize(rows);
    // Clear only new rows; existing rows keep extras until overwritten
    for (int r = oldRows; r < rows; ++r) {
        extras_[r].clear();
    }
}

void CellGrid::markRowDirty(int row)
{
    if (row >= 0 && row < rows_) dirty_[row] = true;
}

void CellGrid::markAllDirty()
{
    allDirty_ = true;
    std::fill(dirty_.begin(), dirty_.end(), true);
}

void CellGrid::clearDirty(int row)
{
    if (row >= 0 && row < rows_) dirty_[row] = false;
}

void CellGrid::clearAllDirty()
{
    allDirty_ = false;
    std::fill(dirty_.begin(), dirty_.end(), false);
}

bool CellGrid::isRowDirty(int row) const
{
    if (allDirty_) return true;
    if (row >= 0 && row < rows_) return dirty_[row];
    return false;
}

bool CellGrid::anyDirty() const
{
    if (allDirty_) return true;
    for (int r = 0; r < rows_; ++r) {
        if (dirty_[r]) return true;
    }
    return false;
}

void CellGrid::clearRow(int row)
{
    if (row < 0 || row >= rows_) return;
    Cell* r = this->row(row);
    for (int c = 0; c < cols_; ++c) {
        r[c] = Cell{};
    }
    extras_[row].clear();
    markRowDirty(row);
}

void CellGrid::clearRow(int row, int startCol, int endCol)
{
    if (row < 0 || row >= rows_) return;
    startCol = std::max(0, startCol);
    endCol = std::min(cols_, endCol);
    Cell* r = this->row(row);
    for (int c = startCol; c < endCol; ++c) {
        r[c] = Cell{};
    }
    // Clear extras for the range; if full row, clear all
    if (startCol == 0 && endCol == cols_) {
        extras_[row].clear();
    } else {
        for (int c = startCol; c < endCol; ++c) {
            extras_[row].erase(c);
        }
    }
    markRowDirty(row);
}

void CellGrid::scrollUp(int top, int bottom, int n)
{
    if (top < 0 || bottom > rows_ || top >= bottom || n <= 0) return;
    int regionHeight = bottom - top;
    n = std::min(n, regionHeight);

    // Move rows [top+n..bottom) up to [top..bottom-n)
    for (int r = top; r < bottom - n; ++r) {
        memcpy(row(r), row(r + n), cols_ * sizeof(Cell));
        extras_[r] = std::move(extras_[r + n]);
        markRowDirty(r);
    }
    // Clear the bottom n rows
    for (int r = bottom - n; r < bottom; ++r) {
        clearRow(r);
        extras_[r].clear();
    }
}

void CellGrid::scrollDown(int top, int bottom, int n)
{
    if (top < 0 || bottom > rows_ || top >= bottom || n <= 0) return;
    int regionHeight = bottom - top;
    n = std::min(n, regionHeight);

    // Move rows [top..bottom-n) down to [top+n..bottom)
    for (int r = bottom - 1; r >= top + n; --r) {
        memcpy(row(r), row(r - n), cols_ * sizeof(Cell));
        extras_[r] = std::move(extras_[r - n]);
        markRowDirty(r);
    }
    // Clear the top n rows
    for (int r = top; r < top + n; ++r) {
        clearRow(r);
        extras_[r].clear();
    }
}

void CellGrid::deleteChars(int row, int col, int count)
{
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Cell* r = this->row(row);
    // Shift cells left
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        memmove(&r[col], &r[col + count], remaining * sizeof(Cell));
    }
    // Clear vacated cells at end
    for (int c = cols_ - count; c < cols_; ++c) {
        r[c] = Cell{};
    }
    markRowDirty(row);
}

void CellGrid::insertChars(int row, int col, int count)
{
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Cell* r = this->row(row);
    // Shift cells right
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        memmove(&r[col + count], &r[col], remaining * sizeof(Cell));
    }
    // Clear inserted cells
    for (int c = col; c < col + count; ++c) {
        r[c] = Cell{};
    }
    markRowDirty(row);
}

const CellExtra* CellGrid::getExtra(int col, int row) const
{
    if (row < 0 || row >= rows_) return nullptr;
    auto& m = extras_[row];
    auto it = m.find(col);
    return (it != m.end()) ? &it->second : nullptr;
}

CellExtra& CellGrid::ensureExtra(int col, int row)
{
    assert(row >= 0 && row < rows_);
    return extras_[row][col];
}

void CellGrid::clearExtra(int col, int row)
{
    if (row >= 0 && row < rows_) extras_[row].erase(col);
}

void CellGrid::clearRowExtras(int row)
{
    if (row >= 0 && row < rows_) extras_[row].clear();
}

std::unordered_map<int, CellExtra> CellGrid::takeRowExtras(int row)
{
    if (row < 0 || row >= rows_) return {};
    auto result = std::move(extras_[row]);
    extras_[row].clear();
    return result;
}

void CellGrid::setRowExtras(int row, std::unordered_map<int, CellExtra>&& extras)
{
    if (row >= 0 && row < rows_) extras_[row] = std::move(extras);
}

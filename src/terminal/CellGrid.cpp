#include "CellGrid.h"
#include <algorithm>
#include <cassert>
#include <cstring>

CellGrid::CellGrid() = default;

CellGrid::CellGrid(int cols, int rows)
    : cols_(cols), rows_(rows)
    , allDirty_(true)
{
    rowVec_.reserve(rows);
    for (int r = 0; r < rows; ++r) rowVec_.push_back(makeRow(cols));
}

std::unique_ptr<CellGrid::Row> CellGrid::makeRow(int cols)
{
    auto r = std::make_unique<Row>();
    r->cells.assign(static_cast<size_t>(cols), Cell{});
    r->dirty = true;
    return r;
}

void CellGrid::resize(int cols, int rows)
{
    if (cols == cols_ && rows == rows_) return;

    if (cols != cols_) {
        for (auto& r : rowVec_) {
            r->cells.resize(static_cast<size_t>(cols));
        }
    }

    if (rows < rows_) {
        rowVec_.resize(rows);
    } else if (rows > rows_) {
        rowVec_.reserve(rows);
        for (int r = static_cast<int>(rowVec_.size()); r < rows; ++r) {
            rowVec_.push_back(makeRow(cols));
        }
    }

    cols_ = cols;
    rows_ = rows;
    allDirty_ = true;
    for (auto& r : rowVec_) r->dirty = true;
}

void CellGrid::markRowDirty(int row)
{
    if (row >= 0 && row < rows_) rowVec_[row]->dirty = true;
}

void CellGrid::markAllDirty()
{
    allDirty_ = true;
    for (auto& r : rowVec_) r->dirty = true;
}

void CellGrid::clearDirty(int row)
{
    if (row >= 0 && row < rows_) rowVec_[row]->dirty = false;
}

void CellGrid::clearAllDirty()
{
    allDirty_ = false;
    for (auto& r : rowVec_) r->dirty = false;
}

bool CellGrid::isRowDirty(int row) const
{
    if (allDirty_) return true;
    if (row >= 0 && row < rows_) return rowVec_[row]->dirty;
    return false;
}

bool CellGrid::anyDirty() const
{
    if (allDirty_) return true;
    for (auto& r : rowVec_) {
        if (r->dirty) return true;
    }
    return false;
}

void CellGrid::clearRowInternal(Row& r, int startCol, int endCol)
{
    for (int c = startCol; c < endCol; ++c) r.cells[c] = Cell{};
}

void CellGrid::clearRow(int row)
{
    if (row < 0 || row >= rows_) return;
    Row& r = *rowVec_[row];
    clearRowInternal(r, 0, cols_);
    r.extras.reset();
    r.dirty = true;
}

void CellGrid::clearRow(int row, int startCol, int endCol)
{
    if (row < 0 || row >= rows_) return;
    startCol = std::max(0, startCol);
    endCol = std::min(cols_, endCol);
    Row& r = *rowVec_[row];
    clearRowInternal(r, startCol, endCol);
    if (r.extras) {
        if (startCol == 0 && endCol == cols_) {
            r.extras.reset();
        } else {
            for (int c = startCol; c < endCol; ++c) r.extras->erase(c);
            if (r.extras->empty()) r.extras.reset();
        }
    }
    r.dirty = true;
}

void CellGrid::scrollUp(int top, int bottom, int n)
{
    if (top < 0 || bottom > rows_ || top >= bottom || n <= 0) return;
    int regionHeight = bottom - top;
    n = std::min(n, regionHeight);

    // Pointer rotation: O(regionHeight) pointer swaps, no per-cell memcpy.
    // The N rows that scroll out of the top of the region wrap around to
    // the bottom and get cleared in-place below.
    std::rotate(rowVec_.begin() + top,
                rowVec_.begin() + top + n,
                rowVec_.begin() + bottom);

    for (int r = top; r < bottom - n; ++r) {
        rowVec_[r]->dirty = true;
    }
    for (int r = bottom - n; r < bottom; ++r) {
        Row& row = *rowVec_[r];
        clearRowInternal(row, 0, cols_);
        row.extras.reset();
        row.dirty = true;
    }
}

void CellGrid::scrollDown(int top, int bottom, int n)
{
    if (top < 0 || bottom > rows_ || top >= bottom || n <= 0) return;
    int regionHeight = bottom - top;
    n = std::min(n, regionHeight);

    std::rotate(rowVec_.begin() + top,
                rowVec_.begin() + bottom - n,
                rowVec_.begin() + bottom);

    for (int r = top; r < top + n; ++r) {
        Row& row = *rowVec_[r];
        clearRowInternal(row, 0, cols_);
        row.extras.reset();
        row.dirty = true;
    }
    for (int r = top + n; r < bottom; ++r) {
        rowVec_[r]->dirty = true;
    }
}

void CellGrid::deleteChars(int row, int col, int count)
{
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Row& r = *rowVec_[row];
    Cell* cells = r.cells.data();
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        memmove(&cells[col], &cells[col + count], remaining * sizeof(Cell));
    }
    for (int c = cols_ - count; c < cols_; ++c) cells[c] = Cell{};

    if (r.extras && !r.extras->empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto& [c, e] : *r.extras) {
            if (c >= col + count) shifted[c - count] = std::move(e);
            else if (c < col) shifted[c] = std::move(e);
        }
        if (shifted.empty()) r.extras.reset();
        else *r.extras = std::move(shifted);
    }
    r.dirty = true;
}

void CellGrid::insertChars(int row, int col, int count)
{
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    count = std::min(count, cols_ - col);
    Row& r = *rowVec_[row];
    Cell* cells = r.cells.data();
    int remaining = cols_ - col - count;
    if (remaining > 0) {
        memmove(&cells[col + count], &cells[col], remaining * sizeof(Cell));
    }
    for (int c = col; c < col + count; ++c) cells[c] = Cell{};

    if (r.extras && !r.extras->empty()) {
        std::unordered_map<int, CellExtra> shifted;
        for (auto& [c, e] : *r.extras) {
            if (c >= col && c + count < cols_) shifted[c + count] = std::move(e);
            else if (c < col) shifted[c] = std::move(e);
        }
        if (shifted.empty()) r.extras.reset();
        else *r.extras = std::move(shifted);
    }
    r.dirty = true;
}

const CellExtra* CellGrid::getExtra(int col, int row) const
{
    if (row < 0 || row >= rows_) return nullptr;
    const auto& ex = rowVec_[row]->extras;
    if (!ex) return nullptr;
    auto it = ex->find(col);
    return (it != ex->end()) ? &it->second : nullptr;
}

CellExtra& CellGrid::ensureExtra(int col, int row)
{
    assert(row >= 0 && row < rows_);
    auto& ex = rowVec_[row]->extras;
    if (!ex) ex = std::make_unique<std::unordered_map<int, CellExtra>>();
    return (*ex)[col];
}

void CellGrid::clearExtra(int col, int row)
{
    if (row < 0 || row >= rows_) return;
    auto& ex = rowVec_[row]->extras;
    if (!ex) return;
    ex->erase(col);
    if (ex->empty()) ex.reset();
}

void CellGrid::clearRowExtras(int row)
{
    if (row < 0 || row >= rows_) return;
    rowVec_[row]->extras.reset();
}

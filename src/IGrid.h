#pragma once

#include "CellTypes.h"

class IGrid {
public:
    virtual ~IGrid() = default;

    virtual int cols() const = 0;
    virtual int rows() const = 0;

    virtual Cell& cell(int col, int row) = 0;
    virtual const Cell& cell(int col, int row) const = 0;
    virtual Cell* row(int row) = 0;
    virtual const Cell* row(int row) const = 0;

    virtual void markRowDirty(int row) = 0;
    virtual void markAllDirty() = 0;
    virtual void clearDirty(int row) = 0;
    virtual void clearAllDirty() = 0;
    virtual bool isRowDirty(int row) const = 0;
    virtual bool anyDirty() const = 0;

    virtual void clearRow(int row) = 0;
    virtual void clearRow(int row, int startCol, int endCol) = 0;
    virtual void scrollUp(int top, int bottom, int n) = 0;
    virtual void scrollDown(int top, int bottom, int n) = 0;
    virtual void deleteChars(int row, int col, int count) = 0;
    virtual void insertChars(int row, int col, int count) = 0;

    virtual const CellExtra* getExtra(int col, int row) const = 0;
    virtual CellExtra& ensureExtra(int col, int row) = 0;
    virtual void clearExtra(int col, int row) = 0;
    virtual void clearRowExtras(int row) = 0;
};

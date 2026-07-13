#pragma once

#include "CellTypes.h"
#include <cstring>

class IGrid
{
public:
    virtual ~IGrid() = default;

    virtual int cols() const = 0;
    virtual int rows() const = 0;

    virtual Cell &cell(int col, int row)             = 0;
    virtual const Cell &cell(int col, int row) const = 0;
    virtual Cell *row(int row)                       = 0;
    virtual const Cell *row(int row) const           = 0;

    virtual void markRowDirty(int row)     = 0;
    virtual void markAllDirty()            = 0;
    virtual void clearDirty(int row)       = 0;
    virtual void clearAllDirty()           = 0;
    virtual bool isRowDirty(int row) const = 0;
    virtual bool anyDirty() const          = 0;

    virtual void clearRow(int row)                           = 0;
    virtual void clearRow(int row, int startCol, int endCol) = 0;
    virtual void scrollUp(int top, int bottom, int n)        = 0;
    virtual void scrollDown(int top, int bottom, int n)      = 0;
    virtual void deleteChars(int row, int col, int count)    = 0;
    virtual void insertChars(int row, int col, int count)    = 0;

    virtual const CellExtra *getExtra(int col, int row) const = 0;
    virtual CellExtra &ensureExtra(int col, int row)          = 0;
    virtual void clearExtra(int col, int row)                 = 0;
    virtual void clearRowExtras(int row)                      = 0;

    // Per-row soft-wrap continuation flag: true when the row's content
    // autowrapped onto the next row. Selection/copy joins such rows
    // without a newline. Defaults are no-ops so grid implementations
    // that don't track it report "not continued".
    virtual bool isRowContinued(int row) const
    {
        (void)row;
        return false;
    }

    virtual void setRowContinued(int row, bool v)
    {
        (void)row;
        (void)v;
    }

    // Mark that the row contains at least one wide cell or wide-spacer.
    // The reflow path uses this in O(1) instead of scanning each cell to
    // decide whether the row qualifies for the memcpy fast path. Default
    // is a no-op so grid implementations that don't track this metadata
    // (e.g. CellGrid) don't need to opt in.
    virtual void markRowHasWide(int row) { (void)row; }

    // BCE (back-color erase). The emulator sets this to a Cell carrying the
    // current SGR background after every SGR change; erase ops (clearRow,
    // scrollUp/Down's revealed rows, deleteChars/insertChars's trailing fills)
    // fill cells from this template instead of a default-constructed Cell.
    // Default value is Cell{} (no bg) so callers that never call setEraseBlank
    // get pre-BCE behavior.
    void setEraseBlank(const Cell &c) { eraseBlank_ = c; }

    const Cell &eraseBlank() const { return eraseBlank_; }

protected:
    Cell eraseBlank_ {};

    // Hot path: when the erase blank is all-zeros, callers can use memset
    // instead of per-cell assignment. True for the common default-bg case.
    // Relies on Cell being trivially copyable with no implicit padding (the
    // static_assert in CellTypes.h checks sizeof). If a future field gains a
    // non-zero default initializer, this check stays correct but the memset
    // fast path in Document::clearPhysicalRow would zero that field — revisit
    // both sites together.
    bool eraseBlankIsTrivial() const
    {
        const Cell zero {};
        return std::memcmp(&eraseBlank_, &zero, sizeof(Cell)) == 0;
    }
};

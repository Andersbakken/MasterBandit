#pragma once

#include "IGrid.h"
#include <deque>
#include <string>
#include <vector>

class Document : public IGrid {
public:
    Document();
    Document(int cols, int screenHeight, int tier1Capacity = 4096, int maxArchiveRows = 100000);

    // --- IGrid interface (screen-relative coordinates) ---
    int cols() const override { return cols_; }
    int rows() const override { return screenHeight_; }

    Cell& cell(int col, int screenRow) override;
    const Cell& cell(int col, int screenRow) const override;
    Cell* row(int screenRow) override;
    const Cell* row(int screenRow) const override;

    void markRowDirty(int screenRow) override;
    void markAllDirty() override;
    void clearDirty(int screenRow) override;
    void clearAllDirty() override;
    bool isRowDirty(int screenRow) const override;
    bool anyDirty() const override;

    void clearRow(int screenRow) override;
    void clearRow(int screenRow, int startCol, int endCol) override;
    void scrollUp(int top, int bottom, int n) override;
    void scrollDown(int top, int bottom, int n) override;
    void deleteChars(int screenRow, int col, int count) override;
    void insertChars(int screenRow, int col, int count) override;

    const CellExtra* getExtra(int col, int screenRow) const override;
    CellExtra& ensureExtra(int col, int screenRow) override;
    void clearExtra(int col, int screenRow) override;
    void clearRowExtras(int screenRow) override;

    // --- History/scrollback API ---
    int historySize() const;
    int archiveSize() const { return static_cast<int>(archive_.size()); }
    const Cell* historyRow(int idx) const;  // idx 0 = oldest
    const std::unordered_map<int, CellExtra>* historyExtras(int idx) const;
    void clearHistory();

    // --- Viewport support ---
    const Cell* viewportRow(int viewRow, int viewportOffset) const;
    const std::unordered_map<int, CellExtra>* viewportExtras(int viewRow, int viewportOffset) const;

    // --- Resize ---
    struct CursorTrack {
        int srcX, srcY;   // input: cursor position as absolute row index (archive + history + screen)
        int dstX, dstY;   // output: cursor position in destination absolute row index
    };
    void resize(int newCols, int newRows, CursorTrack* cursor = nullptr);

    // --- Per-row continuation flag (soft wrap tracking) ---
    bool isRowContinued(int screenRow) const;
    void setRowContinued(int screenRow, bool v);
    bool isHistoryRowContinued(int idx) const; // idx 0 = oldest (archive + tier-1)

    // --- Per-row prompt kind (OSC 133 shell integration) ---
    enum PromptKind : uint8_t {
        UnknownPrompt = 0,
        PromptStart = 1,      // OSC 133;A — primary prompt
        SecondaryPrompt = 2,  // OSC 133;A;k=s — secondary/continuation prompt
        CommandStart = 3,     // OSC 133;B — command input starts
        OutputStart = 4       // OSC 133;C — command output starts
    };
    PromptKind rowPromptKind(int screenRow) const;
    void setRowPromptKind(int screenRow, PromptKind kind);
    PromptKind historyRowPromptKind(int idx) const; // idx 0 = oldest

private:
    int cols_ = 0;
    int screenHeight_ = 0;

    // Ring buffer: holds history (tier 1) + screen rows
    int ringCapacity_ = 0;   // power of 2
    int ringHead_ = 0;       // physical slot after last screen row
    int historyCount_ = 0;   // tier-1 history rows in ring
    std::vector<Cell> ring_;
    std::vector<std::unordered_map<int, CellExtra>> ringExtras_;
    std::vector<bool> continued_;  // per physical ring slot: row was soft-wrapped
    std::vector<PromptKind> promptKind_;  // per physical ring slot: OSC 133 prompt type

    // Dirty tracking (screen-relative)
    std::vector<bool> dirty_;
    bool allDirty_ = true;

    // Tier 2: compressed archive
    struct ArchivedRow { std::string data; bool continued = false; };
    std::deque<ArchivedRow> archive_;
    int maxArchiveRows_ = 0;
    int tier1Capacity_ = 0;       // max history rows before eviction to archive
    mutable std::vector<Cell> parseBuffer_;

    // Internal helpers
    int ringMask() const { return ringCapacity_ - 1; }
    int screenRowToPhysical(int screenRow) const;
    int historyTier1ToPhysical(int tier1Idx) const;
    void evictToArchive();
    void clearPhysicalRow(int physical);
    void growRing();

    static int roundUpPow2(int v);
    static std::string serializeRow(const Cell* cells, int cols);
    void parseArchivedRow(const ArchivedRow& row) const;
};

#include "TerminalSnapshot.h"

#include <CellGrid.h>
#include <Document.h>
#include <IGrid.h>

#include <algorithm>
#include <cstring>
#include <span>

bool TerminalSnapshot::isCellSelected(int col, int absRow) const
{
    if (!selection.active && !selection.valid) return false;

    int r0 = selection.startAbsRow, c0 = selection.startCol;
    int r1 = selection.endAbsRow,   c1 = selection.endCol;

    if (selection.mode == TerminalEmulator::SelectionMode::Rectangle) {
        int minR = std::min(r0, r1), maxR = std::max(r0, r1);
        int minC = std::min(c0, c1), maxC = std::max(c0, c1);
        return absRow >= minR && absRow <= maxR && col >= minC && col <= maxC;
    }

    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        std::swap(r0, r1);
        std::swap(c0, c1);
    }

    if (absRow < r0 || absRow > r1) return false;
    if (absRow == r0 && absRow == r1) return col >= c0 && col <= c1;
    if (absRow == r0) return col >= c0;
    if (absRow == r1) return col <= c1;
    return true;
}

bool TerminalSnapshot::update(TerminalEmulator& term)
{
    std::lock_guard<std::recursive_mutex> _lk(term.mutex());

    if (term.syncOutputActive()) {
        syncOutputActive = true;
        return false;
    }
    syncOutputActive = false;

    const int newRows = term.height();
    const int newCols = term.width();
    const int newOffset = term.viewportOffset();
    const int newHistory = term.document().historySize();

    const bool structuralChange =
        newRows != lastRows_ || newCols != lastCols_ ||
        newOffset != lastViewportOffset_ || newHistory != lastHistorySize_;

    rows = newRows;
    cols = newCols;
    viewportOffset = newOffset;
    historySize = newHistory;

    cursorX = term.cursorX();
    cursorY = term.cursorY();
    cursorShape = term.cursorShape();
    cursorVisible = term.cursorVisible();
    cursorBlinking = term.cursorBlinking();
    defaults = term.defaultColors();

    // Selection: copy by value. Cheap; re-evaluated every frame.
    selection = term.selection();

    // OSC 133 selected command region. Resolve line ids to current absolute
    // rows under the mutex; a pruned command id clears to nullopt inside
    // pruneCommandRing, so a stale id won't reach us here normally.
    selectedCommand.reset();
    if (auto idOpt = term.selectedCommandId(); idOpt && !term.usingAltScreen()) {
        const Document& dref = term.document();
        for (const auto& r : term.commands()) {
            if (r.id != *idOpt) continue;
            int startAbs = dref.firstAbsOfLine(r.promptStartLineId);
            int endAbs;
            int endCol;
            if (r.complete) {
                // lastAbsOfLine gives the last dst row stamped with this line
                // id — handles soft-wrap where one src spans multiple rows.
                endAbs = dref.lastAbsOfLine(r.outputEndLineId);
                endCol = r.outputEndCol;
                // Shells typically emit D in precmd, i.e. at col 0 of the next
                // prompt row, so outputEndLineId points one row past the
                // actual last output line. Roll it back when that's the case.
                if (endCol == 0 && endAbs > startAbs) {
                    endAbs -= 1;
                    endCol = term.width();
                }
            } else {
                endAbs = newHistory + term.cursorY();
                endCol = term.cursorX();
            }
            if (startAbs >= 0 && endAbs >= 0) {
                selectedCommand = SelectedCommandRegion{
                    startAbs,
                    std::max(0, r.promptStartCol),
                    endAbs,
                    endCol,
                };
            }
            break;
        }
    }

    const size_t cellCount = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    cells.resize(cellCount);
    rowDirty.assign(static_cast<size_t>(rows), 0);
    rowExtras.resize(static_cast<size_t>(rows));

    IGrid& grid = term.grid();
    const Document& doc = term.document();
    const bool onAltScreen = (&grid != static_cast<const IGrid*>(&doc));

    for (int r = 0; r < rows; ++r) {
        const bool rowIsDirty = structuralChange || grid.isRowDirty(r);
        if (!rowIsDirty) continue;

        Cell* dst = cells.data() + static_cast<size_t>(r) * static_cast<size_t>(cols);
        std::span<Cell> dstSpan{dst, static_cast<size_t>(cols)};
        if (!term.copyViewportRow(r, dstSpan)) {
            std::memset(dst, 0, sizeof(Cell) * static_cast<size_t>(cols));
        }

        RowExtras& re = rowExtras[static_cast<size_t>(r)];
        re.entries.clear();
        if (onAltScreen) {
            for (int c = 0; c < cols; ++c) {
                if (const CellExtra* ex = grid.getExtra(c, r)) {
                    re.entries.emplace_back(c, *ex);
                }
            }
        } else if (const auto* exMap = doc.viewportExtras(r, viewportOffset)) {
            re.entries.reserve(exMap->size());
            for (const auto& kv : *exMap) {
                re.entries.emplace_back(kv.first, kv.second);
            }
            std::sort(re.entries.begin(), re.entries.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        }

        rowDirty[static_cast<size_t>(r)] = 1;
    }

    // Clear per-row dirty flags on the live grid. The render thread has now
    // captured everything it needs for these rows; subsequent mutations will
    // re-mark them.
    grid.clearAllDirty();

    // Image view capture. Collect imageIds referenced by visible extras, plus
    // any images with active animations (scheduling the next wake-up needs
    // their gap/frameShownAt even when off-screen).
    images.clear();
    const auto& liveRegistry = term.imageRegistry();
    auto captureView = [&](uint32_t imageId) {
        if (images.count(imageId)) return;
        auto it = liveRegistry.find(imageId);
        if (it == liveRegistry.end() || !it->second) return;
        const auto& img = *it->second;
        ImageView view;
        view.entry = it->second;  // shared_ptr copy — keeps image alive past parser delete
        view.pixelWidth  = img.pixelWidth;
        view.pixelHeight = img.pixelHeight;
        view.cellWidth   = img.cellWidth;
        view.cellHeight  = img.cellHeight;
        view.cropX = img.cropX; view.cropY = img.cropY;
        view.cropW = img.cropW; view.cropH = img.cropH;
        view.currentFrameIndex = img.currentFrameIndex;
        view.totalFrames = 1u + static_cast<uint32_t>(img.extraFrames.size());
        view.frameGeneration = img.frameGeneration;
        view.currentFrameGap = img.currentFrameGap();
        view.frameShownAt = img.frameShownAt;
        view.hasAnimation = img.hasAnimation();
        const auto& frame = img.currentFrameRGBA();
        view.currentFrameRGBA = frame.data();
        view.currentFrameRGBASize = frame.size();
        view.placements = img.placements;  // copy — render iteration must not race with parser mutations
        images.emplace(imageId, std::move(view));
    };
    for (const auto& re : rowExtras) {
        for (const auto& [col, ex] : re.entries) {
            (void)col;
            if (ex.imageId) captureView(ex.imageId);
        }
    }
    for (const auto& [id, imgPtr] : liveRegistry) {
        if (imgPtr && imgPtr->hasAnimation()) captureView(id);
    }

    lastRows_ = rows;
    lastCols_ = cols;
    lastViewportOffset_ = viewportOffset;
    lastHistorySize_ = historySize;

    ++version;
    return true;
}

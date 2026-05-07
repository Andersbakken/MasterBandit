#include "TerminalSnapshot.h"

#include <CellGrid.h>
#include <Document.h>
#include <IGrid.h>

#include <algorithm>
#include <cstring>
#include <span>

const TerminalSnapshot::Segment *TerminalSnapshot::segmentAtPixelY(int y, float cellH) const
{
    if (cellH <= 0.0f || segments.empty()) {
        return nullptr;
    }
    // Smooth scroll shifts all segments up by topPixelSubY; adjust y into
    // segment-local space by adding that offset.
    int adjY = y + topPixelSubY;
    if (adjY < 0) {
        return nullptr;
    }
    for (const auto &seg : segments) {
        int startPx = static_cast<int>(static_cast<float>(seg.cellYStart) * cellH);
        int endPx   = static_cast<int>(static_cast<float>(seg.cellYStart + seg.rowCount) * cellH);
        if (adjY >= startPx && adjY < endPx) {
            return &seg;
        }
    }
    return nullptr;
}

bool TerminalSnapshot::update(TerminalEmulator &term)
{
    std::lock_guard<std::recursive_mutex> _lk(term.mutex());

    // Always capture under the mutex — even while a 2026 sync block is in
    // progress. The renderer-side gate (snap.syncOutputActive && heldTexture)
    // keeps the visible frame frozen; we just keep the snapshot fresh so
    // that whenever the renderer is allowed to paint again, it has the
    // latest grid state ready. The previous behaviour (early-return on
    // syncOutputActive) interacted badly with the live-apply parser model:
    // back-to-back sync blocks could starve the render thread of any
    // window in which to capture, and the held texture would lag the
    // grid by entire screens. See TerminalEmulator::injectData.
    const bool wasSyncActive = syncOutputActive;
    syncOutputActive         = term.syncOutputActive();
    // Sync just ended: force a full re-copy this tick so the renderer
    // repaints with the post-sync state even though the per-row dirty
    // bits may have been consumed during the prior in-sync ticks.
    const bool syncJustEnded = wasSyncActive && !syncOutputActive;

    const int newRows    = term.height();
    const int newCols    = term.width();
    const int newOffset  = term.viewportOffset();
    const int newHistory = term.document().historySize();

    const bool structuralChange =
        syncJustEnded ||
        newRows != lastRows_ || newCols != lastCols_ ||
        newOffset != lastViewportOffset_ || newHistory != lastHistorySize_;

    rows           = newRows;
    cols           = newCols;
    viewportOffset = newOffset;
    historySize    = newHistory;

    cursorX        = term.cursorX();
    cursorY        = term.cursorY();
    cursorShape    = term.cursorShape();
    cursorVisible  = term.cursorVisible();
    cursorBlinking = term.cursorBlinking();
    defaults       = term.defaultColors();

    // Selection / OSC 133 command region used to be populated as separate
    // snapshot fields here. Both now flow through the resolved decoration
    // list built later in this function (see the "Decorations" block);
    // kept the parser-side state on TerminalEmulator (mSelection,
    // mSelectedCommandId) but the snapshot mirror only carries the unified
    // decoration view.

    const size_t cellCount = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    cells.resize(cellCount);
    rowDirty.assign(static_cast<size_t>(rows), 0);
    rowExtras.resize(static_cast<size_t>(rows));

    IGrid &grid            = term.grid();
    const Document &doc    = term.document();
    const bool onAltScreen = (&grid != static_cast<const IGrid *>(&doc));

    for (int r = 0; r < rows; ++r) {
        const bool rowIsDirty = structuralChange || grid.isRowDirty(r);
        if (!rowIsDirty) {
            continue;
        }

        Cell *dst = cells.data() + static_cast<size_t>(r) * static_cast<size_t>(cols);
        std::span<Cell> dstSpan { dst, static_cast<size_t>(cols) };
        if (!term.copyViewportRow(r, dstSpan)) {
            std::memset(dst, 0, sizeof(Cell) * static_cast<size_t>(cols));
        }

        RowExtras &re = rowExtras[static_cast<size_t>(r)];
        re.entries.clear();
        if (onAltScreen) {
            for (int c = 0; c < cols; ++c) {
                if (const CellExtra *ex = grid.getExtra(c, r)) {
                    re.entries.emplace_back(c, *ex);
                }
            }
        } else if (const auto *exMap = doc.viewportExtras(r, viewportOffset)) {
            re.entries.reserve(exMap->size());
            for (const auto &kv : *exMap) {
                re.entries.emplace_back(kv.first, kv.second);
            }
            std::sort(re.entries.begin(), re.entries.end(), [](const auto &a, const auto &b)
                      {
                          return a.first < b.first;
                      });
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
    const auto &liveRegistry = term.imageRegistry();
    auto captureView         = [&](uint32_t imageId)
    {
        if (images.count(imageId)) {
            return;
        }
        auto it = liveRegistry.find(imageId);
        if (it == liveRegistry.end() || !it->second) {
            return;
        }
        const auto &img = *it->second;
        ImageView view;
        view.entry                = it->second; // shared_ptr copy — keeps image alive past parser delete
        view.pixelWidth           = img.pixelWidth;
        view.pixelHeight          = img.pixelHeight;
        view.cellWidth            = img.cellWidth;
        view.cellHeight           = img.cellHeight;
        view.cropX                = img.cropX;
        view.cropY                = img.cropY;
        view.cropW                = img.cropW;
        view.cropH                = img.cropH;
        view.currentFrameIndex    = img.currentFrameIndex;
        view.totalFrames          = 1u + static_cast<uint32_t>(img.extraFrames.size());
        view.frameGeneration      = img.frameGeneration;
        view.currentFrameGap      = img.currentFrameGap();
        view.frameShownAt         = img.frameShownAt;
        view.hasAnimation         = img.hasAnimation();
        const auto &frame         = img.currentFrameRGBA();
        view.currentFrameRGBA     = frame.data();
        view.currentFrameRGBASize = frame.size();
        view.placements           = img.placements; // copy — render iteration must not race with parser mutations
        images.emplace(imageId, std::move(view));
    };
    for (const auto &re : rowExtras) {
        for (const auto &[col, ex] : re.entries) {
            (void)col;
            if (ex.imageId) {
                captureView(ex.imageId);
            }
        }
    }
    for (const auto &[id, imgPtr] : liveRegistry) {
        if (imgPtr && imgPtr->hasAnimation()) {
            captureView(id);
        }
    }

    lastRows_           = rows;
    lastCols_           = cols;
    lastViewportOffset_ = viewportOffset;
    lastHistorySize_    = historySize;

    // ---- Visual-layout segment list ----
    //
    // One Row segment per viewport row, plus an Embedded segment for each
    // embedded whose anchor row is visible in the current viewport. Embedded
    // segments occupy (embRows) cell-units of vertical space and push rows
    // below them down by (embRows - 1). Subclass hook supplies the embedded
    // anchor list — alt-screen is handled by the subclass returning empty.
    segments.clear();
    segments.reserve(static_cast<size_t>(rows));

    const int origin = historySize - viewportOffset; // absRow at viewport top
    // Derived for the segment list (and any consumer that wants the anchor
    // line id without touching Document). Emulator owns the integer offset;
    // line id follows.
    topLineId        = doc.lineIdForAbs(origin);
    topPixelSubY     = 0; // smooth / sub-cell scroll is out of scope for v1

    auto viewAnchors = TerminalEmulator::collectVisibleAnchors(term, viewportOffset, rows);

    int cellY        = 0;
    size_t anchorIdx = 0;
    for (int viewRow = 0; viewRow < rows; ++viewRow) {
        int absRow = origin + viewRow;
        Segment seg;
        seg.absRow     = absRow;
        seg.lineId     = doc.lineIdForAbs(absRow);
        seg.cellYStart = cellY;
        if (anchorIdx < viewAnchors.size() && viewAnchors[anchorIdx].viewRow == viewRow) {
            // Emit an Embedded segment here; the anchor row itself is
            // covered by the embedded, so no Row segment is emitted for it.
            seg.kind     = Segment::Kind::Embedded;
            seg.rowCount = std::max(1, viewAnchors[anchorIdx].rows);
            seg.lineId   = viewAnchors[anchorIdx].lineId;
            segments.push_back(seg);
            cellY += seg.rowCount;
            ++anchorIdx;
        } else {
            seg.kind     = Segment::Kind::Row;
            seg.rowCount = 1;
            segments.push_back(seg);
            cellY += 1;
        }
    }

    // ---- Decorations ----
    //
    // Single resolved overlay list: User decorations from term.decorations()
    // (line-id-anchored, persistent), plus system-kind decorations DERIVED
    // here at snapshot time:
    //   - Selection      — from term.resolveSelection()
    //   - CommandRegion  — from term.selectedCommandId() + commandRecord
    //                       (populated below in the same code path that fills
    //                       `selectedCommand`; see the appendDerived block)
    //   - Hyperlink      — from CellExtra.hyperlinkId runs in visible rows
    //
    // Per-row buckets index into `decorations` and only list entries whose
    // resolved row range covers `segments[r].absRow`.
    decorations.clear();
    decorationsByViewRow.assign(static_cast<size_t>(rows), {});
    auto pushDecoration = [&](ResolvedDecoration r)
    {
        int minR        = std::min(r.startAbsRow, r.endAbsRow);
        int maxR        = std::max(r.startAbsRow, r.endAbsRow);
        bool anyOverlap = false;
        for (const auto &seg : segments) {
            if (seg.absRow >= minR && seg.absRow <= maxR) {
                anyOverlap = true;
                break;
            }
        }
        if (!anyOverlap) {
            return;
        }
        int idx = static_cast<int>(decorations.size());
        decorations.push_back(std::move(r));
        for (size_t i = 0; i < segments.size(); ++i) {
            int absRow = segments[i].absRow;
            if (absRow >= minR && absRow <= maxR) {
                decorationsByViewRow[i].push_back(idx);
            }
        }
    };

    // User-kind decorations from live storage.
    {
        const auto &live = term.decorations();
        for (const auto &dec : live) {
            auto resolved = term.resolveDecoration(dec);
            if (!resolved) {
                continue;
            }
            pushDecoration(std::move(*resolved));
        }
    }

    // CommandRegion — derived from term.selectedCommandId() + the matching
    // CommandRecord. Carries the region only; renderer reads
    // kind=CommandRegion from the decoration list and feeds outline color /
    // dim factor from frameState_ config (so live-reload of those config
    // values doesn't require touching this code path). Suppressed on alt
    // screen.
    if (auto idOpt = term.selectedCommandId(); idOpt && !term.usingAltScreen()) {
        const Document &dref = term.document();
        for (const auto &r : term.commands()) {
            if (r.id != *idOpt) {
                continue;
            }
            int startAbs = dref.firstAbsOfLine(r.promptStartLineId);
            int endAbs;
            int endCol;
            if (r.complete) {
                // lastAbsOfLine gives the last dst row stamped with this
                // line id — handles soft-wrap where one src spans multiple
                // rows.
                endAbs = dref.lastAbsOfLine(r.outputEndLineId);
                endCol = r.outputEndCol;
                // Shells typically emit D in precmd, i.e. at col 0 of the
                // next prompt row, so outputEndLineId points one row past
                // the actual last output line. Roll it back when that's
                // the case.
                if (endCol == 0 && endAbs > startAbs) {
                    endAbs -= 1;
                    endCol = term.width();
                }
            } else {
                endAbs = newHistory + term.cursorY();
                endCol = term.cursorX();
            }
            if (startAbs >= 0 && endAbs >= 0) {
                ResolvedDecoration d;
                d.kind        = DecorationKind::CommandRegion;
                d.startAbsRow = startAbs;
                d.startCol    = std::max(0, r.promptStartCol);
                d.endAbsRow   = endAbs;
                d.endCol      = endCol;
                d.shape       = DecorationShape::Range;
                pushDecoration(std::move(d));
            }
            break;
        }
    }

    // Hyperlink — derive from CellExtra.hyperlinkId runs in visible rows.
    // One decoration per consecutive run of identical nonzero hyperlinkId
    // within a single view row; underline style 3 (dotted), color 0 (use
    // cell fg). Composition is first-writer-wins for underline, so cells
    // already SGR-underlined keep their original style+color.
    for (size_t r = 0; r < rowExtras.size(); ++r) {
        const auto &re = rowExtras[r];
        if (re.entries.empty()) {
            continue;
        }
        int absRow      = segments[r].absRow;
        uint32_t runId  = 0;
        int runStartCol = 0;
        int runEndCol   = -1;
        auto flushRun   = [&]()
        {
            if (runId == 0) {
                return;
            }
            ResolvedDecoration d;
            d.kind            = DecorationKind::Hyperlink;
            d.startAbsRow     = absRow;
            d.endAbsRow       = absRow;
            d.startCol        = runStartCol;
            d.endCol          = runEndCol;
            d.shape           = DecorationShape::Range;
            d.style.underline = UnderlineSpec { 3, 0 };
            pushDecoration(std::move(d));
            runId = 0;
        };
        for (const auto &kv : re.entries) {
            int col             = kv.first;
            const CellExtra &ex = kv.second;
            uint32_t hid        = ex.hyperlinkId;
            if (hid == 0) {
                flushRun();
                continue;
            }
            if (runId == hid && col == runEndCol + 1) {
                runEndCol = col;
            } else {
                flushRun();
                runId       = hid;
                runStartCol = col;
                runEndCol   = col;
            }
        }
        flushRun();
    }

    // Selection — derived from the live selection. Constants match the
    // legacy hardcoded selection colors (formerly in RenderEngine.cpp); the
    // composition pass paints them last (highest priority by kind), so any
    // user/hyperlink/command-region decoration underneath is overwritten on
    // selected cells.
    if (auto resOpt = term.resolveSelection()) {
        ResolvedDecoration r;
        r.kind        = DecorationKind::Selection;
        r.startAbsRow = resOpt->startAbsRow;
        r.startCol    = resOpt->startCol;
        r.endAbsRow   = resOpt->endAbsRow;
        r.endCol      = resOpt->endCol;
        r.shape       = resOpt->mode == TerminalEmulator::SelectionMode::Rectangle
                  ? DecorationShape::Rectangle
                  : DecorationShape::Range;
        r.style.fg    = 0xFFFFFFFFu;
        r.style.bg    = 0xFF664422u;
        pushDecoration(std::move(r));
    }

    // Decoration content hash — drives renderer change-detection without a
    // per-pane snapshot copy. FNV-1a over the fields that affect paint:
    // (kind, abs ranges, shape, present-style flags + values).
    {
        decorationsHash = 0xcbf29ce484222325ULL;
        auto mix        = [&](uint64_t v)
        {
            decorationsHash ^= v;
            decorationsHash *= 0x100000001b3ULL;
        };
        for (const auto &d : decorations) {
            mix(static_cast<uint64_t>(d.kind));
            mix(static_cast<uint64_t>(d.shape));
            mix(static_cast<uint64_t>(d.startAbsRow));
            mix(static_cast<uint64_t>(d.startCol));
            mix(static_cast<uint64_t>(d.endAbsRow));
            mix(static_cast<uint64_t>(d.endCol));
            mix(d.style.fg.value_or(0));
            mix(d.style.bg.value_or(0));
            if (d.style.underline) {
                mix(0x100u | static_cast<uint64_t>(d.style.underline->style) | (static_cast<uint64_t>(d.style.underline->color) << 8));
            }
            if (d.style.strikethrough) {
                mix(0x200u | static_cast<uint64_t>(*d.style.strikethrough));
            }
            if (d.style.dimOthers) {
                mix(0x400u | static_cast<uint64_t>(*d.style.dimOthers * 1000.0f));
            }
            mix(static_cast<uint64_t>(d.zPriority));
        }
    }

    ++version;
    return true;
}

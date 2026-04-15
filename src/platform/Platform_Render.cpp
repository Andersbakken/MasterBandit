#include "PlatformDawn.h"
#include "ProceduralGlyphTable.h"
#include "Utf8.h"
#include "Utils.h"
#include "Observability.h"

#include <numeric>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }

void PlatformDawn::releasePopupStates(Pane* pane)
{
    for (const auto& popup : pane->popups()) {
        std::string key = popupStateKey(pane->id(), popup.id);
        auto it = popupRenderStates_.find(key);
        if (it != popupRenderStates_.end()) {
            if (it->second.heldTexture)
                pendingTabBarRelease_.push_back(it->second.heldTexture);
            for (auto* tx : it->second.pendingRelease)
                pendingTabBarRelease_.push_back(tx);
            popupRenderStates_.erase(it);
        }
    }
}

void PlatformDawn::resolveRow(PaneRenderState& rs, int row, FontData* font, float scale,
                              float pixelOriginX, float pixelOriginY)
{
    const TerminalSnapshot& snap = rs.snapshot;
    int cols = snap.cols;
    if (row < 0 || row >= snap.rows || cols <= 0) return;
    int baseIdx = row * cols;
    const Cell* rowData = snap.cells.data() + baseIdx;
    const auto& rowExtraEntries = snap.rowExtras[static_cast<size_t>(row)].entries;

    // Lookup helper — snapshot rowExtras is sorted by column, so binary search.
    auto findExtra = [&](int col) -> const CellExtra* {
        auto it = std::lower_bound(
            rowExtraEntries.begin(), rowExtraEntries.end(), col,
            [](const std::pair<int, CellExtra>& e, int c) { return e.first < c; });
        if (it != rowExtraEntries.end() && it->first == col) return &it->second;
        return nullptr;
    };

    auto& rowCache = rs.rowShapingCache[row];
    rowCache.glyphs.clear();
    rowCache.cellGlyphRanges.assign(cols, {0, 0});
    rowCache.colrDrawCmds.clear();
    rowCache.colrRasterCmds.clear();

    // Pass 1: Resolve per-cell decorations (fg, bg, underline)
    for (int col = 0; col < cols; ++col) {
        ResolvedCell& rc = rs.resolvedCells[baseIdx + col];
        const Cell& cell = rowData[col];

        const auto& dc = snap.defaults;
        uint32_t defFg = static_cast<uint32_t>(dc.fgR) | (static_cast<uint32_t>(dc.fgG) << 8) | (static_cast<uint32_t>(dc.fgB) << 16) | 0xFF000000u;
        uint32_t defBg = (dc.bgR || dc.bgG || dc.bgB)
            ? (static_cast<uint32_t>(dc.bgR) | (static_cast<uint32_t>(dc.bgG) << 8) | (static_cast<uint32_t>(dc.bgB) << 16) | 0xFF000000u)
            : 0x00000000u; // transparent = use clear color
        uint32_t fg = (cell.attrs.fgMode() == CellAttrs::Default) ? defFg : cell.attrs.packFgAsU32();
        uint32_t bg = (cell.attrs.bgMode() == CellAttrs::Default) ? defBg : cell.attrs.packBgAsU32();
        if (cell.attrs.inverse()) {
            // defBg may be 0x00000000 (transparent sentinel for black bg); make it opaque before
            // using it as a foreground color — a zero-alpha fg makes text invisible in the shader.
            uint32_t bgOpaque = (bg == 0u)
                ? (static_cast<uint32_t>(dc.bgR) | (static_cast<uint32_t>(dc.bgG) << 8) | (static_cast<uint32_t>(dc.bgB) << 16) | 0xFF000000u)
                : bg;
            std::swap(fg, bgOpaque);
            bg = bgOpaque;
        }

        uint32_t ulInfo = 0;
        {
            const CellExtra* extra = findExtra(col);
            bool hasUnderline = cell.attrs.underline();
            bool isHyperlink = extra && extra->hyperlinkId;
            if (!hasUnderline && isHyperlink) hasUnderline = true;
            if (hasUnderline) {
                uint8_t style = cell.attrs.underline() ? cell.attrs.underlineStyle() : 3;
                ulInfo = static_cast<uint32_t>(style + 1);
                if (extra && extra->underlineColor) {
                    ulInfo |= (extra->underlineColor & 0x00FFFFFF) << 8;
                }
            }
        }

        rc.glyph_offset = 0;
        rc.glyph_count = 0;
        rc.fg_color = fg;
        rc.bg_color = bg;
        rc.underline_info = ulInfo;
    }

    // Pass 2: Build runs and shape
    // Lazily-resolved replacement glyph (U+FFFD) for codepoints that exist in a
    // font but have no renderable data (e.g. CBDT bitmap-only emoji fonts).
    GlyphInfo replacementGlyph{};
    bool replacementGlyphReady = false;
    auto getReplacementGlyph = [&]() -> const GlyphInfo* {
        if (replacementGlyphReady) return replacementGlyph.is_empty ? nullptr : &replacementGlyph;
        replacementGlyphReady = true;
        const ShapedRun& rep = textSystem_.shapeRun(fontName_, replacementChar_, fontSize_, {});
        if (rep.glyphs.empty()) return nullptr;
        std::shared_lock lock(font->mutex);
        auto it = font->glyphs.find(rep.glyphs[0].glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        replacementGlyph = it->second;
        return &replacementGlyph;
    };

    float cellWidthPx = charWidth_;
    int col = 0;
    while (col < cols) {
        const Cell& cell = rowData[col];

        // Skip empty/spacer cells
        if (cell.wc == 0 || cell.attrs.wideSpacer()) {
            col++;
            continue;
        }

        // Procedural glyph rendering: bypass shaping entirely
        {
            uint32_t tableIdx = ProceduralGlyph::codepointToTableIdx(cell.wc);
            if (tableIdx != ProceduralGlyph::kInvalidIndex && ProceduralGlyph::kTable[tableIdx] != 0) {
                GlyphEntry entry;
                entry.atlas_offset = 0x80000000u | tableIdx;
                entry.ext_min_x = 0;
                entry.ext_min_y = 0;
                entry.ext_max_x = 0;
                entry.ext_max_y = 0;
                entry.upem = 0;
                entry.x_offset = 0;
                entry.y_offset = 0;
                uint32_t glyphIdx = static_cast<uint32_t>(rowCache.glyphs.size());
                rowCache.glyphs.push_back(entry);
                auto& range = rowCache.cellGlyphRanges[col];
                if (range.second == 0) range.first = glyphIdx;
                range.second++;
                col++;
                continue;
            }
        }

        // Determine run-breaking attributes
        bool runBold = cell.attrs.bold();
        bool runItalic = cell.attrs.italic();
        int runStart = col;

        // Extend run while font-affecting attributes match
        int runEnd = col + 1;
        while (runEnd < cols) {
            const Cell& next = rowData[runEnd];
            if (next.wc == 0) break;
            if (next.attrs.wideSpacer()) { runEnd++; continue; } // skip spacers, keep run going
            if (next.attrs.bold() != runBold) break;
            if (next.attrs.italic() != runItalic) break;
            // Don't include procedural glyphs in shaping runs
            if (ProceduralGlyph::codepointToTableIdx(next.wc) != ProceduralGlyph::kInvalidIndex) break;
            runEnd++;
        }

        // Build UTF-8 string and byte-to-cell mapping
        std::string runText;
        runText.reserve(static_cast<size_t>(runEnd - runStart) * 4); // worst case: 4 bytes per codepoint
        std::vector<std::pair<uint32_t, int>> byteToCell; // (byteOffset, cellCol)
        for (int c = runStart; c < runEnd; ++c) {
            if (rowData[c].attrs.wideSpacer()) continue;
            if (rowData[c].wc == 0) continue;
            byteToCell.push_back({static_cast<uint32_t>(runText.size()), c});
            appendUtf8(runText, rowData[c].wc);
            if (const CellExtra* extra = findExtra(c)) {
                for (char32_t cp : extra->combiningCps)
                    appendUtf8(runText, cp);
            }
        }

        if (runText.empty()) {
            col = runEnd;
            continue;
        }

        // Shape the run
        FontStyle runStyle;
        runStyle.bold = runBold;
        runStyle.italic = runItalic;
        const ShapedRun& shaped = textSystem_.shapeRun(fontName_, runText, fontSize_, runStyle, byteToCell);

        // Find contiguous RTL cell ranges for mirroring.
        // Build a map of which byteToCell indices are RTL.
        struct RtlRange { int firstCell, lastCell; }; // inclusive cell columns
        std::vector<RtlRange> rtlRanges;
        {
            int i = 0;
            int n = static_cast<int>(byteToCell.size());
            // Walk through shaped glyphs to identify which byteToCell entries are RTL
            std::vector<bool> cellIsRtl(n, false);
            for (const auto& sg : shaped.glyphs) {
                if (!sg.rtl) continue;
                for (int j = 0; j < n; j++) {
                    if (sg.cluster >= byteToCell[j].first &&
                        (j + 1 >= n || sg.cluster < byteToCell[j + 1].first)) {
                        cellIsRtl[j] = true;
                        break;
                    }
                }
            }
            // Build contiguous ranges of RTL cells
            i = 0;
            while (i < n) {
                if (cellIsRtl[i]) {
                    int start = i;
                    while (i < n && cellIsRtl[i]) i++;
                    rtlRanges.push_back({byteToCell[start].second, byteToCell[i - 1].second});
                } else {
                    i++;
                }
            }
        }

        // Map glyphs to cells via cluster index
        float penX = 0;
        for (const auto& sg : shaped.glyphs) {
            // Find which cell this glyph belongs to via cluster (byte offset)
            int cellCol = -1;
            for (auto it = byteToCell.rbegin(); it != byteToCell.rend(); ++it) {
                if (sg.cluster >= it->first) {
                    cellCol = it->second;
                    break;
                }
            }

            // For RTL glyphs, mirror cell assignment within the RTL range they belong to
            if (sg.rtl && cellCol >= 0) {
                for (const auto& range : rtlRanges) {
                    if (cellCol >= range.firstCell && cellCol <= range.lastCell) {
                        cellCol = range.firstCell + (range.lastCell - cellCol);
                        break;
                    }
                }
            }

            if (cellCol < 0 || cellCol >= cols) {
                penX += sg.xAdvance;
                continue;
            }

            // Look up glyph info in font atlas
            uint64_t glyphId = sg.glyphId;
            if ((glyphId & 0xFFFFFFFF) == 0) {
                penX += sg.xAdvance;
                continue;
            }
            GlyphInfo gi;
            {
                std::shared_lock lock(font->mutex);
                auto git = font->glyphs.find(glyphId);
                if (git == font->glyphs.end() || git->second.is_empty) {
                    lock.unlock();
                    // For printable non-space codepoints, show U+FFFD rather than nothing.
                    // But skip substitution glyphs (ligature/contextual forms) — those
                    // are intentionally empty spacer components whose visual is carried
                    // by another glyph in the same substitution.
                    char32_t wc = (cellCol >= 0 && cellCol < cols) ? rowData[cellCol].wc : 0;
                    bool replaced = false;
                    if (!sg.isSubstitution && wc != 0 && !unicode::isSpace(wc)) {
                        if (const GlyphInfo* rep = getReplacementGlyph()) {
                            gi = *rep;
                            replaced = true;
                        }
                    }
                    if (!replaced) {
                        penX += sg.xAdvance;
                        continue;
                    }
                } else {
                    gi = git->second;
                }
            }

            // COLRv1 emoji: skip Slug path, emit a textured quad instead
            if (gi.is_colr) {
                uint64_t colrKey = glyphId;
                // Determine cell width in pixels (wide chars = 2 cells)
                int cellSpan = 1;
                if (cellCol + 1 < cols && rowData[cellCol + 1].attrs.wideSpacer())
                    cellSpan = 2;
                float cellPxW = static_cast<float>(cellSpan) * cellWidthPx;
                float cellPxH = lineHeight_;

                // Try to acquire a tile in the atlas
                auto result = renderer_.colrAtlas().acquireTile(colrKey, fontSize_);
                if (result.tile) {
                    auto* tile = result.tile;
                    // New tile — need to rasterize
                    const ColrGlyphData* colrData = nullptr;
                    {
                        std::shared_lock lock(font->mutex);
                        auto cit = font->colrGlyphs.find(colrKey);
                        if (cit != font->colrGlyphs.end())
                            colrData = &cit->second;
                    }
                    if (colrData) {
                        Renderer::ColrRasterCmd rcmd;
                        rcmd.data = colrData;
                        rcmd.tile = *tile;
                        // Coordinates in raw design units (same space as Slug atlas data)
                        rcmd.em_origin_x = gi.ext_min_x;
                        rcmd.em_origin_y = gi.ext_min_y;
                        rcmd.em_width = gi.ext_max_x - gi.ext_min_x;
                        rcmd.em_height = gi.ext_max_y - gi.ext_min_y;
                        rowCache.colrRasterCmds.push_back(rcmd);
                    }
                }

                // Always add a draw command (tile may already be cached)
                auto* cached = renderer_.colrAtlas().findTile(colrKey, fontSize_);
                if (cached) {
                    float px = pixelOriginX + static_cast<float>(cellCol) * cellWidthPx;
                    float py = pixelOriginY + static_cast<float>(row) * lineHeight_;

                    Renderer::ColrDrawCmd dcmd;
                    dcmd.x = px;
                    dcmd.y = py;
                    dcmd.w = cellPxW;
                    dcmd.h = cellPxH;
                    dcmd.tile = *cached;
                    rowCache.colrDrawCmds.push_back(dcmd);
                }

                penX += sg.xAdvance;
                continue;
            }

            // Glyph positioning: for substituted glyphs (ligatures, contextual forms),
            // use HarfBuzz advance-based positioning to preserve inter-glyph relationships.
            // For normal glyphs, anchor at cell origin — the terminal grid is authoritative.
            float glyphX, glyphY;
            if (sg.isSubstitution) {
                float cellLocalX = static_cast<float>(cellCol - runStart) * cellWidthPx;
                glyphX = penX + sg.xOffset - cellLocalX;
            } else {
                glyphX = sg.xOffset;
            }
            glyphY = sg.yOffset;

            // Add to row glyph cache
            GlyphEntry entry;
            entry.atlas_offset = gi.atlas_offset;
            entry.ext_min_x = gi.ext_min_x;
            entry.ext_min_y = gi.ext_min_y;
            entry.ext_max_x = gi.ext_max_x;
            entry.ext_max_y = gi.ext_max_y;
            entry.upem = gi.upem;
            // For non-substitution glyphs: if the glyph's right edge overflows the
            // cell boundary, shift it left so it fits — preserving the full shape.
            float adjustedX = glyphX;
            if (!sg.isSubstitution) {
                float upemF = static_cast<float>(gi.upem);
                float extMaxPx = gi.ext_max_x / upemF * fontSize_;
                if (extMaxPx > cellWidthPx) {
                    adjustedX -= (extMaxPx - cellWidthPx);
                }
            }
            entry.x_offset = adjustedX;
            entry.y_offset = glyphY;

            uint32_t glyphIdx = static_cast<uint32_t>(rowCache.glyphs.size());
            rowCache.glyphs.push_back(entry);

            // Track per-cell glyph range
            auto& range = rowCache.cellGlyphRanges[cellCol];
            if (range.second == 0) {
                range.first = glyphIdx;
            }
            range.second++;

            penX += sg.xAdvance;
        }

        col = runEnd;
    }

    rowCache.valid = true;
}


void PlatformDawn::renderFrame()
{
    // Apply any pending surface reconfiguration before acquiring the swapchain
    // image.  configureSurface() must only be called from the render thread.
    if (!isHeadless() && surfaceNeedsReconfigure_.exchange(false, std::memory_order_acq_rel)) {
        uint32_t w, h;
        {
            std::lock_guard<std::mutex> lk(platformMutex_);
            w = fbWidth_;
            h = fbHeight_;
        }
        if (w && h) {
            configureSurface(w, h);
            renderer_.setViewportSize(w, h);
        }
    }

    // Acquire the surface texture before taking platformMutex_.
    // vkAcquireNextImageKHR (called by GetCurrentTexture) uses UINT64_MAX
    // timeout and can stall; keeping it outside the lock means a stall only
    // blocks the render thread, not the main-thread event loop.
    wgpu::SurfaceTexture surfaceTexture;
    wgpu::Texture compositeTarget;
    if (!isHeadless()) {
        surface_.GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.status == wgpu::SurfaceGetCurrentTextureStatus::Outdated) {
            // Surface out of date — reconfigure and retry.
            uint32_t w, h;
            {
                std::lock_guard<std::mutex> lk(platformMutex_);
                w = fbWidth_;
                h = fbHeight_;
            }
            if (w && h) {
                configureSurface(w, h);
                renderer_.setViewportSize(w, h);
            }
            surface_.GetCurrentTexture(&surfaceTexture);
        }
        if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
            needsRedraw_ = true;
            return;
        }
        compositeTarget = surfaceTexture.texture;
    }

    std::unique_lock<std::mutex> plk(platformMutex_);

    if (fbWidth_ == 0 || fbHeight_ == 0) return;

    // If the surface texture is smaller than fbWidth_/fbHeight_ (can happen
    // transiently when the main thread updates fb dimensions before the render
    // thread reconfigures the surface), snap fb to the texture size for this
    // frame to prevent out-of-bounds GPU copies.
    if (!isHeadless() && compositeTarget) {
        uint32_t texW = compositeTarget.GetWidth();
        uint32_t texH = compositeTarget.GetHeight();
        if (texW && texH && (texW != fbWidth_ || texH != fbHeight_)) {
            fbWidth_  = texW;
            fbHeight_ = texH;
            renderer_.setViewportSize(texW, texH);
            surfaceNeedsReconfigure_.store(true, std::memory_order_release);
        }
    }

    Tab* currentTab = activeTab();
    if (!currentTab) return;

    needsRedraw_ = false;
    bool focusChanged = focusChanged_.exchange(false);
    renderer_.colrAtlas().advanceGeneration();

    // Flush any pending TIOCSWINSZ — sends SIGWINCH once after all resize
    // events in this frame have been coalesced.
    // During live resize, defer SIGWINCH so the shell doesn't redraw the prompt
    // on every frame — send it once when the resize settles.
    if (!window_ || !window_->inLiveResize()) {
        for (auto& panePtr : currentTab->layout()->panes()) {
            if (auto* t = panePtr->terminal())
                t->flushPendingResize();
        }
    }

    if (isHeadless()) {
        if (!headlessComposite_) {
            wgpu::TextureDescriptor desc = {};
            desc.size = {fbWidth_, fbHeight_, 1};
            desc.format = wgpu::TextureFormat::BGRA8Unorm;
            desc.usage = wgpu::TextureUsage::RenderAttachment
                       | wgpu::TextureUsage::CopySrc
                       | wgpu::TextureUsage::CopyDst;
            headlessComposite_ = device_.CreateTexture(&desc);
        }
        compositeTarget = headlessComposite_;
    }

    FontData* font = const_cast<FontData*>(textSystem_.getFont(fontName_));
    if (!font) { spdlog::error("renderFrame: font '{}' not found", fontName_); return; }

    float scale = fontSize_ / font->baseSize;

    // Drain deferred releases from GPU completion callbacks (thread-safe)
    {
        auto& state = *deferredReleaseState_;
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto* t : state.textures) texturePool_.release(t);
        state.textures.clear();
        for (auto* cs : state.compute) renderer_.computePool().release(cs);
        state.compute.clear();
    }

    std::vector<Renderer::CompositeEntry> compositeEntries;
    bool pngNeeded = debugIPC_ && debugIPC_->pngScreenshotPending();

    // If the focused pane changed, mark both old and new pane dirty so the
    // cursor switches between solid and hollow without waiting for other content.
    int currentFocusedPaneId = currentTab->layout()->focusedPaneId();
    if (currentFocusedPaneId != lastFocusedPaneId_) {
        auto markDirty = [&](int id) {
            auto it = paneRenderStates_.find(id);
            if (it != paneRenderStates_.end()) it->second.dirty = true;
        };
        markDirty(lastFocusedPaneId_);
        markDirty(currentFocusedPaneId);
        lastFocusedPaneId_ = currentFocusedPaneId;
    } else if (focusChanged) {
        // Window gained/lost OS focus — the focused pane ID hasn't changed
        // but the cursor style (solid vs hollow) and tint depend on
        // windowHasFocus_, so mark the focused pane dirty.
        auto it = paneRenderStates_.find(currentFocusedPaneId);
        if (it != paneRenderStates_.end()) it->second.dirty = true;
    }

    // If an overlay is active, render it as a full-screen terminal instead of panes
    struct RenderTarget {
        TerminalEmulator* term;
        PaneRenderState* rs;
        PaneRect rect;
        bool isFocused;
        Pane* pane = nullptr;       // null for overlays and popups
        bool isPopup = false;
        float pixelOriginX = 0.0f; // for resolveRow (0,0 for popups — texture-local)
        float pixelOriginY = 0.0f;
        int popupParentPaneId = -1;
        std::string popupId;
    };
    std::vector<RenderTarget> renderTargets;

    if (currentTab->hasOverlay()) {
        Terminal* overlay = currentTab->topOverlay();
        if (overlay) {
            if (!window_ || !window_->inLiveResize())
                overlay->flushPendingResize();

            // Use layout content area (excludes tab bar)
            PaneRect tbRect = currentTab->layout()->tabBarRect(fbWidth_, fbHeight_);
            PaneRect fullRect { 0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_) };
            if (!tbRect.isEmpty()) {
                if (tabBarConfig_.position == "top") {
                    fullRect.y = tbRect.h;
                    fullRect.h -= tbRect.h;
                } else {
                    fullRect.h -= tbRect.h;
                }
            }
            // Resize overlay to match content area
            float usableW = std::max(0.0f, static_cast<float>(fullRect.w) - padLeft_ - padRight_);
            float usableH = std::max(0.0f, static_cast<float>(fullRect.h) - padTop_ - padBottom_);
            int wantCols = std::max(1, static_cast<int>(usableW / charWidth_));
            int wantRows = std::max(1, static_cast<int>(usableH / lineHeight_));
            if (overlay->width() != wantCols || overlay->height() != wantRows) {
                overlay->resize(wantCols, wantRows);
            }

            auto& rs = overlayRenderStates_[currentTab];
            int cols = overlay->width();
            int rows = overlay->height();
            if (cols > 0 && rows > 0) {
                size_t needed = static_cast<size_t>(cols) * rows;
                if (rs.resolvedCells.size() != needed) {
                    rs.resolvedCells.resize(needed);
                    rs.rowShapingCache.resize(rows);
                    rs.dirty = true;
                }
            }
            RenderTarget ot;
            ot.term = overlay; ot.rs = &rs; ot.rect = fullRect; ot.isFocused = true;
            ot.pixelOriginX = padLeft_; ot.pixelOriginY = padTop_;
            renderTargets.push_back(std::move(ot));
        }
    } else {
        for (auto& panePtr : currentTab->layout()->panes()) {
            Pane* pane = panePtr.get();
            if (pane->rect().isEmpty()) continue;
            Terminal* term = pane->terminal();
            if (!term) continue;
            bool focused = (pane->id() == currentTab->layout()->focusedPaneId()) && windowHasFocus_.load(std::memory_order_acquire);
            RenderTarget pt;
            pt.term = term; pt.rs = &paneRenderStates_[pane->id()];
            pt.rect = pane->rect(); pt.isFocused = focused; pt.pane = pane;
            pt.pixelOriginX = padLeft_; pt.pixelOriginY = padTop_;
            renderTargets.push_back(std::move(pt));
        }
        // Add popup targets after all panes (composited on top)
        for (auto& panePtr : currentTab->layout()->panes()) {
            Pane* pane = panePtr.get();
            for (const auto& popup : pane->popups()) {
                if (!popup.terminal) continue;
                TerminalEmulator* pterm = popup.terminal.get();
                std::string key = popupStateKey(pane->id(), popup.id);
                PaneRenderState& prs = popupRenderStates_[key];
                int pcols = pterm->width(), prows = pterm->height();
                size_t needed = static_cast<size_t>(pcols) * prows;
                if (prs.resolvedCells.size() != needed) {
                    prs.resolvedCells.resize(needed);
                    prs.rowShapingCache.resize(prows);
                    prs.dirty = true;
                }
                // Popup texture rect: screen-absolute position, popup-sized
                PaneRect popupRect;
                popupRect.x = pane->rect().x + static_cast<int>(padLeft_) + static_cast<int>(popup.cellX * charWidth_);
                popupRect.y = pane->rect().y + static_cast<int>(padTop_)  + static_cast<int>(popup.cellY * lineHeight_);
                popupRect.w = static_cast<int>(popup.cellW * charWidth_);
                popupRect.h = static_cast<int>(popup.cellH * lineHeight_);
                RenderTarget pop;
                pop.term = pterm; pop.rs = &prs; pop.rect = popupRect;
                pop.isFocused = (pane->focusedPopupId() == popup.id);
                pop.isPopup = true;
                pop.pixelOriginX = 0.0f; pop.pixelOriginY = 0.0f;
                pop.popupParentPaneId = pane->id(); pop.popupId = popup.id;
                renderTargets.push_back(std::move(pop));
            }
        }
    }

    // --- Phase 1: Collect dirty rows across all panes and resolve in parallel ---

    // Per-target state computed on main thread before parallel dispatch
    struct TargetResolveInfo {
        int targetIdx;
        bool needsRender;
        bool cursorMoved;
        int curY;
    };
    std::vector<TargetResolveInfo> resolveInfos;

    // Work items: packed as (targetIdx << 16) | row
    std::vector<uint32_t> allWorkItems;

    bool anyRunningAnimation = false;
    uint64_t nextAnimationDueAt = std::numeric_limits<uint64_t>::max();
    for (int ti = 0; ti < static_cast<int>(renderTargets.size()); ++ti) {
        auto& target = renderTargets[ti];
        TerminalEmulator* term = target.term;
        PaneRenderState& rs = *target.rs;
        if (target.rect.isEmpty()) {
            resolveInfos.push_back({ti, false, false, 0});
            continue;
        }

        // Advance animated kitty images for this terminal. Done unconditionally
        // (not inside the needsRender branch) so animations keep advancing even
        // when there's no other reason to render; the return value feeds into
        // needsRender so we only re-render when a frame actually changed.
        bool animationAdvanced = term->tickAnimations();

        // Capture snapshot. This copies all scalar state and dirty viewport
        // rows, then clears the live grid's dirty bits — after this point,
        // per-row dirtiness is read from snap.rowDirty, not the grid.
        rs.snapshot.update(*term);
        const TerminalSnapshot& snap = rs.snapshot;

        // Animation wake-up scheduling — read through snapshot.
        for (const auto& [id, view] : snap.images) {
            if (view.hasAnimation) {
                anyRunningAnimation = true;
                uint32_t gap = view.currentFrameGap;
                if (gap == 0) gap = 40;
                uint64_t due = view.frameShownAt + gap;
                if (due < nextAnimationDueAt) nextAnimationDueAt = due;
            }
        }

        bool cursorMoved = (snap.cursorX != rs.lastCursorX || snap.cursorY != rs.lastCursorY ||
                            snap.cursorVisible != rs.lastCursorVisible);
        // If cursor shape changed (e.g. RIS, DECSCUSR), reset blink to fully visible
        if (snap.cursorShape != rs.lastCursorShape) {
            cursorBlinkOpacity_.store(1.0f, std::memory_order_release);
            cursorBlinkStep_ = 0;
        }
        rs.lastCursorX = snap.cursorX;
        rs.lastCursorY = snap.cursorY;
        rs.lastCursorVisible = snap.cursorVisible;
        rs.lastCursorShape = snap.cursorShape;

        // Detect blink opacity change for a currently-blinking cursor.
        float blinkOpacity = cursorBlinkOpacity_.load(std::memory_order_acquire);
        bool cursorBlinkChanged = snap.cursorBlinking &&
                                  blinkOpacity != rs.lastCursorBlinkOpacity;
        rs.lastCursorBlinkOpacity = blinkOpacity;

        bool anyRowDirty = false;
        for (uint8_t d : snap.rowDirty) { if (d) { anyRowDirty = true; break; } }

        // Selection change forces re-resolve of affected rows — selection
        // mutations don't mark grid rows dirty, and snapshot.update() would
        // otherwise not flag them. Compared field-by-field rather than via
        // operator== to avoid changing the Selection type.
        const auto& ls = rs.lastSelection;
        const auto& cs = snap.selection;
        bool selectionChanged =
            ls.startCol != cs.startCol || ls.startAbsRow != cs.startAbsRow ||
            ls.endCol   != cs.endCol   || ls.endAbsRow   != cs.endAbsRow   ||
            ls.active   != cs.active   || ls.valid       != cs.valid       ||
            ls.mode     != cs.mode;
        rs.lastSelection = cs;

        bool needsRender = rs.dirty || anyRowDirty || cursorMoved ||
                           cursorBlinkChanged || animationAdvanced ||
                           selectionChanged || !rs.heldTexture;

        if (snap.syncOutputActive && rs.heldTexture)
            needsRender = false;

        resolveInfos.push_back({ti, needsRender, cursorMoved, snap.cursorY});

        if (needsRender || pngNeeded) {
            size_t needed = static_cast<size_t>(snap.cols) * snap.rows;
            if (rs.resolvedCells.size() != needed)
                rs.resolvedCells.resize(needed);

            int vo = snap.viewportOffset;
            int histSize = snap.historySize;
            bool viewportShifted = (vo != rs.lastViewportOffset ||
                                    (vo != 0 && histSize != rs.lastHistorySize));
            rs.lastViewportOffset = vo;
            rs.lastHistorySize = histSize;

            if (static_cast<int>(rs.rowShapingCache.size()) != snap.rows)
                rs.rowShapingCache.resize(snap.rows);

            // Snapshot already marks every row dirty on viewport shift (detected
            // as a structural change), but the `rs.dirty && !anyRowDirty` case
            // — e.g. popup added without grid mutation — still needs a forced
            // full resolve. Selection change also forces full resolve so old
            // and new selected rows both re-render.
            if (viewportShifted || selectionChanged || (rs.dirty && !anyRowDirty)) {
                for (int row = 0; row < snap.rows; ++row)
                    allWorkItems.push_back((static_cast<uint32_t>(ti) << 16) | static_cast<uint32_t>(row));
            } else {
                for (int row = 0; row < snap.rows; ++row) {
                    if (snap.rowDirty[row] || (cursorMoved && row == snap.cursorY))
                        allWorkItems.push_back((static_cast<uint32_t>(ti) << 16) | static_cast<uint32_t>(row));
                }
            }
        }
    }

    // Wake the event loop precisely at the next animation frame boundary
    // instead of spinning at display refresh rate.
    if (anyRunningAnimation && nextAnimationDueAt != std::numeric_limits<uint64_t>::max())
        scheduleAnimationWakeup(nextAnimationDueAt);

    // Parallel resolve across all panes.
    //
    // Release platformMutex_ while shaping runs — this is the CPU-heavy
    // phase that doesn't touch main-thread-shared PaneRenderState fields
    // (heldTexture / pendingRelease are untouched here; resolveRow reads
    // rs.snapshot, which is already populated, and writes
    // rs.rowShapingCache / rs.resolvedCells, which only the render thread
    // ever touches).
    //
    // renderActive_ tells main thread's drainPendingExits() to defer
    // structural erasure while the render is holding raw Tab/Pane
    // pointers captured above. When we clear the flag, we wake the event
    // loop so the next tick drains any deferred exits promptly.
    renderActive_.store(true, std::memory_order_release);
    plk.unlock();

    if (allWorkItems.size() > 4) {
        renderWorkers_.dispatch(allWorkItems, [&](uint32_t packed) {
            uint32_t ti = packed >> 16;
            int row = static_cast<int>(packed & 0xFFFF);
            auto& target = renderTargets[ti];
            resolveRow(*target.rs, row, font, scale,
                       target.pixelOriginX, target.pixelOriginY);
        });
    } else {
        for (uint32_t packed : allWorkItems) {
            uint32_t ti = packed >> 16;
            int row = static_cast<int>(packed & 0xFFFF);
            auto& target = renderTargets[ti];
            resolveRow(*target.rs, row, font, scale,
                       target.pixelOriginX, target.pixelOriginY);
        }
    }

    plk.lock();
    renderActive_.store(false, std::memory_order_release);
    if (eventLoop_) eventLoop_->wakeup();

    // Re-snap after shaping in case the main thread updated fb dimensions
    // during the unlock window.
    if (!isHeadless() && compositeTarget) {
        uint32_t texW = compositeTarget.GetWidth();
        uint32_t texH = compositeTarget.GetHeight();
        if (texW && texH && (texW != fbWidth_ || texH != fbHeight_)) {
            fbWidth_  = texW;
            fbHeight_ = texH;
            renderer_.setViewportSize(texW, texH);
            surfaceNeedsReconfigure_.store(true, std::memory_order_release);
        }
    }

    // (Dirty flags are cleared by TerminalSnapshot::update() under the
    // Terminal mutex — nothing to do here.)

    // --- Phase 2: Per-target GPU upload and rendering ---

    float blinkOpacity = cursorBlinkOpacity_.load(std::memory_order_acquire);
    for (int ti = 0; ti < static_cast<int>(renderTargets.size()); ++ti) {
        auto& target = renderTargets[ti];
        TerminalEmulator* term = target.term;
        PaneRenderState& rs = *target.rs;
        const PaneRect& paneRect = target.rect;
        if (paneRect.isEmpty()) continue;
        auto& info = resolveInfos[ti];
        bool needsRender = info.needsRender;
        bool cursorMoved = info.cursorMoved;
        int curY = info.curY;
        const TerminalSnapshot& snap = rs.snapshot;

        if (needsRender || pngNeeded) {

            // Assemble glyph buffer from per-row caches and set cell glyph_offset/glyph_count
            rs.glyphBuffer.clear();
            for (int row = 0; row < snap.rows; ++row) {
                auto& rowCache = rs.rowShapingCache[row];
                if (!rowCache.valid) continue;
                uint32_t rowGlyphBase = static_cast<uint32_t>(rs.glyphBuffer.size());
                rs.glyphBuffer.insert(rs.glyphBuffer.end(),
                                      rowCache.glyphs.begin(), rowCache.glyphs.end());
                int baseIdx = row * snap.cols;
                for (int col = 0; col < snap.cols; ++col) {
                    auto& range = rowCache.cellGlyphRanges[col];
                    rs.resolvedCells[baseIdx + col].glyph_offset = rowGlyphBase + range.first;
                    rs.resolvedCells[baseIdx + col].glyph_count = range.second;
                }
            }
            rs.totalGlyphs = static_cast<uint32_t>(rs.glyphBuffer.size());

            // Apply selection highlight
            bool selectionVisible = snap.selection.valid || snap.selection.active;
            if (selectionVisible) {
                for (int row = 0; row < snap.rows; ++row) {
                    int absRow = snap.historySize - snap.viewportOffset + row;
                    for (int col = 0; col < snap.cols; ++col) {
                        if (snap.isCellSelected(col, absRow)) {
                            int idx = row * snap.cols + col;
                            rs.resolvedCells[idx].bg_color = 0xFF664422;
                            rs.resolvedCells[idx].fg_color = 0xFFFFFFFF;
                        }
                    }
                }
            }

            // Cursor — passed as UBO params, no cell mutation needed
            bool isFocused = target.isFocused;

            uint32_t totalCells = static_cast<uint32_t>(snap.cols) * snap.rows;
            ComputeState* cs = renderer_.computePool().acquire(totalCells);

            // Ensure glyph buffer and vertex buffers are large enough (grow-only)
            uint32_t glyphCount = std::max(rs.totalGlyphs, 1u); // at least 1 to avoid 0-size buffer
            renderer_.computePool().ensureGlyphCapacity(cs, glyphCount);

            renderer_.uploadResolvedCells(queue_, cs, rs.resolvedCells.data(), totalCells);
            renderer_.uploadGlyphs(queue_, cs, rs.glyphBuffer.data(), rs.totalGlyphs);

            // (Rows affected by selection changes are forced to re-resolve via
            // selectionChanged tracking in the pre-dispatch loop above.)

            renderer_.updateFontAtlas(queue_, fontName_, *font);

            // Collect image draw commands — one per (imageId, placementId) pair,
            // sorted by z-index. imgSplitText tracks where z >= 0 starts.
            std::vector<Renderer::ImageDrawCmd> imageCmds;
            size_t imgSplitText = 0;
            std::unordered_set<uint64_t> seenPlacements; // (imageId << 32) | placementId
            std::unordered_set<uint32_t> seenImageGPU;   // GPU upload dedup
            // Fresh set built this render pass; swapped into rs.lastVisibleImageIds below.
            std::unordered_set<uint32_t> paneVisibleImages;
            float vpW = static_cast<float>(paneRect.w);
            float vpH = static_cast<float>(paneRect.h);

            for (int viewRow = 0; viewRow < snap.rows; ++viewRow) {
                // Collect all image extras in this row (may have multiple
                // placements). Snapshot unifies main-screen, alt-screen, and
                // scrollback extras into one sorted list per visible row.
                struct RowExtra { const CellExtra* ex; int viewRow; };
                std::vector<RowExtra> rowExtras;

                const auto& entries = snap.rowExtras[static_cast<size_t>(viewRow)].entries;
                for (const auto& [col, ce] : entries) {
                    (void)col;
                    if (ce.imageId != 0)
                        rowExtras.push_back({&ce, viewRow});
                }

                for (const auto& re : rowExtras) {
                    const CellExtra* ex = re.ex;
                    uint64_t key = (static_cast<uint64_t>(ex->imageId) << 32) | ex->imagePlacementId;
                    if (seenPlacements.count(key)) continue;
                    seenPlacements.insert(key);

                    auto viewIt = snap.images.find(ex->imageId);
                    if (viewIt == snap.images.end()) continue;
                    const auto& view = viewIt->second;
                    const auto& placements = view.placements;

                    // GPU upload — once per image, not per placement. The
                    // renderer caches one texture per frame index, so animation
                    // playback only re-uploads on the first cycle.
                    if (!seenImageGPU.count(ex->imageId)) {
                        seenImageGPU.insert(ex->imageId);
                        renderer_.useImageFrame(queue_, ex->imageId,
                            view.currentFrameIndex, view.totalFrames,
                            view.frameGeneration,
                            view.currentFrameRGBA, view.pixelWidth, view.pixelHeight);
                        paneVisibleImages.insert(ex->imageId);
                    }

                    // Use per-placement display params if available, else image defaults
                    uint32_t dispCellW = view.cellWidth, dispCellH = view.cellHeight;
                    uint32_t dispCropX = view.cropX, dispCropY = view.cropY;
                    uint32_t dispCropW = view.cropW, dispCropH = view.cropH;
                    float subCellX = 0.0f, subCellY = 0.0f;
                    auto plIt = placements.find(ex->imagePlacementId);
                    if (plIt != placements.end()) {
                        const auto& pl = plIt->second;
                        if (pl.cellWidth > 0)  dispCellW = pl.cellWidth;
                        if (pl.cellHeight > 0) dispCellH = pl.cellHeight;
                        if (pl.cropW > 0 || pl.cropH > 0) {
                            dispCropX = pl.cropX; dispCropY = pl.cropY;
                            dispCropW = pl.cropW; dispCropH = pl.cropH;
                        }
                        subCellX = static_cast<float>(pl.cellXOffset);
                        subCellY = static_cast<float>(pl.cellYOffset);
                    }

                    float imgW = dispCellW > 0
                        ? static_cast<float>(dispCellW) * charWidth_
                        : static_cast<float>(view.pixelWidth);
                    float imgH = dispCellH > 0
                        ? static_cast<float>(dispCellH) * lineHeight_
                        : static_cast<float>(view.pixelHeight);
                    float imgX = padLeft_ + static_cast<float>(ex->imageStartCol) * charWidth_ + subCellX;
                    float imgY = padTop_ + (static_cast<float>(re.viewRow) - ex->imageOffsetRow) * lineHeight_ + subCellY;

                    float x0 = std::max(imgX, 0.0f);
                    float y0 = std::max(imgY, 0.0f);
                    float x1 = std::min(imgX + imgW, vpW);
                    float y1 = std::min(imgY + imgH, vpH);

                    if (x1 <= x0 || y1 <= y0) continue;

                    // Source rect crop: map UVs to the crop sub-rectangle.
                    // GPU texture has a 1px transparent border, so image data
                    // occupies [1, width+1] x [1, height+1] in texel coords.
                    float texW = static_cast<float>(view.pixelWidth + 2);
                    float texH = static_cast<float>(view.pixelHeight + 2);
                    float borderU = 1.0f / texW;
                    float borderV = 1.0f / texH;
                    float imgU = static_cast<float>(view.pixelWidth) / texW;
                    float imgV = static_cast<float>(view.pixelHeight) / texH;
                    float cropU0, cropV0, cropU1, cropV1;
                    if (dispCropW > 0) {
                        cropU0 = borderU + static_cast<float>(dispCropX) / texW;
                        cropU1 = borderU + static_cast<float>(dispCropX + dispCropW) / texW;
                    } else {
                        cropU0 = borderU;
                        cropU1 = borderU + imgU;
                    }
                    if (dispCropH > 0) {
                        cropV0 = borderV + static_cast<float>(dispCropY) / texH;
                        cropV1 = borderV + static_cast<float>(dispCropY + dispCropH) / texH;
                    } else {
                        cropV0 = borderV;
                        cropV1 = borderV + imgV;
                    }

                    Renderer::ImageDrawCmd cmd;
                    cmd.imageId = ex->imageId;
                    cmd.x = x0;
                    cmd.y = y0;
                    cmd.w = x1 - x0;
                    cmd.h = y1 - y0;
                    float fracX0 = (x0 - imgX) / imgW;
                    float fracY0 = (y0 - imgY) / imgH;
                    float fracX1 = (x1 - imgX) / imgW;
                    float fracY1 = (y1 - imgY) / imgH;
                    cmd.u0 = cropU0 + fracX0 * (cropU1 - cropU0);
                    cmd.v0 = cropV0 + fracY0 * (cropV1 - cropV0);
                    cmd.u1 = cropU0 + fracX1 * (cropU1 - cropU0);
                    cmd.v1 = cropV0 + fracY1 * (cropV1 - cropV0);
                    cmd.zIndex = plIt != placements.end() ? plIt->second.zIndex : 0;
                    // Insert sorted by z-index
                    auto pos = std::lower_bound(imageCmds.begin(), imageCmds.end(), cmd,
                        [](const Renderer::ImageDrawCmd& a, const Renderer::ImageDrawCmd& b) {
                            return a.zIndex < b.zIndex;
                        });
                    imageCmds.insert(pos, cmd);
                    if (cmd.zIndex < 0) imgSplitText++;
                }
            }

            TerminalComputeParams params = {};
            params.cols = static_cast<uint32_t>(snap.cols);
            params.rows = static_cast<uint32_t>(snap.rows);
            params.cell_width = charWidth_;
            params.cell_height = lineHeight_;
            params.viewport_w = static_cast<float>(paneRect.w);
            params.viewport_h = static_cast<float>(paneRect.h);
            params.font_ascender = font->ascender * scale;
            params.font_size = fontSize_;
            params.pane_origin_x = target.pixelOriginX;
            params.pane_origin_y = target.pixelOriginY;
            params.max_text_vertices = cs->maxTextVertices;

            auto packCursorColor = [&](const TerminalEmulator::DefaultColors& dc, bool applyBlink) {
                uint8_t alpha = applyBlink
                    ? static_cast<uint8_t>(blinkOpacity * 255.0f)
                    : 0xFF;
                return static_cast<uint32_t>(dc.cursorR)
                     | (static_cast<uint32_t>(dc.cursorG) << 8)
                     | (static_cast<uint32_t>(dc.cursorB) << 16)
                     | (static_cast<uint32_t>(alpha) << 24);
            };

            // Cursor
            params.cursor_type = 0;
            if (target.isPopup) {
                // Popup: render the popup terminal's own cursor in texture-local coords
                if (snap.cursorVisible &&
                    snap.cursorX >= 0 && snap.cursorX < snap.cols &&
                    snap.cursorY >= 0 && snap.cursorY < snap.rows) {
                    params.cursor_col   = static_cast<uint32_t>(snap.cursorX);
                    params.cursor_row   = static_cast<uint32_t>(snap.cursorY);
                    bool blink = isFocused && snap.cursorBlinking;
                    params.cursor_color = packCursorColor(snap.defaults, blink);
                    params.cursor_type  = isFocused ? 1u : 2u;
                }
            } else {
                // Main pane: suppress cursor only if a popup covers the cursor's
                // cell (otherwise the cursor is visually unobstructed even when
                // popups are open elsewhere in the pane).
                bool popupHasFocus = target.pane && target.pane->focusedPopup() != nullptr;
                int cursorViewRow = snap.cursorY + snap.viewportOffset;
                bool cursorCovered = target.pane &&
                    target.pane->isCellCoveredByPopup(snap.cursorX, cursorViewRow);
                if (!cursorCovered &&
                    snap.cursorVisible &&
                    snap.cursorX >= 0 && snap.cursorX < snap.cols &&
                    cursorViewRow >= 0 && cursorViewRow < snap.rows) {
                    params.cursor_col   = static_cast<uint32_t>(snap.cursorX);
                    params.cursor_row   = static_cast<uint32_t>(cursorViewRow);
                    bool blink = isFocused && !popupHasFocus && snap.cursorBlinking;
                    params.cursor_color = packCursorColor(snap.defaults, blink);
                    if (!isFocused || popupHasFocus) {
                        params.cursor_type = 2u;
                    } else {
                        switch (snap.cursorShape) {
                        case TerminalEmulator::CursorBlock:
                        case TerminalEmulator::CursorSteadyBlock:
                            params.cursor_type = 1u; break;
                        case TerminalEmulator::CursorUnderline:
                        case TerminalEmulator::CursorSteadyUnderline:
                            params.cursor_type = 3u; break;
                        case TerminalEmulator::CursorBar:
                        case TerminalEmulator::CursorSteadyBar:
                            params.cursor_type = 4u; break;
                        }
                    }
                }
            }

            PooledTexture* newTexture = texturePool_.acquire(
                static_cast<uint32_t>(paneRect.w),
                static_cast<uint32_t>(paneRect.h));

            // Gather COLR commands from all rows
            std::vector<Renderer::ColrDrawCmd> colrDrawCmds;
            std::vector<Renderer::ColrRasterCmd> colrRasterCmds;
            for (const auto& rc : rs.rowShapingCache) {
                colrDrawCmds.insert(colrDrawCmds.end(), rc.colrDrawCmds.begin(), rc.colrDrawCmds.end());
                colrRasterCmds.insert(colrRasterCmds.end(), rc.colrRasterCmds.begin(), rc.colrRasterCmds.end());
            }

            wgpu::CommandEncoderDescriptor encDesc = {};
            wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);

            // Rasterize any new COLRv1 glyphs before rendering the pane
            if (!colrRasterCmds.empty()) {
                renderer_.rasterizeColrGlyphs(encoder, queue_, fontName_, colrRasterCmds);
            }

            const float* tint = isFocused ? activeTint_ : inactiveTint_;
            renderer_.renderToPane(encoder, queue_, fontName_, params, cs, newTexture->view, tint, imageCmds, imgSplitText);

            // Render COLRv1 emoji quads on top
            if (!colrDrawCmds.empty()) {
                renderer_.renderColrQuads(encoder, queue_, newTexture->view,
                                          params.viewport_w, params.viewport_h,
                                          tint, colrDrawCmds);
            }

            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
            pendingComputeRelease_.push_back(cs);

            if (rs.heldTexture) rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = newTexture;
            rs.lastVisibleImageIds = std::move(paneVisibleImages);
            // We just rendered this pane; clear the re-render flag. If an
            // animation's next frame is due on a later tick, tickAnimations()
            // will set needsRender via its return value then.
            rs.dirty = false;
        }

        if (rs.heldTexture) {
            Renderer::CompositeEntry entry;
            entry.texture = rs.heldTexture->texture;
            entry.srcW = static_cast<uint32_t>(paneRect.w);
            entry.srcH = static_cast<uint32_t>(paneRect.h);
            entry.dstX = static_cast<uint32_t>(paneRect.x);
            entry.dstY = static_cast<uint32_t>(paneRect.y);
            compositeEntries.push_back(entry);
        }
    }

    // Release GPU textures for images that have scrolled out of every pane's
    // viewport across every tab. Each pane/popup caches its last visible image
    // set at phase-2 time; we union those and keep whatever's in any of them.
    // This avoids upload/evict churn on cursor blink, focus change, or tab
    // switch — those render frames don't iterate image extras but the caches
    // still vouch for visibility. Scrolling an image into scrollback updates
    // that pane's cache on its next re-render, dropping the imageId, which is
    // when eviction actually happens.
    std::unordered_set<uint32_t> imagesToRetain;
    for (const auto& [paneId, rs] : paneRenderStates_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    for (const auto& [key, rs] : popupRenderStates_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    for (const auto& [tab, rs] : overlayRenderStates_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    renderer_.retainImagesOnly(imagesToRetain);

    // Render tab bar if dirty or window focus changed (tint update)
    if (tabBarVisible() && (tabBarDirty_ || focusChanged)) {
        renderTabBar();
    }

    // Add tab bar texture to composite entries
    Tab* tabBarTab = activeTab();
    if (tabBarTexture_ && tabBarTab) {
        PaneRect tbRect = tabBarTab->layout()->tabBarRect(fbWidth_, fbHeight_);
        if (!tbRect.isEmpty()) {
            Renderer::CompositeEntry entry;
            entry.texture = tabBarTexture_->texture;
            entry.srcW = static_cast<uint32_t>(tbRect.w);
            entry.srcH = static_cast<uint32_t>(tbRect.h);
            entry.dstX = static_cast<uint32_t>(tbRect.x);
            entry.dstY = static_cast<uint32_t>(tbRect.y);
            compositeEntries.push_back(entry);
        }
    }

    // Composite + submit
    if (!compositeEntries.empty()) {
        wgpu::CommandEncoderDescriptor encDesc = {};
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
        renderer_.composite(encoder, compositeTarget, compositeEntries);

        // Update divider tint for window focus state and draw
        const float* windowTint = windowHasFocus_.load(std::memory_order_acquire)
            ? activeTint_ : inactiveTint_;
        renderer_.updateDividerViewport(queue_, fbWidth_, fbHeight_, windowTint);
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it == paneRenderStates_.end() || !it->second.dividerVB) continue;
            renderer_.drawDivider(encoder, compositeTarget,
                                   fbWidth_, fbHeight_, it->second.dividerVB);
        }

        // Draw popup borders (cached GPU buffers, rebuilt on change)
        if (!currentTab->hasOverlay()) {
            for (auto& panePtr : currentTab->layout()->panes()) {
                Pane* pane = panePtr.get();
                auto rsIt = paneRenderStates_.find(pane->id());
                if (rsIt == paneRenderStates_.end()) continue;
                auto& prs = rsIt->second;

                // Sync cached borders with current popups
                bool bordersChanged = prs.popupBorders.size() != pane->popups().size();
                if (!bordersChanged) {
                    for (size_t i = 0; i < pane->popups().size(); ++i) {
                        const auto& pb = prs.popupBorders[i];
                        const auto& pp = pane->popups()[i];
                        if (pb.popupId != pp.id ||
                            pb.cellX != pp.cellX || pb.cellY != pp.cellY ||
                            pb.cellW != pp.cellW || pb.cellH != pp.cellH) {
                            bordersChanged = true;
                            break;
                        }
                    }
                }

                if (bordersChanged) {
                    prs.popupBorders.clear();
                    const PaneRect& pr = pane->rect();
                    float bw = std::max(1.0f, static_cast<float>(dividerWidth_));

                    for (const auto& popup : pane->popups()) {
                        float px = pr.x + padLeft_ + popup.cellX * charWidth_;
                        float py = pr.y + padTop_ + popup.cellY * lineHeight_;
                        float pw = popup.cellW * charWidth_;
                        float ph = popup.cellH * lineHeight_;

                        PaneRenderState::PopupBorder pb;
                        pb.popupId = popup.id;
                        pb.cellX = popup.cellX;
                        pb.cellY = popup.cellY;
                        pb.cellW = popup.cellW;
                        pb.cellH = popup.cellH;
                        renderer_.updateDividerBuffer(queue_, pb.top,
                            px - bw, py - bw, pw + 2 * bw, bw,
                            dividerR_, dividerG_, dividerB_, dividerA_);
                        renderer_.updateDividerBuffer(queue_, pb.bottom,
                            px - bw, py + ph, pw + 2 * bw, bw,
                            dividerR_, dividerG_, dividerB_, dividerA_);
                        renderer_.updateDividerBuffer(queue_, pb.left,
                            px - bw, py, bw, ph,
                            dividerR_, dividerG_, dividerB_, dividerA_);
                        renderer_.updateDividerBuffer(queue_, pb.right,
                            px + pw, py, bw, ph,
                            dividerR_, dividerG_, dividerB_, dividerA_);
                        prs.popupBorders.push_back(std::move(pb));
                    }
                }

                // Draw cached borders
                for (const auto& pb : prs.popupBorders) {
                    renderer_.drawDivider(encoder, compositeTarget, fbWidth_, fbHeight_, pb.top);
                    renderer_.drawDivider(encoder, compositeTarget, fbWidth_, fbHeight_, pb.bottom);
                    renderer_.drawDivider(encoder, compositeTarget, fbWidth_, fbHeight_, pb.left);
                    renderer_.drawDivider(encoder, compositeTarget, fbWidth_, fbHeight_, pb.right);
                }
            }
        }

        // Draw per-pane progress bars at top of pane
        if (tabBarConfig_.progress_bar) for (auto& panePtr : currentTab->layout()->panes()) {
            int st = panePtr->progressState();
            if (st == 0) continue;
            const PaneRect& pr = panePtr->rect();
            float barHeight = progressBarHeight_;
            float barY = static_cast<float>(pr.y);
            float barX = static_cast<float>(pr.x);
            float barW = static_cast<float>(pr.w);
            float edgeSoft = 40.0f * contentScaleX_;

            Renderer::ProgressBarParams pbp{};
            pbp.h = barHeight;
            pbp.a = 1.0f;

            if (st == 1 || st == 2) {
                // Determinate: fill from left, sharp left edge, gradient right edge
                float pct = std::clamp(static_cast<float>(panePtr->progressPct()) / 100.0f, 0.0f, 1.0f);
                pbp.x = barX;
                pbp.y = barY;
                pbp.w = barW;
                pbp.fillFrac = pct;
                pbp.edgeSoftness = edgeSoft;
                pbp.softLeft = 0.0f;
                pbp.softRight = 1.0f;
                pbp.r = (st == 2) ? 0.8f : progressColorR_;
                pbp.g = (st == 2) ? 0.2f : progressColorG_;
                pbp.b = (st == 2) ? 0.2f : progressColorB_;
                renderer_.drawProgressBar(encoder, queue_, compositeTarget,
                                           fbWidth_, fbHeight_, pbp);
            } else if (st == 3) {
                // Indeterminate: sliding segment with gradient edges
                double now = static_cast<double>(TerminalEmulator::mono()) / 1000.0;
                float t = static_cast<float>(std::fmod(now, 2.0) / 2.0);
                float segFrac = 0.3f;
                float segW = barW * segFrac;
                float overshoot = segW;
                float segX = barX - overshoot + t * (barW + 2.0f * overshoot);
                // Clamp to pane — track which edges are clipped
                float x0 = std::max(segX, barX);
                float x1 = std::min(segX + segW, barX + barW);
                bool clippedLeft = (segX < barX);
                bool clippedRight = (segX + segW > barX + barW);
                if (x1 > x0) {
                    pbp.x = x0;
                    pbp.y = barY;
                    pbp.w = x1 - x0;
                    pbp.fillFrac = 1.0f;
                    pbp.edgeSoftness = edgeSoft;
                    pbp.softLeft = clippedLeft ? 0.0f : 1.0f;
                    pbp.softRight = clippedRight ? 0.0f : 1.0f;
                    pbp.r = progressColorR_;
                    pbp.g = progressColorG_;
                    pbp.b = progressColorB_;
                    renderer_.drawProgressBar(encoder, queue_, compositeTarget,
                                               fbWidth_, fbHeight_, pbp);
                }
            }
        }

        if (pngNeeded) {
            // Resolve target texture based on IPC request
            wgpu::Texture srcTexture = compositeTarget;
            uint32_t srcW = fbWidth_, srcH = fbHeight_;

            const auto& target = debugIPC_->pngTarget();
            if (target.starts_with("pane:")) {
                int paneId = std::stoi(target.substr(5));
                auto it = paneRenderStates_.find(paneId);
                if (it != paneRenderStates_.end() && it->second.heldTexture) {
                    srcTexture = it->second.heldTexture->texture;
                    srcW = it->second.heldTexture->width;
                    srcH = it->second.heldTexture->height;
                }
            } else if (target == "tabbar" && tabBarTexture_) {
                srcTexture = tabBarTexture_->texture;
                srcW = tabBarTexture_->width;
                srcH = tabBarTexture_->height;
            }

            // Apply cell-rect cropping (convert cell coords to pixels)
            uint32_t copyX = 0, copyY = 0, copyW = srcW, copyH = srcH;
            const auto& cellRect = debugIPC_->pngCellRect();
            if (cellRect.valid) {
                copyX = static_cast<uint32_t>(cellRect.x * charWidth_ + padLeft_);
                copyY = static_cast<uint32_t>(cellRect.y * lineHeight_ + padTop_);
                copyW = static_cast<uint32_t>(cellRect.w * charWidth_);
                copyH = static_cast<uint32_t>(cellRect.h * lineHeight_);
                // Clamp to texture bounds
                if (copyX + copyW > srcW) copyW = srcW > copyX ? srcW - copyX : 0;
                if (copyY + copyH > srcH) copyH = srcH > copyY ? srcH - copyY : 0;
            }

            if (copyW == 0 || copyH == 0) {
                // Empty region — send empty response and submit
                debugIPC_->onPngReady("");
                wgpu::CommandBuffer cmds = encoder.Finish();
                queue_.Submit(1, &cmds);
            } else {
                uint32_t bytesPerRow = ((copyW * 4 + 255) / 256) * 256;
                uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * copyH;

                wgpu::BufferDescriptor bufDesc = {};
                bufDesc.size = bufferSize;
                bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
                wgpu::Buffer readbackBuf = device_.CreateBuffer(&bufDesc);

                wgpu::TexelCopyTextureInfo src = {};
                src.texture = srcTexture;
                src.origin = {copyX, copyY, 0};
                wgpu::TexelCopyBufferInfo dst = {};
                dst.buffer = readbackBuf;
                dst.layout.bytesPerRow = bytesPerRow;
                dst.layout.rowsPerImage = copyH;

                wgpu::Extent3D extent = {copyW, copyH, 1};
                encoder.CopyTextureToBuffer(&src, &dst, &extent);

                wgpu::CommandBuffer cmds = encoder.Finish();
                queue_.Submit(1, &cmds);

                DebugIPC* ipc = debugIPC_.get();
                uint32_t w = copyW, h = copyH;
                debugIPC_->markReadbackInProgress();
                ++pendingGpuCallbacks_;

                readbackBuf.MapAsync(wgpu::MapMode::Read, 0, bufferSize,
                    wgpu::CallbackMode::AllowSpontaneous,
                    [readbackBuf, ipc, w, h, bytesPerRow, this](wgpu::MapAsyncStatus status, wgpu::StringView) mutable {
                        --pendingGpuCallbacks_;
                        if (status != wgpu::MapAsyncStatus::Success) return;
                        const uint8_t* mapped = static_cast<const uint8_t*>(
                            readbackBuf.GetConstMappedRange(0, static_cast<size_t>(bytesPerRow) * h));
                        if (!mapped) { readbackBuf.Unmap(); return; }

                        std::vector<uint8_t> rgba(w * h * 4);
                        for (uint32_t row = 0; row < h; ++row) {
                            const uint8_t* s = mapped + row * bytesPerRow;
                            uint8_t* d = rgba.data() + row * w * 4;
                            for (uint32_t col = 0; col < w; ++col) {
                                d[col*4+0] = s[col*4+2];
                                d[col*4+1] = s[col*4+1];
                                d[col*4+2] = s[col*4+0];
                                d[col*4+3] = s[col*4+3];
                            }
                        }
                        readbackBuf.Unmap();

                        std::vector<uint8_t> pngData;
                        stbi_write_png_to_func(
                            [](void* ctx, void* data, int size) {
                                auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
                                vec->insert(vec->end(),
                                            static_cast<uint8_t*>(data),
                                            static_cast<uint8_t*>(data) + size);
                            },
                            &pngData, static_cast<int>(w), static_cast<int>(h), 4,
                            rgba.data(), static_cast<int>(w * 4));

                        std::string b64 = base64::encode(pngData.data(), pngData.size());
                        // MapAsync callback fires on a Dawn/GCD worker
                        // thread. DebugIPC state and libwebsockets' fd
                        // watches are main-thread-only, so post the PNG
                        // delivery back to the main thread.
                        postToMainThread([ipc, b64 = std::move(b64)] {
                            ipc->onPngReady(b64);
                        });
                    });
            }
        } else {
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
        }

        // Collect all pending releases from all panes, popups, and tab bar
        std::vector<PooledTexture*> toRelease;
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it != paneRenderStates_.end()) {
                toRelease.insert(toRelease.end(),
                    it->second.pendingRelease.begin(),
                    it->second.pendingRelease.end());
                it->second.pendingRelease.clear();
            }
            for (const auto& popup : panePtr->popups()) {
                auto pit = popupRenderStates_.find(popupStateKey(panePtr->id(), popup.id));
                if (pit != popupRenderStates_.end()) {
                    toRelease.insert(toRelease.end(),
                        pit->second.pendingRelease.begin(),
                        pit->second.pendingRelease.end());
                    pit->second.pendingRelease.clear();
                }
            }
        }
        toRelease.insert(toRelease.end(), pendingTabBarRelease_.begin(), pendingTabBarRelease_.end());
        pendingTabBarRelease_.clear();
        auto texturesToRelease = toRelease;
        auto computeToRelease  = pendingComputeRelease_;
        pendingComputeRelease_.clear();
        // Capture the deferred-release state by shared_ptr so the callback
        // can safely fire even after ~PlatformDawn has run — Dawn's
        // AllowSpontaneous mode makes no guarantee about callback timing.
        auto state = deferredReleaseState_;
        queue_.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
            [state, texturesToRelease, computeToRelease]
            (wgpu::QueueWorkDoneStatus, wgpu::StringView) mutable {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->textures.insert(state->textures.end(),
                    texturesToRelease.begin(), texturesToRelease.end());
                state->compute.insert(state->compute.end(),
                    computeToRelease.begin(), computeToRelease.end());
            });
    }

    if (!isHeadless()) {
        surface_.Present();
    }
    obs::notifyFrame();
}


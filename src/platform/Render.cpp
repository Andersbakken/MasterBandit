#include "PlatformDawn.h"
#include "Log.h"
#include "Utils.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// Append variant: appends UTF-8 bytes directly to an existing string (no allocation)
static void appendUtf8(std::string& s, uint32_t cp)
{
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Convenience: returns a new string (for non-hot-path callers)


void PlatformDawn::resolveRow(PaneRenderState& rs, TerminalEmulator* term, int row, FontData* font, float scale)
{
    if (!term) return;

    int cols = term->width();
    int baseIdx = row * cols;
    const Cell* rowData = term->viewportRow(row);

    // Ensure row shaping cache is sized
    if (static_cast<int>(rs.rowShapingCache.size()) != term->height())
        rs.rowShapingCache.resize(term->height());

    auto& rowCache = rs.rowShapingCache[row];
    rowCache.glyphs.clear();
    rowCache.cellGlyphRanges.assign(cols, {0, 0});

    // Pass 1: Resolve per-cell decorations (fg, bg, underline)
    for (int col = 0; col < cols; ++col) {
        ResolvedCell& rc = rs.resolvedCells[baseIdx + col];
        const Cell& cell = rowData[col];

        const auto& dc = term->defaultColors();
        uint32_t defFg = static_cast<uint32_t>(dc.fgR) | (static_cast<uint32_t>(dc.fgG) << 8) | (static_cast<uint32_t>(dc.fgB) << 16) | 0xFF000000u;
        uint32_t defBg = (dc.bgR || dc.bgG || dc.bgB)
            ? (static_cast<uint32_t>(dc.bgR) | (static_cast<uint32_t>(dc.bgG) << 8) | (static_cast<uint32_t>(dc.bgB) << 16) | 0xFF000000u)
            : 0x00000000u; // transparent = use clear color
        uint32_t fg = (cell.attrs.fgMode() == CellAttrs::Default) ? defFg : cell.attrs.packFgAsU32();
        uint32_t bg = (cell.attrs.bgMode() == CellAttrs::Default) ? defBg : cell.attrs.packBgAsU32();
        if (cell.attrs.inverse()) std::swap(fg, bg);

        uint32_t ulInfo = 0;
        {
            const CellExtra* extra = term->grid().getExtra(col, row);
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
    float cellWidthPx = charWidth_;
    int col = 0;
    while (col < cols) {
        const Cell& cell = rowData[col];

        // Skip empty/spacer cells
        if (cell.wc == 0 || cell.attrs.wideSpacer()) {
            col++;
            continue;
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
        }

        if (runText.empty()) {
            col = runEnd;
            continue;
        }

        // Shape the run
        int boldHint = runBold ? 1 : 0;
        const ShapedRun& shaped = textSystem_.shapeRun(fontName_, runText, fontSize_, boldHint);

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
            auto git = font->glyphs.find(glyphId);
            if (git == font->glyphs.end() || git->second.is_empty) {
                penX += sg.xAdvance;
                continue;
            }
            const GlyphInfo& gi = git->second;

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
            entry.x_offset = glyphX;
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
    if (fbWidth_ == 0 || fbHeight_ == 0) return;
    Tab* currentTab = activeTab();
    if (!currentTab) return;

    // Flush any pending TIOCSWINSZ — sends SIGWINCH once after all resize
    // events in this frame have been coalesced.
    for (auto& panePtr : currentTab->layout()->panes()) {
        if (auto* t = dynamic_cast<Terminal*>(panePtr->activeTerm()))
            t->flushPendingResize();
    }

    wgpu::SurfaceTexture surfaceTexture;
    surface_.GetCurrentTexture(&surfaceTexture);
    if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        return;
    }

    FontData* font = const_cast<FontData*>(textSystem_.getFont(fontName_));
    if (!font) return;

    float scale = fontSize_ / font->baseSize;

    // Drain deferred releases from GPU completion callbacks (thread-safe)
    {
        std::lock_guard<std::mutex> lock(deferredReleaseMutex_);
        for (auto* t : deferredTextureRelease_) texturePool_.release(t);
        deferredTextureRelease_.clear();
        for (auto* cs : deferredComputeRelease_) renderer_.computePool().release(cs);
        deferredComputeRelease_.clear();
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
    }

    // If an overlay is active, render it as a full-screen terminal instead of panes
    struct RenderTarget {
        TerminalEmulator* term;
        PaneRenderState* rs;
        PaneRect rect;
        bool isFocused;
    };
    std::vector<RenderTarget> renderTargets;

    if (currentTab->hasOverlay()) {
        Terminal* overlay = currentTab->topOverlay();
        if (overlay) {
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
            renderTargets.push_back({overlay, &rs, fullRect, true});
        }
    } else {
        for (auto& panePtr : currentTab->layout()->panes()) {
            Pane* pane = panePtr.get();
            if (pane->rect().isEmpty()) continue;
            TerminalEmulator* term = pane->activeTerm();
            if (!term) continue;
            bool focused = (pane->id() == currentTab->layout()->focusedPaneId());
            renderTargets.push_back({term, &paneRenderStates_[pane->id()], pane->rect(), focused});
        }
    }

    for (auto& target : renderTargets) {
        TerminalEmulator* term = target.term;
        PaneRenderState& rs = *target.rs;
        const PaneRect& paneRect = target.rect;
        if (paneRect.isEmpty()) continue;
        const IGrid& g = term->grid();

        int curX = term->cursorX(), curY = term->cursorY();
        bool cursorMoved = (curX != rs.lastCursorX || curY != rs.lastCursorY ||
                            term->cursorVisible() != rs.lastCursorVisible);
        rs.lastCursorX = curX;
        rs.lastCursorY = curY;
        rs.lastCursorVisible = term->cursorVisible();

        bool needsRender = rs.dirty || g.anyDirty() || cursorMoved || !rs.heldTexture;

        // Synchronized output (Mode 2026): defer rendering until the app
        // disables sync, so intermediate states (clear + redraw) aren't shown.
        if (term->syncOutputActive() && rs.heldTexture) {
            needsRender = false;
        }

        if (needsRender || pngNeeded) {
            // Ensure resolvedCells is sized correctly
            size_t needed = static_cast<size_t>(g.cols()) * g.rows();
            if (rs.resolvedCells.size() != needed)
                rs.resolvedCells.resize(needed);

            {
                int vo = term->viewportOffset();
                int histSize = term->document().historySize();
                // Detect whether the viewport content shifted since last frame:
                // viewport offset changed, or new output grew the history while scrolled.
                bool viewportShifted = (vo != rs.lastViewportOffset ||
                                        (vo != 0 && histSize != rs.lastHistorySize));
                rs.lastViewportOffset = vo;
                rs.lastHistorySize = histSize;

                if (viewportShifted) {
                    for (int row = 0; row < g.rows(); ++row)
                        resolveRow(rs, term, row, font, scale);
                } else {
                    for (int row = 0; row < g.rows(); ++row) {
                        if (g.isRowDirty(row) || (cursorMoved && row == curY))
                            resolveRow(rs, term, row, font, scale);
                    }
                }
            }
            const_cast<IGrid&>(g).clearAllDirty();

            // Assemble glyph buffer from per-row caches and set cell glyph_offset/glyph_count
            rs.glyphBuffer.clear();
            for (int row = 0; row < g.rows(); ++row) {
                auto& rowCache = rs.rowShapingCache[row];
                if (!rowCache.valid) continue;
                uint32_t rowGlyphBase = static_cast<uint32_t>(rs.glyphBuffer.size());
                rs.glyphBuffer.insert(rs.glyphBuffer.end(),
                                      rowCache.glyphs.begin(), rowCache.glyphs.end());
                int baseIdx = row * g.cols();
                for (int col = 0; col < g.cols(); ++col) {
                    auto& range = rowCache.cellGlyphRanges[col];
                    rs.resolvedCells[baseIdx + col].glyph_offset = rowGlyphBase + range.first;
                    rs.resolvedCells[baseIdx + col].glyph_count = range.second;
                }
            }
            rs.totalGlyphs = static_cast<uint32_t>(rs.glyphBuffer.size());

            // Apply selection highlight
            bool selectionVisible = term->hasSelection();
            int histSize = term->document().historySize();
            if (selectionVisible) {
                for (int row = 0; row < g.rows(); ++row) {
                    int absRow = histSize - term->viewportOffset() + row;
                    for (int col = 0; col < g.cols(); ++col) {
                        if (term->isCellSelected(col, absRow)) {
                            int idx = row * g.cols() + col;
                            rs.resolvedCells[idx].bg_color = 0xFF664422;
                            rs.resolvedCells[idx].fg_color = 0xFFFFFFFF;
                        }
                    }
                }
            }

            // Cursor — passed as UBO params, no cell mutation needed
            bool isFocused = target.isFocused;

            uint32_t totalCells = static_cast<uint32_t>(g.cols()) * g.rows();
            ComputeState* cs = renderer_.computePool().acquire(totalCells);

            // Ensure glyph buffer and vertex buffers are large enough (grow-only)
            uint32_t glyphCount = std::max(rs.totalGlyphs, 1u); // at least 1 to avoid 0-size buffer
            renderer_.computePool().ensureGlyphCapacity(cs, glyphCount);

            renderer_.uploadResolvedCells(queue_, cs, rs.resolvedCells.data(), totalCells);
            renderer_.uploadGlyphs(queue_, cs, rs.glyphBuffer.data(), rs.totalGlyphs);

            // Mark selection rows dirty for next frame
            if (selectionVisible) {
                for (int row = 0; row < g.rows(); ++row) {
                    int absRow = histSize - term->viewportOffset() + row;
                    for (int col = 0; col < g.cols(); ++col) {
                        if (term->isCellSelected(col, absRow)) {
                            const_cast<IGrid&>(g).markRowDirty(row);
                            break;
                        }
                    }
                }
            }

            renderer_.updateFontAtlas(queue_, fontName_, *font);

            // Collect image draw commands
            std::vector<Renderer::ImageDrawCmd> imageCmds;
            std::unordered_set<uint32_t> seenImages;
            int vo = term->viewportOffset();
            float vpW = static_cast<float>(paneRect.w);
            float vpH = static_cast<float>(paneRect.h);

            for (int viewRow = 0; viewRow < g.rows(); ++viewRow) {
                int absRow = histSize - vo + viewRow;

                const CellExtra* ex = nullptr;
                if (absRow >= histSize) {
                    int gridRow = absRow - histSize;
                    if (gridRow >= 0 && gridRow < g.rows())
                        ex = g.getExtra(0, gridRow);
                } else {
                    auto* extrasMap = term->document().historyExtras(absRow);
                    if (extrasMap) {
                        auto it = extrasMap->find(0);
                        if (it != extrasMap->end()) ex = &it->second;
                    }
                }

                if (!ex || ex->imageId == 0) continue;
                if (seenImages.count(ex->imageId)) continue;
                seenImages.insert(ex->imageId);

                auto it = term->imageRegistry().find(ex->imageId);
                if (it == term->imageRegistry().end()) continue;
                const auto& img = it->second;

                renderer_.ensureImageGPU(queue_, ex->imageId,
                    img.rgba.data(), img.pixelWidth, img.pixelHeight);

                float imgW = static_cast<float>(img.pixelWidth);
                float imgH = static_cast<float>(img.pixelHeight);
                float imgX = 0.0f;
                float imgY = (static_cast<float>(viewRow) - ex->imageOffsetRow) * lineHeight_;

                float x0 = std::max(imgX, 0.0f);
                float y0 = std::max(imgY, 0.0f);
                float x1 = std::min(imgX + imgW, vpW);
                float y1 = std::min(imgY + imgH, vpH);

                if (x1 <= x0 || y1 <= y0) continue;

                Renderer::ImageDrawCmd cmd;
                cmd.imageId = ex->imageId;
                cmd.x = x0;
                cmd.y = y0;
                cmd.w = x1 - x0;
                cmd.h = y1 - y0;
                cmd.u0 = (x0 - imgX) / imgW;
                cmd.v0 = (y0 - imgY) / imgH;
                cmd.u1 = (x1 - imgX) / imgW;
                cmd.v1 = (y1 - imgY) / imgH;
                imageCmds.push_back(cmd);
            }

            TerminalComputeParams params = {};
            params.cols = static_cast<uint32_t>(g.cols());
            params.rows = static_cast<uint32_t>(g.rows());
            params.cell_width = charWidth_;
            params.cell_height = lineHeight_;
            params.viewport_w = static_cast<float>(paneRect.w);
            params.viewport_h = static_cast<float>(paneRect.h);
            params.font_ascender = font->ascender * scale;
            params.font_size = fontSize_;
            params.pane_origin_x = padLeft_;
            params.pane_origin_y = padTop_;
            params.max_text_vertices = cs->maxTextVertices;

            // Cursor
            params.cursor_type = 0;
            if (term->viewportOffset() == 0 && term->cursorVisible() &&
                term->cursorX() >= 0 && term->cursorX() < g.cols() &&
                term->cursorY() >= 0 && term->cursorY() < g.rows()) {
                params.cursor_col   = static_cast<uint32_t>(term->cursorX());
                params.cursor_row   = static_cast<uint32_t>(term->cursorY());
                params.cursor_color = 0xFFCCCCCCu;
                if (!isFocused) {
                    params.cursor_type = 2u; // hollow for unfocused
                } else {
                    switch (term->cursorShape()) {
                    case TerminalEmulator::CursorBlock:
                    case TerminalEmulator::CursorSteadyBlock:
                        params.cursor_type = 1u; // solid block
                        break;
                    case TerminalEmulator::CursorUnderline:
                    case TerminalEmulator::CursorSteadyUnderline:
                        params.cursor_type = 3u; // underline
                        break;
                    case TerminalEmulator::CursorBar:
                    case TerminalEmulator::CursorSteadyBar:
                        params.cursor_type = 4u; // bar
                        break;
                    }
                }
            }

            PooledTexture* newTexture = texturePool_.acquire(
                static_cast<uint32_t>(paneRect.w),
                static_cast<uint32_t>(paneRect.h));

            wgpu::CommandEncoderDescriptor encDesc = {};
            wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
            const float* tint = isFocused ? activeTint_ : inactiveTint_;
            renderer_.renderToPane(encoder, queue_, fontName_, params, cs, newTexture->view, tint, imageCmds);
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
            pendingComputeRelease_.push_back(cs);

            if (rs.heldTexture) rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = newTexture;
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

    // Render tab bar if dirty
    if (tabBarVisible() && tabBarDirty_) {
        renderTabBar();
    }

    // Add tab bar texture to composite entries
    if (tabBarTexture_) {
        PaneRect tbRect = activeTab()->layout()->tabBarRect(fbWidth_, fbHeight_);
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
        renderer_.composite(encoder, surfaceTexture.texture, compositeEntries);

        // Draw per-pane dividers (pre-built vertex buffers, no allocation)
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it == paneRenderStates_.end() || !it->second.dividerVB) continue;
            renderer_.drawDivider(encoder, surfaceTexture.texture,
                                   fbWidth_, fbHeight_, it->second.dividerVB);
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
                renderer_.drawProgressBar(encoder, queue_, surfaceTexture.texture,
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
                    renderer_.drawProgressBar(encoder, queue_, surfaceTexture.texture,
                                               fbWidth_, fbHeight_, pbp);
                }
            }
        }

        if (pngNeeded) {
            uint32_t bytesPerRow = ((fbWidth_ * 4 + 255) / 256) * 256;
            uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * fbHeight_;

            wgpu::BufferDescriptor bufDesc = {};
            bufDesc.size = bufferSize;
            bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
            wgpu::Buffer readbackBuf = device_.CreateBuffer(&bufDesc);

            wgpu::TexelCopyTextureInfo src = {};
            src.texture = surfaceTexture.texture;
            wgpu::TexelCopyBufferInfo dst = {};
            dst.buffer = readbackBuf;
            dst.layout.bytesPerRow = bytesPerRow;
            dst.layout.rowsPerImage = fbHeight_;

            wgpu::Extent3D extent = {fbWidth_, fbHeight_, 1};
            encoder.CopyTextureToBuffer(&src, &dst, &extent);

            wgpu::CommandBuffer cmds = encoder.Finish();
            queue_.Submit(1, &cmds);

            DebugIPC* ipc = debugIPC_.get();
            uint32_t w = fbWidth_, h = fbHeight_;
            debugIPC_->markReadbackInProgress();

            readbackBuf.MapAsync(wgpu::MapMode::Read, 0, bufferSize,
                wgpu::CallbackMode::AllowSpontaneous,
                [readbackBuf, ipc, w, h, bytesPerRow](wgpu::MapAsyncStatus status, wgpu::StringView) mutable {
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
                    ipc->onPngReady(b64);
                });
        } else {
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
        }

        // Collect all pending releases from all panes and tab bar
        std::vector<PooledTexture*> toRelease;
        for (auto& panePtr : currentTab->layout()->panes()) {
            auto it = paneRenderStates_.find(panePtr->id());
            if (it != paneRenderStates_.end()) {
                toRelease.insert(toRelease.end(),
                    it->second.pendingRelease.begin(),
                    it->second.pendingRelease.end());
                it->second.pendingRelease.clear();
            }
        }
        toRelease.insert(toRelease.end(), pendingTabBarRelease_.begin(), pendingTabBarRelease_.end());
        pendingTabBarRelease_.clear();
        auto texturesToRelease = toRelease;
        auto computeToRelease  = pendingComputeRelease_;
        pendingComputeRelease_.clear();
        auto* mutex = &deferredReleaseMutex_;
        auto* deferredTextures = &deferredTextureRelease_;
        auto* deferredCompute = &deferredComputeRelease_;
        queue_.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
            [mutex, deferredTextures, deferredCompute, texturesToRelease, computeToRelease]
            (wgpu::QueueWorkDoneStatus, wgpu::StringView) mutable {
                std::lock_guard<std::mutex> lock(*mutex);
                deferredTextures->insert(deferredTextures->end(),
                    texturesToRelease.begin(), texturesToRelease.end());
                deferredCompute->insert(deferredCompute->end(),
                    computeToRelease.begin(), computeToRelease.end());
            });
    }

    surface_.Present();
    needsRedraw_ = false;
}


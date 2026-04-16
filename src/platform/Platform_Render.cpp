#include "PlatformDawn.h"
#include "ProceduralGlyphTable.h"
#include "Utf8.h"
#include "Utils.h"
#include "Observability.h"

#include <numeric>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }

void PlatformDawn::resolveRow(PaneRenderPrivate& rs, int row, FontData* font, float scale,
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
        const ShapedRun& rep = textSystem_.shapeRun(frameState_.fontName, replacementChar_, frameState_.fontSize, {});
        if (rep.glyphs.empty()) return nullptr;
        std::shared_lock lock(font->mutex);
        auto it = font->glyphs.find(rep.glyphs[0].glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        replacementGlyph = it->second;
        return &replacementGlyph;
    };

    float cellWidthPx = frameState_.charWidth;
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
        const ShapedRun& shaped = textSystem_.shapeRun(frameState_.fontName, runText, frameState_.fontSize, runStyle, byteToCell);

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
                float cellPxH = frameState_.lineHeight;

                // Try to acquire a tile in the atlas
                auto result = renderer_.colrAtlas().acquireTile(colrKey, frameState_.fontSize);
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
                auto* cached = renderer_.colrAtlas().findTile(colrKey, frameState_.fontSize);
                if (cached) {
                    float px = pixelOriginX + static_cast<float>(cellCol) * cellWidthPx;
                    float py = pixelOriginY + static_cast<float>(row) * frameState_.lineHeight;

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
                float extMaxPx = gi.ext_max_x / upemF * frameState_.fontSize;
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
    // Check for pending surface reconfiguration BEFORE acquiring the swapchain
    // texture. configureSurface() invalidates existing textures, so it must
    // happen before GetCurrentTexture.
    if (!isHeadless()) {
        bool needsReconfigure = false;
        uint32_t w = 0, h = 0;
        {
            std::lock_guard<std::mutex> lk(platformMutex_);
            needsReconfigure = renderState_.surfaceNeedsReconfigure;
            if (needsReconfigure) {
                renderState_.surfaceNeedsReconfigure = false;
                w = fbWidth_;
                h = fbHeight_;
            }
            if (renderState_.viewportSizeChanged) {
                renderer_.setViewportSize(fbWidth_, fbHeight_);
                renderState_.viewportSizeChanged = false;
            }
        }
        if (needsReconfigure && w && h) {
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

    // Snapshot the render state under plk (already held).
    // Take a local copy so we can release the lock and use it for the rest
    // of the frame without racing applyPendingMutations().
    frameState_ = renderState_;

    // Consume one-shot flags from the shared copy so the next frame doesn't
    // re-process them.  frameState_ already has the originals.
    renderState_.focusChanged = false;
    renderState_.mainFontAtlasChanged = false;
    renderState_.tabBarFontAtlasChanged = false;
    renderState_.mainFontRemoved = false;
    renderState_.tabBarFontRemoved = false;
    renderState_.viewportSizeChanged = false;
    renderState_.tabBarDirty = false;
    renderState_.dividersDirty = false;

    // From here on, use frameState_ (the local copy) — the lock will be released.
    bool focusChanged = frameState_.focusChanged;
    bool invalidateAllCaches = frameState_.mainFontAtlasChanged ||
                               frameState_.tabBarFontAtlasChanged ||
                               frameState_.mainFontRemoved ||
                               frameState_.tabBarFontRemoved;

    // Handle font atlas GPU work on the render thread
    if (frameState_.mainFontRemoved) {
        renderer_.removeFontAtlas(frameState_.fontName);
    }
    if (frameState_.mainFontAtlasChanged) {
        const FontData* font2 = textSystem_.getFont(frameState_.fontName);
        if (font2) renderer_.uploadFontAtlas(queue_, frameState_.fontName, *font2);
    }
    if (frameState_.tabBarFontRemoved) {
        renderer_.removeFontAtlas(frameState_.tabBarFontName);
    }
    if (frameState_.tabBarFontAtlasChanged) {
        const FontData* tbFont = textSystem_.getFont(frameState_.tabBarFontName);
        if (tbFont) renderer_.uploadFontAtlas(queue_, frameState_.tabBarFontName, *tbFont);
    }
    if (frameState_.viewportSizeChanged) {
        renderer_.setViewportSize(fbWidth_, fbHeight_);
    }

    // Ensure every pane in the shadow copy has a render-private entry
    for (const auto& op : frameState_.panes) {
        paneRenderPrivate_[op.id]; // default-constructs if missing
    }

    // If the surface texture doesn't match fbWidth_/fbHeight_, use texture
    // dims for this frame only without writing back to shared state.
    uint32_t frameFbW = fbWidth_, frameFbH = fbHeight_;
    if (!isHeadless() && compositeTarget) {
        uint32_t texW = compositeTarget.GetWidth();
        uint32_t texH = compositeTarget.GetHeight();
        if (texW && texH && (texW != frameFbW || texH != frameFbH)) {
            frameFbW = texW;
            frameFbH = texH;
            renderer_.setViewportSize(texW, texH);
        }
    }

    // Invalidate row caches if requested (font change, color reload)
    if (invalidateAllCaches) {
        for (auto& [id, rs] : paneRenderPrivate_) {
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
        for (auto& [key, rs] : popupRenderPrivate_) {
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
        {
            auto& rs = overlayRenderPrivate_;
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
    }

    if (frameState_.panes.empty() && !frameState_.hasOverlay) return;

    needsRedraw_ = false;
    renderer_.colrAtlas().advanceGeneration();

    // Flush any pending TIOCSWINSZ — sends SIGWINCH once after all resize
    // events in this frame have been coalesced.
    if (!frameState_.inLiveResize) {
        for (const auto& rpi : frameState_.panes) {
            if (rpi.term)
                static_cast<Terminal*>(rpi.term)->flushPendingResize();
        }
        if (frameState_.overlay)
            static_cast<Terminal*>(frameState_.overlay)->flushPendingResize();
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

    FontData* font = const_cast<FontData*>(textSystem_.getFont(frameState_.fontName));
    if (!font) { spdlog::error("renderFrame: font '{}' not found", frameState_.fontName); return; }

    float scale = frameState_.fontSize / font->baseSize;

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
    int currentFocusedPaneId = frameState_.focusedPaneId;
    if (currentFocusedPaneId != lastFocusedPaneId_) {
        auto markDirty = [&](int id) {
            auto it = paneRenderPrivate_.find(id);
            if (it != paneRenderPrivate_.end()) it->second.dirty = true;
        };
        markDirty(lastFocusedPaneId_);
        markDirty(currentFocusedPaneId);
        lastFocusedPaneId_ = currentFocusedPaneId;
    } else if (focusChanged) {
        // Window gained/lost OS focus — the focused pane ID hasn't changed
        // but the cursor style (solid vs hollow) and tint depend on
        // windowHasFocus_, so mark the focused pane dirty.
        auto it = paneRenderPrivate_.find(currentFocusedPaneId);
        if (it != paneRenderPrivate_.end()) it->second.dirty = true;
    }

    // If an overlay is active, render it as a full-screen terminal instead of panes
    struct RenderTarget {
        TerminalEmulator* term;
        PaneRenderPrivate* rs;
        PaneRect rect;
        bool isFocused;
        int paneId = -1;            // -1 for overlays
        bool hasPopupFocus = false;  // true if a popup has focus in this pane
        const RenderPaneInfo* paneInfo = nullptr; // for popup coverage check
        bool isPopup = false;
        float pixelOriginX = 0.0f; // for resolveRow (0,0 for popups — texture-local)
        float pixelOriginY = 0.0f;
        int popupParentPaneId = -1;
        std::string popupId;
    };
    std::vector<RenderTarget> renderTargets;

    if (frameState_.hasOverlay && frameState_.overlay) {
        TerminalEmulator* overlay = frameState_.overlay;

        // Use layout content area (excludes tab bar).
        // Overlay resize is done on the main thread in buildRenderFrameState(),
        // so dimensions are already correct here.
        const PaneRect& tbRect = frameState_.tabBarRect;
        PaneRect fullRect { 0, 0, static_cast<int>(fbWidth_), static_cast<int>(fbHeight_) };
        if (!tbRect.isEmpty()) {
            if (frameState_.tabBarPosition == "top") {
                fullRect.y = tbRect.h;
                fullRect.h -= tbRect.h;
            } else {
                fullRect.h -= tbRect.h;
            }
        }

        auto& rs = overlayRenderPrivate_;
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
        ot.pixelOriginX = frameState_.padLeft; ot.pixelOriginY = frameState_.padTop;
        renderTargets.push_back(std::move(ot));
    } else {
        for (const auto& rpi : frameState_.panes) {
            if (rpi.rect.isEmpty()) continue;
            if (!rpi.term) continue;
            bool focused = (rpi.id == frameState_.focusedPaneId) && frameState_.windowHasFocus;
            RenderTarget pt;
            pt.term = rpi.term; pt.rs = &paneRenderPrivate_[rpi.id];
            pt.rect = rpi.rect; pt.isFocused = focused;
            pt.paneId = rpi.id;
            pt.hasPopupFocus = !rpi.focusedPopupId.empty();
            pt.paneInfo = &rpi;
            pt.pixelOriginX = frameState_.padLeft; pt.pixelOriginY = frameState_.padTop;
            renderTargets.push_back(std::move(pt));
        }
        // Add popup targets after all panes (composited on top)
        for (const auto& rpi : frameState_.panes) {
            for (const auto& popup : rpi.popups) {
                if (!popup.term) continue;
                TerminalEmulator* pterm = popup.term;
                std::string key = popupStateKey(rpi.id, popup.id);
                PaneRenderPrivate& prs = popupRenderPrivate_[key];
                int pcols = pterm->width(), prows = pterm->height();
                size_t needed = static_cast<size_t>(pcols) * prows;
                if (prs.resolvedCells.size() != needed) {
                    prs.resolvedCells.resize(needed);
                    prs.rowShapingCache.resize(prows);
                    prs.dirty = true;
                }
                // Popup texture rect: screen-absolute position, popup-sized
                PaneRect popupRect;
                popupRect.x = rpi.rect.x + static_cast<int>(frameState_.padLeft) + static_cast<int>(popup.cellX * frameState_.charWidth);
                popupRect.y = rpi.rect.y + static_cast<int>(frameState_.padTop)  + static_cast<int>(popup.cellY * frameState_.lineHeight);
                popupRect.w = static_cast<int>(popup.cellW * frameState_.charWidth);
                popupRect.h = static_cast<int>(popup.cellH * frameState_.lineHeight);
                RenderTarget pop;
                pop.term = pterm; pop.rs = &prs; pop.rect = popupRect;
                pop.isFocused = (rpi.focusedPopupId == popup.id);
                pop.isPopup = true;
                pop.pixelOriginX = 0.0f; pop.pixelOriginY = 0.0f;
                pop.popupParentPaneId = rpi.id; pop.popupId = popup.id;
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
        PaneRenderPrivate& rs = *target.rs;
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
        // Cursor shape change detection — blink reset is handled by the
        // main thread (the shape change originates from injectData on the
        // main thread). We just track lastCursorShape for dirty detection.
        rs.lastCursorX = snap.cursorX;
        rs.lastCursorY = snap.cursorY;
        rs.lastCursorVisible = snap.cursorVisible;
        rs.lastCursorShape = snap.cursorShape;

        // Detect blink opacity change for a currently-blinking cursor.
    float blinkOpacity = frameState_.cursorBlinkOpacity;
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
    // phase that doesn't touch main-thread-shared PaneRenderPrivate fields
    // (heldTexture / pendingRelease are untouched here; resolveRow reads
    // rs.snapshot, which is already populated, and writes
    // rs.rowShapingCache / rs.resolvedCells, which only the render thread
    // ever touches).
    //
    // renderActive_ tells main thread's drainPendingExits() to defer
    // structural erasure while the render is holding raw Tab/Pane
    // pointers captured above. When we clear the flag, we wake the event
    // loop so the next tick drains any deferred exits promptly.
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

    // No re-acquisition of platformMutex_ — the rest of the frame uses only
    // render-private state, renderState_ (immutable after snapshot phase),
    // and GPU objects.

    // --- Phase 2: Per-target GPU upload and rendering ---

    float blinkOpacity = frameState_.cursorBlinkOpacity;
    for (int ti = 0; ti < static_cast<int>(renderTargets.size()); ++ti) {
        auto& target = renderTargets[ti];
        TerminalEmulator* term = target.term;
        PaneRenderPrivate& rs = *target.rs;
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

            renderer_.updateFontAtlas(queue_, frameState_.fontName, *font);

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
                        ? static_cast<float>(dispCellW) * frameState_.charWidth
                        : static_cast<float>(view.pixelWidth);
                    float imgH = dispCellH > 0
                        ? static_cast<float>(dispCellH) * frameState_.lineHeight
                        : static_cast<float>(view.pixelHeight);
                    float imgX = frameState_.padLeft + static_cast<float>(ex->imageStartCol) * frameState_.charWidth + subCellX;
                    float imgY = frameState_.padTop + (static_cast<float>(re.viewRow) - ex->imageOffsetRow) * frameState_.lineHeight + subCellY;

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
            params.cell_width = frameState_.charWidth;
            params.cell_height = frameState_.lineHeight;
            params.viewport_w = static_cast<float>(paneRect.w);
            params.viewport_h = static_cast<float>(paneRect.h);
            params.font_ascender = font->ascender * scale;
            params.font_size = frameState_.fontSize;
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
                bool popupHasFocus = target.hasPopupFocus;
                int cursorViewRow = snap.cursorY + snap.viewportOffset;
                // Check if cursor cell is covered by any popup in this pane
                bool cursorCovered = false;
                if (target.paneInfo) {
                    for (const auto& popup : target.paneInfo->popups) {
                        if (snap.cursorX >= popup.cellX && snap.cursorX < popup.cellX + popup.cellW &&
                            cursorViewRow >= popup.cellY && cursorViewRow < popup.cellY + popup.cellH) {
                            cursorCovered = true;
                            break;
                        }
                    }
                }
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
                renderer_.rasterizeColrGlyphs(encoder, queue_, frameState_.fontName, colrRasterCmds);
            }

            const float* tint = isFocused ? frameState_.activeTint : frameState_.inactiveTint;
            renderer_.renderToPane(encoder, queue_, frameState_.fontName, params, cs, newTexture->view, tint, imageCmds, imgSplitText);

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
    for (const auto& [paneId, rs] : paneRenderPrivate_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    for (const auto& [key, rs] : popupRenderPrivate_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    // Overlay uses a single PaneRenderPrivate
    {
        const auto& rs = overlayRenderPrivate_;
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    renderer_.retainImagesOnly(imagesToRetain);

    // Render tab bar if dirty or window focus changed (tint update)
    if (frameState_.tabBarVisible && (frameState_.tabBarDirty || focusChanged)) {
        renderTabBar();
        frameState_.tabBarDirty = false;
    }

    // Add tab bar texture to composite entries
    if (tabBarTexture_ && !frameState_.tabBarRect.isEmpty()) {
        const PaneRect& tbRect = frameState_.tabBarRect;
        {
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

        // Flush deferred divider GPU state (viewport + per-pane geometry).
        // All GPU buffer writes happen here on the render thread, never on the main thread.
        if (frameState_.dividersDirty || focusChanged) {
            const float* windowTint = frameState_.windowHasFocus
                ? frameState_.activeTint : frameState_.inactiveTint;
            renderer_.updateDividerViewport(queue_, frameFbW, frameFbH, windowTint);
            for (const auto& rpi : frameState_.panes) {
                int pid = rpi.id;
                auto git = frameState_.dividerGeoms.find(pid);
                if (git == frameState_.dividerGeoms.end() || !git->second.valid) continue;
                auto& geom = git->second;
                auto rit = paneRenderPrivate_.find(pid);
                if (rit == paneRenderPrivate_.end()) continue;
                renderer_.updateDividerBuffer(queue_, rit->second.dividerVB,
                    geom.x, geom.y, geom.w, geom.h,
                    geom.r, geom.g, geom.b, geom.a);
            }
            frameState_.dividersDirty = false;
        }

        // Draw per-pane dividers
        for (const auto& rpi : frameState_.panes) {
            auto it = paneRenderPrivate_.find(rpi.id);
            if (it == paneRenderPrivate_.end() || !it->second.dividerVB) continue;
            renderer_.drawDivider(encoder, compositeTarget,
                                   frameFbW, frameFbH, it->second.dividerVB);
        }

        // Draw popup borders (cached GPU buffers, rebuilt on change)
        if (!frameState_.hasOverlay) {
            for (const auto& rpi : frameState_.panes) {
                auto rsIt = paneRenderPrivate_.find(rpi.id);
                if (rsIt == paneRenderPrivate_.end()) continue;
                auto& prs = rsIt->second;

                // Sync cached borders with current popups
                bool bordersChanged = prs.popupBorders.size() != rpi.popups.size();
                if (!bordersChanged) {
                    for (size_t i = 0; i < rpi.popups.size(); ++i) {
                        const auto& pb = prs.popupBorders[i];
                        const auto& pp = rpi.popups[i];
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
                    const PaneRect& pr = rpi.rect;
                    float bw = std::max(1.0f, static_cast<float>(frameState_.dividerWidth));

                    for (const auto& popup : rpi.popups) {
                        float px = pr.x + frameState_.padLeft + popup.cellX * frameState_.charWidth;
                        float py = pr.y + frameState_.padTop + popup.cellY * frameState_.lineHeight;
                        float pw = popup.cellW * frameState_.charWidth;
                        float ph = popup.cellH * frameState_.lineHeight;

                        PaneRenderPrivate::PopupBorder pb;
                        pb.popupId = popup.id;
                        pb.cellX = popup.cellX;
                        pb.cellY = popup.cellY;
                        pb.cellW = popup.cellW;
                        pb.cellH = popup.cellH;
                        renderer_.updateDividerBuffer(queue_, pb.top,
                            px - bw, py - bw, pw + 2 * bw, bw,
                            frameState_.dividerR, frameState_.dividerG, frameState_.dividerB, frameState_.dividerA);
                        renderer_.updateDividerBuffer(queue_, pb.bottom,
                            px - bw, py + ph, pw + 2 * bw, bw,
                            frameState_.dividerR, frameState_.dividerG, frameState_.dividerB, frameState_.dividerA);
                        renderer_.updateDividerBuffer(queue_, pb.left,
                            px - bw, py, bw, ph,
                            frameState_.dividerR, frameState_.dividerG, frameState_.dividerB, frameState_.dividerA);
                        renderer_.updateDividerBuffer(queue_, pb.right,
                            px + pw, py, bw, ph,
                            frameState_.dividerR, frameState_.dividerG, frameState_.dividerB, frameState_.dividerA);
                        prs.popupBorders.push_back(std::move(pb));
                    }
                }

                // Draw cached borders
                for (const auto& pb : prs.popupBorders) {
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.top);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.bottom);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.left);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.right);
                }
            }
        }

        // Draw per-pane progress bars at top of pane
        if (frameState_.progressBarEnabled) for (const auto& rpi : frameState_.panes) {
            int st = rpi.progressState;
            if (st == 0) continue;
            const PaneRect& pr = rpi.rect;
            float barHeight = frameState_.progressBarHeight;
            float barY = static_cast<float>(pr.y);
            float barX = static_cast<float>(pr.x);
            float barW = static_cast<float>(pr.w);
            float edgeSoft = 40.0f * frameState_.contentScaleX;

            Renderer::ProgressBarParams pbp{};
            pbp.h = barHeight;
            pbp.a = 1.0f;

            if (st == 1 || st == 2) {
                // Determinate: fill from left, sharp left edge, gradient right edge
                float pct = std::clamp(static_cast<float>(rpi.progressPct) / 100.0f, 0.0f, 1.0f);
                pbp.x = barX;
                pbp.y = barY;
                pbp.w = barW;
                pbp.fillFrac = pct;
                pbp.edgeSoftness = edgeSoft;
                pbp.softLeft = 0.0f;
                pbp.softRight = 1.0f;
                pbp.r = (st == 2) ? 0.8f : frameState_.progressColorR;
                pbp.g = (st == 2) ? 0.2f : frameState_.progressColorG;
                pbp.b = (st == 2) ? 0.2f : frameState_.progressColorB;
                renderer_.drawProgressBar(encoder, queue_, compositeTarget,
                                           frameFbW, frameFbH, pbp);
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
                    pbp.r = frameState_.progressColorR;
                    pbp.g = frameState_.progressColorG;
                    pbp.b = frameState_.progressColorB;
                    renderer_.drawProgressBar(encoder, queue_, compositeTarget,
                                               frameFbW, frameFbH, pbp);
                }
            }
        }

        if (pngNeeded) {
            // Resolve target texture based on IPC request
            wgpu::Texture srcTexture = compositeTarget;
            uint32_t srcW = frameFbW, srcH = frameFbH;

            const auto& target = debugIPC_->pngTarget();
            if (target.starts_with("pane:")) {
                int paneId = std::stoi(target.substr(5));
                auto it = paneRenderPrivate_.find(paneId);
                if (it != paneRenderPrivate_.end() && it->second.heldTexture) {
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
                copyX = static_cast<uint32_t>(cellRect.x * frameState_.charWidth + frameState_.padLeft);
                copyY = static_cast<uint32_t>(cellRect.y * frameState_.lineHeight + frameState_.padTop);
                copyW = static_cast<uint32_t>(cellRect.w * frameState_.charWidth);
                copyH = static_cast<uint32_t>(cellRect.h * frameState_.lineHeight);
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
        for (const auto& rpi : frameState_.panes) {
            auto it = paneRenderPrivate_.find(rpi.id);
            if (it != paneRenderPrivate_.end()) {
                toRelease.insert(toRelease.end(),
                    it->second.pendingRelease.begin(),
                    it->second.pendingRelease.end());
                it->second.pendingRelease.clear();
            }
            for (const auto& popup : rpi.popups) {
                auto pit = popupRenderPrivate_.find(popupStateKey(rpi.id, popup.id));
                if (pit != popupRenderPrivate_.end()) {
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


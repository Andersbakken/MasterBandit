#include "RenderEngine.h"

#include "AnimationScheduler.h"
#include "DebugIPC.h"
#include "PlatformDawn.h"
#include "ProceduralGlyphTable.h"
#include "RenderThread.h"
#include "Utf8.h"
#include "Utils.h"
#include "Observability.h"
#include "text.h"

#include <eventloop/Window.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }

// Resolve `glyphId` in `font`, falling back to a font-specific replacement
// glyph (typically U+FFFD shaped against the same font) when the lookup
// misses for a renderable codepoint. Returns false when the cell should not
// be drawn: gid==0, missing entry for a substitution / null / whitespace
// codepoint, or no replacement available.
template <typename ReplacementFn>
static bool resolveCellGlyph(FontData& font,
                             uint64_t glyphId,
                             char32_t codepoint,
                             bool isSubstitution,
                             ReplacementFn&& getReplacement,
                             GlyphInfo& out)
{
    if ((glyphId & 0xFFFFFFFFu) == 0u) return false;
    {
        std::shared_lock lock(font.mutex);
        auto it = font.glyphs.find(glyphId);
        if (it != font.glyphs.end() && !it->second.is_empty) {
            out = it->second;
            return true;
        }
    }
    if (isSubstitution || codepoint == 0 || unicode::isSpace(codepoint)) return false;
    if (const GlyphInfo* rep = getReplacement()) {
        out = *rep;
        return true;
    }
    return false;
}

RenderEngine::RenderEngine() = default;

RenderEngine::~RenderEngine()
{
    shutdown();
}

bool RenderEngine::initGpu()
{
    nativeInstance_ = std::make_unique<dawn::native::Instance>();
    wgpu::Instance instance(nativeInstance_->Get());

    wgpu::RequestAdapterOptions adapterOpts = {};
    adapterOpts.powerPreference = wgpu::PowerPreference::HighPerformance;

    auto adapters = nativeInstance_->EnumerateAdapters(&adapterOpts);
    if (adapters.empty()) {
        spdlog::error("No suitable GPU adapter found");
        return false;
    }

    dawn::native::Adapter nativeAdapter = adapters[0];
    wgpu::Adapter adapter(nativeAdapter.Get());

    wgpu::AdapterInfo info = {};
    adapter.GetInfo(&info);
    spdlog::info("GPU Adapter: {}", std::string_view(info.device.data, info.device.length));

    wgpu::DeviceDescriptor deviceDesc = {};
    deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        spdlog::error("Dawn error ({}): {}", static_cast<int>(type),
            std::string_view(message.data, message.length));
    });
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
            if (reason == wgpu::DeviceLostReason::Destroyed) return;
            spdlog::error("Device lost ({}): {}", static_cast<int>(reason),
                std::string_view(message.data, message.length));
        });

    WGPUDevice rawDevice = nativeAdapter.CreateDevice(&deviceDesc);
    if (!rawDevice) {
        spdlog::error("Failed to create Dawn device");
        return false;
    }
    device_ = wgpu::Device::Acquire(rawDevice);
    queue_ = device_.GetQueue();
    texturePool_.init(device_, wgpu::TextureFormat::BGRA8Unorm);
    return true;
}

bool RenderEngine::createSurface(Window* window)
{
    if (!window || !nativeInstance_) return false;
    surface_ = window->createWgpuSurface(nativeInstance_->Get());
    return static_cast<bool>(surface_);
}

bool RenderEngine::initRenderer(const std::string& shaderDir, uint32_t fbWidth, uint32_t fbHeight)
{
    if (!device_) return false;
    renderer_.init(device_, queue_, shaderDir, fbWidth, fbHeight);
    renderer_.initProgressPipeline(device_, shaderDir);
    return true;
}

void RenderEngine::uploadFontAtlas(const std::string& fontName, const FontData& font)
{
    renderer_.uploadFontAtlas(queue_, fontName, font);
}

void RenderEngine::configureSurface(uint32_t width, uint32_t height)
{
    wgpu::SurfaceConfiguration config = {};
    config.device = device_;
    config.format = wgpu::TextureFormat::BGRA8Unorm;
    config.width = width;
    config.height = height;
    // Prefer non-blocking present modes: Mailbox → FifoRelaxed → Fifo.
    // Dawn's vkAcquireNextImageKHR uses UINT64_MAX timeout, so Fifo can stall
    // the render thread indefinitely if the compositor pauses frame consumption.
    {
        static wgpu::PresentMode chosenMode = wgpu::PresentMode::Undefined;
        config.presentMode = wgpu::PresentMode::Fifo;
        wgpu::SurfaceCapabilities caps = {};
        if (surface_.GetCapabilities(device_.GetAdapter(), &caps) == wgpu::Status::Success) {
            for (auto mode : { wgpu::PresentMode::Mailbox,
                               wgpu::PresentMode::FifoRelaxed,
                               wgpu::PresentMode::Fifo }) {
                bool found = false;
                for (size_t i = 0; i < caps.presentModeCount; ++i)
                    if (caps.presentModes[i] == mode) { found = true; break; }
                if (found) { config.presentMode = mode; break; }
            }
        }
        if (config.presentMode != chosenMode) {
            chosenMode = config.presentMode;
            const char* name = config.presentMode == wgpu::PresentMode::Mailbox    ? "Mailbox"
                             : config.presentMode == wgpu::PresentMode::FifoRelaxed ? "FifoRelaxed"
                                                                                    : "Fifo";
            spdlog::info("Surface present mode: {}", name);
        }
    }
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    surface_.Configure(&config);
}

void RenderEngine::shutdown()
{
    if (shutdownCalled_) return;
    shutdownCalled_ = true;

    // Drain any deferred releases that landed via GPU completion callbacks
    // between the render thread exiting and now — they hold raw pointers
    // into the texture / compute pools which are about to be destroyed.
    {
        auto& state = *deferredReleaseState_;
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto* t : state.textures) texturePool_.release(t);
        state.textures.clear();
        for (auto* cs : state.compute) renderer_.computePool().release(cs);
        state.compute.clear();
    }

    // Release held textures before clearing pool
    for (auto& [id, rs] : paneRenderPrivate_) {
        if (rs.heldTexture) texturePool_.release(rs.heldTexture);
        for (auto* t : rs.pendingRelease) texturePool_.release(t);
    }
    paneRenderPrivate_.clear();
    for (auto& [key, rs] : popupRenderPrivate_) {
        if (rs.heldTexture) texturePool_.release(rs.heldTexture);
        for (auto* t : rs.pendingRelease) texturePool_.release(t);
    }
    popupRenderPrivate_.clear();
    for (auto& [key, rs] : embeddedRenderPrivate_) {
        if (rs.heldTexture) texturePool_.release(rs.heldTexture);
        for (auto* t : rs.pendingRelease) texturePool_.release(t);
    }
    embeddedRenderPrivate_.clear();

    // Release tab bar textures
    if (tabBarTexture_) { texturePool_.release(tabBarTexture_); tabBarTexture_ = nullptr; }
    for (auto* t : pendingTabBarRelease_) texturePool_.release(t);
    pendingTabBarRelease_.clear();
    for (auto* t : pendingDestroyRelease_) texturePool_.release(t);
    pendingDestroyRelease_.clear();

    // Release headless composite texture
    headlessComposite_ = nullptr;

    // Release any pending compute states before destroying renderer
    for (auto* cs : pendingComputeRelease_) renderer_.computePool().release(cs);
    pendingComputeRelease_.clear();

    surface_ = nullptr;

    renderer_.destroy();
    queue_ = {};
    device_ = {};
    texturePool_.clear();
    // nativeInstance_ is intentionally NOT reset here.  On NVIDIA, destroying
    // the Vulkan instance (vkDestroyInstance) removes DRI/GLX state that was
    // registered with Xlib via XESetCloseDisplay.  If XCloseDisplay fires
    // after the instance is gone it calls a dangling function pointer and
    // segfaults.  By leaving nativeInstance_ alive here it is destroyed by
    // ~RenderEngine(), which PlatformDawn calls only after XCloseDisplay has
    // already run (renderEngine_.reset() comes after window_->destroy()).
}

void RenderEngine::resolveRow(PaneRenderPrivate& rs, int row, FontData* font, float /*scale*/,
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
            uint32_t bgOpaque = (bg == 0u)
                ? (static_cast<uint32_t>(dc.bgR) | (static_cast<uint32_t>(dc.bgG) << 8) | (static_cast<uint32_t>(dc.bgB) << 16) | 0xFF000000u)
                : bg;
            std::swap(fg, bgOpaque);
            bg = bgOpaque;
        }
        if (cell.attrs.dim()) {
            // SGR 2 (faint/dim): halve the foreground RGB channels.
            uint32_t r = (fg >>  0) & 0xFF;
            uint32_t g = (fg >>  8) & 0xFF;
            uint32_t b = (fg >> 16) & 0xFF;
            uint32_t a = (fg >> 24) & 0xFF;
            fg = (r / 2) | ((g / 2) << 8) | ((b / 2) << 16) | (a << 24);
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
            if (cell.attrs.strikethrough()) {
                ulInfo |= 0x08u; // bit 3: strikethrough
            }
        }

        rc.glyph_offset = 0;
        rc.glyph_count = 0;
        rc.fg_color = fg;
        rc.bg_color = bg;
        rc.underline_info = ulInfo;
    }

    // Pass 2: Build runs and shape
    GlyphInfo replacementGlyph{};
    bool replacementGlyphReady = false;
    auto getReplacementGlyph = [&]() -> const GlyphInfo* {
        if (replacementGlyphReady) return replacementGlyph.is_empty ? nullptr : &replacementGlyph;
        replacementGlyphReady = true;
        const ShapedRun& rep = platform_->textSystem_.shapeRun(frameState_.fontName, "\xEF\xBF\xBD", frameState_.fontSize, {});
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

        if (cell.wc == 0 || cell.attrs.wideSpacer()) {
            col++;
            continue;
        }

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

        bool runBold = cell.attrs.bold();
        bool runItalic = cell.attrs.italic();
        int runStart = col;

        int runEnd = col + 1;
        while (runEnd < cols) {
            const Cell& next = rowData[runEnd];
            if (next.wc == 0) break;
            if (next.attrs.wideSpacer()) { runEnd++; continue; }
            if (next.attrs.bold() != runBold) break;
            if (next.attrs.italic() != runItalic) break;
            if (ProceduralGlyph::codepointToTableIdx(next.wc) != ProceduralGlyph::kInvalidIndex) break;
            runEnd++;
        }

        std::string runText;
        runText.reserve(static_cast<size_t>(runEnd - runStart) * 4);
        std::vector<std::pair<uint32_t, int>> byteToCell;
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

        FontStyle runStyle;
        runStyle.bold = runBold;
        runStyle.italic = runItalic;
        const ShapedRun& shaped = platform_->textSystem_.shapeRun(frameState_.fontName, runText, frameState_.fontSize, runStyle, byteToCell);

        struct RtlRange { int firstCell, lastCell; };
        std::vector<RtlRange> rtlRanges;
        {
            int i = 0;
            int n = static_cast<int>(byteToCell.size());
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

        float penX = 0;
        for (const auto& sg : shaped.glyphs) {
            int cellCol = -1;
            for (auto it = byteToCell.rbegin(); it != byteToCell.rend(); ++it) {
                if (sg.cluster >= it->first) {
                    cellCol = it->second;
                    break;
                }
            }

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

            uint64_t glyphId = sg.glyphId;
            char32_t wc = (cellCol >= 0 && cellCol < cols) ? rowData[cellCol].wc : 0;
            GlyphInfo gi;
            if (!resolveCellGlyph(*font, glyphId, wc, sg.isSubstitution,
                                  getReplacementGlyph, gi)) {
                penX += sg.xAdvance;
                continue;
            }

            if (gi.is_colr) {
                uint64_t colrKey = glyphId;
                int cellSpan = 1;
                if (cellCol + 1 < cols && rowData[cellCol + 1].attrs.wideSpacer())
                    cellSpan = 2;
                float cellPxW = static_cast<float>(cellSpan) * cellWidthPx;
                float cellPxH = frameState_.lineHeight;

                auto result = renderer_.colrAtlas().acquireTile(colrKey, frameState_.fontSize);
                if (result.tile) {
                    auto* tile = result.tile;
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
                        rcmd.em_origin_x = gi.ext_min_x;
                        rcmd.em_origin_y = gi.ext_min_y;
                        rcmd.em_width = gi.ext_max_x - gi.ext_min_x;
                        rcmd.em_height = gi.ext_max_y - gi.ext_min_y;
                        rowCache.colrRasterCmds.push_back(rcmd);
                    }
                }

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

            float glyphX, glyphY;
            if (sg.isSubstitution) {
                float cellLocalX = static_cast<float>(cellCol - runStart) * cellWidthPx;
                glyphX = penX + sg.xOffset - cellLocalX;
            } else {
                glyphX = sg.xOffset;
            }
            glyphY = sg.yOffset;

            GlyphEntry entry;
            entry.atlas_offset = gi.atlas_offset;
            entry.ext_min_x = gi.ext_min_x;
            entry.ext_min_y = gi.ext_min_y;
            entry.ext_max_x = gi.ext_max_x;
            entry.ext_max_y = gi.ext_max_y;
            entry.upem = gi.upem;
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

void RenderEngine::renderTabBar()
{
    if (!frameState_.tabBarVisible) return;
    if (frameState_.tabBarCells.empty()) return;
    const Rect& tbRect = frameState_.tabBarRect;
    if (tbRect.isEmpty()) return;

    int cols = frameState_.tabBarCols;
    if (cols <= 0) return;

    std::vector<ResolvedCell> cells(static_cast<size_t>(cols));
    std::vector<GlyphEntry> tabBarGlyphs;

    FontData* font = const_cast<FontData*>(platform_->textSystem_.getFont(frameState_.tabBarFontName));
    if (!font) return;
    float scale = frameState_.tabBarFontSize / font->baseSize;

    GlyphInfo tabBarReplacementGlyph{};
    bool tabBarReplacementGlyphReady = false;
    auto getTabBarReplacementGlyph = [&]() -> const GlyphInfo* {
        if (tabBarReplacementGlyphReady)
            return tabBarReplacementGlyph.is_empty ? nullptr : &tabBarReplacementGlyph;
        tabBarReplacementGlyphReady = true;
        const ShapedText& rep = platform_->textSystem_.shapeText(
            frameState_.tabBarFontName, "\xEF\xBF\xBD", frameState_.tabBarFontSize);
        if (rep.glyphs.empty()) return nullptr;
        std::shared_lock lock(font->mutex);
        auto it = font->glyphs.find(rep.glyphs[0].glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        tabBarReplacementGlyph = it->second;
        return &tabBarReplacementGlyph;
    };

    for (int col = 0; col < cols; ++col) {
        const auto& tbc = frameState_.tabBarCells[col];
        ResolvedCell& rc = cells[static_cast<size_t>(col)];
        rc.glyph_offset = 0;
        rc.glyph_count = 0;
        rc.fg_color = tbc.fgColor;
        rc.bg_color = tbc.bgColor;
        rc.underline_info = 0;

        if (tbc.ch.empty()) continue;

        int consumed = 0;
        uint32_t cp = utf8::decode(tbc.ch.data(), static_cast<int>(tbc.ch.size()), consumed);

        uint32_t tableIdx = ProceduralGlyph::codepointToTableIdx(cp);
        if (tableIdx != ProceduralGlyph::kInvalidIndex && ProceduralGlyph::kTable[tableIdx] != 0) {
            GlyphEntry entry;
            entry.atlas_offset = 0x80000000u | tableIdx;
            entry.ext_min_x = 0; entry.ext_min_y = 0;
            entry.ext_max_x = 0; entry.ext_max_y = 0;
            entry.upem = 0; entry.x_offset = 0; entry.y_offset = 0;
            rc.glyph_offset = static_cast<uint32_t>(tabBarGlyphs.size());
            rc.glyph_count = 1;
            tabBarGlyphs.push_back(entry);
            continue;
        }

        const ShapedText& shaped = platform_->textSystem_.shapeText(
            frameState_.tabBarFontName, tbc.ch, frameState_.tabBarFontSize);
        if (shaped.glyphs.empty()) continue;
        GlyphInfo gi;
        if (!resolveCellGlyph(*font, shaped.glyphs[0].glyphId,
                              static_cast<char32_t>(cp), false,
                              getTabBarReplacementGlyph, gi)) continue;
        GlyphEntry entry;
        entry.atlas_offset = gi.atlas_offset;
        entry.ext_min_x = gi.ext_min_x; entry.ext_min_y = gi.ext_min_y;
        entry.ext_max_x = gi.ext_max_x; entry.ext_max_y = gi.ext_max_y;
        entry.upem = gi.upem; entry.x_offset = 0.0f; entry.y_offset = 0.0f;
        rc.glyph_offset = static_cast<uint32_t>(tabBarGlyphs.size());
        rc.glyph_count = 1;
        tabBarGlyphs.push_back(entry);
    }

    renderer_.updateFontAtlas(queue_, frameState_.tabBarFontName, *font);

    ComputeState* cs = renderer_.computePool().acquire(static_cast<uint32_t>(cols));
    uint32_t tbGlyphCount = std::max(static_cast<uint32_t>(tabBarGlyphs.size()), 1u);
    renderer_.computePool().ensureGlyphCapacity(cs, tbGlyphCount);
    renderer_.uploadResolvedCells(queue_, cs, cells.data(), static_cast<uint32_t>(cols));
    if (!tabBarGlyphs.empty())
        renderer_.uploadGlyphs(queue_, cs, tabBarGlyphs.data(), static_cast<uint32_t>(tabBarGlyphs.size()));

    TerminalComputeParams params = {};
    params.cols = static_cast<uint32_t>(cols);
    params.rows = 1;
    params.cell_width = frameState_.tabBarCharWidth;
    params.cell_height = frameState_.tabBarLineHeight;
    params.viewport_w = static_cast<float>(tbRect.w);
    params.viewport_h = static_cast<float>(tbRect.h);
    params.font_ascender = font->ascender * scale;
    params.font_size = frameState_.tabBarFontSize;
    params.pane_origin_x = 0.0f;
    params.pane_origin_y = 0.0f;
    params.max_text_vertices = cs->maxTextVertices;

    PooledTexture* newTexture = texturePool_.acquire(
        static_cast<uint32_t>(tbRect.w),
        static_cast<uint32_t>(std::ceil(frameState_.tabBarLineHeight)));

    wgpu::CommandEncoderDescriptor encDesc = {};
    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
    const float* windowTint = frameState_.windowHasFocus
        ? frameState_.activeTint : frameState_.inactiveTint;
    renderer_.renderToPane(encoder, queue_, frameState_.tabBarFontName, params, cs, newTexture->view, windowTint, Renderer::DimParams{}, {});
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    pendingComputeRelease_.push_back(cs);

    if (tabBarTexture_) pendingTabBarRelease_.push_back(tabBarTexture_);
    tabBarTexture_ = newTexture;
    // tabBarDirty_ consumed from renderState_ under the lock at frame start
}

void RenderEngine::renderFrame()
{
    // Bump the frame counter on every exit from this function (including
    // early returns), after all locals that may alias frameState_ pointers
    // have been destroyed. The Graveyard on the main thread waits for this
    // counter to advance past an entry's stamp before freeing it.
    struct FrameCompletionGuard {
        PlatformDawn* platform;
        ~FrameCompletionGuard() {
            if (platform) platform->renderThread_->notifyFrameCompleted();
        }
    } frameGuard { platform_ };

    const bool headless = platform_->isHeadless();

    // Check for pending surface reconfiguration BEFORE acquiring the swapchain
    // texture. configureSurface() invalidates existing textures, so it must
    // happen before GetCurrentTexture.
    if (!headless) {
        auto [needsReconfigure, w, h] = platform_->takeSurfaceReconfigureRequest();
        if (platform_->takeViewportSizeChangedRequest()) {
            auto [vw, vh] = platform_->currentFbSize();
            renderer_.setViewportSize(vw, vh);
        }
        if (needsReconfigure && w && h) {
            configureSurface(w, h);
            renderer_.setViewportSize(w, h);
        }
    }

    // Acquire the surface texture before taking platformMutex_.
    wgpu::SurfaceTexture surfaceTexture;
    wgpu::Texture compositeTarget;
    if (!headless) {
        surface_.GetCurrentTexture(&surfaceTexture);
        if (surfaceTexture.status == wgpu::SurfaceGetCurrentTextureStatus::Outdated) {
            auto [w, h] = platform_->currentFbSize();
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

    // Snapshot the render state under the platform lock.
    if (!platform_->snapshotUnderLock(frameState_)) return;

    auto [fbWidth, fbHeight] = platform_->currentFbSize();
    if (fbWidth == 0 || fbHeight == 0) return;

    bool focusChanged = frameState_.focusChanged;
    bool invalidateAllCaches = frameState_.mainFontAtlasChanged ||
                               frameState_.tabBarFontAtlasChanged ||
                               frameState_.mainFontRemoved ||
                               frameState_.tabBarFontRemoved ||
                               frameState_.invalidateAllRowCaches;

    // Handle font atlas GPU work on the render thread
    if (frameState_.mainFontRemoved) {
        renderer_.removeFontAtlas(frameState_.fontName);
    }
    if (frameState_.mainFontAtlasChanged) {
        const FontData* font2 = platform_->textSystem_.getFont(frameState_.fontName);
        if (font2) renderer_.uploadFontAtlas(queue_, frameState_.fontName, *font2);
    }
    if (frameState_.tabBarFontRemoved) {
        renderer_.removeFontAtlas(frameState_.tabBarFontName);
    }
    if (frameState_.tabBarFontAtlasChanged) {
        const FontData* tbFont = platform_->textSystem_.getFont(frameState_.tabBarFontName);
        if (tbFont) renderer_.uploadFontAtlas(queue_, frameState_.tabBarFontName, *tbFont);
    }
    if (frameState_.viewportSizeChanged) {
        renderer_.setViewportSize(fbWidth, fbHeight);
    }

    // Ensure every pane in the shadow copy has a render-private entry
    for (const auto& op : frameState_.panes) {
        paneRenderPrivate_[op.id];
    }

    // If the surface texture doesn't match fbWidth/fbHeight, use texture
    // dims for this frame only without writing back to shared state.
    uint32_t frameFbW = fbWidth, frameFbH = fbHeight;
    if (!headless && compositeTarget) {
        uint32_t texW = compositeTarget.GetWidth();
        uint32_t texH = compositeTarget.GetHeight();
        if (texW && texH && (texW != frameFbW || texH != frameFbH)) {
            frameFbW = texW;
            frameFbH = texH;
            renderer_.setViewportSize(texW, texH);
        }
    }

    // Drop held textures whose content is no longer trusted (tab switch,
    // framebuffer resize, font change). Setting dirty forces the next
    // render pass to acquire a fresh texture and re-shape all rows; the
    // old heldTexture moves to pendingRelease where the regular deferred
    // drain returns it to the pool after the GPU is done with it.
    auto dropHeldTexture = [](PaneRenderPrivate& rs) {
        if (rs.heldTexture) {
            rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = nullptr;
        }
        rs.dirty = true;
    };
    if (frameState_.releaseAllPaneTextures) {
        for (auto& [id, rs] : paneRenderPrivate_) dropHeldTexture(rs);
        for (auto& [key, rs] : popupRenderPrivate_) dropHeldTexture(rs);
        for (auto& [key, rs] : embeddedRenderPrivate_) dropHeldTexture(rs);
    } else {
        for (const Uuid& paneId : frameState_.releasePaneTextureIds) {
            auto it = paneRenderPrivate_.find(paneId);
            if (it != paneRenderPrivate_.end()) dropHeldTexture(it->second);
            // Popups hang off a pane; release their textures too. Keys are
            // "<uuid>/popupId" — match by prefix.
            std::string popupPrefix = paneId.toString() + "/";
            for (auto& [key, rs] : popupRenderPrivate_) {
                if (key.compare(0, popupPrefix.size(), popupPrefix) == 0) dropHeldTexture(rs);
            }
            // Embeddeds keyed as "<uuid>:lineId" — different separator so
            // the popup prefix doesn't collide.
            std::string embeddedPrefix = paneId.toString() + ":";
            for (auto& [key, rs] : embeddedRenderPrivate_) {
                if (key.compare(0, embeddedPrefix.size(), embeddedPrefix) == 0) dropHeldTexture(rs);
            }
        }
        for (const auto& key : frameState_.releasePopupTextureKeys) {
            auto it = popupRenderPrivate_.find(key);
            if (it != popupRenderPrivate_.end()) dropHeldTexture(it->second);
        }
        for (const auto& key : frameState_.releaseEmbeddedTextureKeys) {
            auto it = embeddedRenderPrivate_.find(key);
            if (it != embeddedRenderPrivate_.end()) dropHeldTexture(it->second);
        }
    }
    if (frameState_.releaseTabBarTexture && tabBarTexture_) {
        pendingTabBarRelease_.push_back(tabBarTexture_);
        tabBarTexture_ = nullptr;
        frameState_.tabBarDirty = true;
    }

    // Process destroys. Extract heldTexture + pendingRelease into a
    // RenderEngine-level staging list so the regular deferred-release
    // path still sees them after the entry is gone from the map. The
    // staging list is drained at the same OnSubmittedWorkDone site as
    // per-entry pendingRelease below.
    auto extractReleases = [this](PaneRenderPrivate& rs) {
        if (rs.heldTexture) pendingDestroyRelease_.push_back(rs.heldTexture);
        pendingDestroyRelease_.insert(pendingDestroyRelease_.end(),
            rs.pendingRelease.begin(), rs.pendingRelease.end());
        rs.heldTexture = nullptr;
        rs.pendingRelease.clear();
    };
    for (const Uuid& paneId : frameState_.destroyedPaneIds) {
        // Drop any popups that belonged to this pane first.
        std::string popupPrefix = paneId.toString() + "/";
        for (auto it = popupRenderPrivate_.begin(); it != popupRenderPrivate_.end(); ) {
            if (it->first.compare(0, popupPrefix.size(), popupPrefix) == 0) {
                extractReleases(it->second);
                it = popupRenderPrivate_.erase(it);
            } else {
                ++it;
            }
        }
        // And any embeddeds — keyed "<uuid>:lineId".
        std::string embeddedPrefix = paneId.toString() + ":";
        for (auto it = embeddedRenderPrivate_.begin(); it != embeddedRenderPrivate_.end(); ) {
            if (it->first.compare(0, embeddedPrefix.size(), embeddedPrefix) == 0) {
                extractReleases(it->second);
                it = embeddedRenderPrivate_.erase(it);
            } else {
                ++it;
            }
        }
        auto it = paneRenderPrivate_.find(paneId);
        if (it != paneRenderPrivate_.end()) {
            extractReleases(it->second);
            paneRenderPrivate_.erase(it);
        }
    }
    for (const auto& key : frameState_.destroyedPopupKeys) {
        auto it = popupRenderPrivate_.find(key);
        if (it != popupRenderPrivate_.end()) {
            extractReleases(it->second);
            popupRenderPrivate_.erase(it);
        }
    }
    for (const auto& key : frameState_.destroyedEmbeddedKeys) {
        auto it = embeddedRenderPrivate_.find(key);
        if (it != embeddedRenderPrivate_.end()) {
            extractReleases(it->second);
            embeddedRenderPrivate_.erase(it);
        }
    }

    if (invalidateAllCaches) {
        for (auto& [id, rs] : paneRenderPrivate_) {
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
        for (auto& [key, rs] : popupRenderPrivate_) {
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
        for (auto& [key, rs] : embeddedRenderPrivate_) {
            for (auto& row : rs.rowShapingCache) row.valid = false;
            rs.dirty = true;
        }
    }

    if (frameState_.panes.empty()) return;

    needsRedraw_ = false;
    renderer_.colrAtlas().advanceGeneration();

    if (headless) {
        if (!headlessComposite_) {
            wgpu::TextureDescriptor desc = {};
            desc.size = {fbWidth, fbHeight, 1};
            desc.format = wgpu::TextureFormat::BGRA8Unorm;
            desc.usage = wgpu::TextureUsage::RenderAttachment
                       | wgpu::TextureUsage::CopySrc
                       | wgpu::TextureUsage::CopyDst;
            headlessComposite_ = device_.CreateTexture(&desc);
        }
        compositeTarget = headlessComposite_;
    }

    FontData* font = const_cast<FontData*>(platform_->textSystem_.getFont(frameState_.fontName));
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
    DebugIPC* debugIPC = platform_->debugIPC_.get();
    bool pngNeeded = debugIPC && debugIPC->pngScreenshotPending();

    Uuid currentFocusedPaneId = frameState_.focusedPaneId;
    if (currentFocusedPaneId != lastFocusedPaneId_) {
        auto markDirty = [&](Uuid id) {
            auto it = paneRenderPrivate_.find(id);
            if (it != paneRenderPrivate_.end()) it->second.dirty = true;
        };
        markDirty(lastFocusedPaneId_);
        markDirty(currentFocusedPaneId);
        lastFocusedPaneId_ = currentFocusedPaneId;
    } else if (focusChanged) {
        auto it = paneRenderPrivate_.find(currentFocusedPaneId);
        if (it != paneRenderPrivate_.end()) it->second.dirty = true;
    }

    struct RenderTarget {
        TerminalEmulator* term;
        PaneRenderPrivate* rs;
        Rect rect;
        bool isFocused;
        Uuid paneId;
        bool hasPopupFocus = false;
        const RenderPaneInfo* paneInfo = nullptr;
        bool isPopup = false;
        bool isEmbedded = false;
        float pixelOriginX = 0.0f;
        float pixelOriginY = 0.0f;
        Uuid popupParentPaneId;
        std::string popupId;
        // Embedded-only: parent pane id and anchor line id.
        Uuid embeddedParentPaneId;
        uint64_t embeddedLineId = 0;
    };
    std::vector<RenderTarget> renderTargets;

    {
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
                Rect popupRect;
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
        // Embedded terminals — anchored to a Document line id. Rendered into
        // their own per-embedded texture sized (embCols*cellW) × (embRows*cellH)
        // with no internal padding. Skipped while the parent is on alt-screen
        // (no persistent scrollback for the anchor line to resolve against).
        for (const auto& rpi : frameState_.panes) {
            if (rpi.onAltScreen) continue;
            for (const auto& em : rpi.embeddeds) {
                if (!em.term) continue;
                std::string key = embeddedStateKey(rpi.id, em.lineId);
                PaneRenderPrivate& ers = embeddedRenderPrivate_[key];
                int ecols = em.term->width(), erows = em.term->height();
                if (ecols <= 0 || erows <= 0) continue;
                size_t needed = static_cast<size_t>(ecols) * erows;
                if (ers.resolvedCells.size() != needed) {
                    ers.resolvedCells.resize(needed);
                    ers.rowShapingCache.resize(erows);
                    ers.dirty = true;
                }
                Rect embRect;
                // The final composite destination is computed at composite
                // time from the parent's snapshot viewport position; here
                // `rect` is just the texture size the embedded renders into.
                embRect.x = 0;
                embRect.y = 0;
                embRect.w = static_cast<int>(std::round(ecols * frameState_.charWidth));
                embRect.h = static_cast<int>(std::round(erows * frameState_.lineHeight));
                RenderTarget et;
                et.term = em.term; et.rs = &ers; et.rect = embRect;
                et.isFocused = em.focused;
                et.isEmbedded = true;
                et.pixelOriginX = 0.0f; et.pixelOriginY = 0.0f;
                et.embeddedParentPaneId = rpi.id;
                et.embeddedLineId = em.lineId;
                renderTargets.push_back(std::move(et));
            }
        }
    }

    // --- Phase 1: Collect dirty rows across all panes and resolve in parallel ---

    struct TargetResolveInfo {
        int targetIdx;
        bool needsRender;
        bool cursorMoved;
        int curY;
    };
    std::vector<TargetResolveInfo> resolveInfos;

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

        bool animationAdvanced = term->tickAnimations();

        rs.snapshot.update(*term);
        const TerminalSnapshot& snap = rs.snapshot;

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
        rs.lastCursorX = snap.cursorX;
        rs.lastCursorY = snap.cursorY;
        rs.lastCursorVisible = snap.cursorVisible;
        rs.lastCursorShape = snap.cursorShape;

        float blinkOpacity = frameState_.cursorBlinkOpacity;
        bool cursorBlinkChanged = snap.cursorBlinking &&
                                  blinkOpacity != rs.lastCursorBlinkOpacity;
        rs.lastCursorBlinkOpacity = blinkOpacity;

        bool popupFocusChanged = !target.isPopup && !target.isEmbedded &&
                                 target.hasPopupFocus != rs.lastHasPopupFocus;
        rs.lastHasPopupFocus = target.hasPopupFocus;

        bool anyRowDirty = false;
        for (uint8_t d : snap.rowDirty) { if (d) { anyRowDirty = true; break; } }

        const auto& ls = rs.lastSelection;
        const auto& cs = snap.selection;
        bool selectionChanged =
            ls.startCol != cs.startCol || ls.startAbsRow != cs.startAbsRow ||
            ls.endCol   != cs.endCol   || ls.endAbsRow   != cs.endAbsRow   ||
            ls.active   != cs.active   || ls.valid       != cs.valid       ||
            ls.mode     != cs.mode;
        rs.lastSelection = cs;

        bool commandSelectionChanged =
            rs.lastSelectedCommand.has_value() != snap.selectedCommand.has_value() ||
            (rs.lastSelectedCommand && snap.selectedCommand &&
             (rs.lastSelectedCommand->startAbsRow != snap.selectedCommand->startAbsRow ||
              rs.lastSelectedCommand->endAbsRow   != snap.selectedCommand->endAbsRow   ||
              rs.lastSelectedCommand->startCol    != snap.selectedCommand->startCol    ||
              rs.lastSelectedCommand->endCol      != snap.selectedCommand->endCol));
        rs.lastSelectedCommand = snap.selectedCommand;

        // If the outline color in config changed (via live reload) and this
        // pane currently has a selection, we need to re-render to repaint
        // the outline with the new color.
        bool outlineColorChanged =
            snap.selectedCommand.has_value() &&
            rs.lastCommandOutlineColor != frameState_.commandOutlineColor;
        rs.lastCommandOutlineColor = frameState_.commandOutlineColor;

        // Unfocused panes render the cursor as a stable hollow outline with
        // no blink modulation (see cursor_type / packCursorColor selection in
        // the per-target render path), so blink-opacity transitions produce
        // pixel-identical output — invalidating on them is wasted work.
        bool needsRender = rs.dirty || anyRowDirty || cursorMoved ||
                           (target.isFocused && cursorBlinkChanged) ||
                           animationAdvanced ||
                           selectionChanged || commandSelectionChanged ||
                           outlineColorChanged || popupFocusChanged ||
                           !rs.heldTexture;

        if (snap.syncOutputActive && rs.heldTexture)
            needsRender = false;

        resolveInfos.push_back({ti, needsRender, cursorMoved, snap.cursorY});

        if (needsRender || pngNeeded) {
            size_t needed = static_cast<size_t>(snap.cols) * snap.rows;
            if (rs.resolvedCells.size() != needed)
                rs.resolvedCells.resize(needed);

            // Shape caches are invalidated only when the viewport's top
            // content changes — i.e. when viewport row 0 shows a different
            // document line than last frame. User scroll, live-tail roll
            // both shift topLineId; scroll-back pinning does not (the
            // visible abs rows stay constant while vo/histSize both grow).
            bool viewportShifted = (snap.topLineId != rs.lastTopLineId);
            rs.lastTopLineId = snap.topLineId;

            if (static_cast<int>(rs.rowShapingCache.size()) != snap.rows)
                rs.rowShapingCache.resize(snap.rows);

            if (viewportShifted || selectionChanged || (rs.dirty && !anyRowDirty)) {
                for (int row = 0; row < snap.rows; ++row)
                    allWorkItems.push_back((static_cast<uint32_t>(ti) << 16) | static_cast<uint32_t>(row));
            } else {
                for (int row = 0; row < snap.rows; ++row) {
                    if (snap.rowDirty[row] ||
                        (cursorMoved && row == snap.cursorY) ||
                        (popupFocusChanged && row == snap.cursorY))
                        allWorkItems.push_back((static_cast<uint32_t>(ti) << 16) | static_cast<uint32_t>(row));
                }
            }
        }
    }

    if (anyRunningAnimation && nextAnimationDueAt != std::numeric_limits<uint64_t>::max()) {
        if (platform_->animScheduler_) platform_->animScheduler_->scheduleAnimationAt(nextAnimationDueAt);
    }

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

    // --- Phase 2: Per-target GPU upload and rendering ---

    float blinkOpacity = frameState_.cursorBlinkOpacity;
    for (int ti = 0; ti < static_cast<int>(renderTargets.size()); ++ti) {
        auto& target = renderTargets[ti];
        TerminalEmulator* term = target.term;
        PaneRenderPrivate& rs = *target.rs;
        const Rect& paneRect = target.rect;
        if (paneRect.isEmpty()) continue;
        auto& info = resolveInfos[ti];
        bool needsRender = info.needsRender;
        bool cursorMoved = info.cursorMoved;
        int curY = info.curY;
        (void)term; (void)cursorMoved; (void)curY;
        const TerminalSnapshot& snap = rs.snapshot;

        if (needsRender || pngNeeded) {

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

            bool selectionVisible = snap.selection.valid || snap.selection.active;
            if (selectionVisible) {
                for (int row = 0; row < snap.rows; ++row) {
                    // segments is always sized == snap.rows (built by
                    // TerminalSnapshot::update).
                    int absRow = snap.segments[row].absRow;
                    for (int col = 0; col < snap.cols; ++col) {
                        if (snap.isCellSelected(col, absRow)) {
                            int idx = row * snap.cols + col;
                            rs.resolvedCells[idx].bg_color = 0xFF664422;
                            rs.resolvedCells[idx].fg_color = 0xFFFFFFFF;
                        }
                    }
                }
            }

            bool isFocused = target.isFocused;

            uint32_t totalCells = static_cast<uint32_t>(snap.cols) * snap.rows;
            ComputeState* cs = renderer_.computePool().acquire(totalCells);

            uint32_t glyphCount = std::max(rs.totalGlyphs, 1u);
            renderer_.computePool().ensureGlyphCapacity(cs, glyphCount);

            renderer_.uploadResolvedCells(queue_, cs, rs.resolvedCells.data(), totalCells);
            renderer_.uploadGlyphs(queue_, cs, rs.glyphBuffer.data(), rs.totalGlyphs);

            renderer_.updateFontAtlas(queue_, frameState_.fontName, *font);

            std::vector<Renderer::ImageDrawCmd> imageCmds;
            size_t imgSplitText = 0;
            std::unordered_set<uint64_t> seenPlacements;
            std::unordered_set<uint32_t> seenImageGPU;
            std::unordered_set<uint32_t> paneVisibleImages;
            float vpW = static_cast<float>(paneRect.w);
            float vpH = static_cast<float>(paneRect.h);

            for (int viewRow = 0; viewRow < snap.rows; ++viewRow) {
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

                    if (!seenImageGPU.count(ex->imageId)) {
                        seenImageGPU.insert(ex->imageId);
                        renderer_.useImageFrame(queue_, ex->imageId,
                            view.currentFrameIndex, view.totalFrames,
                            view.frameGeneration,
                            view.currentFrameRGBA, view.pixelWidth, view.pixelHeight);
                        paneVisibleImages.insert(ex->imageId);
                    }

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

            // Glyph color to use when a solid block cursor sits on top of the
            // cell. Defaulted to the palette background, matching wezterm's
            // classic-inversion default (cursor_fg = theme bg). A future config
            // knob can override; for now the shader only reads this field when
            // cursor_type == 1 so other cursor shapes pay no cost.
            auto packCursorTextColor = [](const TerminalEmulator::DefaultColors& dc) {
                return static_cast<uint32_t>(dc.bgR)
                     | (static_cast<uint32_t>(dc.bgG) << 8)
                     | (static_cast<uint32_t>(dc.bgB) << 16)
                     | (static_cast<uint32_t>(0xFFu) << 24);
            };

            // OSC 133 command outline — pass row range + edge flags. Rows
            // are viewport-relative; out-of-view edges are dropped via flags
            // so the outline stays visible even if partially scrolled away.
            params.selection_start_row    = 0;
            params.selection_end_row      = 0;
            params.selection_outline_flags = 0;
            params.selection_outline_color = 0;
            if (snap.selectedCommand) {
                // Segment 0 carries the absRow at viewport top.
                int origin = snap.segments.front().absRow;
                int startView = snap.selectedCommand->startAbsRow - origin;
                int endView   = snap.selectedCommand->endAbsRow   - origin;
                if (endView >= 0 && startView < snap.rows) {
                    int clampedStart = std::max(0, startView);
                    int clampedEnd   = std::min(snap.rows - 1, endView);
                    uint32_t flags = 0;
                    if (startView >= 0)          flags |= 0x1u;
                    if (endView   <  snap.rows)  flags |= 0x2u;
                    params.selection_start_row    = static_cast<uint32_t>(clampedStart);
                    params.selection_end_row      = static_cast<uint32_t>(clampedEnd);
                    params.selection_outline_flags = flags;
                    params.selection_outline_color = frameState_.commandOutlineColor;
                }
            }

            params.cursor_type = 0;
            // Popups and embeddeds both render their own cursor without the
            // scrollback-viewport offset and without popup-covering logic.
            if (target.isPopup || target.isEmbedded) {
                if (snap.cursorVisible &&
                    snap.cursorX >= 0 && snap.cursorX < snap.cols &&
                    snap.cursorY >= 0 && snap.cursorY < snap.rows) {
                    params.cursor_col   = static_cast<uint32_t>(snap.cursorX);
                    params.cursor_row   = static_cast<uint32_t>(snap.cursorY);
                    bool blink = isFocused && snap.cursorBlinking;
                    params.cursor_color = packCursorColor(snap.defaults, blink);
                    params.cursor_text_color = packCursorTextColor(snap.defaults);
                    params.cursor_type  = isFocused ? 1u : 2u;
                }
            } else {
                bool popupHasFocus = target.hasPopupFocus;
                // Cursor's viewport row. `cursorY + viewportOffset` holds
                // for every segment layout we actually produce: Row
                // segments have absRow == histSize - vo + viewRow, so
                // when the cursor's absRow (= histSize + cursorY) matches
                // segment[K].absRow the answer is K = cursorY + vo. Even
                // when an Embedded segment replaces the anchor row, the
                // same arithmetic holds because the Embedded inherits
                // that slot's absRow.
                int cursorViewRow = snap.cursorY + snap.viewportOffset;
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
                    params.cursor_text_color = packCursorTextColor(snap.defaults);
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

            std::vector<Renderer::ColrDrawCmd> colrDrawCmds;
            std::vector<Renderer::ColrRasterCmd> colrRasterCmds;
            for (const auto& rc : rs.rowShapingCache) {
                colrDrawCmds.insert(colrDrawCmds.end(), rc.colrDrawCmds.begin(), rc.colrDrawCmds.end());
                colrRasterCmds.insert(colrRasterCmds.end(), rc.colrRasterCmds.begin(), rc.colrRasterCmds.end());
            }

            wgpu::CommandEncoderDescriptor encDesc = {};
            wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);

            if (!colrRasterCmds.empty()) {
                renderer_.rasterizeColrGlyphs(encoder, queue_, frameState_.fontName, colrRasterCmds);
            }

            const float* tint = isFocused ? frameState_.activeTint : frameState_.inactiveTint;

            // OSC 133 dim: non-selected rows get rgb multiplied by commandDimFactor.
            // yMin/yMax are fragment-pixel Y bounds matching the clamped selection
            // range; anything outside is dimmed. If the selection is entirely off
            // the viewport, collapse the interval so every fragment is dimmed.
            Renderer::DimParams dim;
            if (snap.selectedCommand) {
                dim.factor = frameState_.commandDimFactor;
                int origin = snap.segments.front().absRow;
                int startView = snap.selectedCommand->startAbsRow - origin;
                int endView   = snap.selectedCommand->endAbsRow   - origin;
                if (endView < 0 || startView >= snap.rows) {
                    dim.yMin = 0.0f;
                    dim.yMax = 0.0f; // pos.y >= 0 always dims
                } else {
                    int clampedStart = std::max(0, startView);
                    int clampedEnd   = std::min(snap.rows - 1, endView);
                    dim.yMin = target.pixelOriginY + static_cast<float>(clampedStart) * frameState_.lineHeight;
                    dim.yMax = target.pixelOriginY + static_cast<float>(clampedEnd + 1) * frameState_.lineHeight;
                }
            }

            renderer_.renderToPane(encoder, queue_, frameState_.fontName, params, cs, newTexture->view, tint, dim, imageCmds, imgSplitText);

            if (!colrDrawCmds.empty()) {
                renderer_.renderColrQuads(encoder, queue_, newTexture->view,
                                          params.viewport_w, params.viewport_h,
                                          tint, dim, colrDrawCmds);
            }

            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
            pendingComputeRelease_.push_back(cs);

            if (rs.heldTexture) rs.pendingRelease.push_back(rs.heldTexture);
            rs.heldTexture = newTexture;
            rs.lastVisibleImageIds = std::move(paneVisibleImages);
            rs.dirty = false;
        }

        if (rs.heldTexture) {
            if (target.isEmbedded) {
                // Embedded's composite entry is emitted alongside the
                // parent pane's strips (see parent branch below), since
                // dst placement depends on the parent's viewport.
            } else if (!target.isPopup) {
                // Parent pane composite. Either a single full-pane blit
                // when no embeddeds displace rows, or a sequence of strips
                // around each embedded's anchor row (Model B displacement).
                // Alt-screen is already filtered at segment-build time
                // (Terminal::collectEmbeddedAnchors returns empty), so
                // Embedded segments only ever appear on the main screen.
                const TerminalSnapshot& psnap = rs.snapshot;
                const int paneTexH = paneRect.h;
                uint32_t texW = rs.heldTexture->texture.GetWidth();

                bool anyEmbedded = false;
                for (const auto& seg : psnap.segments) {
                    if (seg.kind == TerminalSnapshot::Segment::Kind::Embedded) {
                        anyEmbedded = true;
                        break;
                    }
                }

                if (!anyEmbedded) {
                    Renderer::CompositeEntry entry;
                    entry.texture = rs.heldTexture->texture;
                    entry.srcW = static_cast<uint32_t>(paneRect.w);
                    entry.srcH = static_cast<uint32_t>(paneRect.h);
                    entry.dstX = static_cast<uint32_t>(paneRect.x);
                    entry.dstY = static_cast<uint32_t>(paneRect.y);
                    compositeEntries.push_back(entry);
                } else {
                    const float cellH = frameState_.lineHeight;
                    const float cellW = frameState_.charWidth;
                    const float padT  = frameState_.padTop;
                    const float padL  = frameState_.padLeft;

                    auto pushStrip = [&](int srcYi, int dstYi, int hPx) {
                        if (hPx <= 0) return;
                        Renderer::CompositeEntry e;
                        e.texture = rs.heldTexture->texture;
                        e.srcX = 0;
                        e.srcY = static_cast<uint32_t>(std::max(0, srcYi));
                        e.srcW = texW;
                        e.srcH = static_cast<uint32_t>(hPx);
                        e.dstX = static_cast<uint32_t>(paneRect.x);
                        e.dstY = static_cast<uint32_t>(std::max(0, dstYi));
                        compositeEntries.push_back(e);
                    };

                    int srcY = 0;
                    int dstY = paneRect.y;
                    int embWPx = static_cast<int>(std::round(psnap.cols * cellW));

                    for (int viewRow = 0; viewRow < static_cast<int>(psnap.segments.size()); ++viewRow) {
                        const auto& seg = psnap.segments[viewRow];
                        if (seg.kind != TerminalSnapshot::Segment::Kind::Embedded) continue;
                        int anchorSrcY = static_cast<int>(std::round(padT + viewRow * cellH));
                        pushStrip(srcY, dstY, anchorSrcY - srcY);
                        dstY += std::max(0, anchorSrcY - srcY);

                        int embHPx = static_cast<int>(std::round(seg.rowCount * cellH));
                        std::string key = embeddedStateKey(target.paneId, seg.lineId);
                        auto emIt = embeddedRenderPrivate_.find(key);
                        if (emIt != embeddedRenderPrivate_.end() && emIt->second.heldTexture) {
                            Renderer::CompositeEntry e;
                            e.texture = emIt->second.heldTexture->texture;
                            e.srcX = 0; e.srcY = 0;
                            e.srcW = static_cast<uint32_t>(embWPx);
                            e.srcH = static_cast<uint32_t>(embHPx);
                            e.dstX = static_cast<uint32_t>(paneRect.x + padL);
                            e.dstY = static_cast<uint32_t>(dstY);
                            compositeEntries.push_back(e);
                        }
                        dstY += embHPx;
                        srcY = anchorSrcY + static_cast<int>(std::round(cellH));
                    }
                    pushStrip(srcY, dstY, paneTexH - srcY);
                }
            } else {
                // Popup: no scroll, no topPixelSubY — straight blit.
                Renderer::CompositeEntry entry;
                entry.texture = rs.heldTexture->texture;
                entry.srcW = static_cast<uint32_t>(paneRect.w);
                entry.srcH = static_cast<uint32_t>(paneRect.h);
                entry.dstX = static_cast<uint32_t>(paneRect.x);
                entry.dstY = static_cast<uint32_t>(paneRect.y);
                compositeEntries.push_back(entry);
            }
        }
    }

    std::unordered_set<uint32_t> imagesToRetain;
    for (const auto& [paneId, rs] : paneRenderPrivate_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    for (const auto& [key, rs] : popupRenderPrivate_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    for (const auto& [key, rs] : embeddedRenderPrivate_) {
        imagesToRetain.insert(rs.lastVisibleImageIds.begin(),
                              rs.lastVisibleImageIds.end());
    }
    renderer_.retainImagesOnly(imagesToRetain);

    if (frameState_.tabBarVisible && (frameState_.tabBarDirty || focusChanged)) {
        renderTabBar();
        frameState_.tabBarDirty = false;
    }

    if (tabBarTexture_ && !frameState_.tabBarRect.isEmpty()) {
        const Rect& tbRect = frameState_.tabBarRect;
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

    if (!compositeEntries.empty()) {
        wgpu::CommandEncoderDescriptor encDesc = {};
        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
        renderer_.composite(encoder, compositeTarget, compositeEntries);

        if (frameState_.dividersDirty || focusChanged) {
            const float* windowTint = frameState_.windowHasFocus
                ? frameState_.activeTint : frameState_.inactiveTint;
            renderer_.updateDividerViewport(queue_, frameFbW, frameFbH, windowTint);
            for (const auto& rpi : frameState_.panes) {
                const Uuid& pid = rpi.id;
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

        for (const auto& rpi : frameState_.panes) {
            auto it = paneRenderPrivate_.find(rpi.id);
            if (it == paneRenderPrivate_.end() || !it->second.dividerVB) continue;
            // Don't draw stale VBs for panes that no longer own a divider
            auto git = frameState_.dividerGeoms.find(rpi.id);
            if (git == frameState_.dividerGeoms.end() || !git->second.valid) continue;
            renderer_.drawDivider(encoder, compositeTarget,
                                   frameFbW, frameFbH, it->second.dividerVB);
        }

        {
            for (const auto& rpi : frameState_.panes) {
                auto rsIt = paneRenderPrivate_.find(rpi.id);
                if (rsIt == paneRenderPrivate_.end()) continue;
                auto& prs = rsIt->second;

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
                    const Rect& pr = rpi.rect;
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

                for (const auto& pb : prs.popupBorders) {
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.top);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.bottom);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.left);
                    renderer_.drawDivider(encoder, compositeTarget, frameFbW, frameFbH, pb.right);
                }
            }
        }

        if (frameState_.progressBarEnabled) for (const auto& rpi : frameState_.panes) {
            int st = rpi.progressState;
            if (st == 0) continue;
            const Rect& pr = rpi.rect;
            float barHeight = frameState_.progressBarHeight;
            float barY = static_cast<float>(pr.y);
            float barX = static_cast<float>(pr.x);
            float barW = static_cast<float>(pr.w);
            float edgeSoft = 40.0f * frameState_.contentScaleX;

            Renderer::ProgressBarParams pbp{};
            pbp.h = barHeight;
            pbp.a = 1.0f;

            if (st == 1 || st == 2) {
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
                double now = static_cast<double>(TerminalEmulator::mono()) / 1000.0;
                float t = static_cast<float>(std::fmod(now, 2.0) / 2.0);
                float segFrac = 0.3f;
                float segW = barW * segFrac;
                float overshoot = segW;
                float segX = barX - overshoot + t * (barW + 2.0f * overshoot);
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
            wgpu::Texture srcTexture = compositeTarget;
            uint32_t srcW = frameFbW, srcH = frameFbH;

            const auto& target = debugIPC->pngTarget();
            if (target.starts_with("pane:")) {
                // "pane:<uuid>" matches the render-private entry directly.
                // "pane:<N>" (legacy integer id used by tests) resolves to
                // the Nth pane in visible order — kept so the test harness
                // doesn't have to plumb real UUIDs through IPC.
                std::string rest = target.substr(5);
                Uuid paneId = Uuid::fromString(rest);
                if (paneId.isNil() && !rest.empty()) {
                    // Numeric index into frameState_.panes.
                    try {
                        size_t idx = std::stoul(rest);
                        if (idx < frameState_.panes.size())
                            paneId = frameState_.panes[idx].id;
                    } catch (...) { /* leave nil */ }
                }
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

            uint32_t copyX = 0, copyY = 0, copyW = srcW, copyH = srcH;
            const auto& cellRect = debugIPC->pngCellRect();
            if (cellRect.valid) {
                copyX = static_cast<uint32_t>(cellRect.x * frameState_.charWidth + frameState_.padLeft);
                copyY = static_cast<uint32_t>(cellRect.y * frameState_.lineHeight + frameState_.padTop);
                copyW = static_cast<uint32_t>(cellRect.w * frameState_.charWidth);
                copyH = static_cast<uint32_t>(cellRect.h * frameState_.lineHeight);
                if (copyX + copyW > srcW) copyW = srcW > copyX ? srcW - copyX : 0;
                if (copyY + copyH > srcH) copyH = srcH > copyY ? srcH - copyY : 0;
            }

            if (copyW == 0 || copyH == 0) {
                debugIPC->onPngReady("");
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

                DebugIPC* ipc = debugIPC;
                uint32_t w = copyW, h = copyH;
                debugIPC->markReadbackInProgress();
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
                        platform_->eventLoop_->post([ipc, b64 = std::move(b64)] {
                            ipc->onPngReady(b64);
                        });
                    });
            }
        } else {
            wgpu::CommandBuffer commands = encoder.Finish();
            queue_.Submit(1, &commands);
        }

        // Drain every pending-release list, not just the active tab's.
        // Panes belonging to inactive tabs can accumulate textures here
        // (from tab-switch texture drops); without this they'd sit until
        // the tab reactivates, leaking pool capacity.
        std::vector<PooledTexture*> toRelease;
        auto drainPendingRelease = [&](PaneRenderPrivate& rs) {
            toRelease.insert(toRelease.end(),
                rs.pendingRelease.begin(), rs.pendingRelease.end());
            rs.pendingRelease.clear();
        };
        for (auto& [id, rs] : paneRenderPrivate_) drainPendingRelease(rs);
        for (auto& [key, rs] : popupRenderPrivate_) drainPendingRelease(rs);
        for (auto& [key, rs] : embeddedRenderPrivate_) drainPendingRelease(rs);
        toRelease.insert(toRelease.end(), pendingTabBarRelease_.begin(), pendingTabBarRelease_.end());
        pendingTabBarRelease_.clear();
        toRelease.insert(toRelease.end(), pendingDestroyRelease_.begin(), pendingDestroyRelease_.end());
        pendingDestroyRelease_.clear();
        auto texturesToRelease = toRelease;
        auto computeToRelease  = pendingComputeRelease_;
        pendingComputeRelease_.clear();
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

    if (!headless) {
        surface_.Present();
    }
    obs::notifyFrame();
}

#include "PlatformDawn.h"
#include "ProceduralGlyphTable.h"
#include "Utf8.h"
#include "Utils.h"
#include "FontResolver.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::vector<uint8_t> loadFontFile(const std::string& path) { return io::loadFile(path); }

static uint32_t parseHexColor(const std::string& hex, uint32_t def = 0xFF000000) {
    return color::parseHexRGBA(hex, def);
}

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }

// ========================================================================
// Tab bar
// ========================================================================

void PlatformDawn::initTabBar(const TabBarConfig& cfg)
{
    if (cfg.style == "hidden") {
        for (auto& tab : tabs_)
            tab->layout()->setTabBar(0, cfg.position);
        return;
    }

    // For "auto" mode with 1 tab, still load fonts but set height to 0

    // Resolve font
    std::string fontPath = cfg.font.empty() ? primaryFontPath_
                                             : resolveFontFamily(cfg.font);
    if (fontPath.empty()) fontPath = primaryFontPath_;
    float fontSize = (cfg.font_size > 0.0f) ? cfg.font_size * frameState_.contentScaleX : fontSize_;
    tabBarFontSize_ = fontSize;

    auto fontData = loadFontFile(fontPath);
    if (!fontData.empty()) {
        std::vector<std::vector<uint8_t>> fl = {fontData};
        auto nerdPath = (fs::path(exeDir_) / "fonts" / "nerd" / "SymbolsNerdFontMono-Regular.ttf").string();
        auto nerdData = loadFontFile(nerdPath);
        if (!nerdData.empty()) fl.push_back(std::move(nerdData));
        textSystem_.registerFont(tabBarFontName_, fl, 48.0f);
        textSystem_.setPrimaryFontPath(tabBarFontName_, fontPath);
    }

    const FontData* font = textSystem_.getFont(tabBarFontName_);
    if (font) {
        // GPU upload deferred to render thread via pending_.tabBarFontAtlasChanged
        pending_.tabBarFontAtlasChanged = true;
        float scale = tabBarFontSize_ / font->baseSize;
        tabBarLineHeight_ = font->lineHeight * scale;
        const auto& shaped = textSystem_.shapeText(tabBarFontName_, "M", tabBarFontSize_);
        tabBarCharWidth_ = shaped.width;
        if (tabBarCharWidth_ < 1.0f) tabBarCharWidth_ = tabBarFontSize_ * 0.6f;
    } else {
        tabBarLineHeight_ = lineHeight_;
        tabBarCharWidth_  = charWidth_;
    }

    int tabBarH = tabBarVisible() ? static_cast<int>(std::ceil(tabBarLineHeight_)) : 0;
    for (auto& tab : tabs_)
        tab->layout()->setTabBar(tabBarH, cfg.position);

    // Parse colors
    tbBgColor_         = parseHexColor(cfg.colors.background);
    tbActiveBgColor_   = parseHexColor(cfg.colors.active_bg);
    tbActiveFgColor_   = parseHexColor(cfg.colors.active_fg);
    tbInactiveBgColor_ = parseHexColor(cfg.colors.inactive_bg);
    tbInactiveFgColor_ = parseHexColor(cfg.colors.inactive_fg);

    // Parse progress bar settings
    frameState_.progressBarHeight = cfg.progress_height * frameState_.contentScaleX;
    {
        uint8_t r, g, b;
        if (color::parseHex(cfg.progress_color, r, g, b)) {
            frameState_.progressColorR = r / 255.0f;
            frameState_.progressColorG = g / 255.0f;
            frameState_.progressColorB = b / 255.0f;
        }
    }

    tabBarDirty_ = true;
}


void PlatformDawn::renderTabBar()
{
    if (!frameState_.tabBarVisible) return;
    if (frameState_.tabBarCells.empty()) return;
    const PaneRect& tbRect = frameState_.tabBarRect;
    if (tbRect.isEmpty()) return;

    int cols = frameState_.tabBarCols;
    if (cols <= 0) return;

    // Convert pre-computed TabBarCells → ResolvedCells + glyphs (GPU upload)
    std::vector<ResolvedCell> cells(static_cast<size_t>(cols));
    std::vector<GlyphEntry> tabBarGlyphs;

    FontData* font = const_cast<FontData*>(textSystem_.getFont(frameState_.tabBarFontName));
    if (!font) return;
    float scale = frameState_.tabBarFontSize / font->baseSize;

    auto resolveTabBarGlyph = [&](const ShapedText& shaped) -> const GlyphInfo* {
        if (shaped.glyphs.empty()) return nullptr;
        uint64_t glyphId = shaped.glyphs[0].glyphId;
        if ((glyphId & 0xFFFFFFFF) == 0) return nullptr;
        auto it = font->glyphs.find(glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        return &it->second;
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

        const ShapedText& shaped = textSystem_.shapeText(
            frameState_.tabBarFontName, tbc.ch, frameState_.tabBarFontSize);
        const GlyphInfo* gi = resolveTabBarGlyph(shaped);
        if (gi) {
            GlyphEntry entry;
            entry.atlas_offset = gi->atlas_offset;
            entry.ext_min_x = gi->ext_min_x; entry.ext_min_y = gi->ext_min_y;
            entry.ext_max_x = gi->ext_max_x; entry.ext_max_y = gi->ext_max_y;
            entry.upem = gi->upem; entry.x_offset = 0.0f; entry.y_offset = 0.0f;
            rc.glyph_offset = static_cast<uint32_t>(tabBarGlyphs.size());
            rc.glyph_count = 1;
            tabBarGlyphs.push_back(entry);
        }
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
    renderer_.renderToPane(encoder, queue_, frameState_.tabBarFontName, params, cs, newTexture->view, windowTint, {});
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    pendingComputeRelease_.push_back(cs);

    if (tabBarTexture_) pendingTabBarRelease_.push_back(tabBarTexture_);
    tabBarTexture_ = newTexture;
    // tabBarDirty_ consumed from renderState_ under the lock at frame start
}


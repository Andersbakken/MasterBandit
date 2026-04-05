#include "PlatformDawn.h"
#include "Log.h"
#include "Utils.h"
#include "FontResolver.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::vector<uint8_t> loadFontFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

static uint32_t parseHexColor(const std::string& hex, uint32_t def = 0xFF000000) {
    return color::parseHexBGRA(hex, def);
}

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
    float fontSize = (cfg.font_size > 0.0f) ? cfg.font_size * contentScaleX_ : fontSize_;
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
        renderer_.uploadFontAtlas(queue_, tabBarFontName_, *font);
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
    progressBarHeight_ = cfg.progress_height * contentScaleX_;
    {
        uint8_t r, g, b;
        if (color::parseHex(cfg.progress_color, r, g, b)) {
            progressColorR_ = r / 255.0f;
            progressColorG_ = g / 255.0f;
            progressColorB_ = b / 255.0f;
        }
    }

    tabBarDirty_ = true;
}


void PlatformDawn::renderTabBar()
{
    if (!tabBarVisible()) return;
    if (tabs_.empty()) return;

    Tab* active = activeTab();
    if (!active) return;
    PaneRect tbRect = active->layout()->tabBarRect(fbWidth_, fbHeight_);
    if (tbRect.isEmpty()) return;

    int cols = std::max(1, static_cast<int>(tbRect.w / tabBarCharWidth_));
    tabBarCols_ = cols;

    // Build resolved cells for 1 row
    std::vector<ResolvedCell> cells(static_cast<size_t>(cols));
    std::vector<GlyphEntry> tabBarGlyphs;
    for (auto& c : cells) {
        c.glyph_offset = 0;
        c.glyph_count = 0;
        c.fg_color = tbInactiveFgColor_;
        c.bg_color = tbBgColor_;
        c.underline_info = 0;
    }

    // Powerline separator U+E0B0
    const std::string SEP_RIGHT = "\xee\x82\xb0";

    // Indeterminate animation glyphs
    static const char32_t kAnimGlyphs[] = {
        0xf0130, 0xf0a9e, 0xf0a9f, 0xf0aa0, 0xf0aa1,
        0xf0aa2, 0xf0aa3, 0xf0aa4, 0xf0aa5
    };
    static constexpr int kAnimCount = 9;

    auto cp32ToUtf8 = [](char32_t cp) -> std::string {
        std::string s;
        if (cp < 0x80) { s += static_cast<char>(cp); }
        else if (cp < 0x800) { s += static_cast<char>(0xC0|(cp>>6)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else if (cp < 0x10000) { s += static_cast<char>(0xE0|(cp>>12)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        else { s += static_cast<char>(0xF0|(cp>>18)); s += static_cast<char>(0x80|((cp>>12)&0x3F)); s += static_cast<char>(0x80|((cp>>6)&0x3F)); s += static_cast<char>(0x80|(cp&0x3F)); }
        return s;
    };

    auto progressGlyph = [&](Tab* tab) -> std::string {
        Pane* focusedPane = tab->layout()->focusedPane();
        int st = focusedPane ? focusedPane->progressState() : 0;
        int pct = focusedPane ? focusedPane->progressPct() : 0;
        if (st == 0) return "";
        int idx;
        if (st == 3) {
            // Bounce: 0..8..0..8.. (period = 2 * (kAnimCount - 1))
            int period = 2 * (kAnimCount - 1);
            int pos = tabBarAnimFrame_ % period;
            idx = (pos < kAnimCount) ? pos : period - pos;
        } else if (st == 1 || st == 2) {
            idx = std::clamp(pct * kAnimCount / 100, 0, kAnimCount - 1);
        } else {
            return "";
        }
        return cp32ToUtf8(kAnimGlyphs[idx]);
    };

    // Helper: count UTF-8 codepoints in a string
    auto cpLen = [](const std::string& s) -> int {
        int w = 0;
        const char* p = s.c_str();
        while (*p) {
            uint8_t b = static_cast<uint8_t>(*p);
            if (b < 0x80) p++; else if ((b & 0xE0) == 0xC0) p += 2;
            else if ((b & 0xF0) == 0xE0) p += 3; else p += 4;
            w++;
        }
        return w;
    };

    // Helper: truncate UTF-8 string to maxCp codepoints, append ellipsis if truncated
    auto truncUtf8 = [](const std::string& s, int maxCp) -> std::string {
        if (maxCp <= 0) return {};
        int cp = 0;
        const char* p = s.c_str();
        while (*p && cp < maxCp) {
            uint8_t b = static_cast<uint8_t>(*p);
            if (b < 0x80) p++; else if ((b & 0xE0) == 0xC0) p += 2;
            else if ((b & 0xF0) == 0xE0) p += 3; else p += 4;
            cp++;
        }
        if (*p) return std::string(s.c_str(), p) + "\xe2\x80\xa6";
        return s;
    };

    struct TabInfo {
        std::string prefix;  // " [N] " or " icon [N] "
        std::string title;   // full title
        std::string text;    // final rendered text (prefix + truncated title + " ")
        int width;
        bool isActive;
        uint32_t bgColor, fgColor;
    };

    // Build tab info with full titles
    std::vector<TabInfo> tabInfos;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        Tab* tab = tabs_[i].get();
        bool isActive = (i == activeTabIdx_);
        TabInfo ti;
        ti.isActive = isActive;
        ti.bgColor = isActive ? tbActiveBgColor_ : tbInactiveBgColor_;
        ti.fgColor = isActive ? tbActiveFgColor_ : tbInactiveFgColor_;

        ti.prefix = " ";
        std::string pg = tabBarConfig_.progress_icon ? progressGlyph(tab) : "";
        if (!pg.empty()) { ti.prefix += pg; ti.prefix += " "; }
        if (!tab->icon().empty()) { ti.prefix += tab->icon(); ti.prefix += " "; }
        ti.prefix += "[";
        ti.prefix += std::to_string(i + 1);
        ti.prefix += "] ";
        ti.title = tab->title();
        tabInfos.push_back(std::move(ti));
    }

    int numTabs = static_cast<int>(tabInfos.size());
    int sepWidth = 1; // powerline separator per tab
    int availCols = cols;

    // Determine max title length that fits all tabs
    // Start with configured max, shrink until everything fits or titles are gone
    int maxTitleLen = tabBarConfig_.max_title_length > 0 ? tabBarConfig_.max_title_length : 9999;
    for (;;) {
        int total = 0;
        for (auto& ti : tabInfos) {
            std::string truncTitle = truncUtf8(ti.title, maxTitleLen);
            ti.text = ti.prefix + truncTitle + (truncTitle.empty() ? "" : " ");
            if (ti.text.back() != ' ') ti.text += " ";
            ti.width = cpLen(ti.text);
            total += ti.width + sepWidth;
        }
        if (total <= availCols || maxTitleLen <= 0) break;
        maxTitleLen--;
    }

    // Check if we still overflow at minimum (no title text at all)
    int totalWidth = 0;
    for (auto& ti : tabInfos) totalWidth += ti.width + sepWidth;

    // Determine visible tab range if overflow
    int visStart = 0, visEnd = numTabs;
    bool overflowLeft = false, overflowRight = false;
    if (totalWidth > availCols && numTabs > 1) {
        // Start from active tab, expand outward until we fill
        visStart = activeTabIdx_;
        visEnd = activeTabIdx_ + 1;
        int used = tabInfos[activeTabIdx_].width + sepWidth;
        int indicatorWidth = 2; // "< " or " >" indicator

        while (visStart > 0 || visEnd < numTabs) {
            bool expanded = false;
            // Try expanding right
            if (visEnd < numTabs) {
                int need = tabInfos[visEnd].width + sepWidth + (visEnd + 1 < numTabs ? indicatorWidth : 0);
                if (used + need + (visStart > 0 ? indicatorWidth : 0) <= availCols) {
                    used += tabInfos[visEnd].width + sepWidth;
                    visEnd++;
                    expanded = true;
                }
            }
            // Try expanding left
            if (visStart > 0) {
                int need = tabInfos[visStart - 1].width + sepWidth + (visStart - 1 > 0 ? indicatorWidth : 0);
                if (used + need + (visEnd < numTabs ? indicatorWidth : 0) <= availCols) {
                    visStart--;
                    used += tabInfos[visStart].width + sepWidth;
                    expanded = true;
                }
            }
            if (!expanded) break;
        }
        overflowLeft = (visStart > 0);
        overflowRight = (visEnd < numTabs);
    }

    FontData* font = const_cast<FontData*>(textSystem_.getFont(tabBarFontName_));
    if (!font) return;
    float scale = tabBarFontSize_ / font->baseSize;

    auto resolveTabBarGlyph = [&](const ShapedText& shaped) -> const GlyphInfo* {
        if (shaped.glyphs.empty()) return nullptr;
        uint64_t glyphId = shaped.glyphs[0].glyphId;
        if ((glyphId & 0xFFFFFFFF) == 0) return nullptr; // .notdef
        auto it = font->glyphs.find(glyphId);
        if (it == font->glyphs.end() || it->second.is_empty) return nullptr;
        return &it->second;
    };

    auto placeChar = [&](int& col, const std::string& utf8ch, uint32_t fg, uint32_t bg, bool stretchY = false) {
        if (col >= cols) return;
        ResolvedCell& rc = cells[static_cast<size_t>(col)];
        rc.fg_color = fg;
        rc.bg_color = bg;
        // System font fallback happens automatically inside shapeText
        const ShapedText& shaped = textSystem_.shapeText(tabBarFontName_, utf8ch, tabBarFontSize_);
        const GlyphInfo* gi = resolveTabBarGlyph(shaped);

        if (gi) {
            GlyphEntry entry;
            entry.atlas_offset = gi->atlas_offset;
            entry.ext_min_x = gi->ext_min_x;
            entry.ext_min_y = gi->ext_min_y;
            entry.ext_max_x = gi->ext_max_x;
            entry.ext_max_y = gi->ext_max_y;
            entry.upem = gi->upem;
            if (stretchY) {
                entry.upem = gi->upem | 0x80000000u;
            }
            entry.x_offset = 0.0f;
            entry.y_offset = 0.0f;
            rc.glyph_offset = static_cast<uint32_t>(tabBarGlyphs.size());
            rc.glyph_count = 1;
            tabBarGlyphs.push_back(entry);
        }
        col++;
    };

    int col = 0;

    // Left overflow indicator
    if (overflowLeft) {
        placeChar(col, "\xe2\x97\x80", tbInactiveFgColor_, tbBgColor_); // U+25C0 ◀
        placeChar(col, " ", tbInactiveFgColor_, tbBgColor_);
    }

    for (int i = visStart; i < visEnd; ++i) {
        auto& ti = tabInfos[i];
        const char* p = ti.text.c_str();
        while (*p && col < cols) {
            uint8_t b = static_cast<uint8_t>(*p);
            int len = 1;
            if ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            std::string ch(p, static_cast<size_t>(len));
            placeChar(col, ch, ti.fgColor, ti.bgColor);
            p += len;
        }
        // Powerline separator
        uint32_t nextBg = (i + 1 < visEnd)
            ? tabInfos[i + 1].bgColor : tbBgColor_;
        placeChar(col, SEP_RIGHT, ti.bgColor, nextBg, true);
    }

    // Right overflow indicator
    if (overflowRight && col + 2 <= cols) {
        placeChar(col, " ", tbInactiveFgColor_, tbBgColor_);
        placeChar(col, "\xe2\x96\xb6", tbInactiveFgColor_, tbBgColor_); // U+25B6 ▶
    }

    for (; col < cols; col++) {
        cells[static_cast<size_t>(col)].bg_color = tbBgColor_;
        cells[static_cast<size_t>(col)].fg_color = tbInactiveFgColor_;
    }

    renderer_.updateFontAtlas(queue_, tabBarFontName_, *font);

    ComputeState* cs = renderer_.computePool().acquire(static_cast<uint32_t>(cols));
    uint32_t tbGlyphCount = std::max(static_cast<uint32_t>(tabBarGlyphs.size()), 1u);
    renderer_.computePool().ensureGlyphCapacity(cs, tbGlyphCount);
    renderer_.uploadResolvedCells(queue_, cs, cells.data(), static_cast<uint32_t>(cols));
    if (!tabBarGlyphs.empty())
        renderer_.uploadGlyphs(queue_, cs, tabBarGlyphs.data(), static_cast<uint32_t>(tabBarGlyphs.size()));

    TerminalComputeParams params = {};
    params.cols = static_cast<uint32_t>(cols);
    params.rows = 1;
    params.cell_width = tabBarCharWidth_;
    params.cell_height = tabBarLineHeight_;
    params.viewport_w = static_cast<float>(tbRect.w);
    params.viewport_h = static_cast<float>(tbRect.h);
    params.font_ascender = font->ascender * scale;
    params.font_size = tabBarFontSize_;
    params.pane_origin_x = 0.0f;
    params.pane_origin_y = 0.0f;
    params.max_text_vertices = cs->maxTextVertices;

    PooledTexture* newTexture = texturePool_.acquire(
        static_cast<uint32_t>(tbRect.w),
        static_cast<uint32_t>(std::ceil(tabBarLineHeight_)));

    wgpu::CommandEncoderDescriptor encDesc = {};
    wgpu::CommandEncoder encoder = device_.CreateCommandEncoder(&encDesc);
    static constexpr float kNoTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    renderer_.renderToPane(encoder, queue_, tabBarFontName_, params, cs, newTexture->view, kNoTint, {});
    wgpu::CommandBuffer commands = encoder.Finish();
    queue_.Submit(1, &commands);
    pendingComputeRelease_.push_back(cs);

    if (tabBarTexture_) pendingTabBarRelease_.push_back(tabBarTexture_);
    tabBarTexture_ = newTexture;
    tabBarDirty_ = false;
}


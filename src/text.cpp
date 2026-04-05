#include "text.h"

#include <hb.h>
#include <hb-ot.h>
#include <hb-gpu.h>

#include <SheenBidi/SheenBidi.h>
#include <linebreak.h>

#include <algorithm>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

// --- Helper: make combined glyph key ---
static inline uint64_t glyphKey(uint32_t fontIndex, uint32_t glyphId)
{
    return (static_cast<uint64_t>(fontIndex) << 32) | glyphId;
}

// --- TextSystem implementation ---

TextSystem::~TextSystem()
{
    for (auto& [name, font] : fonts_) {
        for (auto& entry : font.hbFonts) {
            if (entry.gpuDraw) hb_gpu_draw_destroy(entry.gpuDraw);
            if (entry.hbFont) hb_font_destroy(entry.hbFont);
            if (entry.hbFace) hb_face_destroy(entry.hbFace);
            if (entry.hbBlob) hb_blob_destroy(entry.hbBlob);
        }
    }
}

void TextSystem::ensureGlyphEncoded(FontData& font, uint32_t fontIndex, uint32_t glyphId)
{
    uint64_t key = glyphKey(fontIndex, glyphId);

    // Fast path: read lock check
    {
        std::shared_lock lock(font.mutex);
        if (font.glyphs.count(key)) return;
    }

    // Cache miss — extract glyph data without holding any lock.
    // Use a thread-local hb_gpu_draw_t since the per-entry one isn't thread-safe.
    thread_local hb_gpu_draw_t* tlGpuDraw = nullptr;
    if (!tlGpuDraw) {
        tlGpuDraw = hb_gpu_draw_create_or_fail();
    }
    hb_gpu_draw_t* g = tlGpuDraw;

    hb_font_t* hbFont;
    hb_face_t* hbFace;
    {
        std::shared_lock lock(font.mutex);
        auto& entry = font.hbFonts[fontIndex];
        hbFont = entry.hbFont;
        hbFace = entry.hbFace;
    }

    unsigned int upem = hb_face_get_upem(hbFace);
    float pixelScale = font.baseSize / static_cast<float>(upem);
    float adv = hb_font_get_glyph_h_advance(hbFont, glyphId) * pixelScale;

    hb_gpu_draw_reset(g);
    hb_gpu_draw_glyph(g, hbFont, glyphId);
    hb_blob_t* blob = hb_gpu_draw_encode(g);

    hb_glyph_extents_t ext;
    hb_gpu_draw_get_extents(g, &ext);

    unsigned int blobLen = 0;
    const char* blobData = hb_blob_get_data(blob, &blobLen);

    GlyphInfo info{};
    info.upem = upem;
    info.advance = adv;

    if (blobLen == 0) {
        info.is_empty = true;
        info.atlas_offset = 0;
        info.ext_min_x = 0;
        info.ext_min_y = 0;
        info.ext_max_x = 0;
        info.ext_max_y = 0;
        std::unique_lock lock(font.mutex);
        if (!font.glyphs.count(key))
            font.glyphs[key] = info;
        hb_gpu_draw_recycle_blob(g, blob);
        return;
    }

    info.is_empty = false;
    info.ext_min_x = static_cast<float>(ext.x_bearing);
    info.ext_min_y = static_cast<float>(ext.y_bearing + ext.height);
    info.ext_max_x = static_cast<float>(ext.x_bearing + ext.width);
    info.ext_max_y = static_cast<float>(ext.y_bearing);

    const int16_t* src = reinterpret_cast<const int16_t*>(blobData);
    uint32_t numInt16 = blobLen / sizeof(int16_t);
    uint32_t numTexels = (numInt16 + 3) / 4;

    // Write lock to insert into atlas and glyph map
    {
        std::unique_lock lock(font.mutex);

        // Double-check: another thread may have inserted while we extracted
        if (font.glyphs.count(key)) {
            hb_gpu_draw_recycle_blob(g, blob);
            return;
        }

        info.atlas_offset = font.atlasUsed;

        uint32_t needed = (font.atlasUsed + numTexels) * 4;
        if (font.atlasData.size() < needed) {
            font.atlasData.resize(needed * 2, 0);
        }

        for (uint32_t i = 0; i < numInt16; i++) {
            font.atlasData[font.atlasUsed * 4 + i] = static_cast<int32_t>(src[i]);
        }
        for (uint32_t i = numInt16; i < numTexels * 4; i++) {
            font.atlasData[font.atlasUsed * 4 + i] = 0;
        }
        font.atlasUsed += numTexels;

        font.glyphs[key] = info;
    }
    hb_gpu_draw_recycle_blob(g, blob);
}

bool TextSystem::registerFont(const std::string& name,
                               const std::vector<std::vector<uint8_t>>& ttfDataList,
                               float baseSize)
{
    if (ttfDataList.empty()) {
        spdlog::error("registerFont '{}': no font data provided", name);
        return false;
    }

    FontData& font = fonts_[name];
    font.name = name;
    font.baseSize = baseSize;

    // 1. Load all fonts via HarfBuzz and create GPU draw objects
    for (const auto& ttfData : ttfDataList) {
        FontData::HBEntry entry;
        entry.hbBlob = hb_blob_create(reinterpret_cast<const char*>(ttfData.data()),
                                       static_cast<unsigned int>(ttfData.size()),
                                       HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
        entry.hbFace = hb_face_create(entry.hbBlob, 0);
        entry.hbFont = hb_font_create(entry.hbFace);
        entry.gpuDraw = hb_gpu_draw_create_or_fail();
        if (!entry.gpuDraw) {
            spdlog::error("registerFont '{}': hb_gpu_draw_create_or_fail() returned null", name);
            hb_font_destroy(entry.hbFont);
            hb_face_destroy(entry.hbFace);
            hb_blob_destroy(entry.hbBlob);
            fonts_.erase(name);
            return false;
        }
        font.hbFonts.push_back(entry);
    }

    // Use primary font for metrics
    const auto& primary = font.hbFonts[0];
    unsigned int upem = hb_face_get_upem(primary.hbFace);
    float pixelScale = baseSize / static_cast<float>(upem);

    // Get font metrics from primary
    hb_font_extents_t extents;
    if (hb_font_get_h_extents(primary.hbFont, &extents)) {
        font.ascender = extents.ascender * pixelScale;
        font.descender = extents.descender * pixelScale;
        font.lineHeight = (extents.ascender - extents.descender + extents.line_gap) * pixelScale;
    } else {
        font.ascender = baseSize * 0.8f;
        font.descender = -baseSize * 0.2f;
        font.lineHeight = baseSize * 1.2f;
    }

    // Pre-allocate atlas storage (will grow as needed)
    font.atlasData.resize(1024 * 1024, 0); // 4MB initial (1M vec4<i32>)

    // Pre-encode ASCII 32-126 using primary font
    for (uint32_t cp = 32; cp <= 126; cp++) {
        for (uint32_t fi = 0; fi < font.hbFonts.size(); fi++) {
            uint32_t gid;
            if (hb_font_get_nominal_glyph(font.hbFonts[fi].hbFont, cp, &gid)) {
                font.codepointToFontIndex[cp] = fi;
                ensureGlyphEncoded(font, fi, gid);
                break;
            }
        }
    }

    spdlog::info("Registered font '{}': {} pre-encoded glyphs, atlas {} texels, baseSize={:.0f}, {} font(s)",
                name, static_cast<uint32_t>(font.glyphs.size()),
                font.atlasUsed, baseSize, font.hbFonts.size());

    return true;
}

bool TextSystem::addFallbackFont(const std::string& name, const std::vector<uint8_t>& ttfData)
{
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return false;

    FontData& font = it->second;

    // Create HarfBuzz objects without holding lock
    FontData::HBEntry entry;
    entry.hbBlob = hb_blob_create(reinterpret_cast<const char*>(ttfData.data()),
                                   static_cast<unsigned int>(ttfData.size()),
                                   HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
    entry.hbFace = hb_face_create(entry.hbBlob, 0);
    entry.hbFont = hb_font_create(entry.hbFace);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        spdlog::error("addFallbackFont '{}': hb_gpu_draw_create_or_fail() returned null", name);
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return false;
    }

    std::unique_lock lock(font.mutex);
    font.hbFonts.push_back(entry);

    spdlog::info("Added fallback font #{} to '{}'", font.hbFonts.size() - 1, name);
    return true;
}

void TextSystem::setSystemFallback(SystemFallbackFn fn)
{
    systemFallback_ = std::move(fn);
}

void TextSystem::setPrimaryFontPath(const std::string& name, const std::string& path)
{
    fontPrimaryPaths_[name] = path;
}

bool TextSystem::addSyntheticBoldVariant(const std::string& name, float xStrength, float yStrength)
{
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return false;

    FontData& font = it->second;
    if (font.hbFonts.empty()) return false;

    const auto& primary = font.hbFonts[0];

    FontData::HBEntry entry;
    entry.hbBlob = hb_blob_reference(primary.hbBlob);
    entry.hbFace = hb_face_reference(primary.hbFace);
    entry.hbFont = hb_font_create(entry.hbFace);
    hb_font_set_synthetic_bold(entry.hbFont, xStrength, yStrength, false);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        spdlog::error("addSyntheticBoldVariant '{}': hb_gpu_draw_create_or_fail() returned null", name);
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return false;
    }

    entry.baseFontIndex = 0;
    entry.style = {.bold = true};

    uint32_t newFi = static_cast<uint32_t>(font.hbFonts.size());
    font.hbFonts.push_back(entry);

    // Register in styled variants map
    uint64_t variantKey = (static_cast<uint64_t>(0) << 8) | entry.style.key();
    font.styledVariants[variantKey] = newFi;

    spdlog::info("Added synthetic bold variant to '{}' (fi={})", name, newFi);
    return true;
}

// --- Font registry: resolve fonts with style ---

static inline uint32_t decodeUtf8(const uint8_t*& p)
{
    if (*p < 0x80) return *p++;
    if ((*p & 0xE0) == 0xC0) { uint32_t cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F); p += 2; return cp; }
    if ((*p & 0xF0) == 0xE0) { uint32_t cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F); p += 3; return cp; }
    uint32_t cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F); p += 4; return cp;
}

uint32_t TextSystem::getStyledVariant(FontData& font, uint32_t baseFi, FontStyle style)
{
    if (style == FontStyle{}) return baseFi;

    uint64_t variantKey = (static_cast<uint64_t>(baseFi) << 8) | style.key();

    // Fast path: read lock
    {
        std::shared_lock lock(font.mutex);
        auto it = font.styledVariants.find(variantKey);
        if (it != font.styledVariants.end()) return it->second;
    }

    // Create synthetic variant without holding lock
    hb_blob_t* blob;
    hb_face_t* face;
    {
        std::shared_lock lock(font.mutex);
        auto& base = font.hbFonts[baseFi];
        blob = hb_blob_reference(base.hbBlob);
        face = hb_face_reference(base.hbFace);
    }

    FontData::HBEntry entry;
    entry.hbBlob = blob;
    entry.hbFace = face;
    entry.hbFont = hb_font_create(entry.hbFace);
    if (style.bold)
        hb_font_set_synthetic_bold(entry.hbFont, boldStrengthX_, boldStrengthY_, false);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return baseFi;
    }
    entry.baseFontIndex = baseFi;
    entry.style = style;

    // Write lock to insert
    std::unique_lock lock(font.mutex);

    // Double-check
    auto it = font.styledVariants.find(variantKey);
    if (it != font.styledVariants.end()) {
        // Another thread created it — discard ours
        hb_gpu_draw_destroy(entry.gpuDraw);
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return it->second;
    }

    uint32_t newFi = static_cast<uint32_t>(font.hbFonts.size());
    font.hbFonts.push_back(entry);
    font.styledVariants[variantKey] = newFi;

    spdlog::debug("Created styled variant fi={} (base={} bold={}) for '{}'",
                  newFi, baseFi, style.bold, font.name);
    return newFi;
}

uint32_t TextSystem::resolveSegment(FontData& font, const uint8_t* text, size_t len, FontStyle style)
{
    uint32_t primaryFi = getStyledVariant(font, 0, style);

    std::shared_lock lock(font.mutex);
    const uint8_t* p = text;
    const uint8_t* end = p + len;
    while (p < end) {
        uint32_t cp = decodeUtf8(p);
        uint32_t gid;
        if (hb_font_get_nominal_glyph(font.hbFonts[0].hbFont, cp, &gid) &&
            !hb_font_get_nominal_glyph(font.hbFonts[primaryFi].hbFont, cp, &gid)) {
            return 0;
        }
    }
    return primaryFi;
}

TextSystem::ResolvedGlyph TextSystem::resolveGlyph(FontData& font, const std::string& fontName,
                                                    char32_t cp, FontStyle style, uint32_t excludeFi)
{
    // Try existing fonts under read lock
    {
        std::shared_lock lock(font.mutex);
        for (uint32_t fi = 0; fi < font.hbFonts.size(); fi++) {
            auto& entry = font.hbFonts[fi];
            if (entry.style != FontStyle{}) continue;
            if (fi == excludeFi) continue;

            uint32_t gid;
            if (hb_font_get_nominal_glyph(entry.hbFont, cp, &gid)) {
                lock.unlock();
                uint32_t styledFi = getStyledVariant(font, fi, style);
                lock.lock();
                uint32_t styledGid;
                if (!hb_font_get_nominal_glyph(font.hbFonts[styledFi].hbFont, cp, &styledGid))
                    styledGid = gid;
                return {styledFi, styledGid};
            }
        }
    }

    // Try system font fallback (rare — loads a new font file)
    if (systemFallback_) {
        auto pathIt = fontPrimaryPaths_.find(fontName);
        std::string primaryPath = (pathIt != fontPrimaryPaths_.end()) ? pathIt->second : "";
        auto fallbackData = systemFallback_(primaryPath, cp);
        if (!fallbackData.empty()) {
            addFallbackFont(fontName, fallbackData);
            std::shared_lock lock(font.mutex);
            uint32_t newFi = static_cast<uint32_t>(font.hbFonts.size() - 1);
            uint32_t gid;
            if (hb_font_get_nominal_glyph(font.hbFonts[newFi].hbFont, cp, &gid)) {
                lock.unlock();
                uint32_t styledFi = getStyledVariant(font, newFi, style);
                lock.lock();
                uint32_t styledGid;
                if (!hb_font_get_nominal_glyph(font.hbFonts[styledFi].hbFont, cp, &styledGid))
                    styledGid = gid;
                return {styledFi, styledGid};
            }
        }
    }

    return {0, 0};
}

ShapedText TextSystem::shapeText(const std::string& fontName, const std::string& text,
                                  float fontSize, float wrapWidth, int align,
                                  FontStyle style)
{
    auto fontIt = fonts_.find(fontName);
    if (fontIt == fonts_.end()) return {};
    FontData& font = fontIt->second;

    if (text.empty())
        return ShapedText{{}, 0, font.lineHeight * (fontSize / font.baseSize)};

    float scale = fontSize / font.baseSize;
    float scaledLineHeight = font.lineHeight * scale;

    // --- Phase 1: Text analysis ---

    // 1a. Unicode line break opportunities
    std::vector<char> breaks(text.size());
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(text.c_str()),
                        text.size(), "en", breaks.data());

    // 1b. BiDi analysis
    SBCodepointSequence cpSeq{SBStringEncodingUTF8, const_cast<char*>(text.c_str()),
                              static_cast<SBUInteger>(text.size())};
    SBAlgorithmRef bidiAlgo = SBAlgorithmCreate(&cpSeq);
    SBParagraphRef bidiPara = SBAlgorithmCreateParagraph(
        bidiAlgo, 0, static_cast<SBUInteger>(text.size()), SBLevelDefaultLTR);
    const SBLevel* levels = SBParagraphGetLevelsPtr(bidiPara);

    // 1c. Script runs
    SBScriptLocatorRef scriptLoc = SBScriptLocatorCreate();
    SBScriptLocatorLoadCodepoints(scriptLoc, &cpSeq);

    struct ScriptRun { SBUInteger offset, length; SBScript script; };
    std::vector<ScriptRun> scriptRuns;
    while (SBScriptLocatorMoveNext(scriptLoc)) {
        const SBScriptAgent* agent = SBScriptLocatorGetAgent(scriptLoc);
        scriptRuns.push_back({agent->offset, agent->length, agent->script});
    }
    SBScriptLocatorRelease(scriptLoc);

    // --- Phase 2: Build segments and shape ---

    struct BidiRun { SBUInteger offset, length; SBLevel level; };
    std::vector<BidiRun> bidiRuns;
    {
        SBUInteger start = 0;
        SBLevel cur = levels[0];
        for (SBUInteger i = 1; i <= static_cast<SBUInteger>(text.size()); i++) {
            if (i == static_cast<SBUInteger>(text.size()) || levels[i] != cur) {
                bidiRuns.push_back({start, i - start, cur});
                if (i < static_cast<SBUInteger>(text.size())) {
                    start = i;
                    cur = levels[i];
                }
            }
        }
    }

    struct Segment { SBUInteger offset, length; SBLevel level; SBScript script; };
    std::vector<Segment> segments;
    for (const auto& br : bidiRuns) {
        SBUInteger brEnd = br.offset + br.length;
        for (const auto& sr : scriptRuns) {
            SBUInteger srEnd = sr.offset + sr.length;
            SBUInteger s = std::max(br.offset, sr.offset);
            SBUInteger e = std::min(brEnd, srEnd);
            if (s < e) segments.push_back({s, e - s, br.level, sr.script});
        }
    }
    std::sort(segments.begin(), segments.end(),
              [](const auto& a, const auto& b) { return a.offset < b.offset; });

    struct GlyphEntry {
        uint64_t glyphKey;
        uint32_t cluster;
        float xAdvance;
        float xOffset;
        float yOffset;
        SBLevel level;
    };
    std::vector<GlyphEntry> allGlyphs;

    for (const auto& seg : segments) {
        uint32_t fi = resolveSegment(font,
            reinterpret_cast<const uint8_t*>(text.c_str() + seg.offset), seg.length, style);

        hb_font_t* segHbFont;
        hb_face_t* segHbFace;
        {
            std::shared_lock lock(font.mutex);
            segHbFont = font.hbFonts[fi].hbFont;
            segHbFace = font.hbFonts[fi].hbFace;
        }

        unsigned int fontUpem = hb_face_get_upem(segHbFace);
        float fontPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(fontUpem);

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), static_cast<int>(text.size()),
                           static_cast<unsigned int>(seg.offset), static_cast<int>(seg.length));
        hb_buffer_set_direction(buf, (seg.level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, hb_script_from_iso15924_tag(
            static_cast<hb_tag_t>(SBScriptGetUnicodeTag(seg.script))));
        hb_buffer_set_language(buf, hb_language_get_default());
        hb_shape(segHbFont, buf, nullptr, 0);

        unsigned int count;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);

        auto pushGlyph = [&](unsigned int idx) {
            uint32_t gid = infos[idx].codepoint;
            uint32_t glyphFi = fi;

            if (gid == 0) {
                // Decode codepoint from cluster
                uint32_t cluster = infos[idx].cluster;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str() + cluster);
                uint32_t cp;
                if (*p < 0x80) cp = *p;
                else if ((*p & 0xE0) == 0xC0) cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
                else if ((*p & 0xF0) == 0xE0) cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
                else cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);

                auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi);
                if (resolved.glyphId != 0) {
                    glyphFi = resolved.fontIndex;
                    gid = resolved.glyphId;
                    hb_font_t* fbFont;
                    hb_face_t* fbFace;
                    {
                        std::shared_lock lock(font.mutex);
                        fbFont = font.hbFonts[glyphFi].hbFont;
                        fbFace = font.hbFonts[glyphFi].hbFace;
                    }
                    unsigned int fbUpem = hb_face_get_upem(fbFace);
                    float fbScale = static_cast<float>(font.baseSize) / static_cast<float>(fbUpem);
                    float adv = hb_font_get_glyph_h_advance(fbFont, gid) * fbScale * scale;
                    ensureGlyphEncoded(font, glyphFi, gid);
                    allGlyphs.push_back({glyphKey(glyphFi, gid), infos[idx].cluster,
                        adv, 0, 0, seg.level});
                    return;
                }
            }

            // Ensure glyph is encoded in atlas
            ensureGlyphEncoded(font, glyphFi, gid);

            allGlyphs.push_back({glyphKey(glyphFi, gid), infos[idx].cluster,
                positions[idx].x_advance * fontPixelScale * scale,
                positions[idx].x_offset * fontPixelScale * scale,
                positions[idx].y_offset * fontPixelScale * scale,
                seg.level});
        };

        if (seg.level & 1) {
            for (int i = static_cast<int>(count) - 1; i >= 0; i--)
                pushGlyph(static_cast<unsigned int>(i));
        } else {
            for (unsigned int i = 0; i < count; i++)
                pushGlyph(i);
        }

        hb_buffer_destroy(buf);
    }

    // --- Phase 3: Line wrapping ---

    struct LineRange {
        size_t firstGlyph, glyphCount;
        SBUInteger byteOffset, byteLength;
        float width;
    };
    std::vector<LineRange> lineRanges;

    {
        size_t lineStartGlyph = 0;
        SBUInteger lineStartByte = 0;
        float lineWidth = 0;
        size_t lastBreakGlyph = 0;
        SBUInteger lastBreakByte = 0;
        float widthAtBreak = 0;
        bool hasBreak = false;

        for (size_t gi = 0; gi < allGlyphs.size(); gi++) {
            const auto& g = allGlyphs[gi];

            if (g.cluster > lineStartByte && g.cluster < text.size()) {
                char brk = breaks[g.cluster - 1];
                if (brk == LINEBREAK_MUSTBREAK) {
                    lineRanges.push_back({lineStartGlyph, gi - lineStartGlyph,
                                          lineStartByte, g.cluster - lineStartByte, lineWidth});
                    lineStartGlyph = gi;
                    lineStartByte = g.cluster;
                    lineWidth = 0;
                    hasBreak = false;
                } else if (brk == LINEBREAK_ALLOWBREAK) {
                    lastBreakGlyph = gi;
                    lastBreakByte = g.cluster;
                    widthAtBreak = lineWidth;
                    hasBreak = true;
                }
            }

            lineWidth += g.xAdvance;

            if (wrapWidth > 0 && lineWidth > wrapWidth && hasBreak) {
                lineRanges.push_back({lineStartGlyph, lastBreakGlyph - lineStartGlyph,
                                      lineStartByte, lastBreakByte - lineStartByte, widthAtBreak});
                lineStartGlyph = lastBreakGlyph;
                lineStartByte = lastBreakByte;
                lineWidth -= widthAtBreak;
                hasBreak = false;
            }
        }

        SBUInteger endByte = static_cast<SBUInteger>(text.size());
        lineRanges.push_back({lineStartGlyph, allGlyphs.size() - lineStartGlyph,
                              lineStartByte, endByte - lineStartByte, lineWidth});
    }

    // --- Phase 4: Visual layout with BiDi reordering ---

    ShapedText result;
    float penY = 0;
    float maxWidth = 0;

    struct ResultLine { size_t startIdx; float width; };
    std::vector<ResultLine> resultLines;

    for (const auto& lr : lineRanges) {
        resultLines.push_back({result.glyphs.size(), 0.0f});

        if (lr.glyphCount == 0 || lr.byteLength == 0) {
            penY += scaledLineHeight;
            continue;
        }

        SBLineRef sbLine = SBParagraphCreateLine(bidiPara, lr.byteOffset, lr.byteLength);
        SBUInteger runCount = SBLineGetRunCount(sbLine);
        const SBRun* runs = SBLineGetRunsPtr(sbLine);

        float penX = 0;
        for (SBUInteger ri = 0; ri < runCount; ri++) {
            const SBRun& run = runs[ri];
            SBUInteger runEnd = run.offset + run.length;
            bool isRTL = (run.level & 1);

            std::vector<size_t> runGlyphIndices;
            for (size_t gi = lr.firstGlyph; gi < lr.firstGlyph + lr.glyphCount; gi++) {
                uint32_t cluster = allGlyphs[gi].cluster;
                if (cluster >= run.offset && cluster < runEnd) {
                    runGlyphIndices.push_back(gi);
                }
            }

            if (isRTL) {
                std::reverse(runGlyphIndices.begin(), runGlyphIndices.end());
            }

            for (size_t gi : runGlyphIndices) {
                const auto& g = allGlyphs[gi];
                ShapedGlyph sg;
                sg.glyphId = g.glyphKey;
                sg.x = penX + g.xOffset;
                sg.y = penY - g.yOffset;
                result.glyphs.push_back(sg);
                penX += g.xAdvance;
            }
        }

        resultLines.back().width = penX;
        maxWidth = std::max(maxWidth, penX);
        penY += scaledLineHeight;
        SBLineRelease(sbLine);
    }

    // Apply alignment
    if (align != 0 && wrapWidth > 0) {
        for (size_t li = 0; li < resultLines.size(); li++) {
            float offset = 0.0f;
            if (align == 1) offset = (wrapWidth - resultLines[li].width) * 0.5f;
            else if (align == 2) offset = wrapWidth - resultLines[li].width;

            size_t endIdx = (li + 1 < resultLines.size())
                ? resultLines[li + 1].startIdx : result.glyphs.size();
            for (size_t j = resultLines[li].startIdx; j < endIdx; j++) {
                result.glyphs[j].x += offset;
            }
        }
    }

    result.width = maxWidth;
    result.height = penY > 0 ? penY : scaledLineHeight;

    SBParagraphRelease(bidiPara);
    SBAlgorithmRelease(bidiAlgo);

    return result;
}

// --- Terminal-specific run shaping (no BiDi reordering, no line wrapping) ---

ShapedRun TextSystem::shapeRun(const std::string& fontName, const std::string& text,
                                float fontSize, FontStyle style)
{
    auto fontIt = fonts_.find(fontName);
    if (fontIt == fonts_.end()) return {};
    FontData& font = fontIt->second;

    if (text.empty()) return {};

    float scale = fontSize / font.baseSize;

    // BiDi analysis — get per-character embedding levels
    SBCodepointSequence cpSeq{SBStringEncodingUTF8, const_cast<char*>(text.c_str()),
                              static_cast<SBUInteger>(text.size())};
    SBAlgorithmRef bidiAlgo = SBAlgorithmCreate(&cpSeq);
    SBParagraphRef bidiPara = SBAlgorithmCreateParagraph(
        bidiAlgo, 0, static_cast<SBUInteger>(text.size()), SBLevelDefaultLTR);
    const SBLevel* levels = SBParagraphGetLevelsPtr(bidiPara);

    // Script run detection
    SBScriptLocatorRef scriptLoc = SBScriptLocatorCreate();
    SBScriptLocatorLoadCodepoints(scriptLoc, &cpSeq);

    struct ScriptRun { SBUInteger offset, length; SBScript script; };
    std::vector<ScriptRun> scriptRuns;
    while (SBScriptLocatorMoveNext(scriptLoc)) {
        const SBScriptAgent* agent = SBScriptLocatorGetAgent(scriptLoc);
        scriptRuns.push_back({agent->offset, agent->length, agent->script});
    }
    SBScriptLocatorRelease(scriptLoc);

    ShapedRun result;

    for (const auto& sr : scriptRuns) {
        uint32_t fi = resolveSegment(font,
            reinterpret_cast<const uint8_t*>(text.c_str() + sr.offset), sr.length, style);

        hb_font_t* hbFont;
        hb_face_t* hbFace;
        {
            std::shared_lock lock(font.mutex);
            hbFont = font.hbFonts[fi].hbFont;
            hbFace = font.hbFonts[fi].hbFace;
        }

        unsigned int fontUpem = hb_face_get_upem(hbFace);
        float fontPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(fontUpem);

        bool segmentRtl = (levels[sr.offset] & 1) != 0;

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), static_cast<int>(text.size()),
                           static_cast<unsigned int>(sr.offset), static_cast<int>(sr.length));
        hb_buffer_set_direction(buf, segmentRtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, hb_script_from_iso15924_tag(
            static_cast<hb_tag_t>(SBScriptGetUnicodeTag(sr.script))));
        hb_buffer_set_language(buf, hb_language_get_default());
        hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_shape(hbFont, buf, nullptr, 0);

        unsigned int count;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);

        for (unsigned int i = 0; i < count; i++) {
            uint32_t gid = infos[i].codepoint;
            uint32_t glyphFi = fi;

            if (gid == 0) {
                // Glyph missing — resolve via registry (handles style + fallback)
                uint32_t cluster = infos[i].cluster;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str() + cluster);
                uint32_t cp;
                if (*p < 0x80) cp = *p;
                else if ((*p & 0xE0) == 0xC0) cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
                else if ((*p & 0xF0) == 0xE0) cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
                else cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);

                auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi);
                if (resolved.glyphId != 0) {
                    glyphFi = resolved.fontIndex;
                    gid = resolved.glyphId;
                    hb_font_t* fbFont;
                    hb_face_t* fbFace;
                    {
                        std::shared_lock lock(font.mutex);
                        fbFont = font.hbFonts[glyphFi].hbFont;
                        fbFace = font.hbFonts[glyphFi].hbFace;
                    }
                    unsigned int fbUpem = hb_face_get_upem(fbFace);
                    float fbScale = static_cast<float>(font.baseSize) / static_cast<float>(fbUpem);
                    float adv = hb_font_get_glyph_h_advance(fbFont, gid) * fbScale * scale;
                    ensureGlyphEncoded(font, glyphFi, gid);
                    result.glyphs.push_back({glyphKey(glyphFi, gid), infos[i].cluster, adv, 0, 0, false, segmentRtl});
                    continue;
                }

                // Still missing — skip
                continue;
            }

            // Normal glyph — check if HarfBuzz substituted it (ligature, contextual form)
            bool isSubstitution = false;
            {
                uint32_t cluster = infos[i].cluster;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str() + cluster);
                uint32_t cp;
                if (*p < 0x80) cp = *p;
                else if ((*p & 0xE0) == 0xC0) cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
                else if ((*p & 0xF0) == 0xE0) cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
                else cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);

                uint32_t nominalGid;
                if (hb_font_get_nominal_glyph(hbFont, cp, &nominalGid)) {
                    isSubstitution = (gid != nominalGid);
                }
            }

            ensureGlyphEncoded(font, glyphFi, gid);
            result.glyphs.push_back({
                glyphKey(glyphFi, gid),
                infos[i].cluster,
                positions[i].x_advance * fontPixelScale * scale,
                positions[i].x_offset * fontPixelScale * scale,
                positions[i].y_offset * fontPixelScale * scale,
                isSubstitution,
                segmentRtl
            });
        }

        hb_buffer_destroy(buf);
    }

    SBParagraphRelease(bidiPara);
    SBAlgorithmRelease(bidiAlgo);

    return result;
}

const FontData* TextSystem::getFont(const std::string& name) const
{
    auto it = fonts_.find(name);
    return it != fonts_.end() ? &it->second : nullptr;
}

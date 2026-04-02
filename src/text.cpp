#include "text.h"

#include <hb.h>
#include <hb-ot.h>

#include <SheenBidi/SheenBidi.h>
#include <linebreak.h>

#include <msdfgen/msdfgen.h>

#include <algorithm>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

// --- HarfBuzz draw callbacks to build msdfgen::Shape ---

struct OutlineContext {
    msdfgen::Shape* shape = nullptr;
    msdfgen::Contour* contour = nullptr;
    double scale = 1.0;
    msdfgen::Point2 last{0, 0};
};

static void hbMoveTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*,
                      float x, float y, void*)
{
    auto* ctx = static_cast<OutlineContext*>(data);
    ctx->contour = &ctx->shape->addContour();
    ctx->last = msdfgen::Point2(x * ctx->scale, y * ctx->scale);
}

static void hbLineTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*,
                      float x, float y, void*)
{
    auto* ctx = static_cast<OutlineContext*>(data);
    if (!ctx->contour) return;
    msdfgen::Point2 to(x * ctx->scale, y * ctx->scale);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->last, to));
    ctx->last = to;
}

static void hbQuadTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*,
                      float cx, float cy, float x, float y, void*)
{
    auto* ctx = static_cast<OutlineContext*>(data);
    if (!ctx->contour) return;
    msdfgen::Point2 control(cx * ctx->scale, cy * ctx->scale);
    msdfgen::Point2 to(x * ctx->scale, y * ctx->scale);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->last, control, to));
    ctx->last = to;
}

static void hbCubicTo(hb_draw_funcs_t*, void* data, hb_draw_state_t*,
                       float c1x, float c1y, float c2x, float c2y,
                       float x, float y, void*)
{
    auto* ctx = static_cast<OutlineContext*>(data);
    if (!ctx->contour) return;
    msdfgen::Point2 ctrl1(c1x * ctx->scale, c1y * ctx->scale);
    msdfgen::Point2 ctrl2(c2x * ctx->scale, c2y * ctx->scale);
    msdfgen::Point2 to(x * ctx->scale, y * ctx->scale);
    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->last, ctrl1, ctrl2, to));
    ctx->last = to;
}

static void hbClosePath(hb_draw_funcs_t*, void* data, hb_draw_state_t*, void*)
{
    auto* ctx = static_cast<OutlineContext*>(data);
    ctx->contour = nullptr;
}

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
            if (entry.hbFont) hb_font_destroy(entry.hbFont);
            if (entry.hbFace) hb_face_destroy(entry.hbFace);
            if (entry.hbBlob) hb_blob_destroy(entry.hbBlob);
        }
    }
}

bool TextSystem::registerFont(const std::string& name,
                               const std::vector<std::vector<uint8_t>>& ttfDataList,
                               float baseSize, float pxRange, bool sharp,
                               const FontCharset& charset)
{
    if (ttfDataList.empty()) {
        spdlog::error("registerFont '{}': no font data provided", name);
        return false;
    }

    FontData font;
    font.name = name;
    font.baseSize = baseSize;
    font.pxRange = pxRange;

    // 1. Load all fonts via HarfBuzz
    for (const auto& ttfData : ttfDataList) {
        FontData::HBEntry entry;
        entry.hbBlob = hb_blob_create(reinterpret_cast<const char*>(ttfData.data()),
                                       static_cast<unsigned int>(ttfData.size()),
                                       HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
        entry.hbFace = hb_face_create(entry.hbBlob, 0);
        entry.hbFont = hb_font_create(entry.hbFace);
        font.hbFonts.push_back(entry);
    }

    // Use primary font for metrics
    const auto& primary = font.hbFonts[0];
    unsigned int upem = hb_face_get_upem(primary.hbFace);
    double pixelScaleD = static_cast<double>(baseSize) / static_cast<double>(upem);
    float pixelScale = static_cast<float>(pixelScaleD);

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

    // Set up HarfBuzz draw funcs for glyph outlines
    hb_draw_funcs_t* drawFuncs = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(drawFuncs, hbMoveTo, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(drawFuncs, hbLineTo, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(drawFuncs, hbQuadTo, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(drawFuncs, hbCubicTo, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(drawFuncs, hbClosePath, nullptr, nullptr);

    // 2. Collect glyph IDs from charset ranges with fallback
    struct GlyphWork {
        uint32_t fontIndex;
        uint32_t glyphId;
        float bearingX, bearingY;
        float width, height;
        float advance;
        std::shared_ptr<msdfgen::Shape> shape;
        std::vector<uint8_t> msdfPixels;
        uint32_t bmpW, bmpH;
    };
    std::vector<GlyphWork> glyphWork;

    // Determine which codepoint ranges to use
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (charset.ranges.empty()) {
        ranges.push_back({32, 126});
    } else {
        ranges = charset.ranges;
    }

    // Collect unique glyph IDs from codepoint ranges, trying fonts in order
    std::unordered_set<uint64_t> collectedGlyphKeys;
    for (const auto& [rangeStart, rangeEnd] : ranges) {
        for (uint32_t cp = rangeStart; cp <= rangeEnd; cp++) {
            for (uint32_t fi = 0; fi < font.hbFonts.size(); fi++) {
                uint32_t gid;
                if (hb_font_get_nominal_glyph(font.hbFonts[fi].hbFont, cp, &gid)) {
                    font.codepointToFontIndex[cp] = fi;
                    collectedGlyphKeys.insert(glyphKey(fi, gid));
                    break;
                }
            }
        }
    }

    // Discover contextual/ligature glyphs via GSUB closure for each requested script
    if (!charset.scripts.empty()) {
        for (uint32_t fi = 0; fi < font.hbFonts.size(); fi++) {
            hb_face_t* face = font.hbFonts[fi].hbFace;

            hb_set_t* glyphSet = hb_set_create();
            for (uint64_t key : collectedGlyphKeys) {
                if (static_cast<uint32_t>(key >> 32) == fi)
                    hb_set_add(glyphSet, static_cast<uint32_t>(key & 0xFFFFFFFF));
            }

            if (hb_set_is_empty(glyphSet)) {
                hb_set_destroy(glyphSet);
                continue;
            }

            std::vector<hb_tag_t> scriptTags;
            for (const auto& s : charset.scripts) {
                scriptTags.push_back(hb_tag_from_string(s.c_str(), static_cast<int>(s.size())));
            }
            scriptTags.push_back(HB_TAG_NONE);

            hb_set_t* lookupSet = hb_set_create();
            hb_ot_layout_collect_lookups(face, HB_OT_TAG_GSUB,
                                         scriptTags.data(), nullptr, nullptr, lookupSet);

            hb_ot_layout_lookups_substitute_closure(face, lookupSet, glyphSet);

            hb_codepoint_t gid = HB_SET_VALUE_INVALID;
            while (hb_set_next(glyphSet, &gid)) {
                collectedGlyphKeys.insert(glyphKey(fi, gid));
            }

            hb_set_destroy(lookupSet);
            hb_set_destroy(glyphSet);
        }
    }

    spdlog::info("Font '{}': {} total unique glyph keys to rasterize", name, collectedGlyphKeys.size());

    // --- Pass 1: Extract outlines (single-threaded) ---
    uint32_t glyphsNoExtents = 0, glyphsWhitespace = 0, glyphsNoContours = 0, glyphsRasterized = 0;
    for (uint64_t key : collectedGlyphKeys) {
        uint32_t fi = static_cast<uint32_t>(key >> 32);
        uint32_t gid = static_cast<uint32_t>(key & 0xFFFFFFFF);
        hb_font_t* hbFont = font.hbFonts[fi].hbFont;

        unsigned int fontUpem = hb_face_get_upem(font.hbFonts[fi].hbFace);
        double fontPixelScaleD = static_cast<double>(baseSize) / static_cast<double>(fontUpem);
        float fontPixelScale = static_cast<float>(fontPixelScaleD);

        hb_glyph_extents_t ext;
        if (!hb_font_get_glyph_extents(hbFont, gid, &ext)) {
            glyphsNoExtents++;
            continue;
        }

        float bX = ext.x_bearing * fontPixelScale;
        float bY = ext.y_bearing * fontPixelScale;
        float gW = ext.width * fontPixelScale;
        float gH = -ext.height * fontPixelScale;
        float adv = hb_font_get_glyph_h_advance(hbFont, gid) * fontPixelScale;

        GlyphWork work;
        work.fontIndex = fi;
        work.glyphId = gid;
        work.bearingX = bX;
        work.bearingY = bY;
        work.width = gW;
        work.height = gH;
        work.advance = adv;

        uint32_t bmpW = static_cast<uint32_t>(std::ceil(gW + 2.0f * pxRange));
        uint32_t bmpH = static_cast<uint32_t>(std::ceil(gH + 2.0f * pxRange));

        if (bmpW < 1 || bmpH < 1 || (gW < 0.5f && gH < 0.5f)) {
            GlyphInfo info{};
            info.bearingX = bX;
            info.bearingY = bY;
            info.width = 0;
            info.height = 0;
            info.advance = adv;
            font.glyphs[key] = info;
            glyphsWhitespace++;
            continue;
        }

        // Extract outline via HarfBuzz
        auto shape = std::make_shared<msdfgen::Shape>();
        OutlineContext octx;
        octx.shape = shape.get();
        octx.scale = fontPixelScaleD;
        hb_font_draw_glyph(hbFont, gid, drawFuncs, &octx);

        if (shape->contours.empty()) {
            GlyphInfo info{};
            info.bearingX = bX;
            info.bearingY = bY;
            info.width = 0;
            info.height = 0;
            info.advance = adv;
            font.glyphs[key] = info;
            glyphsNoContours++;
            continue;
        }

        glyphsRasterized++;
        work.bmpW = bmpW;
        work.bmpH = bmpH;
        work.shape = std::move(shape);
        glyphWork.push_back(std::move(work));
    }

    hb_draw_funcs_destroy(drawFuncs);

    spdlog::info("Font '{}': outlines={}, whitespace={}, no_contours={}, no_extents={}",
                 name, glyphsRasterized, glyphsWhitespace, glyphsNoContours, glyphsNoExtents);

    // --- Pass 2: Generate MSDF bitmaps (single-threaded for simplicity) ---
    for (size_t i = 0; i < glyphWork.size(); i++) {
        auto& g = glyphWork[i];
        g.msdfPixels.resize(g.bmpW * g.bmpH * 4);

        if (sharp) {
            struct Seg { double x0, y0, x1, y1; };
            std::vector<Seg> segs;
            double translateX = static_cast<double>(pxRange) - static_cast<double>(g.bearingX);
            double translateY = static_cast<double>(pxRange) - (static_cast<double>(g.bearingY) - static_cast<double>(g.height));
            for (const auto& contour : g.shape->contours) {
                for (const auto& edge : contour.edges) {
                    msdfgen::Point2 p0 = edge->point(0.0);
                    msdfgen::Point2 p1 = edge->point(1.0);
                    segs.push_back({p0.x + translateX, p0.y + translateY,
                                    p1.x + translateX, p1.y + translateY});
                }
            }

            for (uint32_t bmpRow = 0; bmpRow < g.bmpH; bmpRow++) {
                double scanY = static_cast<double>(bmpRow) + 0.5;
                std::vector<std::pair<double, int>> crossings;
                for (const auto& s : segs) {
                    if ((s.y0 <= scanY && s.y1 > scanY) || (s.y1 <= scanY && s.y0 > scanY)) {
                        double t = (scanY - s.y0) / (s.y1 - s.y0);
                        double xInt = s.x0 + t * (s.x1 - s.x0);
                        int wind = (s.y1 > s.y0) ? 1 : -1;
                        crossings.push_back({xInt, wind});
                    }
                }
                std::sort(crossings.begin(), crossings.end());

                int winding = 0;
                size_t ci = 0;
                uint32_t atlasRow = g.bmpH - 1 - bmpRow;
                for (uint32_t col = 0; col < g.bmpW; col++) {
                    double pixCenter = static_cast<double>(col) + 0.5;
                    while (ci < crossings.size() && crossings[ci].first <= pixCenter) {
                        winding += crossings[ci].second;
                        ci++;
                    }
                    uint8_t v = (winding != 0) ? 255 : 0;
                    uint32_t idx = (atlasRow * g.bmpW + col) * 4;
                    g.msdfPixels[idx + 0] = v;
                    g.msdfPixels[idx + 1] = v;
                    g.msdfPixels[idx + 2] = v;
                    g.msdfPixels[idx + 3] = v;
                }
            }
        } else {
            g.shape->normalize();
            msdfgen::edgeColoringSimple(*g.shape, 3.0);

            msdfgen::Vector2 translate(-g.bearingX + pxRange, -(g.bearingY - g.height) + pxRange);
            msdfgen::SDFTransformation transform(
                msdfgen::Projection(1.0, translate),
                msdfgen::Range(pxRange));

            msdfgen::Bitmap<float, 3> msdfBmp(g.bmpW, g.bmpH);
            msdfgen::generateMSDF(msdfBmp, *g.shape, transform);

            for (uint32_t row = 0; row < g.bmpH; row++) {
                uint32_t srcRow = g.bmpH - 1 - row;
                for (uint32_t col = 0; col < g.bmpW; col++) {
                    const float* px = msdfBmp(col, srcRow);
                    uint32_t idx = (row * g.bmpW + col) * 4;
                    g.msdfPixels[idx + 0] = static_cast<uint8_t>(std::clamp(px[0] * 255.0f + 0.5f, 0.0f, 255.0f));
                    g.msdfPixels[idx + 1] = static_cast<uint8_t>(std::clamp(px[1] * 255.0f + 0.5f, 0.0f, 255.0f));
                    g.msdfPixels[idx + 2] = static_cast<uint8_t>(std::clamp(px[2] * 255.0f + 0.5f, 0.0f, 255.0f));
                    g.msdfPixels[idx + 3] = 255;
                }
            }
        }

        g.shape.reset();
    }

    // 4. Pack glyphs into atlas using shelf packing (with 2px padding between glyphs)
    static constexpr uint32_t GLYPH_PAD = 2;
    uint32_t atlasW = 1024, atlasH = 1024;

    auto tryPack = [&](uint32_t w, uint32_t h) -> bool {
        uint32_t shelfX = 0, shelfY = 0, shelfHeight = 0;
        for (auto& g : glyphWork) {
            uint32_t paddedW = g.bmpW + GLYPH_PAD;
            uint32_t paddedH = g.bmpH + GLYPH_PAD;
            if (shelfX + paddedW > w) {
                shelfX = 0;
                shelfY += shelfHeight;
                shelfHeight = 0;
            }
            if (shelfY + paddedH > h) return false;
            shelfHeight = std::max(shelfHeight, paddedH);
            shelfX += paddedW;
        }
        return true;
    };

    while (!tryPack(atlasW, atlasH)) {
        if (atlasW <= atlasH) atlasW *= 2;
        else atlasH *= 2;
        if (atlasW > 4096 || atlasH > 4096) {
            spdlog::error("Font atlas too large for '{}'", name);
            return false;
        }
    }

    font.atlasWidth = atlasW;
    font.atlasHeight = atlasH;
    font.atlasPixels.resize(atlasW * atlasH * 4, 0);

    // Actually pack and blit
    uint32_t shelfX = 0, shelfY = 0, shelfHeight = 0;
    for (auto& g : glyphWork) {
        uint32_t paddedW = g.bmpW + GLYPH_PAD;
        uint32_t paddedH = g.bmpH + GLYPH_PAD;
        if (shelfX + paddedW > atlasW) {
            shelfX = 0;
            shelfY += shelfHeight;
            shelfHeight = 0;
        }

        for (uint32_t row = 0; row < g.bmpH; row++) {
            std::memcpy(&font.atlasPixels[((shelfY + row) * atlasW + shelfX) * 4],
                        &g.msdfPixels[row * g.bmpW * 4],
                        g.bmpW * 4);
        }

        GlyphInfo info;
        info.u0 = static_cast<float>(shelfX) / atlasW;
        info.v0 = static_cast<float>(shelfY) / atlasH;
        info.u1 = static_cast<float>(shelfX + g.bmpW) / atlasW;
        info.v1 = static_cast<float>(shelfY + g.bmpH) / atlasH;
        info.bearingX = g.bearingX;
        info.bearingY = g.bearingY;
        info.width = static_cast<float>(g.bmpW);
        info.height = static_cast<float>(g.bmpH);
        info.advance = g.advance;
        font.glyphs[glyphKey(g.fontIndex, g.glyphId)] = info;

        shelfX += paddedW;
        shelfHeight = std::max(shelfHeight, paddedH);
    }

    // Add a 1x1 white pixel for background rects
    {
        uint32_t wpX = shelfX;
        uint32_t wpY = shelfY;
        if (wpX + 1 > atlasW) {
            wpX = 0;
            wpY += shelfHeight;
        }
        if (wpY + 1 <= atlasH) {
            uint32_t idx = (wpY * atlasW + wpX) * 4;
            font.atlasPixels[idx + 0] = 255;
            font.atlasPixels[idx + 1] = 255;
            font.atlasPixels[idx + 2] = 255;
            font.atlasPixels[idx + 3] = 255;
            font.whiteU = (static_cast<float>(wpX) + 0.5f) / atlasW;
            font.whiteV = (static_cast<float>(wpY) + 0.5f) / atlasH;
        }
    }

    spdlog::info("Registered font '{}': {} glyphs, atlas {}x{}, baseSize={:.0f}, {} font(s)",
                name, static_cast<uint32_t>(font.glyphs.size()),
                atlasW, atlasH, baseSize, font.hbFonts.size());

    fonts_[name] = std::move(font);
    return true;
}

const ShapedText& TextSystem::shapeText(const std::string& fontName, const std::string& text,
                                         float fontSize, float wrapWidth, int align) const
{
    // Compute cache key
    size_t h = std::hash<std::string>{}(fontName);
    h ^= std::hash<std::string>{}(text) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<float>{}(fontSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<float>{}(wrapWidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(align) + 0x9e3779b9 + (h << 6) + (h >> 2);

    // LRU lookup
    auto it = cacheMap_.find(h);
    if (it != cacheMap_.end()) {
        cacheLru_.splice(cacheLru_.begin(), cacheLru_, it->second);
        return it->second->shaped;
    }

    // Cache miss
    static const ShapedText empty{};

    auto fontIt = fonts_.find(fontName);
    if (fontIt == fonts_.end()) return empty;
    const FontData& font = fontIt->second;

    if (text.empty()) {
        cacheLru_.push_front({h, ShapedText{{}, 0, font.lineHeight * (fontSize / font.baseSize)}});
        cacheMap_[h] = cacheLru_.begin();
        if (cacheMap_.size() > MAX_SHAPE_CACHE) {
            cacheMap_.erase(cacheLru_.back().key);
            cacheLru_.pop_back();
        }
        return cacheLru_.front().shaped;
    }

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

    auto fontIndexForSegment = [&](const Segment& seg) -> uint32_t {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str() + seg.offset);
        const uint8_t* end = p + seg.length;

        bool primaryMissesAny = false;
        while (p < end) {
            uint32_t cp;
            if (*p < 0x80) { cp = *p++; }
            else if ((*p & 0xE0) == 0xC0) { cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F); p += 2; }
            else if ((*p & 0xF0) == 0xE0) { cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F); p += 3; }
            else { cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F); p += 4; }

            uint32_t gid;
            if (!hb_font_get_nominal_glyph(font.hbFonts[0].hbFont, cp, &gid))
                primaryMissesAny = true;
        }

        if (!primaryMissesAny) return 0;

        for (uint32_t fi = 1; fi < font.hbFonts.size(); fi++) {
            p = reinterpret_cast<const uint8_t*>(text.c_str() + seg.offset);
            bool covers = true;
            while (p < end) {
                uint32_t cp;
                if (*p < 0x80) { cp = *p++; }
                else if ((*p & 0xE0) == 0xC0) { cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F); p += 2; }
                else if ((*p & 0xF0) == 0xE0) { cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F); p += 3; }
                else { cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F); p += 4; }

                uint32_t gid;
                if (!hb_font_get_nominal_glyph(font.hbFonts[0].hbFont, cp, &gid) &&
                    !hb_font_get_nominal_glyph(font.hbFonts[fi].hbFont, cp, &gid)) {
                    covers = false;
                    break;
                }
            }
            if (covers) return fi;
        }

        return 0;
    };

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
        uint32_t fi = fontIndexForSegment(seg);

        unsigned int fontUpem = hb_face_get_upem(font.hbFonts[fi].hbFace);
        float fontPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(fontUpem);

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), static_cast<int>(text.size()),
                           static_cast<unsigned int>(seg.offset), static_cast<int>(seg.length));
        hb_buffer_set_direction(buf, (seg.level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, hb_script_from_iso15924_tag(
            static_cast<hb_tag_t>(SBScriptGetUnicodeTag(seg.script))));
        hb_buffer_set_language(buf, hb_language_get_default());
        hb_shape(font.hbFonts[fi].hbFont, buf, nullptr, 0);

        unsigned int count;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);

        auto pushGlyph = [&](unsigned int idx) {
            uint32_t gid = infos[idx].codepoint;
            uint32_t glyphFi = fi;

            if (gid == 0) {
                for (uint32_t tryFi = 0; tryFi < font.hbFonts.size(); tryFi++) {
                    if (tryFi == fi) continue;
                    uint32_t cluster = infos[idx].cluster;
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.c_str() + cluster);
                    uint32_t cp;
                    if (*p < 0x80) cp = *p;
                    else if ((*p & 0xE0) == 0xC0) cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
                    else if ((*p & 0xF0) == 0xE0) cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
                    else cp = (*p & 0x07) << 18 | (*(p+1) & 0x3F) << 12 | (*(p+2) & 0x3F) << 6 | (*(p+3) & 0x3F);

                    uint32_t fallbackGid;
                    if (hb_font_get_nominal_glyph(font.hbFonts[tryFi].hbFont, cp, &fallbackGid)) {
                        gid = fallbackGid;
                        glyphFi = tryFi;
                        unsigned int fbUpem = hb_face_get_upem(font.hbFonts[tryFi].hbFace);
                        float fbScale = static_cast<float>(font.baseSize) / static_cast<float>(fbUpem);
                        float adv = hb_font_get_glyph_h_advance(font.hbFonts[tryFi].hbFont, fallbackGid) * fbScale * scale;
                        allGlyphs.push_back({glyphKey(glyphFi, gid), infos[idx].cluster,
                            adv, 0, 0, seg.level});
                        return;
                    }
                }
            }

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

    // Insert into LRU cache
    cacheLru_.push_front({h, std::move(result)});
    cacheMap_[h] = cacheLru_.begin();

    if (cacheMap_.size() > MAX_SHAPE_CACHE) {
        auto& back = cacheLru_.back();
        cacheMap_.erase(back.key);
        cacheLru_.pop_back();
    }

    return cacheLru_.front().shaped;
}

const FontData* TextSystem::getFont(const std::string& name) const
{
    auto it = fonts_.find(name);
    return it != fonts_.end() ? &it->second : nullptr;
}

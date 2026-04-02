#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;

struct GlyphInfo {
    float u0, v0, u1, v1;           // atlas UV rect
    float bearingX, bearingY;        // offset from pen to glyph top-left (pixels at baseSize)
    float width, height;             // glyph bitmap size in atlas (pixels, includes padding)
    float advance;                   // horizontal advance (pixels at baseSize)
};

// Charset specification for font atlas generation
struct FontCharset {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // codepoint ranges (inclusive)
    std::vector<std::string> scripts;                    // OpenType script tags (e.g. "arab", "latn")
};

struct FontData {
    std::string name;
    float baseSize;                  // px size used for MSDF generation
    float ascender, descender;       // in px at baseSize
    float lineHeight;                // ascender - descender + gap
    float pxRange;                   // MSDF distance field range

    std::vector<uint8_t> atlasPixels;  // RGBA8
    uint32_t atlasWidth, atlasHeight;

    // Glyph ID key = (fontIndex << 32) | glyphId
    std::unordered_map<uint64_t, GlyphInfo> glyphs;

    // 1x1 white pixel in atlas (for background rects)
    float whiteU = 0, whiteV = 0;

    // HarfBuzz font entries (primary + fallbacks)
    struct HBEntry {
        hb_blob_t* hbBlob = nullptr;
        hb_face_t* hbFace = nullptr;
        hb_font_t* hbFont = nullptr;
    };
    std::vector<HBEntry> hbFonts;

    // Which font index covers each codepoint (for shaping font selection)
    std::unordered_map<uint32_t, uint32_t> codepointToFontIndex;
};

struct ShapedGlyph { uint64_t glyphId; float x, y; };
struct ShapedText {
    std::vector<ShapedGlyph> glyphs;
    float width, height;
};

class TextSystem {
public:
    ~TextSystem();
    bool registerFont(const std::string& name,
                      const std::vector<std::vector<uint8_t>>& ttfDataList,
                      float baseSize = 48.0f, float pxRange = 4.0f, bool sharp = false,
                      const FontCharset& charset = {});
    const ShapedText& shapeText(const std::string& fontName, const std::string& text,
                                float fontSize, float wrapWidth = 0, int align = 0) const;
    const FontData* getFont(const std::string& name) const;

private:
    std::unordered_map<std::string, FontData> fonts_;

    // LRU shape cache
    static constexpr size_t MAX_SHAPE_CACHE = 512;
    struct CacheEntry {
        size_t key;
        ShapedText shaped;
    };
    mutable std::list<CacheEntry> cacheLru_;
    mutable std::unordered_map<size_t, std::list<CacheEntry>::iterator> cacheMap_;
};

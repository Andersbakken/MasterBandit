#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_gpu_draw_t;

struct GlyphInfo {
    uint32_t atlas_offset;               // offset into atlasData (in vec4<i32> units)
    float ext_min_x, ext_min_y;          // glyph extents in design units
    float ext_max_x, ext_max_y;
    uint32_t upem;                       // units per em for this glyph's font
    float advance;                       // horizontal advance (pixels at baseSize)
    bool is_empty;                       // true for whitespace/no-contour glyphs
};

struct FontData {
    std::string name;
    float baseSize;                      // px size used for metric scaling
    float ascender, descender;           // in px at baseSize
    float lineHeight;                    // ascender - descender + gap

    // Glyph atlas: storage buffer contents (4 ints per texel)
    std::vector<int32_t> atlasData;
    uint32_t atlasUsed = 0;              // number of vec4<i32> texels used

    // Glyph ID key = (fontIndex << 32) | glyphId
    std::unordered_map<uint64_t, GlyphInfo> glyphs;

    // HarfBuzz font entries (primary + fallbacks)
    struct HBEntry {
        hb_blob_t* hbBlob = nullptr;
        hb_face_t* hbFace = nullptr;
        hb_font_t* hbFont = nullptr;
        hb_gpu_draw_t* gpuDraw = nullptr;
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
                      float baseSize = 48.0f);
    const ShapedText& shapeText(const std::string& fontName, const std::string& text,
                                float fontSize, float wrapWidth = 0, int align = 0,
                                int fontIndexHint = 0);
    const FontData* getFont(const std::string& name) const;
    bool addFallbackFont(const std::string& name, const std::vector<uint8_t>& ttfData);
    bool addSyntheticBoldVariant(const std::string& name, float xStrength = 0.02f, float yStrength = 0.02f);

    // System font fallback callback. Given a primary font path and a codepoint,
    // returns font file data for a system font covering that codepoint.
    // Empty return = no fallback available.
    using SystemFallbackFn = std::function<std::vector<uint8_t>(const std::string& primaryFontPath, char32_t codepoint)>;
    void setSystemFallback(SystemFallbackFn fn);

    // Set the primary font path for a registered font (used by system fallback).
    void setPrimaryFontPath(const std::string& name, const std::string& path);

    // Ensure a glyph is encoded in the atlas; called during shaping
    void ensureGlyphEncoded(FontData& font, uint32_t fontIndex, uint32_t glyphId);

private:
    std::unordered_map<std::string, FontData> fonts_;
    std::unordered_map<std::string, std::string> fontPrimaryPaths_; // font name → primary font file path
    SystemFallbackFn systemFallback_;

    // LRU shape cache
    static constexpr size_t MAX_SHAPE_CACHE = 512;
    struct CacheEntry {
        size_t key;
        ShapedText shaped;
    };
    mutable std::list<CacheEntry> cacheLru_;
    mutable std::unordered_map<size_t, std::list<CacheEntry>::iterator> cacheMap_;
};

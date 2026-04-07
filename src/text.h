#pragma once

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_gpu_draw_t;

#include "ColrTypes.h"

struct GlyphInfo {
    uint32_t atlas_offset;               // offset into atlasData (in vec4<i32> units)
    float ext_min_x, ext_min_y;          // glyph extents in design units
    float ext_max_x, ext_max_y;
    uint32_t upem;                       // units per em for this glyph's font
    float advance;                       // horizontal advance (pixels at baseSize)
    bool is_empty;                       // true for whitespace/no-contour glyphs
    bool is_colr;                        // true if this is a COLRv1 color glyph
};

struct FontStyle {
    bool bold = false;
    bool italic = false;

    bool operator==(const FontStyle& o) const { return bold == o.bold && italic == o.italic; }
    bool operator!=(const FontStyle& o) const { return !(*this == o); }

    // Pack into a small key for hashing/maps
    uint8_t key() const { return (bold ? 1 : 0) | (italic ? 2 : 0); }
};

struct FontData {
    std::string name;
    float baseSize;                      // px size used for metric scaling
    float charWidth;                     // advance of a reference character at baseSize (used to snap fallback glyph advances)
    float ascender, descender;           // in px at baseSize
    float lineHeight;                    // ascender - descender + gap

    // Glyph atlas: storage buffer contents (4 ints per texel)
    std::vector<int32_t> atlasData;
    uint32_t atlasUsed = 1;              // number of vec4<i32> texels used (starts at 1; 0 is reserved as "no glyph" sentinel)

    // Glyph ID key = (fontIndex << 32) | glyphId
    std::unordered_map<uint64_t, GlyphInfo> glyphs;

    // HarfBuzz font entries (primary + fallbacks + styled variants)
    struct HBEntry {
        hb_blob_t* hbBlob = nullptr;
        hb_face_t* hbFace = nullptr;
        hb_font_t* hbFont = nullptr;
        hb_gpu_draw_t* gpuDraw = nullptr;
        uint32_t baseFontIndex = 0;      // which base font this derives from (self for base fonts)
        FontStyle style;                 // what style this entry represents
    };
    std::vector<HBEntry> hbFonts;

    // Maps (baseFontIndex, style.key()) -> hbFonts index for styled variants
    std::unordered_map<uint64_t, uint32_t> styledVariants;

    // Which font index covers each codepoint (for shaping font selection)
    std::unordered_map<uint32_t, uint32_t> codepointToFontIndex;

    // COLRv1 glyph data: keyed by same glyphKey as glyphs map
    std::unordered_map<uint64_t, ColrGlyphData> colrGlyphs;
    bool hasColrPaint = false;  // cached result of hb_ot_color_has_paint()

    // Protects glyphs, atlasData, atlasUsed, hbFonts, styledVariants, codepointToFontIndex.
    // Read lock for lookups, write lock for insertions (new glyphs, fallback fonts, styled variants).
    // Not movable — FontData must be constructed in-place in the fonts_ map.
    mutable std::shared_mutex mutex;
};

struct ShapedGlyph { uint64_t glyphId; float x, y; };
struct ShapedText {
    std::vector<ShapedGlyph> glyphs;
    float width, height;
};

// Terminal-specific shaped run: no BiDi reordering, no line wrapping.
// Preserves cluster info and raw HarfBuzz advances/offsets.
struct ShapedRunGlyph {
    uint64_t glyphId;
    uint32_t cluster;     // byte offset into input text
    float xAdvance;       // scaled pixel advance
    float xOffset;        // scaled pixel offset from pen position
    float yOffset;        // scaled pixel offset from baseline
    bool isSubstitution;  // true if HarfBuzz substituted the glyph (ligature, contextual form)
    bool rtl;             // true if this glyph is from an RTL segment
};
struct ShapedRun {
    std::vector<ShapedRunGlyph> glyphs;
};

class TextSystem {
public:
    ~TextSystem();
    bool registerFont(const std::string& name,
                      const std::vector<std::vector<uint8_t>>& ttfDataList,
                      float baseSize = 48.0f);
    void unregisterFont(const std::string& name);
    ShapedText shapeText(const std::string& fontName, const std::string& text,
                         float fontSize, float wrapWidth = 0, int align = 0,
                         FontStyle style = {});
    ShapedRun shapeRun(const std::string& fontName, const std::string& text,
                       float fontSize, FontStyle style = {});
    const FontData* getFont(const std::string& name) const;
    bool addFallbackFont(const std::string& name, const std::vector<uint8_t>& ttfData);
    bool addSyntheticBoldVariant(const std::string& name, float xStrength = 0.02f, float yStrength = 0.02f);

    // Set bold strength for on-demand synthetic bold creation on fallback fonts
    void setBoldStrength(float x, float y) { boldStrengthX_ = x; boldStrengthY_ = y; }

    // System font fallback callback. Given a primary font path and a codepoint,
    // returns font file data for a system font covering that codepoint.
    // Empty return = no fallback available.
    using SystemFallbackFn = std::function<std::vector<uint8_t>(const std::string& primaryFontPath, char32_t codepoint)>;
    using EmojiFallbackFn  = std::function<std::vector<uint8_t>(char32_t codepoint)>;
    void setSystemFallback(SystemFallbackFn fn);
    void setEmojiFallback(EmojiFallbackFn fn);

    // Set the primary font path for a registered font (used by system fallback).
    void setPrimaryFontPath(const std::string& name, const std::string& path);

    // Ensure a glyph is encoded in the atlas; called during shaping
    void ensureGlyphEncoded(FontData& font, uint32_t fontIndex, uint32_t glyphId);

    // --- Font registry: resolve fonts with style ---

    // Pick the best font index for a text segment with the given style.
    // The styled variant of the primary font is preferred; per-glyph fallback
    // handles codepoints not covered by primary.
    uint32_t resolveSegment(FontData& font, const uint8_t* text, size_t len, FontStyle style);

    // For per-glyph fallback: find a font that has the codepoint, returning
    // the styled variant. Creates synthetic bold on demand for fallback fonts.
    struct ResolvedGlyph {
        uint32_t fontIndex;
        uint32_t glyphId;
    };
    ResolvedGlyph resolveGlyph(FontData& font, const std::string& fontName,
                               char32_t cp, FontStyle style, uint32_t excludeFi);

    // Get or create the styled variant of a given base font index.
    uint32_t getStyledVariant(FontData& font, uint32_t baseFi, FontStyle style);

private:
    std::unordered_map<std::string, FontData> fonts_;
    std::unordered_map<std::string, std::string> fontPrimaryPaths_; // font name → primary font file path
    SystemFallbackFn systemFallback_;
    EmojiFallbackFn  emojiFallback_;
    float boldStrengthX_ = 0.04f, boldStrengthY_ = 0.04f;

};

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtl/phmap.hpp>

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_gpu_draw_t;

#include "ColrTypes.h"

struct GlyphInfo
{
    uint32_t atlas_offset;      // offset into atlasData (in vec4<i32> units)
    float ext_min_x, ext_min_y; // glyph extents in design units
    float ext_max_x, ext_max_y;
    uint32_t upem;      // units per em for this glyph's font
    float advance;      // horizontal advance (pixels at baseSize)
    bool is_empty;      // true for whitespace/no-contour glyphs
    bool is_colr;       // true if this is a COLRv1 color glyph
    uint32_t numTexels; // virtual atlas texels occupied (0 for is_empty/is_colr)
    // Touched on cache hits via atomic_ref<uint32_t> (relaxed). The custom
    // copy ctor below uses atomic_ref too so a struct copy under if_contains
    // doesn't race with another worker's LRU touch. Lost updates ok for LRU.
    uint32_t lastUsedGen { 0 };

    GlyphInfo() = default;

    GlyphInfo(const GlyphInfo &o) { *this = o; }

    GlyphInfo &operator=(const GlyphInfo &o)
    {
        atlas_offset = o.atlas_offset;
        ext_min_x    = o.ext_min_x;
        ext_min_y    = o.ext_min_y;
        ext_max_x    = o.ext_max_x;
        ext_max_y    = o.ext_max_y;
        upem         = o.upem;
        advance      = o.advance;
        is_empty     = o.is_empty;
        is_colr      = o.is_colr;
        numTexels    = o.numTexels;
        // atomic_ref-load source (concurrent LRU touchers may be
        // writing it) and atomic_ref-store dest (other workers may
        // be touching dest's slot too, e.g. when copying out of /
        // into the live map).
        std::atomic_ref<uint32_t>(lastUsedGen).store(std::atomic_ref<uint32_t>(const_cast<uint32_t &>(o.lastUsedGen)).load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

struct FontStyle
{
    bool bold   = false;
    bool italic = false;

    bool operator==(const FontStyle &o) const { return bold == o.bold && italic == o.italic; }

    bool operator!=(const FontStyle &o) const { return !(*this == o); }

    // Pack into a small key for hashing/maps
    uint8_t key() const { return (bold ? 1 : 0) | (italic ? 2 : 0); }
};

struct FontData
{
    std::string name;
    float baseSize;            // px size used for metric scaling
    float charWidth;           // advance of a reference character at baseSize (used to snap fallback glyph advances)
    float ascender, descender; // in px at baseSize
    float lineHeight;          // ascender - descender + gap

    // Glyph atlas: storage buffer contents (4 ints per texel)
    std::vector<int32_t> atlasData;
    uint32_t atlasUsed = 1; // number of vec4<i32> texels used (starts at 1; 0 is reserved as "no glyph" sentinel)

    // LRU eviction: bumped once per render frame; ensureGlyphEncoded and
    // resolveRow set GlyphInfo::lastUsedGen = currentGen on access.
    uint32_t currentGen = 0;

    // Key = (fontIndex << 32) | glyphId. 16 submaps × shared_mutex so
    // concurrent if_contains() readers on the same submap don't serialize;
    // modify_if/lazy_emplace_l/erase_if take the unique side.
    using GlyphMap = gtl::parallel_flat_hash_map<
        uint64_t, GlyphInfo,
        gtl::priv::hash_default_hash<uint64_t>,
        gtl::priv::hash_default_eq<uint64_t>,
        std::allocator<std::pair<const uint64_t, GlyphInfo>>,
        4,
        std::shared_mutex>;
    GlyphMap glyphs;

    // HarfBuzz font entries (primary + fallbacks + styled variants)
    struct HBEntry
    {
        hb_blob_t *hbBlob      = nullptr;
        hb_face_t *hbFace      = nullptr;
        hb_font_t *hbFont      = nullptr;
        hb_gpu_draw_t *gpuDraw = nullptr;
        uint32_t baseFontIndex = 0; // which base font this derives from (self for base fonts)
        FontStyle style;            // what style this entry represents
        uint64_t blobHash = 0;      // content hash of ttfData (0 = none/synthetic variant)
    };

    std::vector<HBEntry> hbFonts;

    // Maps (baseFontIndex, style.key()) -> hbFonts index for styled variants
    std::unordered_map<uint64_t, uint32_t> styledVariants;

    // Which font index covers each codepoint (for shaping font selection)
    std::unordered_map<uint32_t, uint32_t> codepointToFontIndex;

    // COLR paint data; node map gives pointer stability for ColrRasterCmd::data,
    // which is captured under if_contains and consumed post-dispatch.
    using ColrMap = gtl::parallel_node_hash_map<
        uint64_t, ColrGlyphData,
        gtl::priv::hash_default_hash<uint64_t>,
        gtl::priv::hash_default_eq<uint64_t>,
        std::allocator<std::pair<const uint64_t, ColrGlyphData>>,
        4,
        std::shared_mutex>;
    ColrMap colrGlyphs;
    bool hasColrPaint = false; // cached result of hb_ot_color_has_paint()

    // Protects atlasData, atlasUsed, hbFonts, styledVariants, codepointToFontIndex.
    // `glyphs` and `colrGlyphs` use their own per-submap locking.
    mutable std::shared_mutex mutex;

    // Destructor frees HarfBuzz resources held by hbFonts. With shared_ptr
    // ownership in fonts_, this runs only when the last shared_ptr drops —
    // i.e. after every in-flight shaping worker has released its handle.
    // No lock needed: by definition no other thread holds this FontData.
    ~FontData();

    // Monotonic counter bumped every time the atlas storage buffer
    // (atlasData / atlasUsed) changes. The renderer caches the last-seen
    // value per font; an unchanged counter means the GPU copy is still
    // current and re-upload can be skipped. See RENDER_THREADING.md §Atlas
    // Dirty Tracking.
    std::atomic<uint64_t> atlasVersion { 0 };

    // Bumped when a cached GlyphInfo could be stale (atlas repack / eviction).
    // Per-thread caches stamp slots with this and invalidate on mismatch.
    std::atomic<uint64_t> evictionVersion { 0 };
};

// Per-worker glyph cache shared by resolveCellGlyph (RenderEngine.cpp) and
// ensureGlyphEncoded (text.cpp). Keyed by (FontData*, glyphId) + version
// stamp; multi-font scenarios cannot collide. Hits skip the shared map's
// per-submap rwlock entirely.
struct GlyphTlsCache
{
    static constexpr size_t kBits  = 11;
    static constexpr size_t kSize  = size_t { 1 } << kBits;
    static constexpr size_t kMask  = kSize - 1;
    static constexpr int kMaxProbe = 8;

    struct Slot
    {
        const FontData *font = nullptr;
        uint64_t key         = 0;
        uint64_t version     = 0;
        GlyphInfo info {};
        bool is_empty = false;
    };

    std::array<Slot, kSize> slots {};

    static size_t mix(const FontData *f, uint64_t k)
    {
        uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f));
        h ^= k + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        h = (h ^ (h >> 33)) * 0xff51afd7ed558ccdULL;
        h = (h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53ULL;
        return static_cast<size_t>(h ^ (h >> 33));
    }

    enum class GetResult
    {
        Miss,
        HitHave,
        HitEmpty
    };

    GetResult tryGet(const FontData *f, uint64_t key, uint64_t curVersion, GlyphInfo &out)
    {
        size_t i = mix(f, key) & kMask;
        for (int p = 0; p < kMaxProbe; ++p) {
            Slot &s = slots[i];
            if (s.font == nullptr) {
                return GetResult::Miss;
            }
            if (s.font == f && s.key == key) {
                if (s.version != curVersion) {
                    s.font = nullptr;
                    return GetResult::Miss;
                }
                if (s.is_empty) {
                    return GetResult::HitEmpty;
                }
                out = s.info;
                return GetResult::HitHave;
            }
            i = (i + 1) & kMask;
        }
        return GetResult::Miss;
    }

    // Lighter variant: only confirms presence + version match (no payload copy).
    GetResult probe(const FontData *f, uint64_t key, uint64_t curVersion)
    {
        size_t i = mix(f, key) & kMask;
        for (int p = 0; p < kMaxProbe; ++p) {
            Slot &s = slots[i];
            if (s.font == nullptr) {
                return GetResult::Miss;
            }
            if (s.font == f && s.key == key) {
                if (s.version != curVersion) {
                    s.font = nullptr;
                    return GetResult::Miss;
                }
                return s.is_empty ? GetResult::HitEmpty : GetResult::HitHave;
            }
            i = (i + 1) & kMask;
        }
        return GetResult::Miss;
    }

    void put(const FontData *f, uint64_t key, uint64_t curVersion, const GlyphInfo *info, bool empty)
    {
        size_t i = mix(f, key) & kMask;
        for (int p = 0; p < kMaxProbe; ++p) {
            Slot &s = slots[i];
            if (s.font == nullptr || (s.font == f && s.key == key)) {
                s.font     = f;
                s.key      = key;
                s.version  = curVersion;
                s.is_empty = empty;
                if (!empty && info) {
                    s.info = *info;
                }
                return;
            }
            i = (i + 1) & kMask;
        }
        Slot &s    = slots[mix(f, key) & kMask];
        s.font     = f;
        s.key      = key;
        s.version  = curVersion;
        s.is_empty = empty;
        if (!empty && info) {
            s.info = *info;
        }
    }
};

inline thread_local GlyphTlsCache g_glyphTlsCache;

struct ShapedGlyph
{
    uint64_t glyphId;
    float x, y;
};

struct ShapedText
{
    std::vector<ShapedGlyph> glyphs;
    float width, height;
};

// Terminal-specific shaped run: no BiDi reordering, no line wrapping.
// Preserves cluster info and raw HarfBuzz advances/offsets.
struct ShapedRunGlyph
{
    uint64_t glyphId;
    uint32_t cluster;    // byte offset into input text
    float xAdvance;      // scaled pixel advance
    float xOffset;       // scaled pixel offset from pen position
    float yOffset;       // scaled pixel offset from baseline
    bool isSubstitution; // true if HarfBuzz substituted the glyph (ligature, contextual form)
    bool rtl;            // true if this glyph is from an RTL segment
};

struct ShapedRun
{
    std::vector<ShapedRunGlyph> glyphs;
};

class TextSystem
{
public:
    ~TextSystem();
    bool registerFont(const std::string &name,
                      const std::vector<std::vector<uint8_t>> &ttfDataList,
                      float baseSize = 48.0f);
    void unregisterFont(const std::string &name);
    ShapedText shapeText(const std::string &fontName, const std::string &text,
                         float fontSize, float wrapWidth = 0, int align = 0,
                         FontStyle style = {});
    ShapedRun shapeRun(const std::string &fontName, const std::string &text,
                       float fontSize, FontStyle style = {},
                       std::span<const std::pair<uint32_t, int>> byteToCell = {});
    // Returns a shared_ptr that keeps the FontData alive across the caller's
    // entire use, even if registerFont/unregisterFont mutates the registry
    // concurrently. The map's shared_ptr is the registry-side handle; this
    // copy is the user-side handle. HarfBuzz resources stay valid until the
    // last handle drops.
    std::shared_ptr<FontData> getFont(const std::string &name) const;
    // Returns the fontIndex of the added (or already-present, deduped) fallback,
    // or -1 on failure.
    int32_t addFallbackFont(const std::string &name, const std::vector<uint8_t> &ttfData);
    bool addSyntheticBoldVariant(const std::string &name, float xStrength = 0.02f, float yStrength = 0.02f);
    bool addSyntheticItalicVariant(const std::string &name, float slant = 0.2f);

    // Tag an already-loaded font entry as a styled variant (e.g. italic loaded from file)
    void tagFontStyle(const std::string &name, uint32_t fontIndex, FontStyle style);

    // Set bold strength for on-demand synthetic bold creation on fallback fonts
    void setBoldStrength(float x, float y)
    {
        boldStrengthX_ = x;
        boldStrengthY_ = y;
    }

    void setItalicSlant(float s) { italicSlant_ = s; }

    // System font fallback callback. Given a primary font path and a codepoint,
    // returns font file data for a system font covering that codepoint.
    // Empty return = no fallback available.
    using SystemFallbackFn = std::function<std::vector<uint8_t>(const std::string &primaryFontPath, char32_t codepoint)>;
    using EmojiFallbackFn  = std::function<std::vector<uint8_t>(char32_t codepoint)>;
    void setSystemFallback(SystemFallbackFn fn);
    void setEmojiFallback(EmojiFallbackFn fn);

    // Set the primary font path for a registered font (used by system fallback).
    void setPrimaryFontPath(const std::string &name, const std::string &path);

    // Ensure a glyph is encoded in the atlas; called during shaping
    void ensureGlyphEncoded(FontData &font, uint32_t fontIndex, uint32_t glyphId);

    // Bump the per-font LRU generation. Call once per render frame before
    // any glyph cache lookups so subsequent accesses register against the
    // new generation.
    void beginFontFrame(const std::string &name);

    // If atlasUsed (in virtual 4-int16 texels) exceeds budgetTexels, evict
    // the least-recently-used glyphs until usage is at or below targetTexels.
    // Defragments atlasData in place. Drops all colrGlyphs (their embedded
    // atlas offsets become stale post-defrag; they re-encode lazily). The
    // caller must guarantee no concurrent shaping worker is touching this
    // font (call between frames). Returns true if a compaction ran.
    bool compactFontAtlasLRU(const std::string &name,
                             uint32_t budgetTexels,
                             uint32_t targetTexels);

    // --- Font registry: resolve fonts with style ---

    // Pick the best font index for a text segment with the given style.
    // The styled variant of the primary font is preferred; per-glyph fallback
    // handles codepoints not covered by primary.
    uint32_t resolveSegment(FontData &font, const uint8_t *text, size_t len, FontStyle style);

    // For per-glyph fallback: find a font that has the codepoint, returning
    // the styled variant. Creates synthetic bold on demand for fallback fonts.
    struct ResolvedGlyph
    {
        uint32_t fontIndex;
        uint32_t glyphId;
    };

    ResolvedGlyph resolveGlyph(FontData &font, const std::string &fontName,
                               char32_t cp, FontStyle style, uint32_t excludeFi,
                               bool forceEmojiPresentation = false);

    // Get or create the styled variant of a given base font index.
    uint32_t getStyledVariant(FontData &font, uint32_t baseFi, FontStyle style);

private:
    // Locked variant of addFallbackFont — caller must hold registryMutex_
    // shared (or stronger). Used by resolveGlyph() from worker threads
    // already inside the shared critical section of shapeRun/shapeText,
    // since std::shared_mutex is not reentrant.
    int32_t addFallbackFontLocked(FontData &font, const std::string &name,
                                  const std::vector<uint8_t> &ttfData);

    // Guards the font registry (fonts_, fontPrimaryPaths_) and the fallback
    // function pointers below. Main (config hot-reload via registerFont,
    // unregisterFont, setSystemFallback, setEmojiFallback, setPrimaryFontPath)
    // takes exclusive. Render-thread workers (shapeRun, shapeText, getFont)
    // take shared for the duration of the call. Map values are shared_ptr —
    // a concurrent unregisterFont erases the map slot but the FontData lives
    // until every handed-out shared_ptr drops, so HB resources can't be
    // freed while a worker is mid-shape. Per-FontData state has its own
    // FontData::mutex; nests cleanly inside this one. std::shared_mutex is
    // NOT reentrant — internal helpers called from inside a held critical
    // section must use *_Locked variants (see addFallbackFontLocked).
    mutable std::shared_mutex registryMutex_;
    std::unordered_map<std::string, std::shared_ptr<FontData>> fonts_;
    std::unordered_map<std::string, std::string> fontPrimaryPaths_; // font name → primary font file path
    SystemFallbackFn systemFallback_;
    EmojiFallbackFn emojiFallback_;
    float boldStrengthX_ = 0.04f, boldStrengthY_ = 0.04f;
    float italicSlant_ = 0.2f;
};

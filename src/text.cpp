#include "text.h"
#include "ColrEncoder.h"
#include "Utf8.h"
#include "Wcwidth.h"

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

static spdlog::logger& sLog()
{
    static auto l = spdlog::get("font");
    return l ? *l : *spdlog::default_logger();
}

// --- Helper: check if U+FE0F (VS16, emoji variation selector) follows codepoint at cluster ---
static inline bool hasVS16(const std::string& text, uint32_t cluster, int cpConsumed)
{
    size_t pos = cluster + static_cast<size_t>(cpConsumed);
    return pos + 2 < text.size() &&
           static_cast<unsigned char>(text[pos])   == 0xEF &&
           static_cast<unsigned char>(text[pos+1]) == 0xB8 &&
           static_cast<unsigned char>(text[pos+2]) == 0x8F;
}

// --- Helper: make combined glyph key ---
static inline uint64_t glyphKey(uint32_t fontIndex, uint32_t glyphId)
{
    return (static_cast<uint64_t>(fontIndex) << 32) | glyphId;
}


// --- TextSystem implementation ---

FontData::~FontData()
{
    // Runs only when the last shared_ptr drops, so by definition no other
    // thread can be using these resources — no lock needed.
    for (auto& entry : hbFonts) {
        if (entry.gpuDraw) hb_gpu_draw_destroy(entry.gpuDraw);
        if (entry.hbFont)  hb_font_destroy(entry.hbFont);
        if (entry.hbFace)  hb_face_destroy(entry.hbFace);
        if (entry.hbBlob)  hb_blob_destroy(entry.hbBlob);
    }
}

TextSystem::~TextSystem()
{
    // FontData destructors clean up HB resources via shared_ptr drops.
}

void TextSystem::unregisterFont(const std::string& name)
{
    std::unique_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return;
    // The shared_ptr in the map drops here; FontData lives until every
    // worker that captured a copy via getFont/etc releases. HB destruction
    // is in ~FontData, so it can't fire while anyone is mid-shape.
    fonts_.erase(it);
    fontPrimaryPaths_.erase(name);
}

void TextSystem::ensureGlyphEncoded(FontData& font, uint32_t fontIndex, uint32_t glyphId)
{
    uint64_t key = glyphKey(fontIndex, glyphId);

    // Fast path: read lock check
    {
        std::shared_lock lock(font.mutex);
        auto it = font.glyphs.find(key);
        if (it != font.glyphs.end()) {
            // LRU touch under shared_lock — multiple workers may write
            // concurrently. atomic_ref + relaxed makes it formally
            // race-free; lost updates still only mis-age a glyph slightly
            // (which is the explicit acceptable outcome for LRU).
            std::atomic_ref<uint32_t>(it->second.lastUsedGen)
                .store(font.currentGen, std::memory_order_relaxed);
            // If glyph is cached but COLR status unknown, check now
            if (font.hasColrPaint && !it->second.is_colr) {
                hb_face_t* face = font.hbFonts[fontIndex].hbFace;
                if (hb_ot_color_glyph_has_paint(face, glyphId)) {
                    lock.unlock();
                    // Upgrade to write lock to set is_colr and encode paint
                    std::unique_lock wlock(font.mutex);
                    auto wit = font.glyphs.find(key);
                    if (wit != font.glyphs.end() && !wit->second.is_colr) {
                        wit->second.is_colr = true;
                        sLog().info("COLR: late-detected colr glyph gid={} fi={}", glyphId, fontIndex);
                    }
                    wlock.unlock();
                    // Encode paint graph
                    hb_font_t* hbFont;
                    hb_face_t* hbFace;
                    {
                        std::shared_lock rlock(font.mutex);
                        hbFont = font.hbFonts[fontIndex].hbFont;
                        hbFace = font.hbFonts[fontIndex].hbFace;
                    }
                    ColrEncoder::GlyphResolver resolver = [this, &font, fontIndex](
                        hb_font_t*, hb_codepoint_t clipGlyph,
                        float* eminx, float* eminy, float* emaxx, float* emaxy) -> uint32_t
                    {
                        ensureGlyphEncoded(font, fontIndex, clipGlyph);
                        uint64_t clipKey = glyphKey(fontIndex, clipGlyph);
                        std::shared_lock lock2(font.mutex);
                        auto cit = font.glyphs.find(clipKey);
                        if (cit == font.glyphs.end() || cit->second.is_empty) {
                            *eminx = *eminy = *emaxx = *emaxy = 0;
                            return 0;
                        }
                        *eminx = cit->second.ext_min_x;
                        *eminy = cit->second.ext_min_y;
                        *emaxx = cit->second.ext_max_x;
                        *emaxy = cit->second.ext_max_y;
                        return cit->second.atlas_offset;
                    };
                    auto encoded = ColrEncoder::encode(hbFont, glyphId, 0,
                                                       HB_COLOR(0, 0, 0, 255), resolver);
                    if (!encoded.instructions.empty()) {
                        std::unique_lock wlock2(font.mutex);
                        font.colrGlyphs[key] = ColrGlyphData{
                            std::move(encoded.instructions),
                            std::move(encoded.colorStops)
                        };
                        sLog().info("COLR: encoded paint graph for gid={} fi={} ({} instructions)",
                                     glyphId, fontIndex, font.colrGlyphs[key].instructions.size());
                    }
                }
            }
            return;
        }
    }

    // Cache miss — extract glyph data without holding any lock.
    // Use a thread-local hb_gpu_draw_t since the per-entry one isn't thread-safe.
    struct GpuDrawOwner {
        hb_gpu_draw_t* ptr = nullptr;
        ~GpuDrawOwner() { if (ptr) hb_gpu_draw_destroy(ptr); }
    };
    thread_local GpuDrawOwner tlGpuDraw;
    if (!tlGpuDraw.ptr) {
        tlGpuDraw.ptr = hb_gpu_draw_create_or_fail();
    }
    hb_gpu_draw_t* g = tlGpuDraw.ptr;

    hb_font_t* hbFont;
    hb_face_t* hbFace;
    {
        std::shared_lock lock(font.mutex);
        auto& entry = font.hbFonts[fontIndex];
        hbFont = entry.hbFont;
        hbFace = entry.hbFace;
    }

    uint32_t upem = hb_face_get_upem(hbFace);
    float pixelScale = font.baseSize / static_cast<float>(upem);
    float adv = hb_font_get_glyph_h_advance(hbFont, glyphId) * pixelScale;

    hb_gpu_draw_reset(g);
    hb_gpu_draw_glyph(g, hbFont, glyphId);
    hb_blob_t* blob = hb_gpu_draw_encode(g);

    hb_glyph_extents_t ext;
    hb_gpu_draw_get_extents(g, &ext);

    uint32_t blobLen = 0;
    const char* blobData = hb_blob_get_data(blob, &blobLen);

    GlyphInfo info{};
    info.upem = upem;
    info.advance = adv;

    if (blobLen == 0) {
        // COLRv1 glyphs have no outlines — their visuals come from the paint graph.
        // Check for COLR paint before marking as empty.
        bool isColr = font.hasColrPaint && hb_ot_color_glyph_has_paint(hbFace, glyphId);
        if (isColr) {
            // Get extents from HarfBuzz font (not gpu draw, which has no contours)
            hb_glyph_extents_t fontExt;
            if (hb_font_get_glyph_extents(hbFont, glyphId, &fontExt)) {
                info.ext_min_x = static_cast<float>(fontExt.x_bearing);
                info.ext_min_y = static_cast<float>(fontExt.y_bearing + fontExt.height);
                info.ext_max_x = static_cast<float>(fontExt.x_bearing + fontExt.width);
                info.ext_max_y = static_cast<float>(fontExt.y_bearing);
            }
            info.is_empty = false;
            info.is_colr = true;
            info.atlas_offset = 0; // no Slug atlas data
            sLog().debug("COLR: detected COLRv1 glyph gid={} fi={} extents=[{},{},{},{}]",
                         glyphId, fontIndex, info.ext_min_x, info.ext_min_y, info.ext_max_x, info.ext_max_y);
        } else {
            info.is_empty = true;
            info.atlas_offset = 0;
            info.ext_min_x = 0;
            info.ext_min_y = 0;
            info.ext_max_x = 0;
            info.ext_max_y = 0;
        }
        {
            std::unique_lock lock(font.mutex);
            if (!font.glyphs.count(key)) {
                info.numTexels = 0; // empty/COLR placeholder occupies no atlas storage
                info.lastUsedGen = font.currentGen;
                font.glyphs[key] = info;
            }
        }
        hb_gpu_draw_recycle_blob(g, blob);
        if (!isColr) return;
    } else {
        // Non-empty outline — store in Slug atlas
        info.is_empty = false;
        info.ext_min_x = static_cast<float>(ext.x_bearing);
        info.ext_min_y = static_cast<float>(ext.y_bearing + ext.height);
        info.ext_max_x = static_cast<float>(ext.x_bearing + ext.width);
        info.ext_max_y = static_cast<float>(ext.y_bearing);

        const int16_t* src = reinterpret_cast<const int16_t*>(blobData);
        uint32_t numInt16 = blobLen / sizeof(int16_t);
        uint32_t numTexels = (numInt16 + 3) / 4;

        {
            std::unique_lock lock(font.mutex);
            if (font.glyphs.count(key)) {
                hb_gpu_draw_recycle_blob(g, blob);
                return;
            }

            // Atlas layout: storage holds two int16 per i32 component (8 i16 per
            // vec4<i32> texel), halving the GPU buffer size. atlasUsed is the
            // count of "virtual" 4-int16 texels the shader sees; storage size
            // is (atlasUsed + 1) / 2 vec4<i32> texels. Each glyph must start
            // on a storage-texel boundary (even virtual offset) so the shader's
            // hb_gpu_fetch can round offset/2 cleanly.
            if (font.atlasUsed & 1u)
                font.atlasUsed++;

            info.atlas_offset = font.atlasUsed;
            uint32_t glyphStorageBase = font.atlasUsed / 2;
            uint32_t glyphStorageTexels = (numTexels + 1) / 2;
            uint32_t neededI32 = (glyphStorageBase + glyphStorageTexels) * 4;
            if (font.atlasData.size() < neededI32)
                font.atlasData.resize(neededI32 * 2, 0);

            for (uint32_t s = 0; s < glyphStorageTexels; s++) {
                for (uint32_t c = 0; c < 4; c++) {
                    uint32_t lowIdx  = s * 8 + c;
                    uint32_t highIdx = s * 8 + 4 + c;
                    uint32_t lo = (lowIdx  < numInt16) ? static_cast<uint16_t>(src[lowIdx])  : 0u;
                    uint32_t hi = (highIdx < numInt16) ? static_cast<uint16_t>(src[highIdx]) : 0u;
                    font.atlasData[(glyphStorageBase + s) * 4 + c] =
                        static_cast<int32_t>(lo | (hi << 16));
                }
            }
            uint32_t prevUsed = font.atlasUsed;
            font.atlasUsed += numTexels;

            info.numTexels = numTexels;
            info.lastUsedGen = font.currentGen;
            font.glyphs[key] = info;
            font.atlasVersion.fetch_add(1, std::memory_order_release);

            // Log on every 1M-virtual-texel growth boundary to track atlas inflation.
            // Storage is (atlasUsed + 1) / 2 vec4<i32> texels = packed-int16 layout.
            constexpr uint32_t logStep = 1u << 20;
            if (prevUsed / logStep != font.atlasUsed / logStep) {
                uint64_t storageBytes = (static_cast<uint64_t>(font.atlasUsed) + 1) / 2 * 16;
                sLog().warn("FontAtlas '{}' atlasUsed crossed {} virtual texels ({} MB GPU storage), glyphs={}, hbFonts={}, last glyph=(fi={}, gid={}, blobLen={})",
                            font.name, font.atlasUsed,
                            storageBytes / (1024 * 1024),
                            font.glyphs.size(), font.hbFonts.size(),
                            fontIndex, glyphId, blobLen);

                // Break down atlas occupancy by fontIndex/style.
                // Glyph blobs are appended sequentially, so per-glyph size = next_offset - this_offset.
                std::vector<std::pair<uint32_t, uint32_t>> entries; // (atlas_offset, fontIndex)
                entries.reserve(font.glyphs.size());
                for (const auto& [k, gi] : font.glyphs) {
                    if (gi.is_empty || gi.is_colr) continue;
                    entries.push_back({gi.atlas_offset, static_cast<uint32_t>(k >> 32)});
                }
                std::sort(entries.begin(), entries.end());

                struct Bucket { uint64_t texels = 0; uint32_t glyphs = 0; };
                Bucket primary, primaryStyled, fallback, fallbackStyled;
                for (size_t i = 0; i < entries.size(); i++) {
                    uint32_t off = entries[i].first;
                    uint32_t fi = entries[i].second;
                    uint32_t nextOff = (i + 1 < entries.size()) ? entries[i + 1].first : font.atlasUsed;
                    uint32_t sz = (nextOff > off) ? (nextOff - off) : 0;
                    if (fi >= font.hbFonts.size()) continue;
                    bool styled = font.hbFonts[fi].style != FontStyle{};
                    bool isPrimary = (font.hbFonts[fi].baseFontIndex == 0 && (fi == 0 || styled));
                    Bucket& b = isPrimary ? (styled ? primaryStyled : primary)
                                          : (styled ? fallbackStyled : fallback);
                    b.texels += sz;
                    b.glyphs++;
                }
                // Storage MB per virtual-texel count: (texels+1)/2 storage texels * 16 bytes.
                auto mb = [](uint64_t t) { return ((t + 1) / 2 * 16) / (1024 * 1024); };
                sLog().warn("  breakdown: primary={} MB ({} glyphs), primary-styled={} MB ({} glyphs), fallback={} MB ({} glyphs), fallback-styled={} MB ({} glyphs)",
                            mb(primary.texels), primary.glyphs,
                            mb(primaryStyled.texels), primaryStyled.glyphs,
                            mb(fallback.texels), fallback.glyphs,
                            mb(fallbackStyled.texels), fallbackStyled.glyphs);
            }
        }
        hb_gpu_draw_recycle_blob(g, blob);

        // Check for COLRv1 paint data on non-empty outline glyphs
        if (!(font.hasColrPaint &&
              hb_ot_color_has_paint(hbFace) &&
              hb_ot_color_glyph_has_paint(hbFace, glyphId)))
            return;
    }

    // Encode COLRv1 paint graph
    {
        sLog().debug("COLR: encoding glyph {} (font index {})", glyphId, fontIndex);
        // Encode the paint graph. The resolver callback ensures each clip glyph's
        // outline is in the atlas (recursive call to ensureGlyphEncoded).
        ColrEncoder::GlyphResolver resolver = [this, &font, fontIndex](
            hb_font_t* resolverFont, hb_codepoint_t clipGlyph,
            float* eminx, float* eminy, float* emaxx, float* emaxy) -> uint32_t
        {
            ensureGlyphEncoded(font, fontIndex, clipGlyph);
            uint64_t clipKey = glyphKey(fontIndex, clipGlyph);
            std::shared_lock lock(font.mutex);
            auto it = font.glyphs.find(clipKey);
            if (it == font.glyphs.end() || it->second.is_empty) {
                *eminx = *eminy = *emaxx = *emaxy = 0;
                return 0;
            }
            *eminx = it->second.ext_min_x;
            *eminy = it->second.ext_min_y;
            *emaxx = it->second.ext_max_x;
            *emaxy = it->second.ext_max_y;
            return it->second.atlas_offset;
        };

        auto encoded = ColrEncoder::encode(hbFont, glyphId, 0,
                                           HB_COLOR(0, 0, 0, 255), resolver);

        if (!encoded.instructions.empty()) {
            std::unique_lock lock(font.mutex);
            font.colrGlyphs[key] = ColrGlyphData{
                std::move(encoded.instructions),
                std::move(encoded.colorStops)
            };
            // Mark the glyph as COLR
            auto git = font.glyphs.find(key);
            if (git != font.glyphs.end()) {
                git->second.is_colr = true;
            }
        }
    }
}

bool TextSystem::registerFont(const std::string& name,
                               const std::vector<std::vector<uint8_t>>& ttfDataList,
                               float baseSize)
{
    if (ttfDataList.empty()) {
        sLog().error("registerFont '{}': no font data provided", name);
        return false;
    }

    std::unique_lock rlock(registryMutex_);
    // Build the FontData in a fresh shared_ptr; only insert into fonts_
    // on success so a partial-build error doesn't replace an existing
    // entry with a half-populated one. If a font with this name already
    // exists, the prior shared_ptr drops out of fonts_ at the end and
    // any in-flight workers keep their own copies until done.
    auto sptr = std::make_shared<FontData>();
    FontData& font = *sptr;
    font.name = name;
    font.baseSize = baseSize;

    // 1. Load all fonts via HarfBuzz and create GPU draw objects
    for (const auto& ttfData : ttfDataList) {
        FontData::HBEntry entry;
        entry.hbBlob = hb_blob_create(reinterpret_cast<const char*>(ttfData.data()),
                                       static_cast<uint32_t>(ttfData.size()),
                                       HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
        entry.hbFace = hb_face_create(entry.hbBlob, 0);
        entry.hbFont = hb_font_create(entry.hbFace);
        entry.gpuDraw = hb_gpu_draw_create_or_fail();
        if (!entry.gpuDraw) {
            sLog().error("registerFont '{}': hb_gpu_draw_create_or_fail() returned null", name);
            hb_font_destroy(entry.hbFont);
            hb_face_destroy(entry.hbFace);
            hb_blob_destroy(entry.hbBlob);
            // Already-pushed entries in font.hbFonts are cleaned up by
            // ~FontData when sptr drops here.
            return false;
        }
        font.hbFonts.push_back(entry);
    }

    // Use primary font for metrics
    const auto& primary = font.hbFonts[0];
    uint32_t upem = hb_face_get_upem(primary.hbFace);
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

    // Measure reference character advance ('M') for snapping fallback glyph advances
    {
        uint32_t refGid;
        if (hb_font_get_nominal_glyph(primary.hbFont, 'M', &refGid))
            font.charWidth = hb_font_get_glyph_h_advance(primary.hbFont, refGid) * pixelScale;
        else
            font.charWidth = baseSize * 0.6f; // reasonable fallback
    }

    // Check if any font in this set has COLRv1 paint data
    for (const auto& entry : font.hbFonts) {
        if (hb_ot_color_has_paint(entry.hbFace)) {
            font.hasColrPaint = true;
            sLog().info("Font '{}': COLRv1 paint support detected", name);
            break;
        }
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

    sLog().info("Registered font '{}': {} pre-encoded glyphs, atlas {} texels, baseSize={:.0f}, {} font(s)",
                name, static_cast<uint32_t>(font.glyphs.size()),
                font.atlasUsed, baseSize, font.hbFonts.size());

    // Publish the populated FontData. Any prior shared_ptr at this slot
    // drops out of the map; existing workers keep their copy alive.
    fonts_[name] = std::move(sptr);
    return true;
}

int32_t TextSystem::addFallbackFont(const std::string& name, const std::vector<uint8_t>& ttfData)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return -1;
    return addFallbackFontLocked(*it->second, name, ttfData);
}

int32_t TextSystem::addFallbackFontLocked(FontData& font, const std::string& name,
                                           const std::vector<uint8_t>& ttfData)
{
    // Content-hash the blob to dedup concurrent or repeated calls for the same font.
    uint64_t blobHash = std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char*>(ttfData.data()), ttfData.size()));
    if (blobHash == 0) blobHash = 1; // 0 reserved for "no hash"

    // Take the write lock for the whole operation: lookup, dedup check, and append
    // must be atomic w.r.t. concurrent shaping workers that may call this simultaneously.
    std::unique_lock lock(font.mutex);

    // Dedup: if a base (non-styled) fallback with the same content was already added,
    // return its index instead of creating a duplicate.
    for (size_t i = 0; i < font.hbFonts.size(); i++) {
        if (font.hbFonts[i].blobHash == blobHash &&
            font.hbFonts[i].style == FontStyle{}) {
            return static_cast<int32_t>(i);
        }
    }

    FontData::HBEntry entry;
    entry.hbBlob = hb_blob_create(reinterpret_cast<const char*>(ttfData.data()),
                                   static_cast<uint32_t>(ttfData.size()),
                                   HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
    entry.hbFace = hb_face_create(entry.hbBlob, 0);
    entry.hbFont = hb_font_create(entry.hbFace);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        sLog().error("addFallbackFont '{}': hb_gpu_draw_create_or_fail() returned null", name);
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return -1;
    }
    entry.blobHash = blobHash;

    bool hasPaint = hb_ot_color_has_paint(entry.hbFace);
    sLog().info("COLR: fallback font check for '{}': hb_ot_color_has_paint={} face={} fi={}",
                 name, hasPaint, (void*)entry.hbFace, font.hbFonts.size());
    if (!font.hasColrPaint && hasPaint) {
        font.hasColrPaint = true;
        sLog().info("COLR: fallback font for '{}' has COLRv1 paint support", name);
    }

    int32_t newFi = static_cast<int32_t>(font.hbFonts.size());
    font.hbFonts.push_back(entry);

    sLog().info("Added fallback font #{} to '{}'", newFi, name);
    return newFi;
}

void TextSystem::beginFontFrame(const std::string& name)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return;
    // Single-writer (render thread); shaping workers only read currentGen.
    // Plain increment is fine.
    it->second->currentGen++;
}

bool TextSystem::compactFontAtlasLRU(const std::string& name,
                                     uint32_t budgetTexels,
                                     uint32_t targetTexels)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return false;
    FontData& font = *it->second;

    {
        std::shared_lock lock(font.mutex);
        if (font.atlasUsed <= budgetTexels) return false;
    }

    std::unique_lock lock(font.mutex);
    // Re-check under write lock (another thread may have compacted).
    if (font.atlasUsed <= budgetTexels) return false;

    uint32_t prevAtlasUsed = font.atlasUsed;
    size_t prevGlyphs = font.glyphs.size();
    size_t prevColr = font.colrGlyphs.size();

    // Build a list of (lastUsedGen, key) for non-empty / non-COLR glyphs that
    // actually consume atlas storage. Empty + COLR placeholder glyphs stay —
    // they cost nothing and dropping them just churns later re-encodes.
    std::vector<std::pair<uint32_t, uint64_t>> entries;
    entries.reserve(font.glyphs.size());
    for (const auto& [k, gi] : font.glyphs) {
        if (gi.is_empty || gi.is_colr || gi.numTexels == 0) continue;
        entries.push_back({gi.lastUsedGen, k});
    }
    // Sort by lastUsedGen DESCENDING — newest first, oldest at the back.
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Walk from newest, accumulating size. Keep glyphs while accumulated
    // texels stay under targetTexels. Past that, mark for eviction.
    std::unordered_set<uint64_t> keep;
    keep.reserve(entries.size());
    uint64_t kept_texels = 1; // sentinel slot 0
    for (const auto& [gen, key] : entries) {
        const auto& gi = font.glyphs.at(key);
        if (kept_texels + gi.numTexels > targetTexels) break;
        kept_texels += gi.numTexels;
        keep.insert(key);
    }

    // Defragment atlasData: pack kept glyph blobs back-to-back starting at
    // virtual offset 1. Storage is two virtual texels per vec4<i32>. Each
    // glyph still requires even-virtual alignment.
    // Use a temporary destination vector to avoid in-place overlap pitfalls
    // (a kept glyph's source range could be below its new dest range).
    std::vector<int32_t> newData((font.atlasData.size()), 0);
    uint32_t newAtlasUsed = 1;

    // Iterate in newest-first order (entries) so hot glyphs land near the
    // front of the new atlas (better cache locality for next-frame access).
    for (const auto& [gen, key] : entries) {
        if (!keep.count(key)) continue;
        auto mit = font.glyphs.find(key);
        if (mit == font.glyphs.end()) continue;
        GlyphInfo& gi = mit->second;

        // Align new offset to even virtual texel.
        if (newAtlasUsed & 1u) newAtlasUsed++;

        uint32_t srcStorageBase = gi.atlas_offset / 2;
        uint32_t dstStorageBase = newAtlasUsed / 2;
        uint32_t storageTexels = (gi.numTexels + 1) / 2;

        std::copy_n(font.atlasData.data() + srcStorageBase * 4,
                    storageTexels * 4,
                    newData.data() + dstStorageBase * 4);

        gi.atlas_offset = newAtlasUsed;
        newAtlasUsed += gi.numTexels;
    }

    // Erase evicted glyphs. We preserved kept glyphs above; everything else
    // with non-zero numTexels that wasn't kept goes.
    for (auto it2 = font.glyphs.begin(); it2 != font.glyphs.end(); ) {
        const GlyphInfo& gi = it2->second;
        if (!gi.is_empty && !gi.is_colr && gi.numTexels > 0 && !keep.count(it2->first)) {
            it2 = font.glyphs.erase(it2);
        } else {
            ++it2;
        }
    }

    // COLR paint graphs encode atlas offsets of clip glyphs; defrag invalidates
    // those offsets. Drop the cache entirely — they re-encode lazily.
    font.colrGlyphs.clear();

    font.atlasData = std::move(newData);
    font.atlasUsed = newAtlasUsed;
    // Drives the renderer to detect the wrap-back (atlasUsed < uploadedSize)
    // and force a full reupload.
    font.atlasVersion.fetch_add(1, std::memory_order_release);

    sLog().info("FontAtlas '{}' compacted: kept {}/{} glyphs, dropped {} COLR entries, {} -> {} virtual texels ({} MB -> {} MB storage)",
                name, keep.size(), prevGlyphs, prevColr,
                prevAtlasUsed, newAtlasUsed,
                (static_cast<uint64_t>(prevAtlasUsed)   + 1) / 2 * 16 / (1024 * 1024),
                (static_cast<uint64_t>(newAtlasUsed)    + 1) / 2 * 16 / (1024 * 1024));
    return true;
}

void TextSystem::setSystemFallback(SystemFallbackFn fn)
{
    std::unique_lock rlock(registryMutex_);
    systemFallback_ = std::move(fn);
}

void TextSystem::setEmojiFallback(EmojiFallbackFn fn)
{
    std::unique_lock rlock(registryMutex_);
    emojiFallback_ = std::move(fn);
}

void TextSystem::setPrimaryFontPath(const std::string& name, const std::string& path)
{
    std::unique_lock rlock(registryMutex_);
    fontPrimaryPaths_[name] = path;
}

bool TextSystem::addSyntheticBoldVariant(const std::string& name, float xStrength, float yStrength)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return false;

    FontData& font = *it->second;
    if (font.hbFonts.empty()) return false;

    const auto& primary = font.hbFonts[0];

    FontData::HBEntry entry;
    entry.hbBlob = hb_blob_reference(primary.hbBlob);
    entry.hbFace = hb_face_reference(primary.hbFace);
    entry.hbFont = hb_font_create(entry.hbFace);
    hb_font_set_synthetic_bold(entry.hbFont, xStrength, yStrength, false);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        sLog().error("addSyntheticBoldVariant '{}': hb_gpu_draw_create_or_fail() returned null", name);
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

    sLog().info("Added synthetic bold variant to '{}' (fi={})", name, newFi);
    return true;
}

bool TextSystem::addSyntheticItalicVariant(const std::string& name, float slant)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return false;

    FontData& font = *it->second;
    if (font.hbFonts.empty()) return false;

    const auto& primary = font.hbFonts[0];

    FontData::HBEntry entry;
    entry.hbBlob = hb_blob_reference(primary.hbBlob);
    entry.hbFace = hb_face_reference(primary.hbFace);
    entry.hbFont = hb_font_create(entry.hbFace);
    hb_font_set_synthetic_slant(entry.hbFont, slant);
    entry.gpuDraw = hb_gpu_draw_create_or_fail();
    if (!entry.gpuDraw) {
        sLog().error("addSyntheticItalicVariant '{}': hb_gpu_draw_create_or_fail() returned null", name);
        hb_font_destroy(entry.hbFont);
        hb_face_destroy(entry.hbFace);
        hb_blob_destroy(entry.hbBlob);
        return false;
    }

    entry.baseFontIndex = 0;
    entry.style = {.bold = false, .italic = true};

    uint32_t newFi = static_cast<uint32_t>(font.hbFonts.size());
    font.hbFonts.push_back(entry);

    uint64_t variantKey = (static_cast<uint64_t>(0) << 8) | entry.style.key();
    font.styledVariants[variantKey] = newFi;

    sLog().info("Added synthetic italic variant to '{}' (fi={})", name, newFi);
    return true;
}

void TextSystem::tagFontStyle(const std::string& name, uint32_t fontIndex, FontStyle style)
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    if (it == fonts_.end()) return;

    FontData& font = *it->second;
    if (fontIndex >= font.hbFonts.size()) return;

    font.hbFonts[fontIndex].style = style;
    uint64_t variantKey = (static_cast<uint64_t>(0) << 8) | style.key();
    font.styledVariants[variantKey] = fontIndex;
}

// --- Font registry: resolve fonts with style ---


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
    if (style.italic)
        hb_font_set_synthetic_slant(entry.hbFont, italicSlant_);
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

    sLog().debug("Created styled variant fi={} (base={} bold={}) for '{}'",
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
        uint32_t cp = utf8::decodeAdvance(p, end);
        uint32_t gid;
        if (hb_font_get_nominal_glyph(font.hbFonts[0].hbFont, cp, &gid) &&
            !hb_font_get_nominal_glyph(font.hbFonts[primaryFi].hbFont, cp, &gid)) {
            return 0;
        }
    }
    return primaryFi;
}

TextSystem::ResolvedGlyph TextSystem::resolveGlyph(FontData& font, const std::string& fontName,
                                                    char32_t cp, FontStyle style, uint32_t excludeFi,
                                                    bool forceEmojiPresentation)
{
    bool emojiPresentation = forceEmojiPresentation || isWidenedEmoji(cp);
    bool preferNonColr = font.hasColrPaint && !emojiPresentation && (wcwidth(cp) < 2);
    ResolvedGlyph colrFallback = {0, 0};
    ResolvedGlyph nonColrFallback = {0, 0};

    // Try existing fonts under read lock
    {
        std::shared_lock lock(font.mutex);
        for (uint32_t fi = 0; fi < font.hbFonts.size(); fi++) {
            auto& entry = font.hbFonts[fi];
            if (entry.style != FontStyle{}) continue;
            if (fi == excludeFi) continue;

            uint32_t gid;
            if (!hb_font_get_nominal_glyph(entry.hbFont, cp, &gid)) continue;

            bool isColr = hb_ot_color_has_paint(entry.hbFace) &&
                          hb_ot_color_glyph_has_paint(entry.hbFace, gid);

            if (emojiPresentation) {
                // For emoji: prefer COLR, save non-COLR as fallback
                if (isColr) {
                    lock.unlock();
                    uint32_t styledFi = getStyledVariant(font, fi, style);
                    lock.lock();
                    uint32_t styledGid;
                    if (!hb_font_get_nominal_glyph(font.hbFonts[styledFi].hbFont, cp, &styledGid))
                        styledGid = gid;
                    return {styledFi, styledGid};
                }
                if (nonColrFallback.glyphId == 0) nonColrFallback = {fi, gid};
            } else if (preferNonColr && isColr) {
                if (colrFallback.glyphId == 0) colrFallback = {fi, gid};
            } else {
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

    // For emoji with no COLRv1 found yet: try emoji-specific font lookup.
    // Caller (shapeRun/shapeText) holds registryMutex_ shared, so reading
    // emojiFallback_ here is safe and addFallbackFontLocked is reentrant-safe
    // (std::shared_mutex is NOT — never call public addFallbackFont here).
    if (emojiPresentation && emojiFallback_) {
        auto fallbackData = emojiFallback_(cp);
        if (!fallbackData.empty()) {
            int32_t addedFi = addFallbackFontLocked(font, fontName, fallbackData);
            if (addedFi >= 0) {
                std::shared_lock lock(font.mutex);
                uint32_t newFi = static_cast<uint32_t>(addedFi);
                uint32_t gid;
                if (hb_font_get_nominal_glyph(font.hbFonts[newFi].hbFont, cp, &gid)) {
                    bool isColr = hb_ot_color_has_paint(font.hbFonts[newFi].hbFace) &&
                                  hb_ot_color_glyph_has_paint(font.hbFonts[newFi].hbFace, gid);
                    if (isColr) {
                        lock.unlock();
                        uint32_t styledFi = getStyledVariant(font, newFi, style);
                        lock.lock();
                        uint32_t styledGid;
                        if (!hb_font_get_nominal_glyph(font.hbFonts[styledFi].hbFont, cp, &styledGid))
                            styledGid = gid;
                        return {styledFi, styledGid};
                    }
                    if (nonColrFallback.glyphId == 0) nonColrFallback = {newFi, gid};
                }
            }
        }
    }

    // Try system font fallback (rare — loads a new font file)
    if (!emojiPresentation && systemFallback_) {
        auto pathIt = fontPrimaryPaths_.find(fontName);
        std::string primaryPath = (pathIt != fontPrimaryPaths_.end()) ? pathIt->second : "";
        auto fallbackData = systemFallback_(primaryPath, cp);
        if (!fallbackData.empty()) {
            int32_t addedFi = addFallbackFontLocked(font, fontName, fallbackData);
            if (addedFi >= 0) {
                std::shared_lock lock(font.mutex);
                uint32_t newFi = static_cast<uint32_t>(addedFi);
                uint32_t gid;
                if (hb_font_get_nominal_glyph(font.hbFonts[newFi].hbFont, cp, &gid)) {
                    bool isColr = hb_ot_color_has_paint(font.hbFonts[newFi].hbFace) &&
                                  hb_ot_color_glyph_has_paint(font.hbFonts[newFi].hbFace, gid);
                    if (preferNonColr && isColr) {
                        if (colrFallback.glyphId == 0) colrFallback = {newFi, gid};
                    } else {
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
        }
    }

    // Use best available fallback
    ResolvedGlyph fallback = emojiPresentation ? nonColrFallback : colrFallback;
    if (fallback.glyphId != 0) {
        std::shared_lock lock(font.mutex);
        uint32_t fi = fallback.fontIndex;
        uint32_t gid = fallback.glyphId;
        lock.unlock();
        uint32_t styledFi = getStyledVariant(font, fi, style);
        lock.lock();
        uint32_t styledGid;
        if (!hb_font_get_nominal_glyph(font.hbFonts[styledFi].hbFont, cp, &styledGid))
            styledGid = gid;
        return {styledFi, styledGid};
    }

    return {0, 0};
}

ShapedText TextSystem::shapeText(const std::string& fontName, const std::string& text,
                                  float fontSize, float wrapWidth, int align,
                                  FontStyle style)
{
    // Held for the duration: resolveGlyph reads emojiFallback_/systemFallback_/
    // fontPrimaryPaths_ unlocked, and addFallbackFontLocked is called from
    // resolveGlyph assuming this critical section is live.
    std::shared_lock rlock(registryMutex_);
    auto fontIt = fonts_.find(fontName);
    if (fontIt == fonts_.end()) return {};
    FontData& font = *fontIt->second;

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
    // SheenBidi truncates the paragraph to the last well-formed UTF-8
    // boundary; the level array length equals paraLen, which can be
    // shorter than text.size() (or even 0) when the input contains
    // malformed bytes. Iterate against paraLen, not text.size().
    const SBUInteger paraLen = SBParagraphGetLength(bidiPara);

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
    if (paraLen > 0) {
        SBUInteger start = 0;
        SBLevel cur = levels[0];
        for (SBUInteger i = 1; i <= paraLen; i++) {
            if (i == paraLen || levels[i] != cur) {
                bidiRuns.push_back({start, i - start, cur});
                if (i < paraLen) {
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

        uint32_t fontUpem = hb_face_get_upem(segHbFace);
        float fontPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(fontUpem);

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), static_cast<int>(text.size()),
                           static_cast<uint32_t>(seg.offset), static_cast<int>(seg.length));
        hb_buffer_set_direction(buf, (seg.level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, hb_script_from_iso15924_tag(
            static_cast<hb_tag_t>(SBScriptGetUnicodeTag(seg.script))));
        hb_buffer_set_language(buf, hb_language_get_default());
        hb_shape(segHbFont, buf, nullptr, 0);

        uint32_t count;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);

        auto pushGlyph = [&](uint32_t idx) {
            uint32_t gid = infos[idx].codepoint;
            uint32_t glyphFi = fi;

            if (gid == 0) {
                // Decode codepoint from cluster
                uint32_t cluster = infos[idx].cluster;
                int consumed = 0;
                uint32_t cp = utf8::decode(text.c_str() + cluster, static_cast<int>(text.size() - cluster), consumed);

                // Skip variation selectors — they modify the preceding character
                // and are not independently visible.
                if (cp >= 0xFE00 && cp <= 0xFE0F) return;

                bool vs16 = hasVS16(text, cluster, consumed);

                auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi, vs16);
                if (resolved.glyphId != 0) {
                    glyphFi = resolved.fontIndex;
                    gid = resolved.glyphId;
                    int cw = wcwidth(static_cast<wchar_t>(cp));
                    float adv = font.charWidth * scale * (cw > 1 ? 2.0f : 1.0f);
                    ensureGlyphEncoded(font, glyphFi, gid);
                    allGlyphs.push_back({glyphKey(glyphFi, gid), infos[idx].cluster,
                        adv, 0, 0, seg.level});
                    return;
                }
            }

            // For emoji presentation or VS16: try to find a COLR version
            {
                uint32_t cluster = infos[idx].cluster;
                int consumed = 0;
                uint32_t cp = utf8::decode(text.c_str() + cluster, static_cast<int>(text.size() - cluster), consumed);

                // Skip variation selectors — not independently visible
                if (cp >= 0xFE00 && cp <= 0xFE0F) return;

                bool emoji = isWidenedEmoji(static_cast<char32_t>(cp)) || hasVS16(text, cluster, consumed);
                if (emoji) {
                    bool currentIsColr = hb_ot_color_has_paint(segHbFace) &&
                                         hb_ot_color_glyph_has_paint(segHbFace, gid);
                    if (!currentIsColr) {
                        auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi, true);
                        if (resolved.glyphId != 0) {
                            glyphFi = resolved.fontIndex;
                            gid = resolved.glyphId;
                        }
                    }
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
                pushGlyph(static_cast<uint32_t>(i));
        } else {
            for (uint32_t i = 0; i < count; i++)
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
                                float fontSize, FontStyle style,
                                std::span<const std::pair<uint32_t, int>> byteToCell)
{
    // Held for the duration: resolveGlyph reads emojiFallback_/systemFallback_/
    // fontPrimaryPaths_ unlocked, and addFallbackFontLocked is called from
    // resolveGlyph assuming this critical section is live.
    std::shared_lock rlock(registryMutex_);
    auto fontIt = fonts_.find(fontName);
    if (fontIt == fonts_.end()) return {};
    FontData& font = *fontIt->second;

    if (text.empty()) return {};

    float scale = fontSize / font.baseSize;

    // BiDi analysis — get per-character embedding levels
    SBCodepointSequence cpSeq{SBStringEncodingUTF8, const_cast<char*>(text.c_str()),
                              static_cast<SBUInteger>(text.size())};
    SBAlgorithmRef bidiAlgo = SBAlgorithmCreate(&cpSeq);
    SBParagraphRef bidiPara = SBAlgorithmCreateParagraph(
        bidiAlgo, 0, static_cast<SBUInteger>(text.size()), SBLevelDefaultLTR);
    const SBLevel* levels = SBParagraphGetLevelsPtr(bidiPara);
    // SheenBidi truncates the paragraph at the last well-formed UTF-8
    // boundary, so the level array can be shorter than text.size() when
    // the input contains malformed bytes (e.g. cat-ing a binary file).
    // The script locator iterates the full codepoint sequence and may
    // emit runs past the paragraph's end — clamp before indexing.
    const SBUInteger paraLen = SBParagraphGetLength(bidiPara);

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

        uint32_t fontUpem = hb_face_get_upem(hbFace);
        float fontPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(fontUpem);

        bool segmentRtl = sr.offset < paraLen && (levels[sr.offset] & 1) != 0;

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), static_cast<int>(text.size()),
                           static_cast<uint32_t>(sr.offset), static_cast<int>(sr.length));
        hb_buffer_set_direction(buf, segmentRtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, hb_script_from_iso15924_tag(
            static_cast<hb_tag_t>(SBScriptGetUnicodeTag(sr.script))));
        hb_buffer_set_language(buf, hb_language_get_default());
        hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_shape(hbFont, buf, nullptr, 0);

        uint32_t count;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);

        for (uint32_t i = 0; i < count; i++) {
            uint32_t gid = infos[i].codepoint;
            uint32_t glyphFi = fi;

            if (gid == 0) {
                // Glyph missing — resolve via registry (handles style + fallback)
                uint32_t cluster = infos[i].cluster;
                int consumed = 0;
                uint32_t cp = utf8::decode(text.c_str() + cluster, static_cast<int>(text.size() - cluster), consumed);

                // Skip variation selectors — they modify the preceding character
                // and are not independently visible.
                if (cp >= 0xFE00 && cp <= 0xFE0F) continue;

                bool vs16 = hasVS16(text, cluster, consumed);
                bool emoji = isWidenedEmoji(static_cast<char32_t>(cp)) || vs16;

                // For emoji: use byteToCell to find the full byte range for
                // this cell (base + all combiningCps), then re-shape with the
                // emoji font. This handles ZWJ sequences, skin tone modifiers,
                // flag sequences, keycap sequences, etc.
                if (emoji) {
                    // Find which byteToCell entry this cluster belongs to
                    int btcIdx = -1;
                    for (int b = 0; b < static_cast<int>(byteToCell.size()); b++) {
                        if (byteToCell[b].first == cluster) { btcIdx = b; break; }
                    }
                    if (btcIdx >= 0) {
                        uint32_t cellByteStart = byteToCell[btcIdx].first;
                        uint32_t cellByteEnd = (btcIdx + 1 < static_cast<int>(byteToCell.size()))
                            ? byteToCell[btcIdx + 1].first
                            : static_cast<uint32_t>(text.size());

                        // Only attempt multi-codepoint re-shaping if the cell
                        // has more bytes than just the base codepoint
                        if (cellByteEnd - cellByteStart > static_cast<uint32_t>(consumed)) {
                            auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi, true);
                            if (resolved.glyphId != 0) {
                                uint32_t emojiFi = resolved.fontIndex;
                                hb_font_t* emojiHbFont;
                                hb_face_t* emojiHbFace;
                                {
                                    std::shared_lock lock2(font.mutex);
                                    emojiHbFont = font.hbFonts[emojiFi].hbFont;
                                    emojiHbFace = font.hbFonts[emojiFi].hbFace;
                                }

                                hb_buffer_t* emojiBuf = hb_buffer_create();
                                hb_buffer_add_utf8(emojiBuf, text.c_str(), static_cast<int>(text.size()),
                                                   cellByteStart, static_cast<int>(cellByteEnd - cellByteStart));
                                hb_buffer_set_direction(emojiBuf, segmentRtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
                                hb_buffer_set_script(emojiBuf, HB_SCRIPT_COMMON);
                                hb_buffer_set_language(emojiBuf, hb_language_get_default());
                                hb_buffer_set_cluster_level(emojiBuf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
                                hb_shape(emojiHbFont, emojiBuf, nullptr, 0);

                                uint32_t emojiCount;
                                hb_glyph_info_t* emojiInfos = hb_buffer_get_glyph_infos(emojiBuf, &emojiCount);
                                hb_glyph_position_t* emojiPositions = hb_buffer_get_glyph_positions(emojiBuf, &emojiCount);

                                uint32_t emojiUpem = hb_face_get_upem(emojiHbFace);
                                float emojiPixelScale = static_cast<float>(font.baseSize) / static_cast<float>(emojiUpem);

                                bool emittedGlyph = false;
                                for (uint32_t j = 0; j < emojiCount; j++) {
                                    uint32_t eGid = emojiInfos[j].codepoint;
                                    if (eGid == 0) continue;
                                    ensureGlyphEncoded(font, emojiFi, eGid);
                                    float adv = (j == 0)
                                        ? font.charWidth * scale * 2.0f
                                        : emojiPositions[j].x_advance * emojiPixelScale * scale;
                                    result.glyphs.push_back({
                                        glyphKey(emojiFi, eGid), cluster, adv,
                                        emojiPositions[j].x_offset * emojiPixelScale * scale,
                                        emojiPositions[j].y_offset * emojiPixelScale * scale,
                                        false, segmentRtl});
                                    emittedGlyph = true;
                                }

                                hb_buffer_destroy(emojiBuf);

                                if (emittedGlyph) {
                                    // Skip all remaining glyphs that belong to this cell's byte range
                                    while (i + 1 < count && infos[i + 1].cluster < cellByteEnd)
                                        i++;
                                    continue;
                                }
                            }
                        }
                    }
                }

                // Single codepoint fallback
                auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi, vs16);
                if (resolved.glyphId != 0) {
                    glyphFi = resolved.fontIndex;
                    gid = resolved.glyphId;
                    int cw = wcwidth(static_cast<wchar_t>(cp));
                    float adv = font.charWidth * scale * (cw > 1 ? 2.0f : 1.0f);
                    ensureGlyphEncoded(font, glyphFi, gid);
                    result.glyphs.push_back({glyphKey(glyphFi, gid), infos[i].cluster, adv, 0, 0, false, segmentRtl});
                    continue;
                }

                // Still missing — skip
                continue;
            }

            // Normal glyph — check if HarfBuzz substituted it (ligature, contextual form)
            bool isSubstitution = false;
            bool vs16 = false;
            uint32_t cp;
            {
                uint32_t cluster = infos[i].cluster;
                int consumed = 0;
                cp = utf8::decode(text.c_str() + cluster, static_cast<int>(text.size() - cluster), consumed);
                vs16 = hasVS16(text, cluster, consumed);

                uint32_t nominalGid;
                if (hb_font_get_nominal_glyph(hbFont, cp, &nominalGid)) {
                    isSubstitution = (gid != nominalGid);
                }
            }

            // Skip variation selectors — not independently visible
            if (cp >= 0xFE00 && cp <= 0xFE0F) continue;

            // For emoji presentation or VS16: try to find a COLR version
            bool emoji = isWidenedEmoji(static_cast<char32_t>(cp)) || vs16;
            if (emoji) {
                bool currentIsColr = hb_ot_color_has_paint(hbFace) &&
                                     hb_ot_color_glyph_has_paint(hbFace, gid);
                if (!currentIsColr) {
                    auto resolved = resolveGlyph(font, fontName, static_cast<char32_t>(cp), style, fi, true);
                    if (resolved.glyphId != 0) {
                        glyphFi = resolved.fontIndex;
                        gid = resolved.glyphId;
                    }
                }
            }

            ensureGlyphEncoded(font, glyphFi, gid);
            float adv;
            if (!isSubstitution && glyphFi != 0) {
                // Fallback font: snap advance to primary font's cell width so glyphs
                // land on the grid regardless of what the fallback font reports.
                int cw = wcwidth(static_cast<wchar_t>(cp));
                adv = font.charWidth * scale * (cw > 1 ? 2.0f : 1.0f);
            } else {
                adv = positions[i].x_advance * fontPixelScale * scale;
            }
            result.glyphs.push_back({
                glyphKey(glyphFi, gid),
                infos[i].cluster,
                adv,
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

std::shared_ptr<FontData> TextSystem::getFont(const std::string& name) const
{
    std::shared_lock rlock(registryMutex_);
    auto it = fonts_.find(name);
    return it != fonts_.end() ? it->second : nullptr;
}

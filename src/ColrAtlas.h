#pragma once

#include "ColrTypes.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

// Bucket-based atlas for cached COLRv1 emoji tiles.
// Each bucket has a fixed tile size and a single atlas texture.
// Tiles are allocated as grid cells — no packing complexity.
class ColrAtlas {
public:
    static constexpr uint32_t BUCKET_SIZES[] = { 32, 64, 128, 256 };
    static constexpr uint32_t NUM_BUCKETS = 4;
    static constexpr uint32_t ATLAS_DIMENSION = 2048; // atlas texture width/height

    struct TileLocation {
        uint32_t bucket;      // index into BUCKET_SIZES
        uint32_t x, y;        // pixel offset in atlas texture
        uint32_t tile_w, tile_h; // tile size in pixels (== BUCKET_SIZES[bucket])
    };

    // Look up or allocate a tile for a glyph at a given font size.
    // Returns nullptr if the glyph is already cached (no rasterization needed).
    // Returns a TileLocation if a new tile was allocated (needs rasterization).
    // glyph_key: same combined (fontIndex << 32 | glyphId) key used elsewhere.
    const TileLocation* acquireTile(uint64_t glyph_key, float font_size);

    // Check if a glyph is already cached at a suitable bucket for the given font size.
    // Returns the tile location, or nullptr if not cached.
    const TileLocation* findTile(uint64_t glyph_key, float font_size) const;

    // Select the bucket index for a given font size.
    static uint32_t bucketForSize(float font_size);

    // Get the atlas dimensions for a bucket.
    static uint32_t atlasDimension() { return ATLAS_DIMENSION; }
    static uint32_t tilesPerRow(uint32_t bucket) { return ATLAS_DIMENSION / BUCKET_SIZES[bucket]; }

    // Reset all allocations (e.g. on font change).
    void clear();

private:
    struct BucketState {
        uint32_t nextSlot = 0;  // next free grid slot
        uint32_t maxSlots = 0;  // total grid slots in this atlas
    };
    BucketState buckets_[NUM_BUCKETS];

    // Cache: (glyph_key, bucket) -> TileLocation
    struct CacheKey {
        uint64_t glyph_key;
        uint32_t bucket;
        bool operator==(const CacheKey& o) const { return glyph_key == o.glyph_key && bucket == o.bucket; }
    };
    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<uint64_t>()(k.glyph_key) ^ (std::hash<uint32_t>()(k.bucket) * 2654435761u);
        }
    };
    std::unordered_map<CacheKey, TileLocation, CacheKeyHash> cache_;

    // Pending tiles that need rasterization (returned by acquireTile, cleared after dispatch)
    TileLocation lastAllocated_;
};

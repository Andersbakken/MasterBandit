#pragma once

#include "ColrTypes.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// Bucket-based atlas for cached COLRv1 emoji tiles.
// Each bucket has a fixed tile size and its own atlas texture that can grow.
// Tiles are allocated as grid cells — no packing complexity.
// Generation-based eviction reclaims tiles not used in the current frame.
// If all tiles are current-generation and the bucket is full, the atlas
// doubles in size (up to MAX_ATLAS_DIM) rather than thrashing.
class ColrAtlas {
public:
    static constexpr uint32_t BUCKET_SIZES[] = { 32, 64, 128, 256, 512 };
    static constexpr uint32_t NUM_BUCKETS = 5;
    static constexpr uint32_t INITIAL_ATLAS_DIM = 2048;
    static constexpr uint32_t MAX_ATLAS_DIM = 8192;

    struct TileLocation {
        uint32_t bucket;      // index into BUCKET_SIZES
        uint32_t x, y;        // pixel offset in atlas texture
        uint32_t tile_w, tile_h; // tile size in pixels (== BUCKET_SIZES[bucket])
    };

    struct AcquireResult {
        const TileLocation* tile;  // non-null if new tile allocated (needs rasterization)
        bool grew;                 // true if bucket atlas was resized (old content needs copying)
    };

    // Advance the generation counter. Call once per frame before resolving glyphs.
    void advanceGeneration();
    uint32_t generation() const;

    // Look up or allocate a tile for a glyph at a given font size.
    // Returns tile=nullptr if already cached (no rasterization needed).
    // Returns a TileLocation if a new tile was allocated (needs rasterization).
    // If the bucket had to grow, grew=true (renderer must resize the GPU texture).
    AcquireResult acquireTile(uint64_t glyph_key, float font_size);

    // Check if a glyph is cached at a suitable bucket for the given font size.
    // Returns the tile location, or nullptr if not cached.
    // Updates the tile's generation to keep it alive.
    TileLocation* findTile(uint64_t glyph_key, float font_size);

    // Select the bucket index for a given font size.
    static uint32_t bucketForSize(float font_size);

    // Get the current atlas dimension for a bucket.
    uint32_t bucketAtlasDim(uint32_t bucket) const;

    static uint32_t tilesPerRow(uint32_t bucket, uint32_t atlasDim) {
        return atlasDim / BUCKET_SIZES[bucket];
    }

    // Reset all allocations (e.g. on font change).
    void clear();

private:
    struct TileEntry {
        TileLocation loc;
        uint32_t generation = 0;
    };

    struct BucketState {
        uint32_t nextSlot = 0;  // next free grid slot
        uint32_t maxSlots = 0;  // total grid slots in this atlas
        uint32_t atlasDim = INITIAL_ATLAS_DIM;
    };
    BucketState buckets_[NUM_BUCKETS];

    // Cache: (glyph_key, bucket) -> TileEntry
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
    std::unordered_map<CacheKey, TileEntry, CacheKeyHash> cache_;

    uint32_t currentGen_ = 0;

    mutable std::mutex mutex_;

    // Grow a bucket's atlas dimension. Returns false if already at max.
    bool growBucket(uint32_t bucket);

    // Remap existing tile positions after a bucket grows (positions don't change,
    // only maxSlots increases).
    void updateBucketSlots(uint32_t bucket);

    // Try to evict the oldest-generation tile in a bucket. Returns the evicted
    // slot index, or -1 if all tiles are current generation.
    int evictOldest(uint32_t bucket);
};

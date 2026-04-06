#include "ColrAtlas.h"
#include <spdlog/spdlog.h>

uint32_t ColrAtlas::bucketForSize(float font_size) {
    // Pick smallest bucket >= font_size
    for (uint32_t i = 0; i < NUM_BUCKETS; i++) {
        if (BUCKET_SIZES[i] >= static_cast<uint32_t>(font_size + 0.5f))
            return i;
    }
    return NUM_BUCKETS - 1; // largest bucket
}

const ColrAtlas::TileLocation* ColrAtlas::findTile(uint64_t glyph_key, float font_size) const {
    uint32_t bucket = bucketForSize(font_size);
    auto it = cache_.find({glyph_key, bucket});
    if (it != cache_.end())
        return &it->second;
    return nullptr;
}

const ColrAtlas::TileLocation* ColrAtlas::acquireTile(uint64_t glyph_key, float font_size) {
    uint32_t bucket = bucketForSize(font_size);

    // Already cached?
    CacheKey key{glyph_key, bucket};
    auto it = cache_.find(key);
    if (it != cache_.end())
        return nullptr; // no rasterization needed

    // Initialize bucket state on first use
    auto& bs = buckets_[bucket];
    if (bs.maxSlots == 0) {
        uint32_t tpr = tilesPerRow(bucket);
        bs.maxSlots = tpr * tpr;
    }

    // Atlas full?
    if (bs.nextSlot >= bs.maxSlots) {
        spdlog::warn("COLR atlas bucket {} ({}px) is full ({} tiles)",
                     bucket, BUCKET_SIZES[bucket], bs.maxSlots);
        return nullptr;
    }

    // Allocate grid slot
    uint32_t tpr = tilesPerRow(bucket);
    uint32_t slot = bs.nextSlot++;
    uint32_t gx = slot % tpr;
    uint32_t gy = slot / tpr;
    uint32_t tileSize = BUCKET_SIZES[bucket];

    TileLocation loc;
    loc.bucket = bucket;
    loc.x = gx * tileSize;
    loc.y = gy * tileSize;
    loc.tile_w = tileSize;
    loc.tile_h = tileSize;

    auto [inserted, _] = cache_.emplace(key, loc);
    lastAllocated_ = loc;
    return &inserted->second;
}

void ColrAtlas::clear() {
    cache_.clear();
    for (auto& bs : buckets_) {
        bs.nextSlot = 0;
    }
}

#include "ColrAtlas.h"
#include <spdlog/spdlog.h>
#include <limits>

uint32_t ColrAtlas::bucketForSize(float font_size) {
    // Pick smallest bucket >= font_size
    for (uint32_t i = 0; i < NUM_BUCKETS; i++) {
        if (BUCKET_SIZES[i] >= static_cast<uint32_t>(font_size + 0.5f))
            return i;
    }
    return NUM_BUCKETS - 1; // largest bucket
}

uint32_t ColrAtlas::bucketAtlasDim(uint32_t bucket) const {
    if (bucket >= NUM_BUCKETS) return INITIAL_ATLAS_DIM;
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_[bucket].atlasDim;
}

void ColrAtlas::advanceGeneration() {
    std::lock_guard<std::mutex> lock(mutex_);
    currentGen_++;
}

uint32_t ColrAtlas::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentGen_;
}

ColrAtlas::TileLocation* ColrAtlas::findTile(uint64_t glyph_key, float font_size) {
    uint32_t bucket = bucketForSize(font_size);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find({glyph_key, bucket});
    if (it != cache_.end()) {
        it->second.generation = currentGen_;
        return &it->second.loc;
    }
    return nullptr;
}

ColrAtlas::AcquireResult ColrAtlas::acquireTile(uint64_t glyph_key, float font_size) {
    uint32_t bucket = bucketForSize(font_size);
    std::lock_guard<std::mutex> lock(mutex_);

    // Already cached?
    CacheKey key{glyph_key, bucket};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.generation = currentGen_;
        return {nullptr, false};
    }

    auto& bs = buckets_[bucket];

    // Initialize bucket state on first use
    if (bs.maxSlots == 0)
        updateBucketSlots(bucket);

    bool grew = false;

    // Bucket full?
    if (bs.nextSlot >= bs.maxSlots) {
        // Try evicting the oldest tile
        int evicted = evictOldest(bucket);
        if (evicted < 0) {
            // All tiles are current generation — need to grow
            if (!growBucket(bucket)) {
                spdlog::error("COLR atlas bucket {} ({}px) at max size {}px and all tiles in use",
                              bucket, BUCKET_SIZES[bucket], bs.atlasDim);
                return {nullptr, false};
            }
            grew = true;
            // After growing, nextSlot is still valid — just more maxSlots available
        }
        // If we evicted, nextSlot was set to the evicted slot's index... but our
        // slot allocation is linear. Instead, reuse the evicted slot directly.
        if (!grew && evicted >= 0) {
            uint32_t tpr = tilesPerRow(bucket, bs.atlasDim);
            uint32_t slot = static_cast<uint32_t>(evicted);
            uint32_t gx = slot % tpr;
            uint32_t gy = slot / tpr;
            uint32_t tileSize = BUCKET_SIZES[bucket];

            TileEntry entry;
            entry.loc.bucket = bucket;
            entry.loc.x = gx * tileSize;
            entry.loc.y = gy * tileSize;
            entry.loc.tile_w = tileSize;
            entry.loc.tile_h = tileSize;
            entry.generation = currentGen_;

            auto [inserted, _] = cache_.emplace(key, entry);
            return {&inserted->second.loc, false};
        }
    }

    // Allocate next grid slot
    uint32_t tpr = tilesPerRow(bucket, bs.atlasDim);
    uint32_t slot = bs.nextSlot++;
    uint32_t gx = slot % tpr;
    uint32_t gy = slot / tpr;
    uint32_t tileSize = BUCKET_SIZES[bucket];

    TileEntry entry;
    entry.loc.bucket = bucket;
    entry.loc.x = gx * tileSize;
    entry.loc.y = gy * tileSize;
    entry.loc.tile_w = tileSize;
    entry.loc.tile_h = tileSize;
    entry.generation = currentGen_;

    auto [inserted, _] = cache_.emplace(key, entry);
    return {&inserted->second.loc, grew};
}

int ColrAtlas::evictOldest(uint32_t bucket) {
    uint32_t oldestGen = std::numeric_limits<uint32_t>::max();
    CacheKey oldestKey{0, 0};
    int oldestSlot = -1;

    uint32_t tpr = tilesPerRow(bucket, buckets_[bucket].atlasDim);
    uint32_t tileSize = BUCKET_SIZES[bucket];

    for (auto& [ck, te] : cache_) {
        if (ck.bucket != bucket) continue;
        if (te.generation < oldestGen) {
            oldestGen = te.generation;
            oldestKey = ck;
            // Derive slot index from position
            oldestSlot = static_cast<int>((te.loc.y / tileSize) * tpr + (te.loc.x / tileSize));
        }
    }

    // Don't evict current-generation tiles
    if (oldestGen >= currentGen_)
        return -1;

    spdlog::debug("COLR: evicting tile in bucket {} (gen {} vs current {})",
                  bucket, oldestGen, currentGen_);
    cache_.erase(oldestKey);
    return oldestSlot;
}

bool ColrAtlas::growBucket(uint32_t bucket) {
    auto& bs = buckets_[bucket];
    uint32_t newDim = bs.atlasDim * 2;
    if (newDim > MAX_ATLAS_DIM)
        return false;

    uint32_t oldDim = bs.atlasDim;
    bs.atlasDim = newDim;
    updateBucketSlots(bucket);

    spdlog::info("COLR atlas bucket {} ({}px tiles) grew from {}x{} to {}x{} ({} slots)",
                 bucket, BUCKET_SIZES[bucket], oldDim, oldDim, newDim, newDim, bs.maxSlots);
    return true;
}

void ColrAtlas::updateBucketSlots(uint32_t bucket) {
    auto& bs = buckets_[bucket];
    uint32_t tpr = tilesPerRow(bucket, bs.atlasDim);
    bs.maxSlots = tpr * tpr;
}

void ColrAtlas::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    for (auto& bs : buckets_) {
        bs.nextSlot = 0;
        bs.maxSlots = 0;
        bs.atlasDim = INITIAL_ATLAS_DIM;
    }
}

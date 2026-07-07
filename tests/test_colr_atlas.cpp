#include <doctest/doctest.h>

#include "ColrAtlas.h"

#include <set>

// Fill a bucket past its initial capacity so it has to grow, and verify the
// slot allocator never hands out a pixel position already occupied by a live
// tile. Regression test: growBucket doubles atlasDim (and thus tilesPerRow),
// so a linear nextSlot left unchanged across the grow reinterpreted the same
// counter under the new geometry and collided with cached tiles.
TEST_CASE("ColrAtlas: grow does not overlap existing tiles" * doctest::test_suite("[colr]"))
{
    ColrAtlas atlas;
    atlas.advanceGeneration();

    // 512px bucket: 2048/512 = 4 tiles per row => 16 slots before a grow.
    const float fontSize  = 512.0f;
    const uint32_t bucket = ColrAtlas::bucketForSize(fontSize);
    const uint32_t dim0   = atlas.bucketAtlasDim(bucket);
    const uint32_t slots0 = ColrAtlas::tilesPerRow(bucket, dim0) * ColrAtlas::tilesPerRow(bucket, dim0);
    REQUIRE(slots0 == 16);

    // Acquire enough distinct glyphs to fill the bucket and force two grows.
    // All in the same generation, so nothing is evicted — the only way to make
    // room is to grow.
    const int count = static_cast<int>(slots0) * 4 + 3;

    std::set<std::pair<uint32_t, uint32_t>> occupied;
    bool sawGrow = false;
    for (int i = 0; i < count; ++i) {
        ColrAtlas::AcquireResult r = atlas.acquireTile(static_cast<uint64_t>(i) + 1, fontSize);
        REQUIRE(r.tile != nullptr); // distinct keys always allocate
        sawGrow  = sawGrow || r.grew;
        auto pos = std::make_pair(r.tile->x, r.tile->y);
        // Every allocated tile must land on a previously-unoccupied pixel cell.
        CHECK(occupied.find(pos) == occupied.end());
        occupied.insert(pos);
    }
    CHECK(sawGrow);                                       // the bucket actually grew
    CHECK(occupied.size() == static_cast<size_t>(count)); // all positions distinct
}

// After a grow, previously cached glyphs must still resolve to their original
// pixel position (the renderer copies old atlas content preserving pixels).
TEST_CASE("ColrAtlas: cached tiles keep their position across a grow" * doctest::test_suite("[colr]"))
{
    ColrAtlas atlas;
    atlas.advanceGeneration();

    const float fontSize = 512.0f;

    // First tile — record where it lands.
    ColrAtlas::AcquireResult first = atlas.acquireTile(1, fontSize);
    REQUIRE(first.tile != nullptr);
    const uint32_t x0 = first.tile->x, y0 = first.tile->y;

    // Fill and grow.
    for (int i = 2; i < 40; ++i) {
        atlas.acquireTile(static_cast<uint64_t>(i), fontSize);
    }

    ColrAtlas::TileLocation *again = atlas.findTile(1, fontSize);
    REQUIRE(again != nullptr);
    CHECK(again->x == x0);
    CHECK(again->y == y0);
}

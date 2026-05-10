#include "ComputeStatePool.h"
#include "ProceduralGlyphTable.h"
#include <algorithm>
#include <spdlog/spdlog.h>

// SlugVertex is 36 bytes, RectVertex is 32 bytes
static constexpr uint64_t SLUG_VERTEX_SIZE = 36;
static constexpr uint64_t RECT_VERTEX_SIZE = 32;

void ComputeStatePool::init(WGPUDevice device, WGPUBindGroupLayout bindGroupLayout,
                            WGPUBuffer boxDrawingTable, size_t byteLimit)
{
    device_          = device;
    bindGroupLayout_ = bindGroupLayout;
    boxDrawingTable_ = boxDrawingTable;
    byteLimit_       = byteLimit;
}

ComputeState *ComputeStatePool::acquire(uint32_t minCells)
{
    // Best fit: smallest free state with maxCells >= minCells
    ComputeState *best = nullptr;
    size_t bestIdx     = 0;
    for (size_t i = 0; i < free_.size(); ++i) {
        ComputeState *cs = free_[i];
        if (cs->maxCells >= minCells) {
            if (!best || cs->maxCells < best->maxCells) {
                best    = cs;
                bestIdx = i;
            }
        }
    }
    if (best) {
        free_.erase(free_.begin() + static_cast<ptrdiff_t>(bestIdx));
        freeBytes_ -= best->sizeBytes;
        return best;
    }
    return allocate(minCells);
}

void ComputeStatePool::release(ComputeState *state)
{
    if (!state) {
        return;
    }
    free_.push_back(state);
    freeBytes_ += state->sizeBytes;
    evictToLimit();
}

void ComputeStatePool::evictToLimit()
{
    while (freeBytes_ > byteLimit_ && !free_.empty()) {
        ComputeState *victim = free_.front();
        free_.erase(free_.begin());
        freeBytes_ -= victim->sizeBytes;

        spdlog::info("ComputeStatePool: evicting {} cells ({:.1f} KB), free pool {:.1f} KB / limit {:.1f} MB",
                     victim->maxCells,
                     victim->sizeBytes / 1024.0,
                     freeBytes_ / 1024.0,
                     byteLimit_ / (1024.0 * 1024.0));

        all_.erase(std::remove_if(all_.begin(), all_.end(), [victim](const std::unique_ptr<ComputeState> &cs)
                                  {
                                      return cs.get() == victim;
                                  }),
                   all_.end());
    }
}

void ComputeStatePool::clear()
{
    free_.clear();
    freeBytes_ = 0;
    all_.clear();
}

static size_t computeStateBytes(const ComputeState *state)
{
    return static_cast<size_t>(state->maxCells) * sizeof(ResolvedCell) +
        static_cast<size_t>(state->maxGlyphs) * sizeof(GlyphEntry) +
        static_cast<size_t>(state->maxTextVertices) * SLUG_VERTEX_SIZE +
        static_cast<size_t>(state->maxRectVertices) * RECT_VERTEX_SIZE +
        static_cast<size_t>(state->maxInflatedRectVertices) * RECT_VERTEX_SIZE +
        48 + 256;
}

void ComputeStatePool::ensureGlyphCapacity(ComputeState *state, uint32_t glyphCount)
{
    if (!state || !device_) {
        return;
    }

    bool needRebuild = false;
    wgpu::Device device(device_);

    // Grow glyph buffer if needed
    if (glyphCount > state->maxGlyphs) {
        // Add 25% headroom to reduce frequent reallocations
        uint32_t newMax             = glyphCount + glyphCount / 4;
        wgpu::BufferDescriptor desc = {};
        desc.size                   = static_cast<uint64_t>(newMax) * sizeof(GlyphEntry);
        desc.usage                  = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        state->glyphBuffer          = device.CreateBuffer(&desc);
        state->maxGlyphs            = newMax;
        needRebuild                 = true;
    }

    // Grow text vertex buffer if needed: (cells * 6 + glyphs * 6 + 24) vertices
    uint32_t neededTextVerts = state->maxCells * 6 + glyphCount * 6 + 24;
    if (neededTextVerts > state->maxTextVertices) {
        uint32_t newMax              = neededTextVerts + neededTextVerts / 4;
        wgpu::BufferDescriptor desc  = {};
        desc.size                    = static_cast<uint64_t>(newMax) * SLUG_VERTEX_SIZE;
        desc.usage                   = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeTextVertBuffer = device.CreateBuffer(&desc);
        state->maxTextVertices       = newMax;
        needRebuild                  = true;
    }

    if (needRebuild) {
        rebuildBindGroup(state);
        state->sizeBytes = computeStateBytes(state);
    }
}

void ComputeStatePool::ensureRectCapacity(ComputeState *state, uint32_t rectVerts, uint32_t inflatedVerts)
{
    if (!state || !device_) {
        return;
    }

    bool needRebuild = false;
    wgpu::Device device(device_);

    if (rectVerts > state->maxRectVertices) {
        uint32_t newMax              = rectVerts + rectVerts / 4;
        wgpu::BufferDescriptor desc  = {};
        desc.size                    = static_cast<uint64_t>(newMax) * RECT_VERTEX_SIZE;
        desc.usage                   = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeRectVertBuffer = device.CreateBuffer(&desc);
        state->maxRectVertices       = newMax;
        needRebuild                  = true;
    }

    if (inflatedVerts > state->maxInflatedRectVertices) {
        uint32_t newMax                      = inflatedVerts + inflatedVerts / 4;
        wgpu::BufferDescriptor desc          = {};
        desc.size                            = static_cast<uint64_t>(newMax) * RECT_VERTEX_SIZE;
        desc.usage                           = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeInflatedRectVertBuffer = device.CreateBuffer(&desc);
        state->maxInflatedRectVertices       = newMax;
        needRebuild                          = true;
    }

    if (needRebuild) {
        rebuildBindGroup(state);
        state->sizeBytes = computeStateBytes(state);
    }
}

void ComputeStatePool::rebuildBindGroup(ComputeState *state)
{
    wgpu::Device device(device_);

    wgpu::BindGroupEntry bgEntries[8] = {};
    bgEntries[0].binding              = 0;
    bgEntries[0].buffer               = state->computeParamsBuffer;
    bgEntries[0].size                 = (sizeof(TerminalComputeParams) + 255) & ~255u;
    bgEntries[1].binding              = 1;
    bgEntries[1].buffer               = state->resolvedCellBuffer;
    bgEntries[1].size                 = static_cast<uint64_t>(state->maxCells) * sizeof(ResolvedCell);
    bgEntries[2].binding              = 2;
    bgEntries[2].buffer               = state->computeTextVertBuffer;
    bgEntries[2].size                 = static_cast<uint64_t>(state->maxTextVertices) * SLUG_VERTEX_SIZE;
    bgEntries[3].binding              = 3;
    bgEntries[3].buffer               = state->computeRectVertBuffer;
    bgEntries[3].size                 = static_cast<uint64_t>(state->maxRectVertices) * RECT_VERTEX_SIZE;
    bgEntries[4].binding              = 4;
    bgEntries[4].buffer               = state->indirectBuffer;
    bgEntries[4].size                 = 48;
    bgEntries[5].binding              = 5;
    bgEntries[5].buffer               = state->glyphBuffer;
    bgEntries[5].size                 = static_cast<uint64_t>(state->maxGlyphs) * sizeof(GlyphEntry);
    bgEntries[6].binding              = 6;
    bgEntries[6].buffer               = wgpu::Buffer(boxDrawingTable_);
    bgEntries[6].size                 = ProceduralGlyph::kTableSize * sizeof(uint32_t);
    bgEntries[7].binding              = 7;
    bgEntries[7].buffer               = state->computeInflatedRectVertBuffer;
    bgEntries[7].size                 = static_cast<uint64_t>(state->maxInflatedRectVertices) * RECT_VERTEX_SIZE;

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.layout                    = wgpu::BindGroupLayout(bindGroupLayout_);
    bgDesc.entryCount                = 8;
    bgDesc.entries                   = bgEntries;
    state->bindGroup                 = device.CreateBindGroup(&bgDesc);
}

ComputeState *ComputeStatePool::allocate(uint32_t cells)
{
    wgpu::Device device(device_); // temporary owning wrapper (AddRef/Release)

    auto state = std::make_unique<ComputeState>();

    // Resolved cell buffer
    {
        wgpu::BufferDescriptor desc = {};
        desc.size                   = static_cast<uint64_t>(cells) * sizeof(ResolvedCell);
        desc.usage                  = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        state->resolvedCellBuffer   = device.CreateBuffer(&desc);
    }
    // Glyph buffer — start with 1 glyph per cell as initial estimate
    {
        wgpu::BufferDescriptor desc = {};
        desc.size                   = static_cast<uint64_t>(cells) * sizeof(GlyphEntry);
        desc.usage                  = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        state->glyphBuffer          = device.CreateBuffer(&desc);
    }
    // Text vertex buffer: (cells*6 + cells*6 + 24) initially (1 glyph per cell assumption)
    {
        uint32_t initTextVerts       = cells * 6 + cells * 6 + 24;
        wgpu::BufferDescriptor desc  = {};
        desc.size                    = static_cast<uint64_t>(initTextVerts) * SLUG_VERTEX_SIZE;
        desc.usage                   = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeTextVertBuffer = device.CreateBuffer(&desc);
        state->maxTextVertices       = initTextVerts;
    }
    // Rect vertex buffer: initial sizing assumes no procedural glyphs — backgrounds
    // (6/cell) + cursor (24) + selection outline (24). ensureRectCapacity grows
    // this on demand when procedural glyphs / underlines / strikethrough actually
    // appear, with the shader bounds-checking every write.
    uint32_t initialRectVerts = cells * 6 + 48;
    {
        wgpu::BufferDescriptor desc  = {};
        desc.size                    = static_cast<uint64_t>(initialRectVerts) * RECT_VERTEX_SIZE;
        desc.usage                   = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeRectVertBuffer = device.CreateBuffer(&desc);
        state->maxRectVertices       = initialRectVerts;
    }
    // Inflated-bg rect vertex buffer — start at one cell's worth (6 verts) and
    // grow on demand. Most frames have zero inflated-bg cells.
    uint32_t initialInflatedVerts = 6;
    {
        wgpu::BufferDescriptor desc          = {};
        desc.size                            = static_cast<uint64_t>(initialInflatedVerts) * RECT_VERTEX_SIZE;
        desc.usage                           = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeInflatedRectVertBuffer = device.CreateBuffer(&desc);
        state->maxInflatedRectVertices       = initialInflatedVerts;
    }
    // Indirect buffer — 12 u32s now: text args [0..3], rect args [4..7],
    // inflated-rect args [8..11].
    {
        wgpu::BufferDescriptor desc = {};
        desc.size                   = 48;
        desc.usage                  = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
        state->indirectBuffer       = device.CreateBuffer(&desc);
    }
    // Compute params uniform buffer
    {
        wgpu::BufferDescriptor desc = {};
        desc.size                   = (sizeof(TerminalComputeParams) + 255) & ~255u;
        desc.usage                  = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        state->computeParamsBuffer  = device.CreateBuffer(&desc);
    }

    state->maxCells  = cells;
    state->maxGlyphs = cells; // initial 1:1 estimate
    state->sizeBytes = computeStateBytes(state.get());

    rebuildBindGroup(state.get());

    spdlog::debug("ComputeStatePool: allocated {} cells ({:.1f} KB)", cells, state->sizeBytes / 1024.0);

    ComputeState *ptr = state.get();
    all_.push_back(std::move(state));
    return ptr;
}

ComputeStatePool::Stats ComputeStatePool::stats() const
{
    size_t totalBytes = 0;
    for (const auto &cs : all_) {
        totalBytes += cs->sizeBytes;
    }
    return { all_.size(), all_.size() - free_.size(), free_.size(), totalBytes, freeBytes_, byteLimit_ };
}

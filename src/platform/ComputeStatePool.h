#pragma once

#include <dawn/webgpu_cpp.h>
#include "ComputeTypes.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Pool of ComputeState objects with byte-budget LRU eviction.
// Each ComputeState holds the compute buffers + bind group for one render call.
// Acquire returns the smallest free state >= minCells, or allocates a new one.
// Release returns a state to the free list and evicts LRU entries if the free
// list exceeds byteLimit bytes. Eviction is logged at info level.
class ComputeStatePool {
public:
    // device and bindGroupLayout are non-owning (borrowed from Renderer/PlatformDawn).
    // byteLimit: max bytes held in the free list before LRU eviction (default 32 MB).
    void init(WGPUDevice device, WGPUBindGroupLayout bindGroupLayout,
              WGPUBuffer boxDrawingTable = nullptr,
              size_t byteLimit = 32 * 1024 * 1024);

    ComputeState* acquire(uint32_t minCells);
    void release(ComputeState* state);

    // Ensure the glyph buffer and vertex buffers are large enough.
    // Grows buffers and rebuilds bind group if needed. Buffers only grow, never shrink.
    void ensureGlyphCapacity(ComputeState* state, uint32_t glyphCount);

    // Destroy all states. Call before the WebGPU device is released.
    void clear();

    struct Stats {
        size_t total;
        size_t inUse;
        size_t free;
        size_t totalBytes;
        size_t freeBytes;
        size_t limitBytes;
    };
    Stats stats() const;

private:
    void evictToLimit();
    ComputeState* allocate(uint32_t cells);
    void rebuildBindGroup(ComputeState* state);

    WGPUDevice           device_          = nullptr;
    WGPUBindGroupLayout  bindGroupLayout_ = nullptr;
    WGPUBuffer           boxDrawingTable_ = nullptr;
    size_t               byteLimit_       = 32 * 1024 * 1024;
    size_t               freeBytes_       = 0;

    std::vector<std::unique_ptr<ComputeState>> all_;
    std::vector<ComputeState*> free_;  // front = oldest released (LRU)
};

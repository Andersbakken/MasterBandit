#include "ComputeStatePool.h"
#include <spdlog/spdlog.h>
#include <algorithm>

void ComputeStatePool::init(WGPUDevice device, WGPUBindGroupLayout bindGroupLayout, size_t byteLimit)
{
    device_          = device;
    bindGroupLayout_ = bindGroupLayout;
    byteLimit_       = byteLimit;
}

ComputeState* ComputeStatePool::acquire(uint32_t minCells)
{
    // Best fit: smallest free state with maxCells >= minCells
    ComputeState* best = nullptr;
    size_t bestIdx = 0;
    for (size_t i = 0; i < free_.size(); ++i) {
        ComputeState* cs = free_[i];
        if (cs->maxCells >= minCells) {
            if (!best || cs->maxCells < best->maxCells) {
                best = cs;
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

void ComputeStatePool::release(ComputeState* state)
{
    if (!state) return;
    free_.push_back(state);
    freeBytes_ += state->sizeBytes;
    evictToLimit();
}

void ComputeStatePool::evictToLimit()
{
    while (freeBytes_ > byteLimit_ && !free_.empty()) {
        ComputeState* victim = free_.front();
        free_.erase(free_.begin());
        freeBytes_ -= victim->sizeBytes;

        spdlog::info("ComputeStatePool: evicting {} cells ({:.1f} KB), free pool {:.1f} KB / limit {:.1f} MB",
                     victim->maxCells, victim->sizeBytes / 1024.0,
                     freeBytes_ / 1024.0,
                     byteLimit_ / (1024.0 * 1024.0));

        all_.erase(std::remove_if(all_.begin(), all_.end(),
            [victim](const std::unique_ptr<ComputeState>& cs) { return cs.get() == victim; }),
            all_.end());
    }
}

void ComputeStatePool::clear()
{
    free_.clear();
    freeBytes_ = 0;
    all_.clear();
}

ComputeState* ComputeStatePool::allocate(uint32_t cells)
{
    wgpu::Device device(device_);  // temporary owning wrapper (AddRef/Release)

    auto state = std::make_unique<ComputeState>();

    {
        wgpu::BufferDescriptor desc = {};
        desc.size  = static_cast<uint64_t>(cells) * sizeof(ResolvedCell);
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        state->resolvedCellBuffer = device.CreateBuffer(&desc);
    }
    {
        wgpu::BufferDescriptor desc = {};
        desc.size  = static_cast<uint64_t>(cells) * 6 * 36;
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeTextVertBuffer = device.CreateBuffer(&desc);
    }
    {
        // Extra 24 verts to accommodate a hollow cursor (4 thin rects = 24 verts)
        wgpu::BufferDescriptor desc = {};
        desc.size  = (static_cast<uint64_t>(cells) * 6 + 24) * 24;
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Vertex;
        state->computeRectVertBuffer = device.CreateBuffer(&desc);
    }
    {
        wgpu::BufferDescriptor desc = {};
        desc.size  = 32;
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Indirect | wgpu::BufferUsage::CopyDst;
        state->indirectBuffer = device.CreateBuffer(&desc);
    }
    {
        wgpu::BufferDescriptor desc = {};
        desc.size  = (sizeof(TerminalComputeParams) + 255) & ~255u;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        state->computeParamsBuffer = device.CreateBuffer(&desc);
    }

    wgpu::BindGroupEntry bgEntries[5] = {};
    bgEntries[0].binding = 0;
    bgEntries[0].buffer  = state->computeParamsBuffer;
    bgEntries[0].size    = (sizeof(TerminalComputeParams) + 255) & ~255u;
    bgEntries[1].binding = 1;
    bgEntries[1].buffer  = state->resolvedCellBuffer;
    bgEntries[1].size    = static_cast<uint64_t>(cells) * sizeof(ResolvedCell);
    bgEntries[2].binding = 2;
    bgEntries[2].buffer  = state->computeTextVertBuffer;
    bgEntries[2].size    = static_cast<uint64_t>(cells) * 6 * 36;
    bgEntries[3].binding = 3;
    bgEntries[3].buffer  = state->computeRectVertBuffer;
    bgEntries[3].size    = (static_cast<uint64_t>(cells) * 6 + 24) * 24;
    bgEntries[4].binding = 4;
    bgEntries[4].buffer  = state->indirectBuffer;
    bgEntries[4].size    = 32;

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.layout     = wgpu::BindGroupLayout(bindGroupLayout_);
    bgDesc.entryCount = 5;
    bgDesc.entries    = bgEntries;
    state->bindGroup  = device.CreateBindGroup(&bgDesc);

    state->maxCells  = cells;
    state->sizeBytes = static_cast<size_t>(cells) * (32 + 6*36 + 6*24) + 24*24 + 32 + 256;

    spdlog::debug("ComputeStatePool: allocated {} cells ({:.1f} KB)", cells, state->sizeBytes / 1024.0);

    ComputeState* ptr = state.get();
    all_.push_back(std::move(state));
    return ptr;
}

ComputeStatePool::Stats ComputeStatePool::stats() const
{
    size_t totalBytes = 0;
    for (const auto& cs : all_) totalBytes += cs->sizeBytes;
    return { all_.size(), all_.size() - free_.size(), free_.size(),
             totalBytes, freeBytes_, byteLimit_ };
}

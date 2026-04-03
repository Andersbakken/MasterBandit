#pragma once

#include <dawn/webgpu_cpp.h>
#include <cstdint>
#include <memory>
#include <vector>

struct PooledTexture {
    wgpu::Texture     texture;
    wgpu::TextureView view;
    uint32_t          width  = 0;
    uint32_t          height = 0;
    uint64_t          lastUsedFrame = 0;
};

// A simple texture pool with LRU eviction.
// Textures are created with RenderAttachment | CopySrc usage.
// Acquire returns the smallest free texture >= (w, h).
// Release returns a texture to the free list immediately (WebGPU handles GPU lifetime).
// Tick increments the frame counter and evicts stale free entries.
class TexturePool {
public:
    void init(wgpu::Device device, wgpu::TextureFormat format);

    // Returns the smallest free texture that fits (w, h), or allocates a new one.
    // The returned pointer is owned by the pool — caller must release it when done.
    PooledTexture* acquire(uint32_t w, uint32_t h);

    // Return texture to the free list.
    void release(PooledTexture* tex);

    // Advance the frame counter and evict free entries unused for EVICT_FRAMES frames.
    void tick();

    uint64_t currentFrame() const { return frame_; }

private:
    PooledTexture* allocate(uint32_t w, uint32_t h);

    wgpu::Device        device_;
    wgpu::TextureFormat format_ = wgpu::TextureFormat::BGRA8Unorm;
    uint64_t            frame_  = 0;

    // All entries the pool owns. Entries in the free list have no external reference.
    std::vector<std::unique_ptr<PooledTexture>> all_;
    // Subset of all_ that is currently free (not held by anyone).
    std::vector<PooledTexture*> free_;

    static constexpr uint64_t EVICT_FRAMES = 120; // ~2 s at 60 fps
};

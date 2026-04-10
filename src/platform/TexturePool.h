#pragma once

#include <dawn/webgpu_cpp.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct PooledTexture {
    wgpu::Texture     texture;
    wgpu::TextureView view;
    uint32_t          width    = 0;
    uint32_t          height   = 0;
    size_t            sizeBytes = 0;  // width * height * bytesPerPixel
};

// Texture pool with byte-budget eviction.
// Textures are created with RenderAttachment | CopySrc usage.
// Acquire returns the smallest free texture >= (w, h), or allocates a new one.
// Release returns a texture to the free list and evicts LRU entries if the free
// list exceeds byteLimit bytes. Eviction is logged at info level.
class TexturePool {
public:
    // device is non-owning (borrowed from PlatformDawn).
    // byteLimit: max bytes held in the free list before LRU eviction (default 128 MB).
    void init(wgpu::Device& device, wgpu::TextureFormat format,
              size_t byteLimit = 128 * 1024 * 1024);

    PooledTexture* acquire(uint32_t w, uint32_t h);
    void release(PooledTexture* tex);
    void setByteLimit(size_t limit);

    // Destroy all textures immediately. Call before the WebGPU device is released.
    void clear();

    struct Stats {
        size_t total;      // total textures allocated
        size_t inUse;      // currently held (total - free)
        size_t free;       // in free list
        size_t totalBytes; // bytes across all allocated textures
        size_t freeBytes;  // bytes in free list
        size_t limitBytes; // eviction budget
    };
    Stats stats() const;

private:
    void evictToLimit();
    PooledTexture* allocate(uint32_t w, uint32_t h);

    WGPUDevice          device_    = nullptr;
    wgpu::TextureFormat format_    = wgpu::TextureFormat::BGRA8Unorm;
    size_t              byteLimit_ = 128 * 1024 * 1024;
    size_t              freeBytes_ = 0;
    uint32_t            bytesPerPixel_ = 4;

    std::vector<std::unique_ptr<PooledTexture>> all_;
    std::vector<PooledTexture*> free_; // front = oldest released
};

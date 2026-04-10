#include "TexturePool.h"
#include <spdlog/spdlog.h>
#include <algorithm>

void TexturePool::init(wgpu::Device& device, wgpu::TextureFormat format, size_t byteLimit)
{
    device_    = device.Get();
    format_    = format;
    byteLimit_ = byteLimit;

    // Bytes per pixel for the supported formats
    switch (format) {
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::BGRA8Unorm:
        bytesPerPixel_ = 4; break;
    default:
        bytesPerPixel_ = 4; break;
    }
}

PooledTexture* TexturePool::acquire(uint32_t w, uint32_t h)
{
    // Best fit: smallest free texture that fits
    PooledTexture* best = nullptr;
    for (PooledTexture* t : free_) {
        if (t->width >= w && t->height >= h) {
            if (!best || t->sizeBytes < best->sizeBytes)
                best = t;
        }
    }

    if (best) {
        free_.erase(std::find(free_.begin(), free_.end(), best));
        freeBytes_ -= best->sizeBytes;
        return best;
    }

    return allocate(w, h);
}

void TexturePool::release(PooledTexture* tex)
{
    if (!tex) return;
    free_.push_back(tex);
    freeBytes_ += tex->sizeBytes;
    evictToLimit();
}

void TexturePool::setByteLimit(size_t limit)
{
    byteLimit_ = limit;
    evictToLimit();
}

void TexturePool::evictToLimit()
{
    while (freeBytes_ > byteLimit_ && !free_.empty()) {
        // Evict the oldest entry (front = LRU)
        PooledTexture* victim = free_.front();
        free_.erase(free_.begin());
        freeBytes_ -= victim->sizeBytes;

        spdlog::info("TexturePool: evicting {}x{} ({:.1f} KB), free pool {:.1f} KB / limit {:.1f} MB",
                     victim->width, victim->height,
                     victim->sizeBytes / 1024.0,
                     freeBytes_ / 1024.0,
                     byteLimit_ / (1024.0 * 1024.0));

        all_.erase(std::remove_if(all_.begin(), all_.end(),
            [victim](const std::unique_ptr<PooledTexture>& p) { return p.get() == victim; }),
            all_.end());
    }
}

void TexturePool::clear()
{
    free_.clear();
    freeBytes_ = 0;
    all_.clear();
}

PooledTexture* TexturePool::allocate(uint32_t w, uint32_t h)
{
    WGPUTextureDescriptor desc = {};
    WGPUExtent3D size          = { w, h, 1 };
    desc.size          = size;
    desc.format        = static_cast<WGPUTextureFormat>(format_);
    desc.usage         = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    desc.dimension     = WGPUTextureDimension_2D;
    desc.mipLevelCount = 1;
    desc.sampleCount   = 1;

    auto entry         = std::make_unique<PooledTexture>();
    entry->texture     = wgpu::Texture::Acquire(wgpuDeviceCreateTexture(device_, &desc));
    entry->view        = entry->texture.CreateView();
    entry->width       = w;
    entry->height      = h;
    entry->sizeBytes   = static_cast<size_t>(w) * h * bytesPerPixel_;

    spdlog::debug("TexturePool: allocated {}x{} ({:.1f} KB)", w, h, entry->sizeBytes / 1024.0);

    PooledTexture* ptr = entry.get();
    all_.push_back(std::move(entry));
    return ptr;
}

TexturePool::Stats TexturePool::stats() const
{
    size_t totalBytes = 0;
    for (const auto& t : all_) totalBytes += t->sizeBytes;
    return { all_.size(), all_.size() - free_.size(), free_.size(),
             totalBytes, freeBytes_, byteLimit_ };
}

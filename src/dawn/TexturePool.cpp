#include "TexturePool.h"
#include <spdlog/spdlog.h>
#include <algorithm>

void TexturePool::init(wgpu::Device device, wgpu::TextureFormat format)
{
    device_ = device;
    format_ = format;
}

PooledTexture* TexturePool::acquire(uint32_t w, uint32_t h)
{
    // Find the smallest free texture that fits
    PooledTexture* best = nullptr;
    for (PooledTexture* t : free_) {
        if (t->width >= w && t->height >= h) {
            if (!best || (t->width * t->height < best->width * best->height))
                best = t;
        }
    }

    if (best) {
        free_.erase(std::find(free_.begin(), free_.end(), best));
        best->lastUsedFrame = frame_;
        return best;
    }

    return allocate(w, h);
}

void TexturePool::release(PooledTexture* tex)
{
    if (!tex) return;
    tex->lastUsedFrame = frame_;
    free_.push_back(tex);
}

void TexturePool::tick()
{
    ++frame_;

    // Evict free entries that haven't been used recently
    free_.erase(std::remove_if(free_.begin(), free_.end(),
        [this](PooledTexture* t) {
            if (frame_ - t->lastUsedFrame < EVICT_FRAMES) return false;
            spdlog::debug("TexturePool: evicting {}x{} (last used frame {})",
                          t->width, t->height, t->lastUsedFrame);
            // Remove from all_ too
            all_.erase(std::remove_if(all_.begin(), all_.end(),
                [t](const std::unique_ptr<PooledTexture>& p) { return p.get() == t; }),
                all_.end());
            return true; // already erased from all_, pointer is dangling — that's ok
                         // because we're removing it from free_ too
        }), free_.end());
}

PooledTexture* TexturePool::allocate(uint32_t w, uint32_t h)
{
    wgpu::TextureDescriptor desc = {};
    desc.size            = { w, h, 1 };
    desc.format          = format_;
    desc.usage           = wgpu::TextureUsage::RenderAttachment |
                           wgpu::TextureUsage::CopySrc;
    desc.dimension       = wgpu::TextureDimension::e2D;
    desc.mipLevelCount   = 1;
    desc.sampleCount     = 1;

    auto entry        = std::make_unique<PooledTexture>();
    entry->texture    = device_.CreateTexture(&desc);
    entry->view       = entry->texture.CreateView();
    entry->width      = w;
    entry->height     = h;
    entry->lastUsedFrame = frame_;

    spdlog::debug("TexturePool: allocated {}x{}", w, h);

    PooledTexture* ptr = entry.get();
    all_.push_back(std::move(entry));
    return ptr;
}

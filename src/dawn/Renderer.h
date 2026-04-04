#pragma once

#include <dawn/webgpu_cpp.h>
#include "text.h"
#include "ComputeTypes.h"
#include "ComputeStatePool.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Per-vertex data for Slug text rendering (36 bytes)
struct SlugVertex {
    float pos[2];
    float texcoord[2];
    float normal[2];
    float emPerPos;
    uint32_t atlas_offset;
    uint32_t tint;  // packed RGBA8
};

// Per-vertex data for rect rendering (24 bytes)
struct RectVertex {
    float pos[2];
    float color[4];
};

class Renderer {
public:
    struct ImageDrawCmd {
        uint32_t imageId;
        float x, y, w, h;       // screen pixel rect (clipped to viewport)
        float u0, v0, u1, v1;   // UV coords into the image texture
    };

    void init(wgpu::Device& device, wgpu::Queue& queue,
              const std::string& shaderDir,
              uint32_t width, uint32_t height);

    void uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void updateFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void setViewportSize(uint32_t width, uint32_t height);

    // Release all WebGPU resources. Call before the device is destroyed.
    void destroy();

    // Compute pipeline
    void initComputePipeline(wgpu::Device& device, const std::string& shaderDir);

    // Compute state pool (byte-budget eviction, logged at info level)
    ComputeStatePool& computePool() { return computePool_; }

    void uploadResolvedCells(wgpu::Queue& queue, ComputeState* state,
                             const ResolvedCell* cells, uint32_t count);

    // Render terminal content to an externally-provided texture from the TexturePool.
    void renderToPane(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                      const std::string& fontName,
                      const TerminalComputeParams& params,
                      ComputeState* computeState,
                      wgpu::TextureView target,
                      const std::vector<ImageDrawCmd>& imageCmds = {});

    // Composite entry: a rendered pane texture and where to place it on the swapchain.
    struct CompositeEntry {
        wgpu::Texture texture;
        uint32_t srcW, srcH;         // used region of the texture (== pane pixel size)
        uint32_t dstX, dstY;         // destination pixel offset on swapchain
    };

    // Composite N pane textures onto the swapchain.
    // Clears swapchain to black first, then copies each entry to its destination rect.
    void composite(wgpu::CommandEncoder& encoder,
                   wgpu::Texture swapchainTexture,
                   const std::vector<CompositeEntry>& entries);

    // Persistent divider support.
    // Call updateDividerViewport whenever the swapchain dimensions change.
    void updateDividerViewport(wgpu::Queue& queue, uint32_t fbWidth, uint32_t fbHeight);

    // Build (or update) a 6-vertex buffer for a single divider rect.
    // Pass a null Buffer to create a new one; pass an existing Buffer to overwrite in place.
    void updateDividerBuffer(wgpu::Queue& queue,
                             wgpu::Buffer& vb,
                             float x, float y, float w, float h,
                             float r, float g, float b, float a);

    // Draw one divider from its pre-built vertex buffer.
    void drawDivider(wgpu::CommandEncoder& encoder,
                     wgpu::Texture swapchainTexture,
                     uint32_t fbWidth, uint32_t fbHeight,
                     const wgpu::Buffer& vb);

    // Image rendering
    struct ImageGPU {
        wgpu::Texture texture;
        wgpu::TextureView view;
        wgpu::BindGroup bindGroup;
        uint32_t width, height;
    };

    void initImagePipeline(wgpu::Device& device, const std::string& shaderDir);
    void ensureImageGPU(wgpu::Queue& queue, uint32_t imageId,
                        const uint8_t* rgba, uint32_t width, uint32_t height);
    void renderImages(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                      wgpu::TextureView target,
                      const std::vector<ImageDrawCmd>& cmds);

private:
    wgpu::Device device_;
    uint32_t viewportW_ = 0, viewportH_ = 0;

    // Text pipeline (Slug)
    wgpu::ShaderModule textShader_;
    wgpu::RenderPipeline textPipeline_;
    wgpu::BindGroupLayout textBindGroupLayout_;

    struct FontGPU {
        wgpu::Buffer storageBuffer;
        wgpu::Buffer uniformBuffer;
        wgpu::BindGroup bindGroup;
        uint32_t uploadedSize = 0;
        uint32_t storageCapacity = 0;
    };
    std::unordered_map<std::string, FontGPU> fontGPU_;

    // Rect pipeline
    wgpu::ShaderModule rectShader_;
    wgpu::RenderPipeline rectPipeline_;
    wgpu::BindGroupLayout rectBindGroupLayout_;
    wgpu::Buffer rectUniformBuffer_;
    wgpu::BindGroup rectBindGroup_;

    // Persistent divider bind group — uses swapchain viewport, updated on resize
    wgpu::Buffer dividerUniformBuffer_;
    wgpu::BindGroup dividerBindGroup_;

    // Compute pipeline
    wgpu::ComputePipeline computePipeline_;
    wgpu::BindGroupLayout computeBindGroupLayout_;
    bool computeInitialized_ = false;

    ComputeStatePool computePool_;

    // Image pipeline
    wgpu::ShaderModule imageShader_;
    wgpu::RenderPipeline imagePipeline_;
    wgpu::BindGroupLayout imageBindGroupLayout_;
    wgpu::Buffer imageUniformBuffer_;
    wgpu::Buffer imageVertexBuffer_;
    wgpu::Sampler imageSampler_;
    std::unordered_map<uint32_t, ImageGPU> imageGPU_;
    bool imagePipelineReady_ = false;
};

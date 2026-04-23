#pragma once

#include <dawn/webgpu_cpp.h>
#include "text.h"
#include "ColrAtlas.h"
#include "ComputeTypes.h"
#include "ComputeStatePool.h"
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Per-vertex data for rect rendering (32 bytes)
struct RectVertex {
    float pos[2];
    float color[4];
    float edge_dist[2];
};

class Renderer {
public:
    struct ImageDrawCmd {
        uint32_t imageId;
        float x, y, w, h;       // screen pixel rect (clipped to viewport)
        float u0, v0, u1, v1;   // UV coords into the image texture
        int32_t zIndex = 0;     // z-layering: <0 = below text, >=0 = above text
    };

    void init(wgpu::Device& device, wgpu::Queue& queue,
              const std::string& shaderDir,
              uint32_t width, uint32_t height);

    void uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void updateFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void removeFontAtlas(const std::string& fontName);

    void setViewportSize(uint32_t width, uint32_t height);

    // Release all WebGPU resources. Call before the device is destroyed.
    void destroy();

    // Compute pipeline
    void initComputePipeline(wgpu::Device& device, wgpu::Queue& queue, const std::string& shaderDir);

    // Compute state pool (byte-budget eviction, logged at info level)
    ComputeStatePool& computePool() { return computePool_; }

    void uploadResolvedCells(wgpu::Queue& queue, ComputeState* state,
                             const ResolvedCell* cells, uint32_t count);
    void uploadGlyphs(wgpu::Queue& queue, ComputeState* state,
                      const GlyphEntry* glyphs, uint32_t count);

    // OSC 133 dim: non-selected rows are multiplied by `factor` in the fragment
    // shader (branchless — always multiplies). `factor == 1.0` is the
    // multiplicative identity (no dim). `yMin`/`yMax` are fragment-pixel
    // boundaries: fragments with position.y in [yMin, yMax) are left untouched
    // (multiplied by 1.0 instead of factor).
    struct DimParams {
        float factor = 1.0f;
        float yMin = 0.0f;
        float yMax = 0.0f;
    };

    // Render terminal content to an externally-provided texture from the TexturePool.
    // pane_tint: RGBA multiplier applied to all rendered content (1,1,1,1 = no tint).
    // imageCmds must be sorted by zIndex. splitBelowText is the index where
    // z >= 0 starts (i.e., [0, splitBelowText) renders below text, [splitBelowText, size) above).
    void renderToPane(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                      const std::string& fontName,
                      const TerminalComputeParams& params,
                      ComputeState* computeState,
                      wgpu::TextureView target,
                      const float pane_tint[4],
                      const DimParams& dim,
                      const std::vector<ImageDrawCmd>& imageCmds = {},
                      size_t imgSplitText = 0);

    // Composite entry: a rendered pane texture and where to place it on the swapchain.
    // srcX/srcY let a single texture be composited as multiple vertical strips —
    // used when an embedded terminal slices the parent pane around its anchor row.
    struct CompositeEntry {
        wgpu::Texture texture;
        uint32_t srcX = 0, srcY = 0; // offset into the source texture
        uint32_t srcW, srcH;         // extent copied from the texture
        uint32_t dstX, dstY;         // destination pixel offset on swapchain
    };

    // Composite N pane textures onto the swapchain.
    // Clears swapchain to black first, then copies each entry to its destination rect.
    void composite(wgpu::CommandEncoder& encoder,
                   wgpu::Texture swapchainTexture,
                   const std::vector<CompositeEntry>& entries);

    // Persistent divider support.
    // Call updateDividerViewport whenever the swapchain dimensions change.
    void updateDividerViewport(wgpu::Queue& queue, uint32_t fbWidth, uint32_t fbHeight,
                               const float* tint = nullptr);

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

    // Progress bar rendering
    struct ProgressBarParams {
        float x, y, w, h;     // bar rect in pixels
        float fillFrac;        // 0-1 fill fraction
        float edgeSoftness;    // gradient edge width in pixels
        float r, g, b, a;     // color
        float softLeft;        // 1.0 = gradient, 0.0 = sharp edge
        float softRight;       // 1.0 = gradient, 0.0 = sharp edge
    };
    void initProgressPipeline(wgpu::Device& device, const std::string& shaderDir);
    void drawProgressBar(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                         wgpu::Texture target, uint32_t fbW, uint32_t fbH,
                         const ProgressBarParams& params);

    // Image rendering. Animated kitty images keep one texture per frame index
    // so playback is just a sampler swap — no re-upload on every cycle.
    struct ImageFrame {
        wgpu::Texture texture;
        wgpu::TextureView view;
        wgpu::BindGroup bindGroup;
        uint32_t uploadedVersion { 0 };  // 0 = never uploaded
    };
    struct ImageGPU {
        uint32_t width = 0, height = 0;       // logical image size
        uint32_t texWidth = 0, texHeight = 0; // GPU texture size (image + 2px border)
        std::vector<ImageFrame> frames;       // indexed by frame index
        uint32_t currentFrameIndex = 0;
    };

    void initImagePipeline(wgpu::Device& device, const std::string& shaderDir);
    // Ensures the slot for `frameIndex` exists and contains pixel data matching
    // `contentVersion`, then selects it as the active frame for renderImages.
    // If the slot already matches `contentVersion`, no upload happens.
    void useImageFrame(wgpu::Queue& queue, uint32_t imageId,
                       uint32_t frameIndex, uint32_t totalFrames,
                       uint32_t contentVersion,
                       const uint8_t* rgba, uint32_t width, uint32_t height);

    // Evict GPU textures for any image not in `keep`. Used after each frame
    // to release images that scrolled out of every pane's viewport — they'll
    // be re-uploaded lazily by useImageFrame when/if they scroll back in.
    void retainImagesOnly(const std::unordered_set<uint32_t>& keep);
    void renderImages(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                      wgpu::TextureView target,
                      float paneWidth, float paneHeight,
                      const float* tint,
                      const DimParams& dim,
                      const std::vector<ImageDrawCmd>& cmds,
                      size_t start = 0, size_t count = std::numeric_limits<size_t>::max());

    // COLRv1 emoji rasterization
    void initColrPipeline(wgpu::Device& device, const std::string& shaderDir);

    // Rasterize a batch of COLRv1 glyphs into the atlas.
    // Each entry: (glyph paint data, tile location, em-space extents).
    struct ColrRasterCmd {
        const ColrGlyphData* data;
        ColrAtlas::TileLocation tile;
        float em_origin_x, em_origin_y;
        float em_width, em_height;
    };
    void rasterizeColrGlyphs(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                             const std::string& fontName,
                             const std::vector<ColrRasterCmd>& cmds);

    // Render COLR emoji as textured quads.
    struct ColrDrawCmd {
        float x, y, w, h;           // screen pixel rect
        ColrAtlas::TileLocation tile; // atlas location
    };
    void renderColrQuads(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                         wgpu::TextureView target,
                         float viewport_w, float viewport_h,
                         const float* tint,
                         const DimParams& dim,
                         const std::vector<ColrDrawCmd>& cmds);

    ColrAtlas& colrAtlas() { return colrAtlas_; }

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
        // Last observed FontData::atlasVersion. Render thread compares vs.
        // current version to skip upload when atlas hasn't changed, avoiding
        // the shared_mutex shared_lock on the hot path.
        uint64_t uploadedVersion = 0;
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

    // Progress bar pipeline
    wgpu::ShaderModule progressShader_;
    wgpu::RenderPipeline progressPipeline_;
    wgpu::BindGroupLayout progressBindGroupLayout_;
    wgpu::Buffer progressUniformBuffer_;
    wgpu::BindGroup progressBindGroup_;
    bool progressPipelineReady_ = false;

    // Compute pipeline
    wgpu::ComputePipeline computePipeline_;
    wgpu::BindGroupLayout computeBindGroupLayout_;
    wgpu::Buffer boxDrawingTableBuffer_;
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
    static constexpr size_t MaxImageSlots = 256;

    // COLRv1 rasterizer
    wgpu::ComputePipeline colrComputePipeline_;
    wgpu::BindGroupLayout colrComputeBGL_;
    bool colrPipelineReady_ = false;

    ColrAtlas colrAtlas_;

    // Per-bucket atlas textures (created on demand)
    struct ColrBucketGPU {
        wgpu::Texture texture;
        wgpu::TextureView view;
        wgpu::BindGroup renderBindGroup;  // for the quad render pass
        uint32_t atlasDim = 0;  // dimension at creation time (0 = not created)
    };
    ColrBucketGPU colrBuckets_[ColrAtlas::NUM_BUCKETS];

    // Reusable GPU buffers for rasterization dispatch
    wgpu::Buffer colrGlyphInfoBuffer_;
    wgpu::Buffer colrInstructionBuffer_;
    wgpu::Buffer colrColorStopBuffer_;
    uint32_t colrInstructionCapacity_ = 0;
    uint32_t colrColorStopCapacity_ = 0;
    uint32_t colrGlyphInfoCapacity_ = 0;

    // Quad rendering (reuses image pipeline shader + sampler)
    wgpu::Buffer colrQuadVertexBuffer_;
    uint32_t colrQuadVertexCapacity_ = 0;

    void ensureColrBucket(wgpu::CommandEncoder& encoder, wgpu::Queue& queue, uint32_t bucket);
};

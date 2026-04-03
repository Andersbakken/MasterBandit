#pragma once

#include <dawn/webgpu_cpp.h>
#include "text.h"
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

// CPU-resolved cell data for GPU upload (32 bytes)
struct ResolvedCell {
    uint32_t atlas_offset;   // 0 = empty/spacer
    float ext_min_x, ext_min_y, ext_max_x, ext_max_y;
    uint32_t upem;
    uint32_t fg_color;       // packed RGBA8
    uint32_t bg_color;       // packed RGBA8 (0 = default/transparent)
};
static_assert(sizeof(ResolvedCell) == 32);

// Compute shader uniform params (32 bytes)
struct TerminalComputeParams {
    uint32_t cols;
    uint32_t rows;
    float cell_width;
    float cell_height;
    float viewport_w;
    float viewport_h;
    float font_ascender;
    float _pad;
};
static_assert(sizeof(TerminalComputeParams) == 32);

class Renderer {
public:
    void init(wgpu::Device& device, wgpu::Queue& queue,
              const std::string& shaderDir,
              uint32_t width, uint32_t height);

    void uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void updateFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void setViewportSize(uint32_t width, uint32_t height);

    // Compute pipeline
    void initComputePipeline(wgpu::Device& device, const std::string& shaderDir);
    void resizeComputeBuffers(wgpu::Device& device, uint32_t cols, uint32_t rows);
    void uploadResolvedCells(wgpu::Queue& queue, const ResolvedCell* cells, uint32_t count);

    // Compute-based render: compute dispatch + indirect draw
    void renderFrameCompute(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                            wgpu::TextureView swapchainView,
                            const std::string& fontName,
                            const TerminalComputeParams& params);

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

    // Compute pipeline
    wgpu::ComputePipeline computePipeline_;
    wgpu::BindGroupLayout computeBindGroupLayout_;
    wgpu::Buffer resolvedCellBuffer_;      // Storage
    wgpu::Buffer computeTextVertBuffer_;   // Storage | Vertex
    wgpu::Buffer computeRectVertBuffer_;   // Storage | Vertex
    wgpu::Buffer indirectBuffer_;          // Storage | Indirect | CopyDst
    wgpu::Buffer computeParamsBuffer_;     // Uniform | CopyDst
    wgpu::BindGroup computeBindGroup_;
    uint32_t gridCols_ = 0, gridRows_ = 0;
    bool computeReady_ = false;
};

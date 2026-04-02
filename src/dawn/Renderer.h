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

class Renderer {
public:
    void init(wgpu::Device& device, wgpu::Queue& queue,
              const std::string& shaderDir,
              uint32_t width, uint32_t height);

    void uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    // Incremental atlas update (upload only new data since last upload)
    void updateFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font);

    void setViewportSize(uint32_t width, uint32_t height);

    void queueGlyphVertices(const std::string& fontName,
                            const SlugVertex* verts, uint32_t count);
    void queueRect(float x, float y, float w, float h,
                   float r, float g, float b, float a);
    void clearQueues();

    void renderFrame(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                     wgpu::TextureView swapchainView);

private:
    wgpu::Device device_;
    uint32_t viewportW_ = 0, viewportH_ = 0;

    // Text pipeline (Slug)
    wgpu::ShaderModule textShader_;
    wgpu::RenderPipeline textPipeline_;
    wgpu::BindGroupLayout textBindGroupLayout_;
    wgpu::Buffer textVertexBuffer_;

    static constexpr uint32_t MAX_TEXT_VERTS = 16384 * 6;

    struct FontGPU {
        wgpu::Buffer storageBuffer;      // glyph atlas storage<read>
        wgpu::Buffer uniformBuffer;      // TextUniforms
        wgpu::BindGroup bindGroup;
        uint32_t uploadedSize = 0;       // texels uploaded so far
        uint32_t storageCapacity = 0;    // total capacity in vec4<i32>
    };
    std::unordered_map<std::string, FontGPU> fontGPU_;

    std::unordered_map<std::string, std::vector<SlugVertex>> textVertsByFont_;

    // Rect pipeline
    wgpu::ShaderModule rectShader_;
    wgpu::RenderPipeline rectPipeline_;
    wgpu::BindGroupLayout rectBindGroupLayout_;
    wgpu::Buffer rectVertexBuffer_;
    wgpu::Buffer rectUniformBuffer_;
    wgpu::BindGroup rectBindGroup_;

    static constexpr uint32_t MAX_RECT_VERTS = 4096 * 6;

    std::vector<RectVertex> rectVerts_;
};

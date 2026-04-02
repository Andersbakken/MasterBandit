#pragma once

#include <dawn/webgpu_cpp.h>
#include "text.h"
#include <string>
#include <unordered_map>
#include <vector>

struct ScreenQuad {
    float x, y, w, h;
    float u0, v0, u1, v1;
    float tintR, tintG, tintB, tintA;
};

class Renderer {
public:
    void init(wgpu::Device& device, wgpu::Queue& queue,
              const std::string& shaderDir,
              uint32_t width, uint32_t height);

    void uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                         const FontData& font, bool sharp = false);

    void setViewportSize(uint32_t width, uint32_t height);

    void queueTextQuad(const std::string& fontName, const ScreenQuad& quad);
    void clearTextQuads();

    void renderFrame(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                     wgpu::TextureView swapchainView);

private:
    wgpu::Device device_;
    uint32_t viewportW_ = 0, viewportH_ = 0;

    // Text pipeline
    wgpu::ShaderModule textShader_;
    wgpu::RenderPipeline textPipeline_;
    wgpu::BindGroupLayout textBindGroupLayout_;
    wgpu::Sampler fontAtlasSampler_;
    wgpu::Sampler fontAtlasNearestSampler_;
    wgpu::Buffer textVertexBuffer_;

    static constexpr uint32_t MAX_TEXT_QUADS = 16384;

    struct FontGPU {
        wgpu::Texture tex;
        wgpu::TextureView view;
        wgpu::Buffer uniformBuffer;
        wgpu::BindGroup bindGroup;
    };
    std::unordered_map<std::string, FontGPU> fontGPU_;

    std::unordered_map<std::string, std::vector<ScreenQuad>> textQuadsByFont_;
};

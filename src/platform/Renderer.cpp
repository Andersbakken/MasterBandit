#include "Renderer.h"
#include "ProceduralGlyphTable.h"
#include "ColrAtlas.h"
#include "renderer_utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

void Renderer::init(wgpu::Device& device, wgpu::Queue& queue,
                    const std::string& shaderDir,
                    uint32_t width, uint32_t height)
{
    device_ = device;
    viewportW_ = width;
    viewportH_ = height;

    // ========================================
    // Text pipeline (Slug)
    // ========================================
    textShader_ = renderer_utils::createShaderModule(device_,
        (fs::path(shaderDir) / "slug_text.wgsl").string().c_str());
    if (!textShader_) {
        spdlog::error("Failed to load slug_text.wgsl shader");
        return;
    }

    // Bind group layout: uniform (b0), storage buffer (b1)
    wgpu::BindGroupLayoutEntry textEntries[2] = {};
    textEntries[0].binding = 0;
    textEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    textEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    textEntries[1].binding = 1;
    textEntries[1].visibility = wgpu::ShaderStage::Fragment;
    textEntries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    wgpu::BindGroupLayoutDescriptor textBglDesc = {};
    textBglDesc.entryCount = 2;
    textBglDesc.entries = textEntries;
    textBindGroupLayout_ = device_.CreateBindGroupLayout(&textBglDesc);

    wgpu::PipelineLayoutDescriptor textPlDesc = {};
    textPlDesc.bindGroupLayoutCount = 1;
    textPlDesc.bindGroupLayouts = &textBindGroupLayout_;
    wgpu::PipelineLayout textPipelineLayout = device_.CreatePipelineLayout(&textPlDesc);

    // Vertex layout: pos(2f) + texcoord(2f) + normal(2f) + emPerPos(f32) + atlas_offset(u32) + tint(u32)
    wgpu::VertexAttribute textVertAttrs[6] = {};
    textVertAttrs[0].format = wgpu::VertexFormat::Float32x2;
    textVertAttrs[0].offset = offsetof(SlugVertex, pos);
    textVertAttrs[0].shaderLocation = 0;
    textVertAttrs[1].format = wgpu::VertexFormat::Float32x2;
    textVertAttrs[1].offset = offsetof(SlugVertex, texcoord);
    textVertAttrs[1].shaderLocation = 1;
    textVertAttrs[2].format = wgpu::VertexFormat::Float32x2;
    textVertAttrs[2].offset = offsetof(SlugVertex, normal);
    textVertAttrs[2].shaderLocation = 2;
    textVertAttrs[3].format = wgpu::VertexFormat::Float32;
    textVertAttrs[3].offset = offsetof(SlugVertex, emPerPos);
    textVertAttrs[3].shaderLocation = 3;
    textVertAttrs[4].format = wgpu::VertexFormat::Uint32;
    textVertAttrs[4].offset = offsetof(SlugVertex, atlas_offset);
    textVertAttrs[4].shaderLocation = 4;
    textVertAttrs[5].format = wgpu::VertexFormat::Uint32;
    textVertAttrs[5].offset = offsetof(SlugVertex, tint);
    textVertAttrs[5].shaderLocation = 5;

    wgpu::VertexBufferLayout textVertBufLayout = {};
    textVertBufLayout.arrayStride = sizeof(SlugVertex);
    textVertBufLayout.stepMode = wgpu::VertexStepMode::Vertex;
    textVertBufLayout.attributeCount = 6;
    textVertBufLayout.attributes = textVertAttrs;

    // Blend state
    wgpu::BlendState textBlend = {};
    textBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    textBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    textBlend.color.operation = wgpu::BlendOperation::Add;
    textBlend.alpha.srcFactor = wgpu::BlendFactor::One;
    textBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    textBlend.alpha.operation = wgpu::BlendOperation::Add;

    wgpu::ColorTargetState textColorTarget = {};
    textColorTarget.format = wgpu::TextureFormat::BGRA8Unorm;
    textColorTarget.blend = &textBlend;

    wgpu::FragmentState textFragState = {};
    textFragState.module = textShader_;
    textFragState.entryPoint = "fs_main";
    textFragState.targetCount = 1;
    textFragState.targets = &textColorTarget;

    wgpu::RenderPipelineDescriptor textPipeDesc = {};
    textPipeDesc.layout = textPipelineLayout;
    textPipeDesc.vertex.module = textShader_;
    textPipeDesc.vertex.entryPoint = "vs_main";
    textPipeDesc.vertex.bufferCount = 1;
    textPipeDesc.vertex.buffers = &textVertBufLayout;
    textPipeDesc.fragment = &textFragState;
    textPipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    textPipeline_ = device_.CreateRenderPipeline(&textPipeDesc);

    // ========================================
    // Rect pipeline
    // ========================================
    rectShader_ = renderer_utils::createShaderModule(device_,
        (fs::path(shaderDir) / "rect.wgsl").string().c_str());
    if (!rectShader_) {
        spdlog::error("Failed to load rect.wgsl shader");
        return;
    }

    // Bind group layout: uniform (b0)
    wgpu::BindGroupLayoutEntry rectEntries[1] = {};
    rectEntries[0].binding = 0;
    rectEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    rectEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    wgpu::BindGroupLayoutDescriptor rectBglDesc = {};
    rectBglDesc.entryCount = 1;
    rectBglDesc.entries = rectEntries;
    rectBindGroupLayout_ = device_.CreateBindGroupLayout(&rectBglDesc);

    wgpu::PipelineLayoutDescriptor rectPlDesc = {};
    rectPlDesc.bindGroupLayoutCount = 1;
    rectPlDesc.bindGroupLayouts = &rectBindGroupLayout_;
    wgpu::PipelineLayout rectPipelineLayout = device_.CreatePipelineLayout(&rectPlDesc);

    // Vertex layout: pos(2f) + color(4f) + edge_dist(2f)
    wgpu::VertexAttribute rectVertAttrs[3] = {};
    rectVertAttrs[0].format = wgpu::VertexFormat::Float32x2;
    rectVertAttrs[0].offset = offsetof(RectVertex, pos);
    rectVertAttrs[0].shaderLocation = 0;
    rectVertAttrs[1].format = wgpu::VertexFormat::Float32x4;
    rectVertAttrs[1].offset = offsetof(RectVertex, color);
    rectVertAttrs[1].shaderLocation = 1;
    rectVertAttrs[2].format = wgpu::VertexFormat::Float32x2;
    rectVertAttrs[2].offset = offsetof(RectVertex, edge_dist);
    rectVertAttrs[2].shaderLocation = 2;

    wgpu::VertexBufferLayout rectVertBufLayout = {};
    rectVertBufLayout.arrayStride = sizeof(RectVertex);
    rectVertBufLayout.stepMode = wgpu::VertexStepMode::Vertex;
    rectVertBufLayout.attributeCount = 3;
    rectVertBufLayout.attributes = rectVertAttrs;

    // Blend state for rects
    wgpu::BlendState rectBlend = {};
    rectBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    rectBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    rectBlend.color.operation = wgpu::BlendOperation::Add;
    rectBlend.alpha.srcFactor = wgpu::BlendFactor::One;
    rectBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    rectBlend.alpha.operation = wgpu::BlendOperation::Add;

    wgpu::ColorTargetState rectColorTarget = {};
    rectColorTarget.format = wgpu::TextureFormat::BGRA8Unorm;
    rectColorTarget.blend = &rectBlend;

    wgpu::FragmentState rectFragState = {};
    rectFragState.module = rectShader_;
    rectFragState.entryPoint = "fs_main";
    rectFragState.targetCount = 1;
    rectFragState.targets = &rectColorTarget;

    wgpu::RenderPipelineDescriptor rectPipeDesc = {};
    rectPipeDesc.layout = rectPipelineLayout;
    rectPipeDesc.vertex.module = rectShader_;
    rectPipeDesc.vertex.entryPoint = "vs_main";
    rectPipeDesc.vertex.bufferCount = 1;
    rectPipeDesc.vertex.buffers = &rectVertBufLayout;
    rectPipeDesc.fragment = &rectFragState;
    rectPipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    rectPipeline_ = device_.CreateRenderPipeline(&rectPipeDesc);

    // Rect uniform buffer (viewport)
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = 32; // vec2f viewport + vec2f pad + vec4f pane_tint
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        rectUniformBuffer_ = device_.CreateBuffer(&desc);

        float uniforms[8] = {
            static_cast<float>(viewportW_),
            static_cast<float>(viewportH_),
            0.0f, 0.0f,
            1.0f, 1.0f, 1.0f, 1.0f  // pane_tint default = no tint
        };
        queue.WriteBuffer(rectUniformBuffer_, 0, uniforms, sizeof(uniforms));
    }

    // Rect bind group
    {
        wgpu::BindGroupEntry bindings[1] = {};
        bindings[0].binding = 0;
        bindings[0].buffer = rectUniformBuffer_;
        bindings[0].size = 32;

        wgpu::BindGroupDescriptor bgDesc = {};
        bgDesc.layout = rectBindGroupLayout_;
        bgDesc.entryCount = 1;
        bgDesc.entries = bindings;
        rectBindGroup_ = device_.CreateBindGroup(&bgDesc);
    }

    // Initialize compute pipeline
    initComputePipeline(device_, queue, shaderDir);

    // Initialize image pipeline
    initImagePipeline(device_, shaderDir);

    // Initialize COLR rasterizer pipeline
    initColrPipeline(device_, shaderDir);
}

void Renderer::uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                                const FontData& font)
{
    FontGPU gpu;

    // Storage buffer for glyph atlas (pre-allocate 4MB = 256K vec4<i32>)
    uint32_t capacity = std::max(font.atlasUsed, 256u * 1024u);
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = static_cast<uint64_t>(capacity) * 4 * sizeof(int32_t);
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        gpu.storageBuffer = device_.CreateBuffer(&desc);
        gpu.storageCapacity = capacity;
    }

    // Upload current atlas data
    if (font.atlasUsed > 0) {
        queue.WriteBuffer(gpu.storageBuffer, 0, font.atlasData.data(),
                          static_cast<uint64_t>(font.atlasUsed) * 4 * sizeof(int32_t));
    }
    gpu.uploadedSize = font.atlasUsed;

    // Uniform buffer: mat4x4f mvp + vec2f viewport + f32 gamma + f32 stem_darkening
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = 96;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        gpu.uniformBuffer = device_.CreateBuffer(&desc);

        float w = static_cast<float>(viewportW_);
        float h = static_cast<float>(viewportH_);
        // clang-format off
        float uniforms[24] = {
            2.0f/w,  0.0f,    0.0f, 0.0f,
            0.0f,   -2.0f/h,  0.0f, 0.0f,
            0.0f,    0.0f,    1.0f, 0.0f,
           -1.0f,    1.0f,    0.0f, 1.0f,
            w, h,
            1.0f,
            1.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
        };
        // clang-format on
        queue.WriteBuffer(gpu.uniformBuffer, 0, uniforms, sizeof(uniforms));
    }

    // Bind group
    wgpu::BindGroupEntry bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].buffer = gpu.uniformBuffer;
    bindings[0].size = 96;
    bindings[1].binding = 1;
    bindings[1].buffer = gpu.storageBuffer;
    bindings[1].size = static_cast<uint64_t>(capacity) * 4 * sizeof(int32_t);

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.layout = textBindGroupLayout_;
    bgDesc.entryCount = 2;
    bgDesc.entries = bindings;
    gpu.bindGroup = device_.CreateBindGroup(&bgDesc);

    fontGPU_[fontName] = std::move(gpu);

    spdlog::info("Uploaded font atlas '{}' to GPU ({} texels)", fontName, font.atlasUsed);
}

void Renderer::updateFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                                const FontData& font)
{
    auto it = fontGPU_.find(fontName);
    if (it == fontGPU_.end()) return;

    FontGPU& gpu = it->second;

    if (font.atlasUsed <= gpu.uploadedSize) return;

    if (font.atlasUsed > gpu.storageCapacity) {
        uploadFontAtlas(queue, fontName, font);
        return;
    }

    uint64_t offset = static_cast<uint64_t>(gpu.uploadedSize) * 4 * sizeof(int32_t);
    uint64_t size = static_cast<uint64_t>(font.atlasUsed - gpu.uploadedSize) * 4 * sizeof(int32_t);
    queue.WriteBuffer(gpu.storageBuffer, offset,
                      font.atlasData.data() + gpu.uploadedSize * 4, size);
    gpu.uploadedSize = font.atlasUsed;
}

void Renderer::removeFontAtlas(const std::string& fontName)
{
    fontGPU_.erase(fontName);
}

void Renderer::destroy()
{
    // Release all WebGPU resources so they don't outlive the device/glfwTerminate.
    fontGPU_.clear();
    imageGPU_.clear();
    device_              = nullptr;
    textShader_          = nullptr;
    textPipeline_        = nullptr;
    textBindGroupLayout_ = nullptr;
    rectShader_          = nullptr;
    rectPipeline_        = nullptr;
    rectBindGroupLayout_ = nullptr;
    rectUniformBuffer_   = nullptr;
    rectBindGroup_       = nullptr;
    computePool_.clear();
    computePipeline_        = nullptr;
    computeBindGroupLayout_ = nullptr;
    computeInitialized_     = false;
    imageShader_         = nullptr;
    imagePipeline_       = nullptr;
    imageBindGroupLayout_ = nullptr;
    imageUniformBuffer_  = nullptr;
    imageVertexBuffer_   = nullptr;
    imageSampler_        = nullptr;
    imagePipelineReady_  = false;
    colrComputePipeline_ = nullptr;
    colrComputeBGL_      = nullptr;
    colrPipelineReady_   = false;
    colrGlyphInfoBuffer_ = nullptr;
    colrInstructionBuffer_ = nullptr;
    colrColorStopBuffer_ = nullptr;
    colrQuadVertexBuffer_ = nullptr;
    for (auto& bg : colrBuckets_) {
        bg.texture = nullptr;
        bg.view = nullptr;
        bg.renderBindGroup = nullptr;
        bg.atlasDim = 0;
    }
    colrAtlas_.clear();
}

void Renderer::setViewportSize(uint32_t width, uint32_t height)
{
    viewportW_ = width;
    viewportH_ = height;
}


// ========================================
// Compute pipeline
// ========================================

void Renderer::initComputePipeline(wgpu::Device& device, wgpu::Queue& queue, const std::string& shaderDir)
{
    auto shaderModule = renderer_utils::createShaderModule(device,
        (fs::path(shaderDir) / "terminal_compute.wgsl").string().c_str());
    if (!shaderModule) {
        spdlog::error("Failed to load terminal_compute.wgsl shader");
        return;
    }

    // Bind group layout: 6 bindings
    wgpu::BindGroupLayoutEntry entries[6] = {};

    // b0: uniform params
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    // b1: resolved cells (read-only storage)
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b2: text vertices (read-write storage)
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::Storage;

    // b3: rect vertices (read-write storage)
    entries[3].binding = 3;
    entries[3].visibility = wgpu::ShaderStage::Compute;
    entries[3].buffer.type = wgpu::BufferBindingType::Storage;

    // b4: counters/indirect args (read-write storage)
    entries[4].binding = 4;
    entries[4].visibility = wgpu::ShaderStage::Compute;
    entries[4].buffer.type = wgpu::BufferBindingType::Storage;

    // b5: glyph entries (read-only storage)
    entries[5].binding = 5;
    entries[5].visibility = wgpu::ShaderStage::Compute;
    entries[5].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b6: box drawing table (read-only storage)
    wgpu::BindGroupLayoutEntry entry6 = {};
    entry6.binding = 6;
    entry6.visibility = wgpu::ShaderStage::Compute;
    entry6.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    wgpu::BindGroupLayoutEntry allEntries[7];
    for (int i = 0; i < 6; i++) allEntries[i] = entries[i];
    allEntries[6] = entry6;

    wgpu::BindGroupLayoutDescriptor bglDesc = {};
    bglDesc.entryCount = 7;
    bglDesc.entries = allEntries;
    computeBindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc = {};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &computeBindGroupLayout_;
    wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

    wgpu::ComputePipelineDescriptor cpDesc = {};
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = "main";
    computePipeline_ = device.CreateComputePipeline(&cpDesc);

    // Create and upload box drawing table buffer
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = sizeof(ProceduralGlyph::kTable);
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        boxDrawingTableBuffer_ = device.CreateBuffer(&desc);
        queue.WriteBuffer(boxDrawingTableBuffer_, 0, ProceduralGlyph::kTable, sizeof(ProceduralGlyph::kTable));
    }

    computePool_.init(device.Get(), computeBindGroupLayout_.Get(), boxDrawingTableBuffer_.Get());
    computeInitialized_ = true;
    spdlog::info("Compute pipeline initialized");
}


void Renderer::uploadResolvedCells(wgpu::Queue& queue, ComputeState* state,
                                    const ResolvedCell* cells, uint32_t count)
{
    if (!computeInitialized_ || !state || count == 0) return;
    queue.WriteBuffer(state->resolvedCellBuffer, 0, cells,
                      static_cast<uint64_t>(count) * sizeof(ResolvedCell));
}

void Renderer::uploadGlyphs(wgpu::Queue& queue, ComputeState* state,
                              const GlyphEntry* glyphs, uint32_t count)
{
    if (!computeInitialized_ || !state || count == 0) return;
    queue.WriteBuffer(state->glyphBuffer, 0, glyphs,
                      static_cast<uint64_t>(count) * sizeof(GlyphEntry));
}

// ========================================
// Image pipeline
// ========================================

struct ImageVertex {
    float pos[2];
    float uv[2];
};

// ── Progress bar pipeline ──────────────────────────────────────────────────

void Renderer::initProgressPipeline(wgpu::Device& device, const std::string& shaderDir)
{
    progressShader_ = renderer_utils::createShaderModule(device,
        (fs::path(shaderDir) / "progress_bar.wgsl").string().c_str());
    if (!progressShader_) {
        spdlog::error("Failed to load progress_bar.wgsl shader");
        return;
    }

    // Uniform buffer for progress params (80 bytes padded to 16-byte alignment)
    struct GPUProgressParams {
        float rect[4];
        float fill_frac;
        float edge_softness;
        float viewport_w;
        float viewport_h;
        float color[4];
        float soft_edges[2];
        float _pad2[2];
    };

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = sizeof(GPUProgressParams);
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    progressUniformBuffer_ = device.CreateBuffer(&ubDesc);

    // Bind group layout: single uniform buffer
    wgpu::BindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    entry.buffer.type = wgpu::BufferBindingType::Uniform;

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 1;
    bglDesc.entries = &entry;
    progressBindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

    wgpu::BindGroupEntry bgEntry{};
    bgEntry.binding = 0;
    bgEntry.buffer = progressUniformBuffer_;
    bgEntry.size = sizeof(GPUProgressParams);

    wgpu::BindGroupDescriptor bgDesc{};
    bgDesc.layout = progressBindGroupLayout_;
    bgDesc.entryCount = 1;
    bgDesc.entries = &bgEntry;
    progressBindGroup_ = device.CreateBindGroup(&bgDesc);

    // Pipeline
    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &progressBindGroupLayout_;
    wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

    wgpu::ColorTargetState colorTarget{};
    colorTarget.format = wgpu::TextureFormat::BGRA8Unorm;
    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha.srcFactor = wgpu::BlendFactor::One;
    blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    colorTarget.blend = &blend;

    wgpu::FragmentState fragment{};
    fragment.module = progressShader_;
    fragment.targetCount = 1;
    fragment.targets = &colorTarget;

    wgpu::RenderPipelineDescriptor rpDesc{};
    rpDesc.layout = pipelineLayout;
    rpDesc.vertex.module = progressShader_;
    rpDesc.fragment = &fragment;
    rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    progressPipeline_ = device.CreateRenderPipeline(&rpDesc);
    progressPipelineReady_ = true;
    spdlog::info("Progress bar pipeline initialized");
}

void Renderer::drawProgressBar(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                                wgpu::Texture target, uint32_t fbW, uint32_t fbH,
                                const ProgressBarParams& p)
{
    if (!progressPipelineReady_) return;

    struct GPUProgressParams {
        float rect[4];
        float fill_frac;
        float edge_softness;
        float viewport_w;
        float viewport_h;
        float color[4];
        float soft_edges[2];
        float _pad2[2];
    };

    GPUProgressParams gpu{};
    gpu.rect[0] = p.x;
    gpu.rect[1] = p.y;
    gpu.rect[2] = p.w;
    gpu.rect[3] = p.h;
    gpu.fill_frac = p.fillFrac;
    gpu.edge_softness = p.edgeSoftness;
    gpu.viewport_w = static_cast<float>(fbW);
    gpu.viewport_h = static_cast<float>(fbH);
    gpu.color[0] = p.r;
    gpu.color[1] = p.g;
    gpu.color[2] = p.b;
    gpu.color[3] = p.a;
    gpu.soft_edges[0] = p.softLeft;
    gpu.soft_edges[1] = p.softRight;
    gpu._pad2[0] = gpu._pad2[1] = 0;

    queue.WriteBuffer(progressUniformBuffer_, 0, &gpu, sizeof(gpu));

    wgpu::TextureViewDescriptor tvDesc{};
    wgpu::TextureView targetView = target.CreateView(&tvDesc);

    wgpu::RenderPassColorAttachment colorAtt{};
    colorAtt.view = targetView;
    colorAtt.loadOp = wgpu::LoadOp::Load;
    colorAtt.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor rpDesc{};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &colorAtt;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
    pass.SetPipeline(progressPipeline_);
    pass.SetBindGroup(0, progressBindGroup_);
    pass.Draw(6); // 2 triangles
    pass.End();
}

void Renderer::initImagePipeline(wgpu::Device& device, const std::string& shaderDir)
{
    imageShader_ = renderer_utils::createShaderModule(device,
        (fs::path(shaderDir) / "image_quad.wgsl").string().c_str());
    if (!imageShader_) {
        spdlog::error("Failed to load image_quad.wgsl shader");
        return;
    }

    // Bind group layout: uniform, texture, sampler
    wgpu::BindGroupLayoutEntry entries[3] = {};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Vertex;
    entries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bglDesc = {};
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    imageBindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc = {};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &imageBindGroupLayout_;
    wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

    // Vertex layout: pos(2f) + uv(2f)
    wgpu::VertexAttribute attrs[2] = {};
    attrs[0].format = wgpu::VertexFormat::Float32x2;
    attrs[0].offset = 0;
    attrs[0].shaderLocation = 0;
    attrs[1].format = wgpu::VertexFormat::Float32x2;
    attrs[1].offset = 8;
    attrs[1].shaderLocation = 1;

    wgpu::VertexBufferLayout vbLayout = {};
    vbLayout.arrayStride = sizeof(ImageVertex);
    vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
    vbLayout.attributeCount = 2;
    vbLayout.attributes = attrs;

    // Blend for alpha
    wgpu::BlendState blend = {};
    blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.color.operation = wgpu::BlendOperation::Add;
    blend.alpha.srcFactor = wgpu::BlendFactor::One;
    blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha.operation = wgpu::BlendOperation::Add;

    wgpu::ColorTargetState colorTarget = {};
    colorTarget.format = wgpu::TextureFormat::BGRA8Unorm;
    colorTarget.blend = &blend;

    wgpu::FragmentState fragState = {};
    fragState.module = imageShader_;
    fragState.entryPoint = "fs_main";
    fragState.targetCount = 1;
    fragState.targets = &colorTarget;

    wgpu::RenderPipelineDescriptor pipeDesc = {};
    pipeDesc.layout = pipelineLayout;
    pipeDesc.vertex.module = imageShader_;
    pipeDesc.vertex.entryPoint = "vs_main";
    pipeDesc.vertex.bufferCount = 1;
    pipeDesc.vertex.buffers = &vbLayout;
    pipeDesc.fragment = &fragState;
    pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;

    imagePipeline_ = device.CreateRenderPipeline(&pipeDesc);

    // Uniform buffer (viewport size)
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = 16; // vec2f + padding
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        imageUniformBuffer_ = device.CreateBuffer(&desc);
    }

    // Vertex buffer (dynamic, 6 verts per image)
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = MaxImageSlots * 6 * sizeof(ImageVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        imageVertexBuffer_ = device.CreateBuffer(&desc);
    }

    // Sampler
    {
        wgpu::SamplerDescriptor desc = {};
        desc.magFilter = wgpu::FilterMode::Linear;
        desc.minFilter = wgpu::FilterMode::Linear;
        desc.addressModeU = wgpu::AddressMode::ClampToEdge;
        desc.addressModeV = wgpu::AddressMode::ClampToEdge;
        imageSampler_ = device.CreateSampler(&desc);
    }

    imagePipelineReady_ = true;
    spdlog::info("Image pipeline initialized");
}

void Renderer::ensureImageGPU(wgpu::Queue& queue, uint32_t imageId,
                               const uint8_t* rgba, uint32_t width, uint32_t height)
{
    if (imageGPU_.count(imageId)) return;

    ImageGPU gpu;
    gpu.width = width;
    gpu.height = height;

    // Create texture with 1px transparent border on each side to match
    // kitty's GL_CLAMP_TO_BORDER behavior — linear filtering at the edge
    // blends toward transparent black, producing a soft edge.
    uint32_t texW = width + 2, texH = height + 2;
    gpu.texWidth = texW;
    gpu.texHeight = texH;

    wgpu::TextureDescriptor texDesc = {};
    texDesc.size = {texW, texH, 1};
    texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texDesc.dimension = wgpu::TextureDimension::e2D;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    gpu.texture = device_.CreateTexture(&texDesc);
    gpu.view = gpu.texture.CreateView();

    // Upload image data at offset (1,1), leaving the border as zero (transparent black)
    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = gpu.texture;
    dst.origin = {1, 1, 0};
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = width * 4;
    layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    queue.WriteTexture(&dst, rgba, width * height * 4, &layout, &extent);

    // Create bind group
    wgpu::BindGroupEntry bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].buffer = imageUniformBuffer_;
    bindings[0].size = 16;
    bindings[1].binding = 1;
    bindings[1].textureView = gpu.view;
    bindings[2].binding = 2;
    bindings[2].sampler = imageSampler_;

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.layout = imageBindGroupLayout_;
    bgDesc.entryCount = 3;
    bgDesc.entries = bindings;
    gpu.bindGroup = device_.CreateBindGroup(&bgDesc);

    imageGPU_[imageId] = std::move(gpu);
}

void Renderer::updateImageGPU(wgpu::Queue& queue, uint32_t imageId,
                                const uint8_t* rgba, uint32_t width, uint32_t height,
                                uint32_t generation)
{
    auto it = imageGPU_.find(imageId);
    if (it == imageGPU_.end()) return;
    auto& gpu = it->second;
    if (gpu.frameGeneration == generation) return; // already up to date
    if (gpu.width != width || gpu.height != height) return; // dimensions changed, need full recreate

    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = gpu.texture;
    dst.origin = {1, 1, 0}; // upload inside the 1px border
    wgpu::TexelCopyBufferLayout layout = {};
    layout.bytesPerRow = width * 4;
    layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    queue.WriteTexture(&dst, rgba, width * height * 4, &layout, &extent);

    gpu.frameGeneration = generation;
}

void Renderer::renderImages(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                             wgpu::TextureView target,
                             float paneWidth, float paneHeight,
                             const std::vector<ImageDrawCmd>& cmds,
                             size_t rangeStart, size_t rangeCount)
{
    if (!imagePipelineReady_) return;
    size_t rangeEnd = std::min(rangeStart + rangeCount, cmds.size());
    if (rangeStart >= rangeEnd) return;

    // Update viewport uniform — use pane dimensions, not global framebuffer
    float uniforms[4] = { paneWidth, paneHeight, 0, 0 };
    queue.WriteBuffer(imageUniformBuffer_, 0, uniforms, sizeof(uniforms));

    wgpu::RenderPassColorAttachment att = {};
    att.view = target;
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor rpDesc = {};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &att;

    // Upload all image vertices into the shared vertex buffer at different offsets,
    // then record draw calls. This avoids WriteBuffer overwriting previous vertices
    // before the GPU executes the draws (all passes share one command encoder).
    size_t vertexStride = 6 * sizeof(ImageVertex);
    size_t count = 0;

    for (size_t ci = rangeStart; ci < rangeEnd; ++ci) {
        const auto& cmd = cmds[ci];
        if (!imageGPU_.count(cmd.imageId)) continue;
        if (count >= MaxImageSlots) {
            spdlog::error("image render: exceeded {} image slots, dropping remaining images", MaxImageSlots);
            break;
        }

        float x0 = cmd.x, y0 = cmd.y;
        float x1 = cmd.x + cmd.w, y1 = cmd.y + cmd.h;

        ImageVertex verts[6] = {
            {{x0, y0}, {cmd.u0, cmd.v0}},
            {{x1, y0}, {cmd.u1, cmd.v0}},
            {{x0, y1}, {cmd.u0, cmd.v1}},
            {{x1, y0}, {cmd.u1, cmd.v0}},
            {{x1, y1}, {cmd.u1, cmd.v1}},
            {{x0, y1}, {cmd.u0, cmd.v1}},
        };
        queue.WriteBuffer(imageVertexBuffer_, count * vertexStride, verts, sizeof(verts));
        count++;
    }

    size_t idx = 0;
    for (size_t ci = rangeStart; ci < rangeEnd; ++ci) {
        const auto& cmd = cmds[ci];
        auto it = imageGPU_.find(cmd.imageId);
        if (it == imageGPU_.end()) continue;
        if (idx >= count) break;

        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetViewport(0.0f, 0.0f, paneWidth, paneHeight, 0.0f, 1.0f);
        pass.SetPipeline(imagePipeline_);
        pass.SetBindGroup(0, it->second.bindGroup);
        pass.SetVertexBuffer(0, imageVertexBuffer_, idx * vertexStride, vertexStride);
        pass.Draw(6);
        pass.End();
        idx++;
    }
}

void Renderer::renderToPane(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                             const std::string& fontName,
                             const TerminalComputeParams& params,
                             ComputeState* computeState,
                             wgpu::TextureView target,
                             const float pane_tint[4],
                             const std::vector<ImageDrawCmd>& imageCmds,
                             size_t imgSplitText)
{
    if (!computeInitialized_) return;
    if (!computeState) return;

    auto fontIt = fontGPU_.find(fontName);
    if (fontIt == fontGPU_.end()) return;

    // Update uniforms — use full pane viewport (including padding) for NDC transform
    {
        float w = params.viewport_w;
        float h = params.viewport_h;
        float uniforms[8] = { w, h, 0.0f, 0.0f,
                               pane_tint[0], pane_tint[1], pane_tint[2], pane_tint[3] };
        queue.WriteBuffer(rectUniformBuffer_, 0, uniforms, sizeof(uniforms));

        FontGPU& gpu = fontIt->second;
        float textUniforms[24] = {
            2.0f/w,  0.0f,    0.0f, 0.0f,
            0.0f,   -2.0f/h,  0.0f, 0.0f,
            0.0f,    0.0f,    1.0f, 0.0f,
           -1.0f,    1.0f,    0.0f, 1.0f,
            w, h,
            1.0f,
            1.0f,
            pane_tint[0], pane_tint[1], pane_tint[2], pane_tint[3],
        };
        queue.WriteBuffer(gpu.uniformBuffer, 0, textUniforms, sizeof(textUniforms));
    }

    queue.WriteBuffer(computeState->computeParamsBuffer, 0, &params, sizeof(params));

    uint32_t indirectInit[8] = {0, 1, 0, 0, 0, 1, 0, 0};
    queue.WriteBuffer(computeState->indirectBuffer, 0, indirectInit, sizeof(indirectInit));

    // Compute pass — generates vertex data
    {
        wgpu::ComputePassDescriptor cpDesc = {};
        wgpu::ComputePassEncoder computePass = encoder.BeginComputePass(&cpDesc);
        computePass.SetPipeline(computePipeline_);
        computePass.SetBindGroup(0, computeState->bindGroup);
        uint32_t totalCells = params.cols * params.rows;
        uint32_t workgroups = (totalCells + 255) / 256;
        computePass.DispatchWorkgroups(workgroups, 1, 1);
        computePass.End();
    }

    // Clear pass
    {
        wgpu::RenderPassColorAttachment att = {};
        att.view     = target;
        att.loadOp   = wgpu::LoadOp::Clear;
        att.storeOp  = wgpu::StoreOp::Store;
        att.clearValue = {0.0, 0.0, 0.0, 1.0};
        wgpu::RenderPassDescriptor rpDesc = {};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &att;
        encoder.BeginRenderPass(&rpDesc).End();
    }

    // Use full pane viewport (including padding) so NDC maps correctly.
    float contentW = params.viewport_w;
    float contentH = params.viewport_h;

    // Rect pass
    {
        wgpu::RenderPassColorAttachment att = {};
        att.view    = target;
        att.loadOp  = wgpu::LoadOp::Load;
        att.storeOp = wgpu::StoreOp::Store;
        wgpu::RenderPassDescriptor rpDesc = {};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetViewport(0.0f, 0.0f, contentW, contentH, 0.0f, 1.0f);
        pass.SetPipeline(rectPipeline_);
        pass.SetBindGroup(0, rectBindGroup_);
        pass.SetVertexBuffer(0, computeState->computeRectVertBuffer);
        pass.DrawIndirect(computeState->indirectBuffer, 16);
        pass.End();
    }

    // Below-text images (z < 0) — between backgrounds and text
    if (imgSplitText > 0)
        renderImages(encoder, queue, target, contentW, contentH, imageCmds, 0, imgSplitText);

    // Text pass
    {
        wgpu::RenderPassColorAttachment att = {};
        att.view    = target;
        att.loadOp  = wgpu::LoadOp::Load;
        att.storeOp = wgpu::StoreOp::Store;
        wgpu::RenderPassDescriptor rpDesc = {};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetViewport(0.0f, 0.0f, contentW, contentH, 0.0f, 1.0f);
        pass.SetPipeline(textPipeline_);
        pass.SetBindGroup(0, fontIt->second.bindGroup);
        pass.SetVertexBuffer(0, computeState->computeTextVertBuffer);
        pass.DrawIndirect(computeState->indirectBuffer, 0);
        pass.End();
    }

    // Above-text images (z >= 0) — after text
    if (imgSplitText < imageCmds.size())
        renderImages(encoder, queue, target, contentW, contentH, imageCmds, imgSplitText);
}

void Renderer::composite(wgpu::CommandEncoder& encoder,
                          wgpu::Texture swapchainTexture,
                          const std::vector<CompositeEntry>& entries)
{
    // Clear swapchain to black
    {
        wgpu::TextureViewDescriptor viewDesc = {};
        wgpu::TextureView swapView = swapchainTexture.CreateView(&viewDesc);
        wgpu::RenderPassColorAttachment att = {};
        att.view       = swapView;
        att.loadOp     = wgpu::LoadOp::Clear;
        att.storeOp    = wgpu::StoreOp::Store;
        att.clearValue = {0.0, 0.0, 0.0, 1.0};
        wgpu::RenderPassDescriptor rpDesc = {};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &att;
        encoder.BeginRenderPass(&rpDesc).End();
    }

    // Copy each pane texture to its rect on the swapchain
    for (const auto& e : entries) {
        if (!e.texture || e.srcW == 0 || e.srcH == 0) continue;

        wgpu::TexelCopyTextureInfo src = {};
        src.texture = e.texture;
        src.origin  = { 0, 0, 0 };

        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = swapchainTexture;
        dst.origin  = { e.dstX, e.dstY, 0 };

        // Clamp to actual texture size to guard against stale entries after resize.
        wgpu::Extent3D texSize = e.texture.GetWidth() > 0
            ? wgpu::Extent3D{ e.texture.GetWidth(), e.texture.GetHeight(), 1 }
            : wgpu::Extent3D{ e.srcW, e.srcH, 1 };
        wgpu::Extent3D extent = { std::min(e.srcW, texSize.width),
                                  std::min(e.srcH, texSize.height), 1 };
        encoder.CopyTextureToTexture(&src, &dst, &extent);
    }
}

void Renderer::updateDividerViewport(wgpu::Queue& queue, uint32_t fbWidth, uint32_t fbHeight)
{
    if (!rectBindGroupLayout_) return;

    // Dividers always use no tint (1,1,1,1); viewport in first two floats.
    float uniforms[8] = { static_cast<float>(fbWidth), static_cast<float>(fbHeight),
                           0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    if (!dividerUniformBuffer_) {
        wgpu::BufferDescriptor desc = {};
        desc.size  = 32; // matches RectUniforms: vec2f viewport + vec2f pad + vec4f pane_tint
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        dividerUniformBuffer_ = device_.CreateBuffer(&desc);

        wgpu::BindGroupEntry entry = {};
        entry.binding = 0;
        entry.buffer  = dividerUniformBuffer_;
        entry.size    = 32;
        wgpu::BindGroupDescriptor bgDesc = {};
        bgDesc.layout     = rectBindGroupLayout_;
        bgDesc.entryCount = 1;
        bgDesc.entries    = &entry;
        dividerBindGroup_ = device_.CreateBindGroup(&bgDesc);
    }

    queue.WriteBuffer(dividerUniformBuffer_, 0, uniforms, sizeof(uniforms));
}

void Renderer::updateDividerBuffer(wgpu::Queue& queue,
                                    wgpu::Buffer& vb,
                                    float x, float y, float w, float h,
                                    float r, float g, float b, float a)
{
    RectVertex verts[6];
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    auto set = [&](int i, float px, float py) {
        verts[i].pos[0] = px; verts[i].pos[1] = py;
        verts[i].color[0] = r; verts[i].color[1] = g;
        verts[i].color[2] = b; verts[i].color[3] = a;
        verts[i].edge_dist[0] = 1e3f; verts[i].edge_dist[1] = 1e3f;
    };
    set(0, x0, y0); set(1, x1, y0); set(2, x0, y1);
    set(3, x1, y0); set(4, x1, y1); set(5, x0, y1);

    if (!vb) {
        wgpu::BufferDescriptor desc = {};
        desc.size  = sizeof(verts);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vb = device_.CreateBuffer(&desc);
    }
    queue.WriteBuffer(vb, 0, verts, sizeof(verts));
}

void Renderer::drawDivider(wgpu::CommandEncoder& encoder,
                            wgpu::Texture swapchainTexture,
                            uint32_t fbWidth, uint32_t fbHeight,
                            const wgpu::Buffer& vb)
{
    if (!vb || !dividerBindGroup_ || !rectPipeline_) return;

    wgpu::TextureViewDescriptor viewDesc = {};
    wgpu::TextureView swapView = swapchainTexture.CreateView(&viewDesc);
    wgpu::RenderPassColorAttachment att = {};
    att.view    = swapView;
    att.loadOp  = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rpDesc = {};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &att;

    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
    pass.SetViewport(0.0f, 0.0f,
                     static_cast<float>(fbWidth), static_cast<float>(fbHeight),
                     0.0f, 1.0f);
    pass.SetPipeline(rectPipeline_);
    pass.SetBindGroup(0, dividerBindGroup_);
    pass.SetVertexBuffer(0, vb);
    pass.Draw(6);
    pass.End();
}

// ========================================
// COLRv1 rasterizer pipeline
// ========================================

void Renderer::initColrPipeline(wgpu::Device& device, const std::string& shaderDir)
{
    auto shaderModule = renderer_utils::createShaderModule(device,
        (fs::path(shaderDir) / "colr_rasterize.wgsl").string().c_str());
    if (!shaderModule) {
        spdlog::error("Failed to load colr_rasterize.wgsl shader");
        return;
    }

    // Bind group layout: 5 bindings
    wgpu::BindGroupLayoutEntry entries[5] = {};

    // b0: glyph table (read-only storage)
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b1: instructions (read-only storage)
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b2: color stops (read-only storage)
    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b3: slug atlas (read-only storage)
    entries[3].binding = 3;
    entries[3].visibility = wgpu::ShaderStage::Compute;
    entries[3].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;

    // b4: output texture (write-only storage texture)
    entries[4].binding = 4;
    entries[4].visibility = wgpu::ShaderStage::Compute;
    entries[4].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[4].storageTexture.format = wgpu::TextureFormat::RGBA8Unorm;
    entries[4].storageTexture.viewDimension = wgpu::TextureViewDimension::e2D;

    wgpu::BindGroupLayoutDescriptor bglDesc = {};
    bglDesc.entryCount = 5;
    bglDesc.entries = entries;
    colrComputeBGL_ = device.CreateBindGroupLayout(&bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc = {};
    plDesc.bindGroupLayoutCount = 1;
    plDesc.bindGroupLayouts = &colrComputeBGL_;
    wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

    wgpu::ComputePipelineDescriptor cpDesc = {};
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = shaderModule;
    cpDesc.compute.entryPoint = "main";
    colrComputePipeline_ = device.CreateComputePipeline(&cpDesc);

    colrPipelineReady_ = true;
    spdlog::info("COLR rasterizer pipeline initialized");
}

void Renderer::ensureColrBucket(wgpu::CommandEncoder& encoder, wgpu::Queue& queue, uint32_t bucket)
{
    if (bucket >= ColrAtlas::NUM_BUCKETS) return;
    auto& bg = colrBuckets_[bucket];
    uint32_t dim = colrAtlas_.bucketAtlasDim(bucket);

    // Already created at the correct size — nothing to do
    if (bg.atlasDim == dim && bg.atlasDim != 0) return;

    uint32_t oldDim = bg.atlasDim;
    wgpu::Texture oldTexture = bg.texture;

    wgpu::TextureDescriptor texDesc = {};
    texDesc.size = { dim, dim, 1 };
    texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    texDesc.usage = wgpu::TextureUsage::StorageBinding |
                    wgpu::TextureUsage::TextureBinding |
                    wgpu::TextureUsage::CopySrc |
                    wgpu::TextureUsage::CopyDst;
    texDesc.dimension = wgpu::TextureDimension::e2D;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    bg.texture = device_.CreateTexture(&texDesc);
    bg.view = bg.texture.CreateView();

    // If growing from an existing texture, copy old content into the new one
    if (oldDim > 0 && oldTexture) {
        wgpu::TexelCopyTextureInfo src = {};
        src.texture = oldTexture;
        src.origin = { 0, 0, 0 };

        wgpu::TexelCopyTextureInfo dst = {};
        dst.texture = bg.texture;
        dst.origin = { 0, 0, 0 };

        wgpu::Extent3D copySize = { oldDim, oldDim, 1 };
        encoder.CopyTextureToTexture(&src, &dst, &copySize);

        spdlog::info("COLR atlas bucket {} grew from {}x{} to {}x{}, copied old content",
                     bucket, oldDim, oldDim, dim, dim);
    } else {
        spdlog::info("COLR atlas bucket {} ({}px tiles, {}x{} atlas) created",
                     bucket, ColrAtlas::BUCKET_SIZES[bucket], dim, dim);
    }

    // Create render bind group (reuses image pipeline layout: uniform, texture, sampler)
    if (imagePipelineReady_) {
        wgpu::BindGroupEntry bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].buffer = imageUniformBuffer_;
        bindings[0].size = 16;
        bindings[1].binding = 1;
        bindings[1].textureView = bg.view;
        bindings[2].binding = 2;
        bindings[2].sampler = imageSampler_;

        wgpu::BindGroupDescriptor bgDesc = {};
        bgDesc.layout = imageBindGroupLayout_;
        bgDesc.entryCount = 3;
        bgDesc.entries = bindings;
        bg.renderBindGroup = device_.CreateBindGroup(&bgDesc);
    }

    bg.atlasDim = dim;
}

void Renderer::rasterizeColrGlyphs(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                                    const std::string& fontName,
                                    const std::vector<ColrRasterCmd>& cmds)
{
    if (!colrPipelineReady_ || cmds.empty()) return;

    spdlog::debug("COLR rasterize: {} glyphs", cmds.size());

    auto fontIt = fontGPU_.find(fontName);
    if (fontIt == fontGPU_.end()) return;

    // Gather all instructions and color stops into contiguous buffers
    std::vector<ColrGlyphInfoGPU> glyphInfos;
    std::vector<uint32_t> allInstructions;
    std::vector<ColrColorStop> allColorStops;

    for (const auto& cmd : cmds) {
        ColrGlyphInfoGPU info{};
        info.instr_offset = static_cast<uint32_t>(allInstructions.size());
        info.instr_length = static_cast<uint32_t>(cmd.data->instructions.size());
        info.output_x = cmd.tile.x;
        info.output_y = cmd.tile.y;
        info.tile_w = cmd.tile.tile_w;
        info.tile_h = cmd.tile.tile_h;
        info.em_width = cmd.em_width;
        info.em_height = cmd.em_height;
        info.em_origin_x = cmd.em_origin_x;
        info.em_origin_y = cmd.em_origin_y;

        uint32_t stopBase = static_cast<uint32_t>(allColorStops.size());
        bool hasGradients = !cmd.data->colorStops.empty();

        if (hasGradients) {
            // Copy instructions and adjust color stop offsets to be global
            uint32_t instrStart = static_cast<uint32_t>(allInstructions.size());
            allInstructions.insert(allInstructions.end(),
                                   cmd.data->instructions.begin(),
                                   cmd.data->instructions.end());
            uint32_t pc = instrStart;
            while (pc < allInstructions.size()) {
                uint32_t header = allInstructions[pc];
                uint8_t opcode = header & 0xFF;
                uint32_t payloadLen = (header >> 8) & 0x3FF;
                uint32_t payload = pc + 1;
                pc = payload + payloadLen;

                switch (opcode) {
                    case 0x07: // FillLinearGrad: payload[6] = stop_offset
                    case 0x08: // FillRadialGrad: payload[6] = stop_offset
                        if (payloadLen >= 7)
                            allInstructions[payload + 6] += stopBase;
                        break;
                    case 0x09: // FillSweepGrad: payload[4] = stop_offset
                        if (payloadLen >= 5)
                            allInstructions[payload + 4] += stopBase;
                        break;
                    default: break;
                }
            }
            allColorStops.insert(allColorStops.end(),
                                 cmd.data->colorStops.begin(),
                                 cmd.data->colorStops.end());
        } else {
            // No gradients — append instructions directly, no patching needed
            allInstructions.insert(allInstructions.end(),
                                   cmd.data->instructions.begin(),
                                   cmd.data->instructions.end());
        }
        glyphInfos.push_back(info);
    }

    // Ensure color stops buffer has at least one entry (GPU needs valid binding)
    if (allColorStops.empty()) {
        allColorStops.push_back({0.0f, 0});
    }

    // Sort glyph infos by bucket so each bucket is a contiguous range
    std::vector<uint32_t> sortOrder(glyphInfos.size());
    std::iota(sortOrder.begin(), sortOrder.end(), 0u);
    std::sort(sortOrder.begin(), sortOrder.end(), [&](uint32_t a, uint32_t b) {
        return cmds[a].tile.bucket < cmds[b].tile.bucket;
    });
    std::vector<ColrGlyphInfoGPU> sortedGlyphInfos(glyphInfos.size());
    for (uint32_t i = 0; i < sortOrder.size(); i++)
        sortedGlyphInfos[i] = glyphInfos[sortOrder[i]];

    // Upload glyph info buffer (single upload, sorted by bucket)
    {
        uint32_t needed = static_cast<uint32_t>(sortedGlyphInfos.size());
        if (needed > colrGlyphInfoCapacity_) {
            colrGlyphInfoCapacity_ = std::max(needed, 32u);
            wgpu::BufferDescriptor desc = {};
            desc.size = colrGlyphInfoCapacity_ * sizeof(ColrGlyphInfoGPU);
            desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            colrGlyphInfoBuffer_ = device_.CreateBuffer(&desc);
        }
        queue.WriteBuffer(colrGlyphInfoBuffer_, 0, sortedGlyphInfos.data(),
                          sortedGlyphInfos.size() * sizeof(ColrGlyphInfoGPU));
    }

    // Upload instruction buffer
    {
        uint32_t needed = static_cast<uint32_t>(allInstructions.size());
        if (needed > colrInstructionCapacity_) {
            colrInstructionCapacity_ = std::max(needed, 4096u);
            wgpu::BufferDescriptor desc = {};
            desc.size = colrInstructionCapacity_ * sizeof(uint32_t);
            desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            colrInstructionBuffer_ = device_.CreateBuffer(&desc);
        }
        queue.WriteBuffer(colrInstructionBuffer_, 0, allInstructions.data(),
                          allInstructions.size() * sizeof(uint32_t));
    }

    // Upload color stop buffer
    {
        uint32_t needed = static_cast<uint32_t>(allColorStops.size());
        if (needed > colrColorStopCapacity_) {
            colrColorStopCapacity_ = std::max(needed, 256u);
            wgpu::BufferDescriptor desc = {};
            desc.size = colrColorStopCapacity_ * sizeof(ColrColorStop);
            desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
            colrColorStopBuffer_ = device_.CreateBuffer(&desc);
        }
        queue.WriteBuffer(colrColorStopBuffer_, 0, allColorStops.data(),
                          allColorStops.size() * sizeof(ColrColorStop));
    }

    // Dispatch per bucket using ranges into the sorted glyph info buffer
    uint32_t rangeStart = 0;
    uint32_t totalGlyphs = static_cast<uint32_t>(sortedGlyphInfos.size());
    while (rangeStart < totalGlyphs) {
        uint32_t bi = cmds[sortOrder[rangeStart]].tile.bucket;
        uint32_t rangeEnd = rangeStart + 1;
        while (rangeEnd < totalGlyphs && cmds[sortOrder[rangeEnd]].tile.bucket == bi)
            rangeEnd++;
        uint32_t bucketCount = rangeEnd - rangeStart;

        ensureColrBucket(encoder, queue, bi);
        auto& bg = colrBuckets_[bi];

        // Bind group with offset into the glyph info buffer for this bucket's range
        uint64_t glyphInfoOffset = static_cast<uint64_t>(rangeStart) * sizeof(ColrGlyphInfoGPU);
        uint64_t glyphInfoSize = static_cast<uint64_t>(bucketCount) * sizeof(ColrGlyphInfoGPU);

        wgpu::BindGroupEntry bgEntries[5] = {};
        bgEntries[0].binding = 0;
        bgEntries[0].buffer = colrGlyphInfoBuffer_;
        bgEntries[0].offset = glyphInfoOffset;
        bgEntries[0].size = glyphInfoSize;
        bgEntries[1].binding = 1;
        bgEntries[1].buffer = colrInstructionBuffer_;
        bgEntries[1].size = allInstructions.size() * sizeof(uint32_t);
        bgEntries[2].binding = 2;
        bgEntries[2].buffer = colrColorStopBuffer_;
        bgEntries[2].size = allColorStops.size() * sizeof(ColrColorStop);
        bgEntries[3].binding = 3;
        bgEntries[3].buffer = fontIt->second.storageBuffer;
        bgEntries[3].size = fontIt->second.uploadedSize > 0 ?
            static_cast<uint64_t>(fontIt->second.uploadedSize) * 4 * sizeof(int32_t) :
            fontIt->second.storageCapacity * sizeof(int32_t);
        bgEntries[4].binding = 4;
        bgEntries[4].textureView = bg.view;

        wgpu::BindGroupDescriptor bgDesc = {};
        bgDesc.layout = colrComputeBGL_;
        bgDesc.entryCount = 5;
        bgDesc.entries = bgEntries;
        wgpu::BindGroup bindGroup = device_.CreateBindGroup(&bgDesc);

        uint32_t tileSize = ColrAtlas::BUCKET_SIZES[bi];
        uint32_t workgroupsX = (tileSize + 15) / 16;
        uint32_t workgroupsY = (tileSize + 15) / 16;

        wgpu::ComputePassDescriptor cpDesc = {};
        wgpu::ComputePassEncoder computePass = encoder.BeginComputePass(&cpDesc);
        computePass.SetPipeline(colrComputePipeline_);
        computePass.SetBindGroup(0, bindGroup);
        computePass.DispatchWorkgroups(workgroupsX, workgroupsY, bucketCount);
        computePass.End();

        rangeStart = rangeEnd;
    }
}

void Renderer::renderColrQuads(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                                wgpu::TextureView target,
                                float viewport_w, float viewport_h,
                                const std::vector<ColrDrawCmd>& cmds)
{
    if (!imagePipelineReady_ || cmds.empty()) return;

    // Update viewport uniform
    float uniforms[4] = { viewport_w, viewport_h, 0, 0 };
    queue.WriteBuffer(imageUniformBuffer_, 0, uniforms, sizeof(uniforms));

    // Group quads by bucket
    for (uint32_t bi = 0; bi < ColrAtlas::NUM_BUCKETS; bi++) {
        auto& bg = colrBuckets_[bi];
        if (bg.atlasDim == 0) continue;

        std::vector<ImageVertex> verts;
        for (const auto& cmd : cmds) {
            if (cmd.tile.bucket != bi) continue;

            float x0 = cmd.x, y0 = cmd.y;
            float x1 = cmd.x + cmd.w, y1 = cmd.y + cmd.h;

            float atlasDim = static_cast<float>(bg.atlasDim);
            float u0 = static_cast<float>(cmd.tile.x) / atlasDim;
            float v0 = static_cast<float>(cmd.tile.y) / atlasDim;
            float u1 = static_cast<float>(cmd.tile.x + cmd.tile.tile_w) / atlasDim;
            float v1 = static_cast<float>(cmd.tile.y + cmd.tile.tile_h) / atlasDim;

            verts.push_back({{x0, y0}, {u0, v0}});
            verts.push_back({{x1, y0}, {u1, v0}});
            verts.push_back({{x0, y1}, {u0, v1}});
            verts.push_back({{x1, y0}, {u1, v0}});
            verts.push_back({{x1, y1}, {u1, v1}});
            verts.push_back({{x0, y1}, {u0, v1}});
        }

        if (verts.empty()) continue;

        // Ensure vertex buffer capacity
        uint32_t needed = static_cast<uint32_t>(verts.size());
        if (needed > colrQuadVertexCapacity_) {
            colrQuadVertexCapacity_ = std::max(needed, 256u);
            wgpu::BufferDescriptor desc = {};
            desc.size = colrQuadVertexCapacity_ * sizeof(ImageVertex);
            desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
            colrQuadVertexBuffer_ = device_.CreateBuffer(&desc);
        }
        queue.WriteBuffer(colrQuadVertexBuffer_, 0, verts.data(),
                          verts.size() * sizeof(ImageVertex));

        wgpu::RenderPassColorAttachment att = {};
        att.view = target;
        att.loadOp = wgpu::LoadOp::Load;
        att.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor rpDesc = {};
        rpDesc.colorAttachmentCount = 1;
        rpDesc.colorAttachments = &att;

        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetViewport(0.0f, 0.0f, viewport_w, viewport_h, 0.0f, 1.0f);
        pass.SetPipeline(imagePipeline_);
        pass.SetBindGroup(0, bg.renderBindGroup);
        pass.SetVertexBuffer(0, colrQuadVertexBuffer_);
        pass.Draw(static_cast<uint32_t>(verts.size()));
        pass.End();
    }
}

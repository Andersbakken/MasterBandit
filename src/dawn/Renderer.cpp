#include "Renderer.h"
#include "renderer_utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
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
    rectEntries[0].visibility = wgpu::ShaderStage::Vertex;
    rectEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    wgpu::BindGroupLayoutDescriptor rectBglDesc = {};
    rectBglDesc.entryCount = 1;
    rectBglDesc.entries = rectEntries;
    rectBindGroupLayout_ = device_.CreateBindGroupLayout(&rectBglDesc);

    wgpu::PipelineLayoutDescriptor rectPlDesc = {};
    rectPlDesc.bindGroupLayoutCount = 1;
    rectPlDesc.bindGroupLayouts = &rectBindGroupLayout_;
    wgpu::PipelineLayout rectPipelineLayout = device_.CreatePipelineLayout(&rectPlDesc);

    // Vertex layout: pos(2f) + color(4f)
    wgpu::VertexAttribute rectVertAttrs[2] = {};
    rectVertAttrs[0].format = wgpu::VertexFormat::Float32x2;
    rectVertAttrs[0].offset = offsetof(RectVertex, pos);
    rectVertAttrs[0].shaderLocation = 0;
    rectVertAttrs[1].format = wgpu::VertexFormat::Float32x4;
    rectVertAttrs[1].offset = offsetof(RectVertex, color);
    rectVertAttrs[1].shaderLocation = 1;

    wgpu::VertexBufferLayout rectVertBufLayout = {};
    rectVertBufLayout.arrayStride = sizeof(RectVertex);
    rectVertBufLayout.stepMode = wgpu::VertexStepMode::Vertex;
    rectVertBufLayout.attributeCount = 2;
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
        desc.size = 16; // vec2f viewport + padding
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        rectUniformBuffer_ = device_.CreateBuffer(&desc);

        float uniforms[4] = {
            static_cast<float>(viewportW_),
            static_cast<float>(viewportH_),
            0.0f, 0.0f
        };
        queue.WriteBuffer(rectUniformBuffer_, 0, uniforms, sizeof(uniforms));
    }

    // Rect bind group
    {
        wgpu::BindGroupEntry bindings[1] = {};
        bindings[0].binding = 0;
        bindings[0].buffer = rectUniformBuffer_;
        bindings[0].size = 16;

        wgpu::BindGroupDescriptor bgDesc = {};
        bgDesc.layout = rectBindGroupLayout_;
        bgDesc.entryCount = 1;
        bgDesc.entries = bindings;
        rectBindGroup_ = device_.CreateBindGroup(&bgDesc);
    }

    // Initialize compute pipeline
    initComputePipeline(device_, shaderDir);

    // Initialize image pipeline
    initImagePipeline(device_, shaderDir);
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
}

void Renderer::setViewportSize(uint32_t width, uint32_t height)
{
    viewportW_ = width;
    viewportH_ = height;
}


// ========================================
// Compute pipeline
// ========================================

void Renderer::initComputePipeline(wgpu::Device& device, const std::string& shaderDir)
{
    auto shaderModule = renderer_utils::createShaderModule(device,
        (fs::path(shaderDir) / "terminal_compute.wgsl").string().c_str());
    if (!shaderModule) {
        spdlog::error("Failed to load terminal_compute.wgsl shader");
        return;
    }

    // Bind group layout: 5 bindings
    wgpu::BindGroupLayoutEntry entries[5] = {};

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

    wgpu::BindGroupLayoutDescriptor bglDesc = {};
    bglDesc.entryCount = 5;
    bglDesc.entries = entries;
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

    computePool_.init(device.Get(), computeBindGroupLayout_.Get());
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

// ========================================
// Image pipeline
// ========================================

struct ImageVertex {
    float pos[2];
    float uv[2];
};

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
        desc.size = 256 * 6 * sizeof(ImageVertex); // up to 256 images per frame
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

    // Create texture
    wgpu::TextureDescriptor texDesc = {};
    texDesc.size = {width, height, 1};
    texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texDesc.dimension = wgpu::TextureDimension::e2D;
    texDesc.mipLevelCount = 1;
    texDesc.sampleCount = 1;
    gpu.texture = device_.CreateTexture(&texDesc);
    gpu.view = gpu.texture.CreateView();

    // Upload pixel data
    wgpu::TexelCopyTextureInfo dst = {};
    dst.texture = gpu.texture;
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

void Renderer::renderImages(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                             wgpu::TextureView target,
                             const std::vector<ImageDrawCmd>& cmds)
{
    if (!imagePipelineReady_ || cmds.empty()) return;

    // Update viewport uniform
    float uniforms[4] = { static_cast<float>(viewportW_), static_cast<float>(viewportH_), 0, 0 };
    queue.WriteBuffer(imageUniformBuffer_, 0, uniforms, sizeof(uniforms));

    wgpu::RenderPassColorAttachment att = {};
    att.view = target;
    att.loadOp = wgpu::LoadOp::Load;
    att.storeOp = wgpu::StoreOp::Store;

    wgpu::RenderPassDescriptor rpDesc = {};
    rpDesc.colorAttachmentCount = 1;
    rpDesc.colorAttachments = &att;

    // Draw each image as a separate draw call (different bind group per image)
    for (const auto& cmd : cmds) {
        auto it = imageGPU_.find(cmd.imageId);
        if (it == imageGPU_.end()) continue;

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
        queue.WriteBuffer(imageVertexBuffer_, 0, verts, sizeof(verts));

        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rpDesc);
        pass.SetPipeline(imagePipeline_);
        pass.SetBindGroup(0, it->second.bindGroup);
        pass.SetVertexBuffer(0, imageVertexBuffer_);
        pass.Draw(6);
        pass.End();
    }
}

void Renderer::renderToPane(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                             const std::string& fontName,
                             const TerminalComputeParams& params,
                             ComputeState* computeState,
                             wgpu::TextureView target,
                             const std::vector<ImageDrawCmd>& imageCmds)
{
    if (!computeInitialized_) return;
    if (!computeState) return;

    auto fontIt = fontGPU_.find(fontName);
    if (fontIt == fontGPU_.end()) return;

    // Update uniforms — use pane dimensions (cols*cell_width, rows*cell_height) for NDC transform
    {
        float w = static_cast<float>(params.cols) * params.cell_width;
        float h = static_cast<float>(params.rows) * params.cell_height;
        float uniforms[4] = { w, h, 0.0f, 0.0f };
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
            0.0f, 0.0f, 0.0f, 0.0f,
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

    // Content dimensions — may be smaller than the texture if a larger pool texture was reused.
    // Setting an explicit viewport ensures NDC maps to the content area, not the full texture.
    float contentW = static_cast<float>(params.cols) * params.cell_width;
    float contentH = static_cast<float>(params.rows) * params.cell_height;

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

    // Image pass
    if (!imageCmds.empty())
        renderImages(encoder, queue, target, imageCmds);

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

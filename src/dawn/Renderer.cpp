#include "Renderer.h"
#include "renderer_utils.h"
#include <spdlog/spdlog.h>
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

    // Text vertex buffer
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = MAX_TEXT_VERTS * sizeof(SlugVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        textVertexBuffer_ = device_.CreateBuffer(&desc);
    }

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

    // Rect vertex buffer
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = MAX_RECT_VERTS * sizeof(RectVertex);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        rectVertexBuffer_ = device_.CreateBuffer(&desc);
    }

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
    // = 64 + 8 + 4 + 4 = 80 bytes, round to 96 for alignment
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = 96;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        gpu.uniformBuffer = device_.CreateBuffer(&desc);

        // Build orthographic MVP: maps pixel coords to NDC
        float w = static_cast<float>(viewportW_);
        float h = static_cast<float>(viewportH_);
        // clang-format off
        float uniforms[24] = {
            // mat4x4 mvp (column-major orthographic projection)
            2.0f/w,  0.0f,    0.0f, 0.0f,
            0.0f,   -2.0f/h,  0.0f, 0.0f,
            0.0f,    0.0f,    1.0f, 0.0f,
           -1.0f,    1.0f,    0.0f, 1.0f,
            // vec2f viewport
            w, h,
            // f32 gamma
            1.0f,
            // f32 stem_darkening
            1.0f,
            // padding
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

    // Check if we need to grow the storage buffer
    if (font.atlasUsed > gpu.storageCapacity) {
        // Need to recreate with larger capacity
        uploadFontAtlas(queue, fontName, font);
        return;
    }

    // Incremental upload: only the new data
    uint64_t offset = static_cast<uint64_t>(gpu.uploadedSize) * 4 * sizeof(int32_t);
    uint64_t size = static_cast<uint64_t>(font.atlasUsed - gpu.uploadedSize) * 4 * sizeof(int32_t);
    queue.WriteBuffer(gpu.storageBuffer, offset,
                      font.atlasData.data() + gpu.uploadedSize * 4, size);
    gpu.uploadedSize = font.atlasUsed;
}

void Renderer::setViewportSize(uint32_t width, uint32_t height)
{
    viewportW_ = width;
    viewportH_ = height;

    // Update rect uniform buffer
    float uniforms[4] = {
        static_cast<float>(width),
        static_cast<float>(height),
        0.0f, 0.0f
    };
    // Note: we need queue to write, but we don't have it here.
    // The viewport update for text uniforms will be handled by re-uploading font atlas.
    // For rect, we'll update in renderFrame.
}

void Renderer::queueGlyphVertices(const std::string& fontName,
                                   const SlugVertex* verts, uint32_t count)
{
    auto& vec = textVertsByFont_[fontName];
    vec.insert(vec.end(), verts, verts + count);
}

void Renderer::queueRect(float x, float y, float w, float h,
                          float r, float g, float b, float a)
{
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    RectVertex verts[6] = {
        {{x0, y0}, {r, g, b, a}},
        {{x1, y0}, {r, g, b, a}},
        {{x0, y1}, {r, g, b, a}},
        {{x1, y0}, {r, g, b, a}},
        {{x1, y1}, {r, g, b, a}},
        {{x0, y1}, {r, g, b, a}},
    };
    rectVerts_.insert(rectVerts_.end(), std::begin(verts), std::end(verts));
}

void Renderer::clearQueues()
{
    for (auto& [name, verts] : textVertsByFont_) {
        verts.clear();
    }
    rectVerts_.clear();
}

void Renderer::renderFrame(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                            wgpu::TextureView swapchainView)
{
    // Update rect uniform with current viewport
    {
        float uniforms[4] = {
            static_cast<float>(viewportW_),
            static_cast<float>(viewportH_),
            0.0f, 0.0f
        };
        queue.WriteBuffer(rectUniformBuffer_, 0, uniforms, sizeof(uniforms));
    }

    // Update text uniforms with current viewport for all fonts
    for (auto& [fontName, gpu] : fontGPU_) {
        float w = static_cast<float>(viewportW_);
        float h = static_cast<float>(viewportH_);
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
        queue.WriteBuffer(gpu.uniformBuffer, 0, uniforms, sizeof(uniforms));
    }

    // Clear pass (black background)
    {
        wgpu::RenderPassColorAttachment clearAttachment = {};
        clearAttachment.view = swapchainView;
        clearAttachment.loadOp = wgpu::LoadOp::Clear;
        clearAttachment.storeOp = wgpu::StoreOp::Store;
        clearAttachment.clearValue = {0.0, 0.0, 0.0, 1.0};

        wgpu::RenderPassDescriptor clearPassDesc = {};
        clearPassDesc.colorAttachmentCount = 1;
        clearPassDesc.colorAttachments = &clearAttachment;

        wgpu::RenderPassEncoder clearPass = encoder.BeginRenderPass(&clearPassDesc);
        clearPass.End();
    }

    // Rect pass (selection, cursor — rendered before text so text overlays)
    if (!rectVerts_.empty()) {
        uint32_t vertCount = static_cast<uint32_t>(rectVerts_.size());
        if (vertCount > MAX_RECT_VERTS) vertCount = MAX_RECT_VERTS;

        queue.WriteBuffer(rectVertexBuffer_, 0, rectVerts_.data(),
                          vertCount * sizeof(RectVertex));

        wgpu::RenderPassColorAttachment rectAttachment = {};
        rectAttachment.view = swapchainView;
        rectAttachment.loadOp = wgpu::LoadOp::Load;
        rectAttachment.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor rectPassDesc = {};
        rectPassDesc.colorAttachmentCount = 1;
        rectPassDesc.colorAttachments = &rectAttachment;

        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rectPassDesc);
        pass.SetPipeline(rectPipeline_);
        pass.SetBindGroup(0, rectBindGroup_);
        pass.SetVertexBuffer(0, rectVertexBuffer_);
        pass.Draw(vertCount);
        pass.End();
    }

    // Text pass (Slug)
    for (auto& [fontName, verts] : textVertsByFont_) {
        if (verts.empty()) continue;
        if (fontGPU_.find(fontName) == fontGPU_.end()) continue;

        uint32_t vertCount = static_cast<uint32_t>(verts.size());
        if (vertCount > MAX_TEXT_VERTS) vertCount = MAX_TEXT_VERTS;

        queue.WriteBuffer(textVertexBuffer_, 0, verts.data(),
                          vertCount * sizeof(SlugVertex));

        wgpu::RenderPassColorAttachment textAttachment = {};
        textAttachment.view = swapchainView;
        textAttachment.loadOp = wgpu::LoadOp::Load;
        textAttachment.storeOp = wgpu::StoreOp::Store;

        wgpu::RenderPassDescriptor textPassDesc = {};
        textPassDesc.colorAttachmentCount = 1;
        textPassDesc.colorAttachments = &textAttachment;

        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&textPassDesc);
        pass.SetPipeline(textPipeline_);
        pass.SetBindGroup(0, fontGPU_[fontName].bindGroup);
        pass.SetVertexBuffer(0, textVertexBuffer_);
        pass.Draw(vertCount);
        pass.End();
    }
}

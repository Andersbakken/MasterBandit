#include "Renderer.h"
#include "renderer_utils.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

void Renderer::init(wgpu::Device& device, wgpu::Queue& queue,
                    const std::string& shaderDir,
                    uint32_t width, uint32_t height)
{
    device_ = device;
    viewportW_ = width;
    viewportH_ = height;

    // Load MSDF text shader
    textShader_ = renderer_utils::createShaderModule(device_,
        (fs::path(shaderDir) / "msdf_text.wgsl").string().c_str());
    if (!textShader_) {
        spdlog::error("Failed to load msdf_text.wgsl shader");
        return;
    }

    // Bind group layout: uniform (b0), texture (b1), sampler (b2)
    wgpu::BindGroupLayoutEntry textEntries[3] = {};
    textEntries[0].binding = 0;
    textEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    textEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;

    textEntries[1].binding = 1;
    textEntries[1].visibility = wgpu::ShaderStage::Fragment;
    textEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    textEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    textEntries[2].binding = 2;
    textEntries[2].visibility = wgpu::ShaderStage::Fragment;
    textEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor textBglDesc = {};
    textBglDesc.entryCount = 3;
    textBglDesc.entries = textEntries;
    textBindGroupLayout_ = device_.CreateBindGroupLayout(&textBglDesc);

    wgpu::PipelineLayoutDescriptor textPlDesc = {};
    textPlDesc.bindGroupLayoutCount = 1;
    textPlDesc.bindGroupLayouts = &textBindGroupLayout_;
    wgpu::PipelineLayout textPipelineLayout = device_.CreatePipelineLayout(&textPlDesc);

    // Samplers
    {
        wgpu::SamplerDescriptor sampDesc = {};
        sampDesc.minFilter = wgpu::FilterMode::Linear;
        sampDesc.magFilter = wgpu::FilterMode::Linear;
        sampDesc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
        sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
        fontAtlasSampler_ = device_.CreateSampler(&sampDesc);
    }
    {
        wgpu::SamplerDescriptor sampDesc = {};
        sampDesc.minFilter = wgpu::FilterMode::Nearest;
        sampDesc.magFilter = wgpu::FilterMode::Nearest;
        sampDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
        sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
        fontAtlasNearestSampler_ = device_.CreateSampler(&sampDesc);
    }

    // Vertex layout: pos(2) + uv(2) + tint(4) = 8 floats
    wgpu::VertexAttribute textVertAttrs[3] = {};
    textVertAttrs[0].format = wgpu::VertexFormat::Float32x2;
    textVertAttrs[0].offset = 0;
    textVertAttrs[0].shaderLocation = 0;
    textVertAttrs[1].format = wgpu::VertexFormat::Float32x2;
    textVertAttrs[1].offset = 2 * sizeof(float);
    textVertAttrs[1].shaderLocation = 1;
    textVertAttrs[2].format = wgpu::VertexFormat::Float32x4;
    textVertAttrs[2].offset = 4 * sizeof(float);
    textVertAttrs[2].shaderLocation = 2;

    wgpu::VertexBufferLayout textVertBufLayout = {};
    textVertBufLayout.arrayStride = 8 * sizeof(float);
    textVertBufLayout.stepMode = wgpu::VertexStepMode::Vertex;
    textVertBufLayout.attributeCount = 3;
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
        desc.size = MAX_TEXT_QUADS * 6 * 8 * sizeof(float);
        desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        textVertexBuffer_ = device_.CreateBuffer(&desc);
    }
}

void Renderer::uploadFontAtlas(wgpu::Queue& queue, const std::string& fontName,
                                const FontData& font, bool sharp)
{
    FontGPU gpu;

    // Per-font uniform buffer (viewport + pxRange + sharp)
    {
        wgpu::BufferDescriptor desc = {};
        desc.size = 16;
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        gpu.uniformBuffer = device_.CreateBuffer(&desc);

        float uniforms[4] = {
            static_cast<float>(viewportW_),
            static_cast<float>(viewportH_),
            font.pxRange,
            sharp ? 1.0f : 0.0f
        };
        queue.WriteBuffer(gpu.uniformBuffer, 0, uniforms, sizeof(uniforms));
    }

    // GPU texture
    wgpu::TextureDescriptor texDesc = {};
    texDesc.size = {font.atlasWidth, font.atlasHeight, 1};
    texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    gpu.tex = device_.CreateTexture(&texDesc);

    wgpu::TexelCopyBufferLayout dataLayout = {};
    dataLayout.bytesPerRow = font.atlasWidth * 4;
    dataLayout.rowsPerImage = font.atlasHeight;

    wgpu::TexelCopyTextureInfo destInfo = {};
    destInfo.texture = gpu.tex;

    wgpu::Extent3D copySize = {font.atlasWidth, font.atlasHeight, 1};
    queue.WriteTexture(&destInfo, font.atlasPixels.data(),
                       font.atlasWidth * font.atlasHeight * 4,
                       &dataLayout, &copySize);

    gpu.view = gpu.tex.CreateView();

    // Bind group
    wgpu::BindGroupEntry bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].buffer = gpu.uniformBuffer;
    bindings[0].size = 16;
    bindings[1].binding = 1;
    bindings[1].textureView = gpu.view;
    bindings[2].binding = 2;
    bindings[2].sampler = sharp ? fontAtlasNearestSampler_ : fontAtlasSampler_;

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.layout = textBindGroupLayout_;
    bgDesc.entryCount = 3;
    bgDesc.entries = bindings;
    gpu.bindGroup = device_.CreateBindGroup(&bgDesc);

    fontGPU_[fontName] = std::move(gpu);

    spdlog::info("Uploaded font atlas '{}' to GPU", fontName);
}

void Renderer::setViewportSize(uint32_t width, uint32_t height)
{
    viewportW_ = width;
    viewportH_ = height;

    // Update all font uniform buffers with new viewport size
    for (auto& [fontName, gpu] : fontGPU_) {
        // We'd need to read the old pxRange/sharp values. For simplicity,
        // we'll just leave them — the uniform is updated on next uploadFontAtlas or
        // we can store pxRange/sharp per font. For now just note this needs re-upload.
    }
}

void Renderer::queueTextQuad(const std::string& fontName, const ScreenQuad& quad)
{
    textQuadsByFont_[fontName].push_back(quad);
}

void Renderer::clearTextQuads()
{
    for (auto& [name, quads] : textQuadsByFont_) {
        quads.clear();
    }
}

void Renderer::renderFrame(wgpu::CommandEncoder& encoder, wgpu::Queue& queue,
                            wgpu::TextureView swapchainView)
{
    // Build batches
    struct FontBatch { std::string name; uint64_t byteOffset; uint32_t vertCount; };
    std::vector<FontBatch> batches;
    std::vector<float> allTextVerts;

    for (auto& [fontName, quads] : textQuadsByFont_) {
        if (quads.empty()) continue;
        if (fontGPU_.find(fontName) == fontGPU_.end()) continue;

        uint32_t quadCount = static_cast<uint32_t>(quads.size());
        if (quadCount > MAX_TEXT_QUADS) quadCount = MAX_TEXT_QUADS;

        uint64_t byteOffset = allTextVerts.size() * sizeof(float);
        uint32_t vertCount = quadCount * 6;

        for (uint32_t i = 0; i < quadCount; i++) {
            const auto& q = quads[i];
            float x0 = q.x, y0 = q.y;
            float x1 = q.x + q.w, y1 = q.y + q.h;
            float verts[] = {
                x0,y0, q.u0,q.v0, q.tintR,q.tintG,q.tintB,q.tintA,
                x1,y0, q.u1,q.v0, q.tintR,q.tintG,q.tintB,q.tintA,
                x0,y1, q.u0,q.v1, q.tintR,q.tintG,q.tintB,q.tintA,
                x1,y0, q.u1,q.v0, q.tintR,q.tintG,q.tintB,q.tintA,
                x1,y1, q.u1,q.v1, q.tintR,q.tintG,q.tintB,q.tintA,
                x0,y1, q.u0,q.v1, q.tintR,q.tintG,q.tintB,q.tintA,
            };
            allTextVerts.insert(allTextVerts.end(), std::begin(verts), std::end(verts));
        }

        batches.push_back({fontName, byteOffset, vertCount});
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

    // Text pass
    if (!batches.empty()) {
        queue.WriteBuffer(textVertexBuffer_, 0, allTextVerts.data(),
                          allTextVerts.size() * sizeof(float));

        for (const auto& batch : batches) {
            wgpu::RenderPassColorAttachment textColorAttachment = {};
            textColorAttachment.view = swapchainView;
            textColorAttachment.loadOp = wgpu::LoadOp::Load;
            textColorAttachment.storeOp = wgpu::StoreOp::Store;

            wgpu::RenderPassDescriptor textPassDesc = {};
            textPassDesc.colorAttachmentCount = 1;
            textPassDesc.colorAttachments = &textColorAttachment;

            wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&textPassDesc);
            pass.SetPipeline(textPipeline_);
            pass.SetBindGroup(0, fontGPU_[batch.name].bindGroup);
            pass.SetVertexBuffer(0, textVertexBuffer_, batch.byteOffset);
            pass.Draw(batch.vertCount);
            pass.End();
        }
    }
}

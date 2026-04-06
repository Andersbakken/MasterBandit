// Slug text rendering shader (hb-gpu / Bezier curve rasterization)
// Vertex format: pos(2f), texcoord(2f), normal(2f), emPerPos(f32), atlas_offset(u32), tint(u32)

#include "hb-gpu-vertex.wgsl"
#include "hb-gpu-fragment.wgsl"

struct VSInput {
    @location(0) pos: vec2f,
    @location(1) texcoord: vec2f,
    @location(2) normal: vec2f,
    @location(3) emPerPos: f32,
    @location(4) atlas_offset: u32,
    @location(5) tint: u32,
};

struct VSOutput {
    @builtin(position) position: vec4f,
    @location(0) texcoord: vec2f,
    @location(1) @interpolate(flat) atlas_offset: u32,
    @location(2) @interpolate(flat) tint: vec4f,
};

struct TextUniforms {
    mvp: mat4x4f,
    viewport: vec2f,
    gamma: f32,
    stem_darkening: f32,
    pane_tint: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: TextUniforms;
@group(0) @binding(1) var<storage, read> hb_gpu_atlas: array<vec4<i32>>;

@vertex
fn vs_main(in: VSInput) -> VSOutput {
    var out: VSOutput;

    // Unpack tint from u32 RGBA8
    let r = f32((in.tint >> 0u) & 0xFFu) / 255.0;
    let g = f32((in.tint >> 8u) & 0xFFu) / 255.0;
    let b = f32((in.tint >> 16u) & 0xFFu) / 255.0;
    let a = f32((in.tint >> 24u) & 0xFFu) / 255.0;
    out.tint = vec4f(r, g, b, a);

    // Dilate vertex by half a pixel for anti-aliasing
    let jac = vec4f(in.emPerPos, 0.0, 0.0, -in.emPerPos);
    let dilated = hb_gpu_dilate(in.pos, in.texcoord, in.normal, jac,
                                 uniforms.mvp, uniforms.viewport);

    out.position = uniforms.mvp * vec4f(dilated[0], 0.0, 1.0);
    out.texcoord = dilated[1];
    out.atlas_offset = in.atlas_offset;
    return out;
}

@fragment
fn fs_main(in: VSOutput) -> @location(0) vec4f {
    var coverage = hb_gpu_render(in.texcoord, in.atlas_offset, &hb_gpu_atlas);

    if (uniforms.stem_darkening > 0.0) {
        let brightness = max(max(in.tint.r, in.tint.g), in.tint.b);
        let ppem = hb_gpu_ppem(in.texcoord, in.atlas_offset, &hb_gpu_atlas);
        coverage = hb_gpu_darken(coverage, brightness, ppem);
    }

    coverage = pow(coverage, uniforms.gamma);

    if (coverage < 0.01) {
        discard;
    }
    return vec4f(in.tint.rgb * uniforms.pane_tint.rgb, in.tint.a * coverage);
}

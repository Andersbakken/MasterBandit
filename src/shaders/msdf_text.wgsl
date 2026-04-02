// MSDF text rendering shader
// Vertex format: pos.xy, uv.xy, tint.rgba = 8 floats.

struct VSInput {
    @location(0) pos: vec2f,
    @location(1) uv: vec2f,
    @location(2) tint: vec4f,
};

struct VSOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) tint: vec4f,
};

struct TextUniforms {
    viewport: vec2f,
    pxRange: f32,
    sharp: f32,       // > 0.5 = hard cutoff (pixel fonts)
};

@group(0) @binding(0) var<uniform> uniforms: TextUniforms;
@group(0) @binding(1) var fontAtlas: texture_2d<f32>;
@group(0) @binding(2) var fontSampler: sampler;

@vertex
fn vs_main(in: VSInput) -> VSOutput {
    var out: VSOutput;
    let ndc_x = in.pos.x / uniforms.viewport.x * 2.0 - 1.0;
    let ndc_y = 1.0 - in.pos.y / uniforms.viewport.y * 2.0;
    out.position = vec4f(ndc_x, ndc_y, 0.0, 1.0);
    out.uv = in.uv;
    out.tint = in.tint;
    return out;
}

fn median(r: f32, g: f32, b: f32) -> f32 {
    return max(min(r, g), min(max(r, g), b));
}

@fragment
fn fs_main(in: VSOutput) -> @location(0) vec4f {
    let msdf = textureSample(fontAtlas, fontSampler, in.uv);
    let texSize = vec2f(textureDimensions(fontAtlas, 0));
    let unitRange = uniforms.pxRange / texSize;
    let screenTexSize = vec2f(1.0) / fwidth(in.uv);
    let screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
    var opacity: f32;
    if (uniforms.sharp > 0.5) {
        opacity = msdf.a;
    } else {
        let sd = median(msdf.r, msdf.g, msdf.b);
        let screenPxDist = screenPxRange * (sd - 0.5);
        opacity = clamp(screenPxDist + 0.5, 0.0, 1.0);
    }
    if (opacity < 0.01) {
        discard;
    }
    return vec4f(in.tint.rgb, in.tint.a * opacity);
}

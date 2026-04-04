// Simple rectangle shader (cursor, selection, underlines)
// Vertex format: pos(2f), color(4f) = 24 bytes

struct VSInput {
    @location(0) pos: vec2f,
    @location(1) color: vec4f,
};

struct VSOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
};

struct RectUniforms {
    viewport:   vec2f,
    _pad:       vec2f,
    pane_tint:  vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: RectUniforms;

@vertex
fn vs_main(in: VSInput) -> VSOutput {
    var out: VSOutput;
    let ndc_x = in.pos.x / uniforms.viewport.x * 2.0 - 1.0;
    let ndc_y = 1.0 - in.pos.y / uniforms.viewport.y * 2.0;
    out.position = vec4f(ndc_x, ndc_y, 0.0, 1.0);
    out.color = in.color;
    return out;
}

@fragment
fn fs_main(in: VSOutput) -> @location(0) vec4f {
    return in.color * uniforms.pane_tint;
}

// Simple rectangle shader (cursor, selection, underlines)
// Vertex format: pos(2f), color(4f), edge_dist(2f) = 32 bytes

struct VSInput {
    @location(0) pos: vec2f,
    @location(1) color: vec4f,
    @location(2) edge_dist: vec2f,
};

struct VSOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) edge_dist: vec2f,
};

struct RectUniforms {
    viewport:   vec2f,
    _pad:       vec2f,
    pane_tint:  vec4f,
    // OSC 133 dim: .x = factor (0 = disabled, <1 = multiplier for non-selected rows),
    // .y = selection_y_min (pixels), .z = selection_y_max (pixels).
    dim_params: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: RectUniforms;

@vertex
fn vs_main(in: VSInput) -> VSOutput {
    var out: VSOutput;
    let ndc_x = in.pos.x / uniforms.viewport.x * 2.0 - 1.0;
    let ndc_y = 1.0 - in.pos.y / uniforms.viewport.y * 2.0;
    out.position = vec4f(ndc_x, ndc_y, 0.0, 1.0);
    out.color = in.color;
    out.edge_dist = in.edge_dist;
    return out;
}

@fragment
fn fs_main(in: VSOutput) -> @location(0) vec4f {
    let d = min(in.edge_dist.x, in.edge_dist.y);
    let aa = smoothstep(0.0, 1.0, d);
    let inside = in.position.y >= uniforms.dim_params.y && in.position.y < uniforms.dim_params.z;
    let dim = select(uniforms.dim_params.x, 1.0, inside);
    var c = in.color * uniforms.pane_tint;
    c = vec4f(c.rgb * dim, c.a * aa);
    return c;
}

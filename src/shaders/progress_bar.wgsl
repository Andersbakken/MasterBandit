// Progress bar shader with smooth gradient edges

struct Params {
    rect: vec4f,          // x, y, w, h in pixels (visible/clipped rect)
    fill_frac: f32,       // 0.0-1.0 fill fraction (1.0 for indeterminate segment)
    edge_softness: f32,   // gradient edge width in pixels
    viewport_w: f32,
    viewport_h: f32,
    color: vec4f,         // RGBA
    soft_edges: vec2f,    // (left_soft, right_soft): 1.0 = gradient, 0.0 = sharp
    _pad2: vec2f,
};

@group(0) @binding(0) var<uniform> params: Params;

struct VertexOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,       // 0..1 within the bar rect
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOut {
    // Fullscreen-covering quad (2 triangles), clipped by fragment shader
    let corners = array<vec2f, 6>(
        vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
        vec2f(1.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0),
    );
    let c = corners[idx];

    // Map to bar rect in pixel coords
    let px = params.rect.x + c.x * params.rect.z;
    let py = params.rect.y + c.y * params.rect.w;

    // Convert to NDC
    let ndc_x = (px / params.viewport_w) * 2.0 - 1.0;
    let ndc_y = 1.0 - (py / params.viewport_h) * 2.0;

    var out: VertexOut;
    out.pos = vec4f(ndc_x, ndc_y, 0.0, 1.0);
    out.uv = c;
    return out;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4f {
    let x_px = in.uv.x * params.rect.z; // pixel position within bar
    let bar_w = params.rect.z;
    let fill_w = bar_w * params.fill_frac;
    let soft = params.edge_softness;

    // Right edge gradient (fill extent)
    let right_soft = soft * params.soft_edges.y;
    var alpha = smoothstep(fill_w + right_soft, fill_w - right_soft, x_px);

    // Left edge gradient
    let left_soft = soft * params.soft_edges.x;
    if (left_soft > 0.0) {
        alpha *= smoothstep(0.0, left_soft, x_px);
    }

    return vec4f(params.color.rgb, params.color.a * alpha);
}

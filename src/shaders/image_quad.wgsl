// Image quad shader: renders textured rectangles for inline images

struct Params {
    viewport_w: f32,
    viewport_h: f32,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var img_texture: texture_2d<f32>;
@group(0) @binding(2) var img_sampler: sampler;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs_main(@location(0) pos: vec2f, @location(1) uv: vec2f) -> VsOut {
    var out: VsOut;
    out.pos = vec4f(
        pos.x / params.viewport_w * 2.0 - 1.0,
        1.0 - pos.y / params.viewport_h * 2.0,
        0.0, 1.0
    );
    out.uv = uv;
    return out;
}

@fragment fn fs_main(in: VsOut) -> @location(0) vec4f {
    return textureSample(img_texture, img_sampler, in.uv);
}

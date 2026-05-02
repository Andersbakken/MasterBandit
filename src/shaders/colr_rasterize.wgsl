// COLRv1 rasterizer compute shader
// Interprets a flat instruction buffer to rasterize color emoji into cached tiles.

// --- Instruction opcodes (must match ColrOp:: in ColrEncoder.h) ---
const OP_PUSH_TRANSFORM:   u32 = 0x01u;
const OP_POP_TRANSFORM:    u32 = 0x02u;
const OP_PUSH_CLIP_GLYPH:  u32 = 0x03u;
const OP_PUSH_CLIP_RECT:   u32 = 0x04u;
const OP_POP_CLIP:         u32 = 0x05u;
const OP_FILL_SOLID:       u32 = 0x06u;
const OP_FILL_LINEAR_GRAD: u32 = 0x07u;
const OP_FILL_RADIAL_GRAD: u32 = 0x08u;
const OP_FILL_SWEEP_GRAD:  u32 = 0x09u;
const OP_PUSH_GROUP:       u32 = 0x0Au;
const OP_POP_GROUP:        u32 = 0x0Bu;

// Gradient extend modes (matches hb_paint_extend_t)
const EXTEND_PAD:     u32 = 0u;
const EXTEND_REPEAT:  u32 = 1u;
const EXTEND_REFLECT: u32 = 2u;

// Stack limits
const MAX_TRANSFORM_DEPTH: u32 = 8u;
const MAX_CLIP_DEPTH:      u32 = 8u;
const MAX_GROUP_DEPTH:     u32 = 4u;

// --- Slug constants (from hb-gpu-fragment.wgsl) ---
const HB_GPU_UNITS_PER_EM: f32 = 4.0;
const HB_GPU_INV_UNITS: f32 = 1.0 / 4.0;

// --- Per-glyph info (indexed by workgroup) ---
struct ColrGlyphInfo {
    instr_offset: u32,   // start index into instruction buffer (u32 units)
    instr_length: u32,   // total u32s of instructions
    output_x: u32,       // x offset in output atlas (pixels)
    output_y: u32,       // y offset in output atlas (pixels)
    tile_w: u32,         // tile width in pixels
    tile_h: u32,         // tile height in pixels
    em_width: f32,       // glyph width in em-space
    em_height: f32,      // glyph height in em-space
    em_origin_x: f32,    // em-space origin x (left edge)
    em_origin_y: f32,    // em-space origin y (bottom edge)
    _pad0: u32,
    _pad1: u32,
};

struct ColrColorStop {
    offset: f32,
    color: u32,   // packed RGBA8 (R low byte)
};

@group(0) @binding(0) var<storage, read> glyph_table: array<ColrGlyphInfo>;
@group(0) @binding(1) var<storage, read> instructions: array<u32>;
@group(0) @binding(2) var<storage, read> color_stops: array<ColrColorStop>;
@group(0) @binding(3) var<storage, read> slug_atlas: array<vec4<i32>>;
@group(0) @binding(4) var output: texture_storage_2d<rgba8unorm, write>;

// =====================================================================
// Slug coverage evaluation (ported from hb-gpu-fragment.wgsl)
// =====================================================================

// MasterBandit packed atlas: two int16 per i32 component. See hb-gpu-fragment.wgsl.
fn slug_fetch(offset: i32) -> vec4<i32> {
    let raw = slug_atlas[offset >> 1];
    if ((offset & 1) == 0) {
        return vec4<i32>((raw.r << 16u) >> 16u,
                         (raw.g << 16u) >> 16u,
                         (raw.b << 16u) >> 16u,
                         (raw.a << 16u) >> 16u);
    }
    return vec4<i32>(raw.r >> 16u, raw.g >> 16u, raw.b >> 16u, raw.a >> 16u);
}

fn slug_calc_root_code(y1: f32, y2: f32, y3: f32) -> u32 {
    let i1 = bitcast<u32>(y1) >> 31u;
    let i2 = bitcast<u32>(y2) >> 30u;
    let i3 = bitcast<u32>(y3) >> 29u;
    var shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);
    return (0x2E74u >> shift) & 0x0101u;
}

fn slug_solve_horiz(a: vec2f, b: vec2f, p1: vec2f) -> vec2f {
    let ra = 1.0 / a.y;
    let rb = 0.5 / b.y;
    let d = sqrt(max(b.y * b.y - a.y * p1.y, 0.0));
    var t1 = (b.y - d) * ra;
    var t2 = (b.y + d) * ra;
    if (a.y == 0.0) { t1 = p1.y * rb; t2 = t1; }
    return vec2f((a.x * t1 - b.x * 2.0) * t1 + p1.x,
                 (a.x * t2 - b.x * 2.0) * t2 + p1.x);
}

fn slug_solve_vert(a: vec2f, b: vec2f, p1: vec2f) -> vec2f {
    let ra = 1.0 / a.x;
    let rb = 0.5 / b.x;
    let d = sqrt(max(b.x * b.x - a.x * p1.x, 0.0));
    var t1 = (b.x - d) * ra;
    var t2 = (b.x + d) * ra;
    if (a.x == 0.0) { t1 = p1.x * rb; t2 = t1; }
    return vec2f((a.y * t1 - b.y * 2.0) * t1 + p1.y,
                 (a.y * t2 - b.y * 2.0) * t2 + p1.y);
}

fn slug_calc_coverage(xcov: f32, ycov: f32, xwgt: f32, ywgt: f32) -> f32 {
    let coverage = max(abs(xcov * xwgt + ycov * ywgt) /
                       max(xwgt + ywgt, 1.0 / 65536.0),
                       min(abs(xcov), abs(ycov)));
    return clamp(coverage, 0.0, 1.0);
}

// Evaluate Slug coverage at em-space position for a glyph in the atlas.
// pixels_per_em: known from tile resolution.
fn slug_coverage(render_coord: vec2f, pixels_per_em: vec2f, glyph_loc_: u32) -> f32 {
    let glyph_loc = i32(glyph_loc_);

    let header0 = slug_fetch(glyph_loc);
    let header1 = slug_fetch(glyph_loc + 1);
    let ext = vec4f(header0) * HB_GPU_INV_UNITS;
    let num_h_bands = header1.r;
    let num_v_bands = header1.g;

    let ext_size = ext.zw - ext.xy;
    let band_scale = vec2f(f32(num_v_bands), f32(num_h_bands)) / max(ext_size, vec2f(1.0 / 65536.0));
    let band_offset = -ext.xy * band_scale;

    let band_index = clamp(vec2<i32>(render_coord * band_scale + band_offset),
                           vec2<i32>(0, 0),
                           vec2<i32>(num_v_bands - 1, num_h_bands - 1));

    let band_base = glyph_loc + 2;

    // --- Horizontal coverage ---
    var xcov: f32 = 0.0;
    var xwgt: f32 = 0.0;

    let hband_data = slug_fetch(band_base + band_index.y);
    let h_curve_count = hband_data.r;
    let h_split = f32(hband_data.a) * HB_GPU_INV_UNITS;
    let h_left_ray = (render_coord.x < h_split);
    var h_data_offset: i32;
    if (h_left_ray) { h_data_offset = hband_data.b + 32768; }
    else            { h_data_offset = hband_data.g + 32768; }

    for (var ci: i32 = 0; ci < h_curve_count; ci++) {
        let curve_offset = slug_fetch(glyph_loc + h_data_offset + ci).r + 32768;
        let raw12 = slug_fetch(glyph_loc + curve_offset);
        let raw3 = slug_fetch(glyph_loc + curve_offset + 1);
        let q12 = vec4f(raw12) * HB_GPU_INV_UNITS;
        let q3 = vec2f(vec2<i32>(raw3.r, raw3.g)) * HB_GPU_INV_UNITS;
        let p12 = q12 - vec4f(render_coord, render_coord);
        let p3 = q3 - render_coord;

        if (h_left_ray) {
            if (min(min(p12.x, p12.z), p3.x) * pixels_per_em.x > 0.5) { break; }
        } else {
            if (max(max(p12.x, p12.z), p3.x) * pixels_per_em.x < -0.5) { break; }
        }

        let code = slug_calc_root_code(p12.y, p12.w, p3.y);
        if (code != 0u) {
            let a = q12.xy - q12.zw * 2.0 + q3;
            let b = q12.xy - q12.zw;
            let r = slug_solve_horiz(a, b, p12.xy) * pixels_per_em.x;
            var cov: vec2f;
            if (h_left_ray) { cov = clamp(vec2f(0.5) - r, vec2f(0.0), vec2f(1.0)); }
            else            { cov = clamp(r + vec2f(0.5), vec2f(0.0), vec2f(1.0)); }

            if ((code & 1u) != 0u) {
                xcov += cov.x;
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                xcov -= cov.y;
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    // --- Vertical coverage ---
    var ycov: f32 = 0.0;
    var ywgt: f32 = 0.0;

    let vband_data = slug_fetch(band_base + num_h_bands + band_index.x);
    let v_curve_count = vband_data.r;
    let v_split = f32(vband_data.a) * HB_GPU_INV_UNITS;
    let v_left_ray = (render_coord.y < v_split);
    var v_data_offset: i32;
    if (v_left_ray) { v_data_offset = vband_data.b + 32768; }
    else            { v_data_offset = vband_data.g + 32768; }

    for (var ci: i32 = 0; ci < v_curve_count; ci++) {
        let curve_offset = slug_fetch(glyph_loc + v_data_offset + ci).r + 32768;
        let raw12 = slug_fetch(glyph_loc + curve_offset);
        let raw3 = slug_fetch(glyph_loc + curve_offset + 1);
        let q12 = vec4f(raw12) * HB_GPU_INV_UNITS;
        let q3 = vec2f(vec2<i32>(raw3.r, raw3.g)) * HB_GPU_INV_UNITS;
        let p12 = q12 - vec4f(render_coord, render_coord);
        let p3 = q3 - render_coord;

        if (v_left_ray) {
            if (min(min(p12.y, p12.w), p3.y) * pixels_per_em.y > 0.5) { break; }
        } else {
            if (max(max(p12.y, p12.w), p3.y) * pixels_per_em.y < -0.5) { break; }
        }

        let code = slug_calc_root_code(p12.x, p12.z, p3.x);
        if (code != 0u) {
            let a = q12.xy - q12.zw * 2.0 + q3;
            let b = q12.xy - q12.zw;
            let r = slug_solve_vert(a, b, p12.xy) * pixels_per_em.y;
            var cov: vec2f;
            if (v_left_ray) { cov = clamp(vec2f(0.5) - r, vec2f(0.0), vec2f(1.0)); }
            else            { cov = clamp(r + vec2f(0.5), vec2f(0.0), vec2f(1.0)); }

            if ((code & 1u) != 0u) {
                ycov -= cov.x;
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                ycov += cov.y;
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return slug_calc_coverage(xcov, ycov, xwgt, ywgt);
}

// =====================================================================
// Transform helpers
// =====================================================================

// Affine transform stored as 6 floats: xx, yx, xy, yy, dx, dy
// Represents: [xx xy dx]   Applied as: x' = xx*x + xy*y + dx
//             [yx yy dy]               y' = yx*x + yy*y + dy

struct Affine {
    xx: f32, yx: f32,
    xy: f32, yy: f32,
    dx: f32, dy: f32,
};

fn affine_identity() -> Affine {
    return Affine(1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
}

// Multiply: result = a * b (apply b first, then a)
fn affine_mul(a: Affine, b: Affine) -> Affine {
    return Affine(
        a.xx * b.xx + a.xy * b.yx,
        a.yx * b.xx + a.yy * b.yx,
        a.xx * b.xy + a.xy * b.yy,
        a.yx * b.xy + a.yy * b.yy,
        a.xx * b.dx + a.xy * b.dy + a.dx,
        a.yx * b.dx + a.yy * b.dy + a.dy,
    );
}

// Invert an affine transform
fn affine_invert(a: Affine) -> Affine {
    let det = a.xx * a.yy - a.xy * a.yx;
    let inv_det = 1.0 / det;
    return Affine(
        a.yy * inv_det,
        -a.yx * inv_det,
        -a.xy * inv_det,
        a.xx * inv_det,
        (a.xy * a.dy - a.yy * a.dx) * inv_det,
        (a.yx * a.dx - a.xx * a.dy) * inv_det,
    );
}

// Apply affine transform to a point
fn affine_apply(a: Affine, p: vec2f) -> vec2f {
    return vec2f(a.xx * p.x + a.xy * p.y + a.dx,
                 a.yx * p.x + a.yy * p.y + a.dy);
}

// =====================================================================
// Color helpers
// =====================================================================

fn unpack_rgba8(c: u32) -> vec4f {
    return vec4f(
        f32((c >>  0u) & 0xFFu) / 255.0,
        f32((c >>  8u) & 0xFFu) / 255.0,
        f32((c >> 16u) & 0xFFu) / 255.0,
        f32((c >> 24u) & 0xFFu) / 255.0,
    );
}

// Premultiply alpha
fn premul(c: vec4f) -> vec4f {
    return vec4f(c.rgb * c.a, c.a);
}

// SRC_OVER composite (premultiplied)
fn composite_over(src: vec4f, dst: vec4f) -> vec4f {
    return src + dst * (1.0 - src.a);
}

// Read a float from the instruction buffer at given offset
fn read_f32(offset: u32) -> f32 {
    return bitcast<f32>(instructions[offset]);
}

// =====================================================================
// Gradient evaluation
// =====================================================================

fn apply_extend(t: f32, mode: u32) -> f32 {
    switch (mode) {
        case 1u: { // REPEAT
            return t - floor(t);
        }
        case 2u: { // REFLECT
            let m = t - 2.0 * floor(t * 0.5);
            return select(m, 2.0 - m, m > 1.0);
        }
        default: { // PAD
            return clamp(t, 0.0, 1.0);
        }
    }
}

// Sample a gradient color line at parameter t (premultiplied interpolation)
fn sample_color_line(stop_offset: u32, stop_count: u32, t: f32) -> vec4f {
    if (stop_count == 0u) { return vec4f(0.0); }
    if (stop_count == 1u) { return premul(unpack_rgba8(color_stops[stop_offset].color)); }

    // Clamp to first/last stop
    let first = color_stops[stop_offset];
    let last = color_stops[stop_offset + stop_count - 1u];

    if (t <= first.offset) { return premul(unpack_rgba8(first.color)); }
    if (t >= last.offset)  { return premul(unpack_rgba8(last.color)); }

    // Find the two stops bracketing t
    var i = 0u;
    for (i = 0u; i < stop_count - 1u; i++) {
        if (color_stops[stop_offset + i + 1u].offset >= t) { break; }
    }

    let s0 = color_stops[stop_offset + i];
    let s1 = color_stops[stop_offset + i + 1u];

    let frac = (t - s0.offset) / max(s1.offset - s0.offset, 1.0 / 65536.0);

    // Interpolate in premultiplied space (per OpenType spec)
    let c0 = premul(unpack_rgba8(s0.color));
    let c1 = premul(unpack_rgba8(s1.color));
    return mix(c0, c1, frac);
}

// =====================================================================
// Main compute kernel
// =====================================================================

@compute @workgroup_size(16, 16)
fn main(@builtin(global_invocation_id) gid: vec3u,
        @builtin(workgroup_id) wid: vec3u) {

    // Z workgroup = glyph index, X/Y = pixel coordinates
    let glyph = glyph_table[wid.z];

    let local_x = gid.x;
    let local_y = gid.y;

    // Bounds check
    if (local_x >= glyph.tile_w || local_y >= glyph.tile_h) { return; }

    // Map pixel to em-space coordinate (pixel center)
    let pixel = vec2f(f32(local_x) + 0.5, f32(local_y) + 0.5);
    let em_per_pixel = vec2f(glyph.em_width / f32(glyph.tile_w),
                              glyph.em_height / f32(glyph.tile_h));
    // Base em-space position (before any transforms)
    // Y is flipped: pixel Y=0 is top, design-unit Y=max is top
    let base_em = vec2f(glyph.em_origin_x + pixel.x * em_per_pixel.x,
                        glyph.em_origin_y + glyph.em_height - pixel.y * em_per_pixel.y);

    let pixels_per_em = vec2f(f32(glyph.tile_w) / glyph.em_width,
                               f32(glyph.tile_h) / glyph.em_height);

    // State stacks
    var transform_stack: array<Affine, MAX_TRANSFORM_DEPTH>;
    var transform_top: u32 = 0u;
    transform_stack[0] = affine_identity();

    var clip_stack: array<f32, MAX_CLIP_DEPTH>;
    var clip_top: u32 = 0u;

    var group_stack: array<vec4f, MAX_GROUP_DEPTH>;
    var group_top: u32 = 0u;

    var out_color = vec4f(0.0);  // premultiplied RGBA, starts transparent

    // Walk instruction buffer
    var pc = glyph.instr_offset;
    let end = pc + glyph.instr_length;

    while (pc < end) {
        let header = instructions[pc];
        let opcode = header & 0xFFu;
        let payload_len = (header >> 8u) & 0x3FFu;
        let payload = pc + 1u;
        pc = payload + payload_len;

        switch (opcode) {

        case OP_PUSH_TRANSFORM: {
            let t = Affine(
                read_f32(payload + 0u), read_f32(payload + 1u),
                read_f32(payload + 2u), read_f32(payload + 3u),
                read_f32(payload + 4u), read_f32(payload + 5u),
            );
            let parent = transform_stack[transform_top];
            transform_top = min(transform_top + 1u, MAX_TRANSFORM_DEPTH - 1u);
            transform_stack[transform_top] = affine_mul(parent, t);
        }

        case OP_POP_TRANSFORM: {
            transform_top = select(transform_top - 1u, 0u, transform_top == 0u);
        }

        case OP_PUSH_CLIP_GLYPH: {
            let atlas_offset = instructions[payload];
            // Extents in design units
            let ext_min = vec2f(read_f32(payload + 1u), read_f32(payload + 2u));
            let ext_max = vec2f(read_f32(payload + 3u), read_f32(payload + 4u));

            // Transform pixel position into glyph's local coordinate space
            let inv_t = affine_invert(transform_stack[transform_top]);
            let em_pos = affine_apply(inv_t, base_em);

            // Compute pixels_per_em in local space for AA
            // Use the inverse transform's scale to adjust
            let sx = length(vec2f(inv_t.xx, inv_t.yx));
            let local_ppe = pixels_per_em / max(vec2f(sx), vec2f(1.0 / 65536.0));

            var coverage = 0.0;
            if (atlas_offset != 0u) {
                coverage = slug_coverage(em_pos, local_ppe, atlas_offset);
            }

            // Intersect with parent clip
            let parent_clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            clip_stack[clip_top] = parent_clip * coverage;
            clip_top = min(clip_top + 1u, MAX_CLIP_DEPTH - 1u);
        }

        case OP_PUSH_CLIP_RECT: {
            let inv_t = affine_invert(transform_stack[transform_top]);
            let local_pos = affine_apply(inv_t, base_em);

            let rect_min = vec2f(read_f32(payload), read_f32(payload + 1u));
            let rect_max = vec2f(read_f32(payload + 2u), read_f32(payload + 3u));

            var inside = 1.0;
            if (local_pos.x < rect_min.x || local_pos.x > rect_max.x ||
                local_pos.y < rect_min.y || local_pos.y > rect_max.y) {
                inside = 0.0;
            }

            let parent_clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            clip_stack[clip_top] = parent_clip * inside;
            clip_top = min(clip_top + 1u, MAX_CLIP_DEPTH - 1u);
        }

        case OP_POP_CLIP: {
            clip_top = select(clip_top - 1u, 0u, clip_top == 0u);
        }

        case OP_FILL_SOLID: {
            let rgba = unpack_rgba8(instructions[payload]);
            let fill = premul(rgba);
            let clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            let src = vec4f(fill.rgb * clip, fill.a * clip);
            out_color = composite_over(src, out_color);
        }

        case OP_FILL_RADIAL_GRAD: {
            let cx0 = read_f32(payload + 0u);
            let cy0 = read_f32(payload + 1u);
            let r0  = read_f32(payload + 2u);
            let cx1 = read_f32(payload + 3u);
            let cy1 = read_f32(payload + 4u);
            let r1  = read_f32(payload + 5u);
            let stop_offset = instructions[payload + 6u];
            let stop_count  = instructions[payload + 7u];
            let extend_mode = instructions[payload + 8u];

            // Transform pixel to gradient's local space
            let inv_t = affine_invert(transform_stack[transform_top]);
            let local_pos = affine_apply(inv_t, base_em);

            // Two-point conical gradient: find t where point is on circle
            // Parametric: center(t) = c0 + t*(c1-c0), radius(t) = r0 + t*(r1-r0)
            // Solve: |local_pos - center(t)|^2 = radius(t)^2
            let dc = vec2f(cx1 - cx0, cy1 - cy0);
            let dr = r1 - r0;
            let dp = local_pos - vec2f(cx0, cy0);

            let a_coeff = dot(dc, dc) - dr * dr;
            let b_coeff = dot(dp, dc) - r0 * dr;
            let c_coeff = dot(dp, dp) - r0 * r0;

            var t = 0.0;
            if (abs(a_coeff) < 1e-6) {
                // Linear case
                if (abs(b_coeff) > 1e-6) { t = c_coeff / (2.0 * b_coeff); }
            } else {
                let disc = b_coeff * b_coeff - a_coeff * c_coeff;
                if (disc >= 0.0) {
                    let sq = sqrt(disc);
                    let t1 = (b_coeff + sq) / a_coeff;
                    let t2 = (b_coeff - sq) / a_coeff;
                    // Pick the larger t where radius >= 0
                    if (r0 + t1 * dr >= 0.0) { t = t1; }
                    else if (r0 + t2 * dr >= 0.0) { t = t2; }
                }
            }

            t = apply_extend(t, extend_mode);
            let grad_color = sample_color_line(stop_offset, stop_count, t);
            let clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            let src = grad_color * clip;
            out_color = composite_over(src, out_color);
        }

        case OP_FILL_LINEAR_GRAD: {
            let x0 = read_f32(payload + 0u);
            let y0 = read_f32(payload + 1u);
            let x1 = read_f32(payload + 2u);
            let y1 = read_f32(payload + 3u);
            // x2,y2 is the rotation point (defines the gradient line perpendicular)
            let x2 = read_f32(payload + 4u);
            let y2 = read_f32(payload + 5u);
            let stop_offset = instructions[payload + 6u];
            let stop_count  = instructions[payload + 7u];
            let extend_mode = instructions[payload + 8u];

            let inv_t = affine_invert(transform_stack[transform_top]);
            let local_pos = affine_apply(inv_t, base_em);

            // Project onto gradient line
            let p0 = vec2f(x0, y0);
            let p1 = vec2f(x1, y1);
            let line = p1 - p0;
            let len_sq = dot(line, line);
            var t = 0.0;
            if (len_sq > 1e-12) {
                t = dot(local_pos - p0, line) / len_sq;
            }

            t = apply_extend(t, extend_mode);
            let grad_color = sample_color_line(stop_offset, stop_count, t);
            let clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            let src = grad_color * clip;
            out_color = composite_over(src, out_color);
        }

        case OP_FILL_SWEEP_GRAD: {
            let cx = read_f32(payload + 0u);
            let cy = read_f32(payload + 1u);
            let start_angle = read_f32(payload + 2u);
            let end_angle   = read_f32(payload + 3u);
            let stop_offset = instructions[payload + 4u];
            let stop_count  = instructions[payload + 5u];
            let extend_mode = instructions[payload + 6u];

            let inv_t = affine_invert(transform_stack[transform_top]);
            let local_pos = affine_apply(inv_t, base_em);

            let dp = local_pos - vec2f(cx, cy);
            let angle = atan2(-dp.y, dp.x);  // COLRv1 uses counter-clockwise
            var t = (angle - start_angle) / (end_angle - start_angle);

            t = apply_extend(t, extend_mode);
            let grad_color = sample_color_line(stop_offset, stop_count, t);
            let clip = select(1.0, clip_stack[clip_top - 1u], clip_top > 0u);
            let src = grad_color * clip;
            out_color = composite_over(src, out_color);
        }

        case OP_PUSH_GROUP: {
            group_stack[group_top] = out_color;
            group_top = min(group_top + 1u, MAX_GROUP_DEPTH - 1u);
            out_color = vec4f(0.0);
        }

        case OP_POP_GROUP: {
            let mode = instructions[payload];
            let src = out_color;
            group_top = select(group_top - 1u, 0u, group_top == 0u);
            let dst = group_stack[group_top];

            // Porter-Duff SRC_OVER (mode 2) is the common case
            switch (mode) {
                case 2u: { out_color = composite_over(src, dst); }  // SRC_OVER
                case 0u: { out_color = vec4f(0.0); }                // CLEAR
                case 1u: { out_color = src; }                       // SRC
                case 3u: { out_color = dst; }                       // DEST
                default: { out_color = composite_over(src, dst); }  // fallback to SRC_OVER
            }
        }

        default: {
            // Unknown opcode — skip (pc already advanced past payload)
        }

        }  // switch
    }  // while

    // Write output pixel
    let out_x = glyph.output_x + local_x;
    let out_y = glyph.output_y + local_y;
    textureStore(output, vec2u(out_x, out_y), out_color);
}

// Terminal compute shader: generates text + rect vertices from resolved cells

struct TerminalParams {
    cols: u32,
    rows: u32,
    cell_width: f32,    // pixels per cell column
    cell_height: f32,   // pixels per cell row
    viewport_w: f32,
    viewport_h: f32,
    font_ascender: f32, // ascender in pixels
    font_size: f32,     // font size in pixels
    pane_origin_x: f32, // pixel X offset of pane within window
    pane_origin_y: f32, // pixel Y offset of pane within window
    cursor_col: u32,
    cursor_row: u32,
    cursor_type: u32,   // 0=none 1=solid 2=hollow 3=underline 4=bar
    cursor_color: u32,  // packed RGBA8
    max_text_vertices: u32, // safety cap for text vertex emission
};

struct ResolvedCellGPU {
    glyph_offset: u32,   // index into glyphs array
    glyph_count: u32,    // number of glyphs for this cell
    fg_color: u32,        // packed RGBA8
    bg_color: u32,        // packed RGBA8 (0 = transparent/default)
    underline_info: u32,  // bits 0-2: style (0=none 1=straight 2=double 3=curly 4=dotted)
                          // bits 8-31: color packed RGB8 (0 = use fg_color)
};

struct GlyphEntryGPU {
    atlas_offset: u32,
    ext_min_x: f32,
    ext_min_y: f32,
    ext_max_x: f32,
    ext_max_y: f32,
    upem: u32,
    x_offset: f32,       // position relative to cell origin (pixels)
    y_offset: f32,       // position relative to baseline (pixels)
};

// Scalar layout to avoid vec2f padding
struct SlugVertexStorage {
    pos_x: f32,
    pos_y: f32,
    tc_x: f32,
    tc_y: f32,
    norm_x: f32,
    norm_y: f32,
    em_per_pos: f32,
    atlas_offset: u32,
    tint: u32,
};

struct RectVertexStorage {
    pos_x: f32,
    pos_y: f32,
    r: f32,
    g: f32,
    b: f32,
    a: f32,
};

@group(0) @binding(0) var<uniform> params: TerminalParams;
@group(0) @binding(1) var<storage, read> cells: array<ResolvedCellGPU>;
@group(0) @binding(2) var<storage, read_write> text_verts: array<SlugVertexStorage>;
@group(0) @binding(3) var<storage, read_write> rect_verts: array<RectVertexStorage>;
@group(0) @binding(4) var<storage, read_write> counters: array<atomic<u32>>;
@group(0) @binding(5) var<storage, read> glyphs: array<GlyphEntryGPU>;
@group(0) @binding(6) var<storage, read> box_drawing_table: array<u32>;
// counters layout (also indirect draw args):
// [0] text_vertexCount  [1] text_instanceCount(=1)  [2] text_firstVertex(=0)  [3] text_firstInstance(=0)
// [4] rect_vertexCount  [5] rect_instanceCount(=1)  [6] rect_firstVertex(=0)  [7] rect_firstInstance(=0)

@compute @workgroup_size(256, 1, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let idx = gid.x;
    let total = params.cols * params.rows;
    if (idx >= total) {
        return;
    }

    let col = idx % params.cols;
    let row = idx / params.cols;
    let cell = cells[idx];

    let base_x = f32(col) * params.cell_width + params.pane_origin_x;
    let base_y = f32(row) * params.cell_height + params.pane_origin_y;

    // Background rect
    if (cell.bg_color != 0u) {
        let base_idx = atomicAdd(&counters[4], 6u);

        let r = f32(cell.bg_color & 0xFFu) / 255.0;
        let g = f32((cell.bg_color >> 8u) & 0xFFu) / 255.0;
        let b = f32((cell.bg_color >> 16u) & 0xFFu) / 255.0;
        let a = f32((cell.bg_color >> 24u) & 0xFFu) / 255.0;

        let x0 = base_x;
        let y0 = base_y;
        let x1 = base_x + params.cell_width;
        let y1 = base_y + params.cell_height;

        rect_verts[base_idx + 0u] = RectVertexStorage(x0, y0, r, g, b, a);
        rect_verts[base_idx + 1u] = RectVertexStorage(x1, y0, r, g, b, a);
        rect_verts[base_idx + 2u] = RectVertexStorage(x0, y1, r, g, b, a);
        rect_verts[base_idx + 3u] = RectVertexStorage(x1, y0, r, g, b, a);
        rect_verts[base_idx + 4u] = RectVertexStorage(x1, y1, r, g, b, a);
        rect_verts[base_idx + 5u] = RectVertexStorage(x0, y1, r, g, b, a);
    }

    // Cursor
    if (col == params.cursor_col && row == params.cursor_row && params.cursor_type != 0u) {
        let cr = f32(params.cursor_color & 0xFFu) / 255.0;
        let cg = f32((params.cursor_color >> 8u) & 0xFFu) / 255.0;
        let cb = f32((params.cursor_color >> 16u) & 0xFFu) / 255.0;
        let ca = f32((params.cursor_color >> 24u) & 0xFFu) / 255.0;

        let x0 = base_x;
        let y0 = base_y;
        let x1 = base_x + params.cell_width;
        let y1 = base_y + params.cell_height;

        let t = 2.0; // cursor thickness in pixels

        if (params.cursor_type == 1u) {
            // Solid block cursor
            let base_idx = atomicAdd(&counters[4], 6u);
            rect_verts[base_idx + 0u] = RectVertexStorage(x0, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 1u] = RectVertexStorage(x1, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 2u] = RectVertexStorage(x0, y1, cr, cg, cb, ca);
            rect_verts[base_idx + 3u] = RectVertexStorage(x1, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 4u] = RectVertexStorage(x1, y1, cr, cg, cb, ca);
            rect_verts[base_idx + 5u] = RectVertexStorage(x0, y1, cr, cg, cb, ca);
        } else if (params.cursor_type == 2u) {
            // Hollow block cursor: 4 thin border rects
            let base_idx = atomicAdd(&counters[4], 24u);
            // Top
            rect_verts[base_idx +  0u] = RectVertexStorage(x0,   y0,   cr, cg, cb, ca);
            rect_verts[base_idx +  1u] = RectVertexStorage(x1,   y0,   cr, cg, cb, ca);
            rect_verts[base_idx +  2u] = RectVertexStorage(x0,   y0+t, cr, cg, cb, ca);
            rect_verts[base_idx +  3u] = RectVertexStorage(x1,   y0,   cr, cg, cb, ca);
            rect_verts[base_idx +  4u] = RectVertexStorage(x1,   y0+t, cr, cg, cb, ca);
            rect_verts[base_idx +  5u] = RectVertexStorage(x0,   y0+t, cr, cg, cb, ca);
            // Bottom
            rect_verts[base_idx +  6u] = RectVertexStorage(x0,   y1-t, cr, cg, cb, ca);
            rect_verts[base_idx +  7u] = RectVertexStorage(x1,   y1-t, cr, cg, cb, ca);
            rect_verts[base_idx +  8u] = RectVertexStorage(x0,   y1,   cr, cg, cb, ca);
            rect_verts[base_idx +  9u] = RectVertexStorage(x1,   y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 10u] = RectVertexStorage(x1,   y1,   cr, cg, cb, ca);
            rect_verts[base_idx + 11u] = RectVertexStorage(x0,   y1,   cr, cg, cb, ca);
            // Left
            rect_verts[base_idx + 12u] = RectVertexStorage(x0,   y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 13u] = RectVertexStorage(x0+t, y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 14u] = RectVertexStorage(x0,   y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 15u] = RectVertexStorage(x0+t, y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 16u] = RectVertexStorage(x0+t, y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 17u] = RectVertexStorage(x0,   y1-t, cr, cg, cb, ca);
            // Right
            rect_verts[base_idx + 18u] = RectVertexStorage(x1-t, y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 19u] = RectVertexStorage(x1,   y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 20u] = RectVertexStorage(x1-t, y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 21u] = RectVertexStorage(x1,   y0+t, cr, cg, cb, ca);
            rect_verts[base_idx + 22u] = RectVertexStorage(x1,   y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 23u] = RectVertexStorage(x1-t, y1-t, cr, cg, cb, ca);
        } else if (params.cursor_type == 3u) {
            // Underline cursor: thin rect at bottom of cell
            let base_idx = atomicAdd(&counters[4], 6u);
            rect_verts[base_idx + 0u] = RectVertexStorage(x0, y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 1u] = RectVertexStorage(x1, y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 2u] = RectVertexStorage(x0, y1,   cr, cg, cb, ca);
            rect_verts[base_idx + 3u] = RectVertexStorage(x1, y1-t, cr, cg, cb, ca);
            rect_verts[base_idx + 4u] = RectVertexStorage(x1, y1,   cr, cg, cb, ca);
            rect_verts[base_idx + 5u] = RectVertexStorage(x0, y1,   cr, cg, cb, ca);
        } else if (params.cursor_type == 4u) {
            // Bar cursor: thin rect at left of cell
            let base_idx = atomicAdd(&counters[4], 6u);
            rect_verts[base_idx + 0u] = RectVertexStorage(x0,   y0, cr, cg, cb, ca);
            rect_verts[base_idx + 1u] = RectVertexStorage(x0+t, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 2u] = RectVertexStorage(x0,   y1, cr, cg, cb, ca);
            rect_verts[base_idx + 3u] = RectVertexStorage(x0+t, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 4u] = RectVertexStorage(x0+t, y1, cr, cg, cb, ca);
            rect_verts[base_idx + 5u] = RectVertexStorage(x0,   y1, cr, cg, cb, ca);
        }
    }

    // Text glyphs — iterate cell's glyph list
    let tint = cell.fg_color;
    let fg_r = f32((tint >> 0u) & 0xFFu) / 255.0;
    let fg_g = f32((tint >> 8u) & 0xFFu) / 255.0;
    let fg_b = f32((tint >> 16u) & 0xFFu) / 255.0;
    let fg_a = f32((tint >> 24u) & 0xFFu) / 255.0;

    for (var gi = 0u; gi < cell.glyph_count; gi++) {
        let g = glyphs[cell.glyph_offset + gi];
        if (g.atlas_offset == 0u) {
            continue;
        }

        // Procedural box/block drawing: atlas_offset bit 31 = table index
        if ((g.atlas_offset & 0x80000000u) != 0u) {
            let table_idx = g.atlas_offset & 0x7FFFFFFFu;
            let entry = box_drawing_table[table_idx];
            let entry_type = entry & 0x07u;

            if (entry_type == 1u) {
                // Rect block: fill a rectangle in cell-eighths
                let rx0 = (entry >> 3u) & 0x0Fu;
                let rx1 = (entry >> 7u) & 0x0Fu;
                let ry0 = (entry >> 11u) & 0x0Fu;
                let ry1 = (entry >> 15u) & 0x0Fu;
                let cw = params.cell_width;
                let ch = params.cell_height;
                let px0 = base_x + cw * f32(rx0) / 8.0;
                let py0 = base_y + ch * f32(ry0) / 8.0;
                let px1 = base_x + cw * f32(rx1) / 8.0;
                let py1 = base_y + ch * f32(ry1) / 8.0;
                let ri = atomicAdd(&counters[4], 6u);
                rect_verts[ri + 0u] = RectVertexStorage(px0, py0, fg_r, fg_g, fg_b, fg_a);
                rect_verts[ri + 1u] = RectVertexStorage(px1, py0, fg_r, fg_g, fg_b, fg_a);
                rect_verts[ri + 2u] = RectVertexStorage(px0, py1, fg_r, fg_g, fg_b, fg_a);
                rect_verts[ri + 3u] = RectVertexStorage(px1, py0, fg_r, fg_g, fg_b, fg_a);
                rect_verts[ri + 4u] = RectVertexStorage(px1, py1, fg_r, fg_g, fg_b, fg_a);
                rect_verts[ri + 5u] = RectVertexStorage(px0, py1, fg_r, fg_g, fg_b, fg_a);
            } else if (entry_type == 2u) {
                // Quadrant: 4-bit mask (UL=1, UR=2, LL=4, LR=8)
                let mask = (entry >> 3u) & 0x0Fu;
                let cw = params.cell_width;
                let ch = params.cell_height;
                let mx = base_x + cw * 0.5;
                let my = base_y + ch * 0.5;
                let left  = base_x;
                let right = base_x + cw;
                let top   = base_y;
                let bot   = base_y + ch;
                var quad_count = 0u;
                if ((mask & 1u) != 0u) { quad_count += 1u; } // UL
                if ((mask & 2u) != 0u) { quad_count += 1u; } // UR
                if ((mask & 4u) != 0u) { quad_count += 1u; } // LL
                if ((mask & 8u) != 0u) { quad_count += 1u; } // LR
                let ri = atomicAdd(&counters[4], quad_count * 6u);
                var qi = 0u;
                if ((mask & 1u) != 0u) { // UL
                    let b = ri + qi * 6u;
                    rect_verts[b+0u] = RectVertexStorage(left, top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+1u] = RectVertexStorage(mx,   top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+2u] = RectVertexStorage(left, my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+3u] = RectVertexStorage(mx,   top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+4u] = RectVertexStorage(mx,   my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+5u] = RectVertexStorage(left, my,  fg_r, fg_g, fg_b, fg_a);
                    qi += 1u;
                }
                if ((mask & 2u) != 0u) { // UR
                    let b = ri + qi * 6u;
                    rect_verts[b+0u] = RectVertexStorage(mx,    top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+1u] = RectVertexStorage(right, top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+2u] = RectVertexStorage(mx,    my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+3u] = RectVertexStorage(right, top, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+4u] = RectVertexStorage(right, my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+5u] = RectVertexStorage(mx,    my,  fg_r, fg_g, fg_b, fg_a);
                    qi += 1u;
                }
                if ((mask & 4u) != 0u) { // LL
                    let b = ri + qi * 6u;
                    rect_verts[b+0u] = RectVertexStorage(left, my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+1u] = RectVertexStorage(mx,   my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+2u] = RectVertexStorage(left, bot, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+3u] = RectVertexStorage(mx,   my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+4u] = RectVertexStorage(mx,   bot, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+5u] = RectVertexStorage(left, bot, fg_r, fg_g, fg_b, fg_a);
                    qi += 1u;
                }
                if ((mask & 8u) != 0u) { // LR
                    let b = ri + qi * 6u;
                    rect_verts[b+0u] = RectVertexStorage(mx,    my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+1u] = RectVertexStorage(right, my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+2u] = RectVertexStorage(mx,    bot, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+3u] = RectVertexStorage(right, my,  fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+4u] = RectVertexStorage(right, bot, fg_r, fg_g, fg_b, fg_a);
                    rect_verts[b+5u] = RectVertexStorage(mx,    bot, fg_r, fg_g, fg_b, fg_a);
                    qi += 1u;
                }
            } else if (entry_type == 4u) {
                // Box drawing line: centered lines connecting edges
                let left_w  = (entry >> 3u)  & 0x03u;
                let right_w = (entry >> 5u)  & 0x03u;
                let up_w    = (entry >> 7u)  & 0x03u;
                let down_w  = (entry >> 9u)  & 0x03u;
                let cw = params.cell_width;
                let ch = params.cell_height;
                let cx = base_x + cw * 0.5;
                let cy = base_y + ch * 0.5;
                // Line thickness: light=1px, heavy=3px, double=1px (gap+line)
                // Count segments needed
                var seg_count = 0u;
                if (left_w  != 0u && left_w  != 3u) { seg_count += 1u; }
                if (right_w != 0u && right_w != 3u) { seg_count += 1u; }
                if (up_w    != 0u && up_w    != 3u) { seg_count += 1u; }
                if (down_w  != 0u && down_w  != 3u) { seg_count += 1u; }
                // Double lines need 2 segments each
                if (left_w  == 3u) { seg_count += 2u; }
                if (right_w == 3u) { seg_count += 2u; }
                if (up_w    == 3u) { seg_count += 2u; }
                if (down_w  == 3u) { seg_count += 2u; }
                if (seg_count == 0u) { continue; }
                let ri = atomicAdd(&counters[4], seg_count * 6u);
                var si = 0u;

                // Helper: emit a filled rect as 6 vertices
                // Horizontal segments (left/right)
                let has_h = (left_w != 0u || right_w != 0u);
                let has_v = (up_w != 0u || down_w != 0u);

                if (has_h) {
                    let hx0 = select(cx, base_x, left_w != 0u);
                    let hx1 = select(cx, base_x + cw, right_w != 0u);
                    let max_hw = max(left_w, right_w);
                    let ht = select(1.0, 3.0, max_hw == 2u);
                    if (max_hw != 3u) {
                        // Single or heavy horizontal line
                        let hy0 = cy - ht * 0.5;
                        let hy1 = cy + ht * 0.5;
                        let b = ri + si * 6u;
                        rect_verts[b+0u] = RectVertexStorage(hx0, hy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+1u] = RectVertexStorage(hx1, hy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+2u] = RectVertexStorage(hx0, hy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+3u] = RectVertexStorage(hx1, hy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+4u] = RectVertexStorage(hx1, hy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+5u] = RectVertexStorage(hx0, hy1, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                    } else {
                        // Double horizontal lines
                        let gap = 2.0;
                        let hy0a = cy - gap - 1.0;
                        let hy1a = cy - gap;
                        let hy0b = cy + gap;
                        let hy1b = cy + gap + 1.0;
                        let b1 = ri + si * 6u;
                        rect_verts[b1+0u] = RectVertexStorage(hx0, hy0a, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+1u] = RectVertexStorage(hx1, hy0a, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+2u] = RectVertexStorage(hx0, hy1a, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+3u] = RectVertexStorage(hx1, hy0a, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+4u] = RectVertexStorage(hx1, hy1a, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+5u] = RectVertexStorage(hx0, hy1a, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                        let b2 = ri + si * 6u;
                        rect_verts[b2+0u] = RectVertexStorage(hx0, hy0b, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+1u] = RectVertexStorage(hx1, hy0b, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+2u] = RectVertexStorage(hx0, hy1b, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+3u] = RectVertexStorage(hx1, hy0b, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+4u] = RectVertexStorage(hx1, hy1b, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+5u] = RectVertexStorage(hx0, hy1b, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                    }
                }

                if (has_v) {
                    let vy0 = select(cy, base_y, up_w != 0u);
                    let vy1 = select(cy, base_y + ch, down_w != 0u);
                    let max_vw = max(up_w, down_w);
                    let vt = select(1.0, 3.0, max_vw == 2u);
                    if (max_vw != 3u) {
                        let vx0 = cx - vt * 0.5;
                        let vx1 = cx + vt * 0.5;
                        let b = ri + si * 6u;
                        rect_verts[b+0u] = RectVertexStorage(vx0, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+1u] = RectVertexStorage(vx1, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+2u] = RectVertexStorage(vx0, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+3u] = RectVertexStorage(vx1, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+4u] = RectVertexStorage(vx1, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b+5u] = RectVertexStorage(vx0, vy1, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                    } else {
                        let gap = 2.0;
                        let vx0a = cx - gap - 1.0;
                        let vx1a = cx - gap;
                        let vx0b = cx + gap;
                        let vx1b = cx + gap + 1.0;
                        let b1 = ri + si * 6u;
                        rect_verts[b1+0u] = RectVertexStorage(vx0a, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+1u] = RectVertexStorage(vx1a, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+2u] = RectVertexStorage(vx0a, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+3u] = RectVertexStorage(vx1a, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+4u] = RectVertexStorage(vx1a, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b1+5u] = RectVertexStorage(vx0a, vy1, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                        let b2 = ri + si * 6u;
                        rect_verts[b2+0u] = RectVertexStorage(vx0b, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+1u] = RectVertexStorage(vx1b, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+2u] = RectVertexStorage(vx0b, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+3u] = RectVertexStorage(vx1b, vy0, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+4u] = RectVertexStorage(vx1b, vy1, fg_r, fg_g, fg_b, fg_a);
                        rect_verts[b2+5u] = RectVertexStorage(vx0b, vy1, fg_r, fg_g, fg_b, fg_a);
                        si += 1u;
                    }
                }
            }
            // Shade (type 3) not yet implemented — skip for now
            continue;
        }

        let base_idx = atomicAdd(&counters[0], 6u);
        if (base_idx + 6u > params.max_text_vertices) {
            break; // safety cap
        }

        let stretch_y = (g.upem & 0x80000000u) != 0u;
        let upem = f32(g.upem & 0x7FFFFFFFu);
        let font_size = params.font_size;
        let em_per_pos = upem / font_size;

        // Pixel-space glyph extents
        let ext_min_x_px = g.ext_min_x / upem * font_size;
        let ext_min_y_px = g.ext_min_y / upem * font_size;
        let ext_max_x_px = g.ext_max_x / upem * font_size;
        let ext_max_y_px = g.ext_max_y / upem * font_size;

        // Pen position: cell origin + glyph offsets from HarfBuzz
        let pen_x = base_x + g.x_offset;
        let pen_y_base = base_y + params.font_ascender + g.y_offset;

        let x0 = pen_x + ext_min_x_px;
        var y0 = pen_y_base - ext_max_y_px; // flip Y
        let x1 = pen_x + ext_max_x_px;
        var y1 = pen_y_base - ext_min_y_px;

        // Texcoords in design units (glyph bounding box)
        let tc_x0 = g.ext_min_x;
        let tc_y0 = g.ext_min_y;
        let tc_x1 = g.ext_max_x;
        let tc_y1 = g.ext_max_y;

        if (stretch_y) {
            y0 = base_y;
            y1 = base_y + params.cell_height;
        }

        let ao = g.atlas_offset;

        // 6 vertices: TL, TR, BL, TR, BR, BL
        text_verts[base_idx + 0u] = SlugVertexStorage(x0, y0, tc_x0, tc_y1, -1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 1u] = SlugVertexStorage(x1, y0, tc_x1, tc_y1,  1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 2u] = SlugVertexStorage(x0, y1, tc_x0, tc_y0, -1.0,  1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 3u] = SlugVertexStorage(x1, y0, tc_x1, tc_y1,  1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 4u] = SlugVertexStorage(x1, y1, tc_x1, tc_y0,  1.0,  1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 5u] = SlugVertexStorage(x0, y1, tc_x0, tc_y0, -1.0,  1.0, em_per_pos, ao, tint);
    }

    // Underline rendering
    let ul_style = cell.underline_info & 0x07u;
    if (ul_style != 0u) {
        let ul_color_packed = cell.underline_info >> 8u;
        var ur: f32; var ug: f32; var ub: f32; var ua: f32;
        if (ul_color_packed != 0u) {
            ur = f32(ul_color_packed & 0xFFu) / 255.0;
            ug = f32((ul_color_packed >> 8u) & 0xFFu) / 255.0;
            ub = f32((ul_color_packed >> 16u) & 0xFFu) / 255.0;
            ua = 1.0;
        } else {
            // Use foreground color
            ur = f32(cell.fg_color & 0xFFu) / 255.0;
            ug = f32((cell.fg_color >> 8u) & 0xFFu) / 255.0;
            ub = f32((cell.fg_color >> 16u) & 0xFFu) / 255.0;
            ua = f32((cell.fg_color >> 24u) & 0xFFu) / 255.0;
        }

        let ul_t = 1.5; // underline thickness
        let ul_y = base_y + params.cell_height - 2.0; // position near bottom

        if (ul_style == 1u) {
            // Straight underline
            let base_idx = atomicAdd(&counters[4], 6u);
            rect_verts[base_idx + 0u] = RectVertexStorage(base_x, ul_y, ur, ug, ub, ua);
            rect_verts[base_idx + 1u] = RectVertexStorage(base_x + params.cell_width, ul_y, ur, ug, ub, ua);
            rect_verts[base_idx + 2u] = RectVertexStorage(base_x, ul_y + ul_t, ur, ug, ub, ua);
            rect_verts[base_idx + 3u] = RectVertexStorage(base_x + params.cell_width, ul_y, ur, ug, ub, ua);
            rect_verts[base_idx + 4u] = RectVertexStorage(base_x + params.cell_width, ul_y + ul_t, ur, ug, ub, ua);
            rect_verts[base_idx + 5u] = RectVertexStorage(base_x, ul_y + ul_t, ur, ug, ub, ua);
        } else if (ul_style == 2u) {
            // Double underline: two thin lines
            let gap = 2.0;
            let base_idx = atomicAdd(&counters[4], 12u);
            // Top line
            rect_verts[base_idx + 0u] = RectVertexStorage(base_x, ul_y - gap, ur, ug, ub, ua);
            rect_verts[base_idx + 1u] = RectVertexStorage(base_x + params.cell_width, ul_y - gap, ur, ug, ub, ua);
            rect_verts[base_idx + 2u] = RectVertexStorage(base_x, ul_y - gap + 1.0, ur, ug, ub, ua);
            rect_verts[base_idx + 3u] = RectVertexStorage(base_x + params.cell_width, ul_y - gap, ur, ug, ub, ua);
            rect_verts[base_idx + 4u] = RectVertexStorage(base_x + params.cell_width, ul_y - gap + 1.0, ur, ug, ub, ua);
            rect_verts[base_idx + 5u] = RectVertexStorage(base_x, ul_y - gap + 1.0, ur, ug, ub, ua);
            // Bottom line
            rect_verts[base_idx + 6u] = RectVertexStorage(base_x, ul_y + 1.0, ur, ug, ub, ua);
            rect_verts[base_idx + 7u] = RectVertexStorage(base_x + params.cell_width, ul_y + 1.0, ur, ug, ub, ua);
            rect_verts[base_idx + 8u] = RectVertexStorage(base_x, ul_y + 2.0, ur, ug, ub, ua);
            rect_verts[base_idx + 9u] = RectVertexStorage(base_x + params.cell_width, ul_y + 1.0, ur, ug, ub, ua);
            rect_verts[base_idx + 10u] = RectVertexStorage(base_x + params.cell_width, ul_y + 2.0, ur, ug, ub, ua);
            rect_verts[base_idx + 11u] = RectVertexStorage(base_x, ul_y + 2.0, ur, ug, ub, ua);
        } else if (ul_style == 3u) {
            // Curly underline: approximate with 4 small rects in a wave
            let segments = 4u;
            let seg_w = params.cell_width / f32(segments);
            let amplitude = 2.0;
            let base_idx = atomicAdd(&counters[4], segments * 6u);
            for (var s = 0u; s < segments; s++) {
                let sx0 = base_x + f32(s) * seg_w;
                let sx1 = sx0 + seg_w;
                // Alternate up/down
                var sy0: f32; var sy1: f32;
                if (s % 2u == 0u) {
                    sy0 = ul_y - amplitude;
                    sy1 = ul_y;
                } else {
                    sy0 = ul_y;
                    sy1 = ul_y + amplitude;
                }
                let si = base_idx + s * 6u;
                rect_verts[si + 0u] = RectVertexStorage(sx0, sy0, ur, ug, ub, ua);
                rect_verts[si + 1u] = RectVertexStorage(sx1, sy0, ur, ug, ub, ua);
                rect_verts[si + 2u] = RectVertexStorage(sx0, sy1, ur, ug, ub, ua);
                rect_verts[si + 3u] = RectVertexStorage(sx1, sy0, ur, ug, ub, ua);
                rect_verts[si + 4u] = RectVertexStorage(sx1, sy1, ur, ug, ub, ua);
                rect_verts[si + 5u] = RectVertexStorage(sx0, sy1, ur, ug, ub, ua);
            }
        } else {
            // Dotted (style 4): dashed segments
            let segments = 3u;
            let seg_w = params.cell_width / f32(segments * 2u);
            let base_idx = atomicAdd(&counters[4], segments * 6u);
            for (var s = 0u; s < segments; s++) {
                let sx0 = base_x + f32(s * 2u) * seg_w;
                let sx1 = sx0 + seg_w;
                let si = base_idx + s * 6u;
                rect_verts[si + 0u] = RectVertexStorage(sx0, ul_y, ur, ug, ub, ua);
                rect_verts[si + 1u] = RectVertexStorage(sx1, ul_y, ur, ug, ub, ua);
                rect_verts[si + 2u] = RectVertexStorage(sx0, ul_y + ul_t, ur, ug, ub, ua);
                rect_verts[si + 3u] = RectVertexStorage(sx1, ul_y, ur, ug, ub, ua);
                rect_verts[si + 4u] = RectVertexStorage(sx1, ul_y + ul_t, ur, ug, ub, ua);
                rect_verts[si + 5u] = RectVertexStorage(sx0, ul_y + ul_t, ur, ug, ub, ua);
            }
        }
    }
}

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
    cursor_type: u32,   // 0=none 1=solid 2=hollow
    cursor_color: u32,  // packed RGBA8
};

struct ResolvedCellGPU {
    atlas_offset: u32,
    ext_min_x: f32,
    ext_min_y: f32,
    ext_max_x: f32,
    ext_max_y: f32,
    upem: u32,
    fg_color: u32,   // packed RGBA8
    bg_color: u32,   // packed RGBA8 (0 = transparent/default)
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

        if (params.cursor_type == 1u) {
            // Solid cursor: one filled rect
            let base_idx = atomicAdd(&counters[4], 6u);
            rect_verts[base_idx + 0u] = RectVertexStorage(x0, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 1u] = RectVertexStorage(x1, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 2u] = RectVertexStorage(x0, y1, cr, cg, cb, ca);
            rect_verts[base_idx + 3u] = RectVertexStorage(x1, y0, cr, cg, cb, ca);
            rect_verts[base_idx + 4u] = RectVertexStorage(x1, y1, cr, cg, cb, ca);
            rect_verts[base_idx + 5u] = RectVertexStorage(x0, y1, cr, cg, cb, ca);
        } else {
            // Hollow cursor: 4 thin border rects
            let t = 1.5; // border thickness in pixels
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
        }
    }

    // Text glyph quad
    if (cell.atlas_offset != 0u) {
        let base_idx = atomicAdd(&counters[0], 6u);

        let stretch_y = (cell.upem & 0x80000000u) != 0u;
        let upem = f32(cell.upem & 0x7FFFFFFFu);
        let font_size = params.font_size;
        let em_per_pos = upem / font_size;

        // Pixel-space glyph extents
        let ext_min_x_px = cell.ext_min_x / upem * font_size;
        let ext_min_y_px = cell.ext_min_y / upem * font_size;
        let ext_max_x_px = cell.ext_max_x / upem * font_size;
        let ext_max_y_px = cell.ext_max_y / upem * font_size;

        // Pen position: baseX at cell left, baseY at ascender
        let pen_x = base_x;
        let pen_y_base = base_y + params.font_ascender;

        let x0 = pen_x + ext_min_x_px;
        var y0 = pen_y_base - ext_max_y_px; // flip Y
        let x1 = pen_x + ext_max_x_px;
        var y1 = pen_y_base - ext_min_y_px;

        // Stretch glyph vertically to fill cell
        if (stretch_y) {
            y0 = base_y;
            y1 = base_y + params.cell_height;
        }

        // Texcoords in design units
        let tc_x0 = cell.ext_min_x;
        let tc_y0 = cell.ext_min_y;
        let tc_x1 = cell.ext_max_x;
        let tc_y1 = cell.ext_max_y;

        let ao = cell.atlas_offset;
        let tint = cell.fg_color;

        // 6 vertices: TL, TR, BL, TR, BR, BL
        // Normal: corner*2-1 where corner is (0,0)/(1,0)/(0,1)/(1,1)
        text_verts[base_idx + 0u] = SlugVertexStorage(x0, y0, tc_x0, tc_y1, -1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 1u] = SlugVertexStorage(x1, y0, tc_x1, tc_y1,  1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 2u] = SlugVertexStorage(x0, y1, tc_x0, tc_y0, -1.0,  1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 3u] = SlugVertexStorage(x1, y0, tc_x1, tc_y1,  1.0, -1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 4u] = SlugVertexStorage(x1, y1, tc_x1, tc_y0,  1.0,  1.0, em_per_pos, ao, tint);
        text_verts[base_idx + 5u] = SlugVertexStorage(x0, y1, tc_x0, tc_y0, -1.0,  1.0, em_per_pos, ao, tint);
    }
}

#pragma once

#include <dawn/webgpu_cpp.h>
#include <cstddef>
#include <cstdint>

// CPU-resolved cell data uploaded to GPU for the compute shader (20 bytes)
// Glyph rendering data is stored separately in GlyphEntry buffer.
struct ResolvedCell {
    uint32_t glyph_offset;    // index into GlyphEntry buffer
    uint32_t glyph_count;     // number of glyphs for this cell (0 = empty/spacer)
    uint32_t fg_color;        // packed RGBA8
    uint32_t bg_color;        // packed RGBA8 (0 = default/transparent)
    uint32_t underline_info;  // bits 0-2: style (0=none, 1=straight, 2=double, 3=curly, 4=dotted)
                              // bits 8-31: color packed RGB8 (0 = use fg_color)
};
static_assert(sizeof(ResolvedCell) == 20);

// Per-glyph data in a separate storage buffer (32 bytes)
// Multiple glyphs may map to one cell (combining marks, decomposed characters).
// Ligature glyphs appear only in the first cell; subsequent cells have glyph_count=0.
struct GlyphEntry {
    uint32_t atlas_offset;
    float ext_min_x, ext_min_y, ext_max_x, ext_max_y;
    uint32_t upem;
    float x_offset;           // position relative to cell origin (pixels, from HarfBuzz)
    float y_offset;           // position relative to baseline (pixels, from HarfBuzz)
};
static_assert(sizeof(GlyphEntry) == 32);

// Compute shader uniform params (60 bytes)
struct TerminalComputeParams {
    uint32_t cols;
    uint32_t rows;
    float    cell_width;
    float    cell_height;
    float    viewport_w;
    float    viewport_h;
    float    font_ascender;
    float    font_size;
    float    pane_origin_x;
    float    pane_origin_y;
    // Cursor (0=none, 1=solid, 2=hollow)
    uint32_t cursor_col;
    uint32_t cursor_row;
    uint32_t cursor_type;
    uint32_t cursor_color; // packed RGBA8
    uint32_t max_text_vertices;  // safety cap for text vertex emission
};
static_assert(sizeof(TerminalComputeParams) == 60);

// Per-glyph info for the COLRv1 rasterizer compute shader (48 bytes)
struct ColrGlyphInfoGPU {
    uint32_t instr_offset;   // start index into instruction buffer (u32 units)
    uint32_t instr_length;   // total u32s of instructions
    uint32_t output_x;       // x offset in output atlas (pixels)
    uint32_t output_y;       // y offset in output atlas (pixels)
    uint32_t tile_w;         // tile width in pixels
    uint32_t tile_h;         // tile height in pixels
    float    em_width;       // glyph width in em-space
    float    em_height;      // glyph height in em-space
    float    em_origin_x;    // em-space origin x (left edge)
    float    em_origin_y;    // em-space origin y (bottom edge)
    uint32_t _pad0;
    uint32_t _pad1;
};
static_assert(sizeof(ColrGlyphInfoGPU) == 48);

// One set of compute buffers + bind group for a single render call
struct ComputeState {
    wgpu::Buffer    resolvedCellBuffer;
    wgpu::Buffer    glyphBuffer;
    wgpu::Buffer    computeTextVertBuffer;
    wgpu::Buffer    computeRectVertBuffer;
    wgpu::Buffer    indirectBuffer;
    wgpu::Buffer    computeParamsBuffer;
    wgpu::BindGroup bindGroup;
    uint32_t        maxCells  = 0;
    uint32_t        maxGlyphs = 0;
    uint32_t        maxTextVertices = 0;
    size_t          sizeBytes = 0;
};

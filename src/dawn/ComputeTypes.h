#pragma once

#include <dawn/webgpu_cpp.h>
#include <cstddef>
#include <cstdint>

// CPU-resolved cell data uploaded to GPU for the compute shader (32 bytes)
struct ResolvedCell {
    uint32_t atlas_offset;
    float ext_min_x, ext_min_y, ext_max_x, ext_max_y;
    uint32_t upem;
    uint32_t fg_color;   // packed RGBA8
    uint32_t bg_color;   // packed RGBA8 (0 = default/transparent)
};
static_assert(sizeof(ResolvedCell) == 32);

// Compute shader uniform params (56 bytes)
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
};
static_assert(sizeof(TerminalComputeParams) == 56);

// One set of compute buffers + bind group for a single render call
struct ComputeState {
    wgpu::Buffer    resolvedCellBuffer;
    wgpu::Buffer    computeTextVertBuffer;
    wgpu::Buffer    computeRectVertBuffer;
    wgpu::Buffer    indirectBuffer;
    wgpu::Buffer    computeParamsBuffer;
    wgpu::BindGroup bindGroup;
    uint32_t        maxCells  = 0;
    size_t          sizeBytes = 0;
};

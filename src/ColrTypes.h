#pragma once

#include <cstdint>
#include <vector>

// Opcodes for the COLRv1 instruction buffer.
// Each instruction is a header u32 followed by payload u32s.
// Header: bits 0-7 = opcode, bits 8-17 = payload length (u32 count).
// Unknown opcodes can be skipped by reading the payload length.
namespace ColrOp {
    constexpr uint8_t PushTransform    = 0x01;  // [xx, yx, xy, yy, dx, dy] (6 floats)
    constexpr uint8_t PopTransform     = 0x02;  // []
    constexpr uint8_t PushClipGlyph    = 0x03;  // [atlas_offset, ext_min_x, ext_min_y, ext_max_x, ext_max_y] (5 words)
    constexpr uint8_t PushClipRect     = 0x04;  // [x_min, y_min, x_max, y_max] (4 floats)
    constexpr uint8_t PopClip          = 0x05;  // []
    constexpr uint8_t FillSolid        = 0x06;  // [rgba] (1 word, packed RGBA8)
    constexpr uint8_t FillLinearGrad   = 0x07;  // [x0,y0, x1,y1, x2,y2, stop_offset, stop_count, extend] (9 words)
    constexpr uint8_t FillRadialGrad   = 0x08;  // [cx0,cy0,r0, cx1,cy1,r1, stop_offset, stop_count, extend] (9 words)
    constexpr uint8_t FillSweepGrad    = 0x09;  // [cx,cy, start_angle, end_angle, stop_offset, stop_count, extend] (7 words)
    constexpr uint8_t PushGroup        = 0x0A;  // []
    constexpr uint8_t PopGroup         = 0x0B;  // [composite_mode] (1 word)
}

// GPU-side color stop: offset + packed RGBA8 (8 bytes)
struct ColrColorStop {
    float offset;
    uint32_t color;  // packed RGBA8 (R in low byte)
};

// Result of encoding a single COLRv1 glyph's paint graph
struct ColrGlyphData {
    std::vector<uint32_t> instructions;
    std::vector<ColrColorStop> colorStops;
};

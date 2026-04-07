#pragma once

#include <cstdint>

// Procedural glyph table: box drawing, block elements, braille, sextants,
// octants, powerline symbols, and wedge triangles.
//
// Multiple non-contiguous Unicode ranges are concatenated into one flat array.
// Use codepointToTableIdx() to map a codepoint to a table index.
//
// Each uint32_t encodes the geometry for one character.
//
// Encoding (4-bit type field):
//   bits 0-3:   type
//     0 = none
//     1 = rect         axis-aligned rectangle in cell-eighths
//     2 = quadrant     quarter-cell fill (4-bit mask)
//     3 = shade        uniform fill at fractional density
//     4 = box_line     centered lines connecting edges
//     5 = braille      8-dot pattern in 2x4 grid
//     6 = sextant      2x3 grid fill (6-bit mask)
//     7 = octant       2x4 grid fill (8-bit mask)
//     8 = powerline    triangle/chevron shapes (shape ID)
//     9 = wedge        wedge/smooth triangle (shape ID)
//    10 = slug         rendered via Slug/bezier text pipeline (shape ID)
//
//   type 1 (rect):
//     bits 4-7:   x0 (0-8)
//     bits 8-11:  x1 (0-8)
//     bits 12-15: y0 (0-8)
//     bits 16-19: y1 (0-8)
//
//   type 2 (quadrant):
//     bits 4-7:   mask (bit0=UL, bit1=UR, bit2=LL, bit3=LR)
//
//   type 3 (shade):
//     bits 4-5:   level (1=light/25%, 2=medium/50%, 3=dark/75%)
//
//   type 4 (box_line):
//     bits 4-5:   left  (0=none, 1=light, 2=heavy, 3=double)
//     bits 6-7:   right
//     bits 8-9:   up
//     bits 10-11: down
//     bits 12-15: style (0=solid, 1=dash2, 2=dash3, 3=dash4, 4=round, 5=diag)
//
//   type 5 (braille):
//     bits 4-11:  8-bit dot mask (standard braille bit order)
//
//   type 6 (sextant):
//     bits 4-9:   6-bit mask (2x3 grid: bit0=UL, bit1=UR, bit2=ML, bit3=MR, bit4=LL, bit5=LR)
//
//   type 7 (octant):
//     bits 4-11:  8-bit mask (2x4 grid: bits 0-7 top-to-bottom, left-to-right)
//
//   type 8 (powerline):
//     bits 4-9:   shape ID (0-36 for E0B0-E0D4)
//
//   type 9 (wedge):
//     bits 4-9:   shape ID (0-51 for U+1FB3C-U+1FB6F)
//
//   type 10 (slug):
//     bits 4-7:   shape ID (0-3 for the 4 semi-circles)

namespace ProceduralGlyph {

// --- Macros for building table entries ---

#define PG_NONE  0u
#define PG_RECT(x0, x1, y0, y1)  (1u | ((x0)<<4) | ((x1)<<8) | ((y0)<<12) | ((y1)<<16))
#define PG_QUAD(mask)             (2u | ((mask)<<4))
#define PG_SHADE(level)           (3u | ((level)<<4))
#define PG_LINE(l, r, u, d)      (4u | ((l)<<4) | ((r)<<6) | ((u)<<8) | ((d)<<10))
#define PG_LINED(l, r, u, d, s)  (4u | ((l)<<4) | ((r)<<6) | ((u)<<8) | ((d)<<10) | ((s)<<12))
#define PG_BRAILLE(mask)          (5u | ((mask)<<4))
#define PG_SEXTANT(mask)          (6u | ((mask)<<4))
#define PG_OCTANT(mask)           (7u | ((mask)<<4))
#define PG_POWERLINE(id)          (8u | ((id)<<4))
#define PG_WEDGE(id)              (9u | ((id)<<4))
#define PG_SLUG(id)               (10u | ((id)<<4))

// Line weights
#define L 1u  // light
#define H 2u  // heavy
#define D 3u  // double

// Dash styles
#define DASH2 1u
#define DASH3 2u
#define DASH4 3u
#define ROUND 4u
#define DIAG  5u

// --- Range definitions ---

inline constexpr uint32_t kBoxDrawingBase = 0x2500;
inline constexpr uint32_t kBoxDrawingSize = 160;  // U+2500-U+259F

inline constexpr uint32_t kBrailleBase = 0x2800;
inline constexpr uint32_t kBrailleSize = 256;     // U+2800-U+28FF

inline constexpr uint32_t kSextantBase = 0x1FB00;
inline constexpr uint32_t kSextantSize = 60;      // U+1FB00-U+1FB3B

inline constexpr uint32_t kWedgeBase = 0x1FB3C;
inline constexpr uint32_t kWedgeSize = 52;         // U+1FB3C-U+1FB6F

inline constexpr uint32_t kOctantBase = 0x1CD00;
inline constexpr uint32_t kOctantSize = 255;       // U+1CD00-U+1CDFE

inline constexpr uint32_t kPowerlineBase = 0xE0B0;
inline constexpr uint32_t kPowerlineSize = 37;     // U+E0B0-U+E0D4

// Offsets into the concatenated table
inline constexpr uint32_t kBrailleOffset  = kBoxDrawingSize;
inline constexpr uint32_t kSextantOffset  = kBrailleOffset + kBrailleSize;
inline constexpr uint32_t kWedgeOffset    = kSextantOffset + kSextantSize;
inline constexpr uint32_t kOctantOffset   = kWedgeOffset + kWedgeSize;
inline constexpr uint32_t kPowerlineOffset = kOctantOffset + kOctantSize;

inline constexpr uint32_t kTableSize = kPowerlineOffset + kPowerlineSize;

// --- Codepoint to table index lookup ---

inline constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

inline constexpr uint32_t codepointToTableIdx(uint32_t cp) {
    if (cp >= kBoxDrawingBase && cp < kBoxDrawingBase + kBoxDrawingSize)
        return cp - kBoxDrawingBase;
    if (cp >= kBrailleBase && cp < kBrailleBase + kBrailleSize)
        return kBrailleOffset + (cp - kBrailleBase);
    if (cp >= kSextantBase && cp < kSextantBase + kSextantSize)
        return kSextantOffset + (cp - kSextantBase);
    if (cp >= kWedgeBase && cp < kWedgeBase + kWedgeSize)
        return kWedgeOffset + (cp - kWedgeBase);
    if (cp >= kOctantBase && cp < kOctantBase + kOctantSize)
        return kOctantOffset + (cp - kOctantBase);
    if (cp >= kPowerlineBase && cp < kPowerlineBase + kPowerlineSize)
        return kPowerlineOffset + (cp - kPowerlineBase);
    return kInvalidIndex;
}

// --- Sextant mask helper ---
// Sextant codepoints skip masks 0 (empty), 21 (left half), 42 (right half), 63 (full).
// Grid: bit0=UL, bit1=UR, bit2=ML, bit3=MR, bit4=LL, bit5=LR

inline constexpr uint32_t sextantMask(uint32_t offset) {
    uint32_t mask = offset + 1;
    if (mask >= 21) mask++;   // skip 21 (left half = 0b010101)
    if (mask >= 42) mask++;   // skip 42 (right half = 0b101010)
    // skip 63 not needed since we only have 60 entries (max mask = 62)
    return mask;
}

// --- The table ---

inline constexpr uint32_t kTable[kTableSize] = {

    // ===================================================================
    // Box Drawing Characters (U+2500-U+257F) + Block Elements (U+2580-U+259F)
    // 160 entries, indexed by (codepoint - 0x2500)
    // ===================================================================

    // U+2500  ─  LIGHT HORIZONTAL
    PG_LINE(L, L, 0, 0),
    // U+2501  ━  HEAVY HORIZONTAL
    PG_LINE(H, H, 0, 0),
    // U+2502  │  LIGHT VERTICAL
    PG_LINE(0, 0, L, L),
    // U+2503  ┃  HEAVY VERTICAL
    PG_LINE(0, 0, H, H),

    // U+2504  ┄  LIGHT TRIPLE DASH HORIZONTAL
    PG_LINED(L, L, 0, 0, DASH3),
    // U+2505  ┅  HEAVY TRIPLE DASH HORIZONTAL
    PG_LINED(H, H, 0, 0, DASH3),
    // U+2506  ┆  LIGHT TRIPLE DASH VERTICAL
    PG_LINED(0, 0, L, L, DASH3),
    // U+2507  ┇  HEAVY TRIPLE DASH VERTICAL
    PG_LINED(0, 0, H, H, DASH3),

    // U+2508  ┈  LIGHT QUADRUPLE DASH HORIZONTAL
    PG_LINED(L, L, 0, 0, DASH4),
    // U+2509  ┉  HEAVY QUADRUPLE DASH HORIZONTAL
    PG_LINED(H, H, 0, 0, DASH4),
    // U+250A  ┊  LIGHT QUADRUPLE DASH VERTICAL
    PG_LINED(0, 0, L, L, DASH4),
    // U+250B  ┋  HEAVY QUADRUPLE DASH VERTICAL
    PG_LINED(0, 0, H, H, DASH4),

    // U+250C  ┌  LIGHT DOWN AND RIGHT
    PG_LINE(0, L, 0, L),
    // U+250D  ┍  DOWN LIGHT AND RIGHT HEAVY
    PG_LINE(0, H, 0, L),
    // U+250E  ┎  DOWN HEAVY AND RIGHT LIGHT
    PG_LINE(0, L, 0, H),
    // U+250F  ┏  HEAVY DOWN AND RIGHT
    PG_LINE(0, H, 0, H),

    // U+2510  ┐  LIGHT DOWN AND LEFT
    PG_LINE(L, 0, 0, L),
    // U+2511  ┑  DOWN LIGHT AND LEFT HEAVY
    PG_LINE(H, 0, 0, L),
    // U+2512  ┒  DOWN HEAVY AND LEFT LIGHT
    PG_LINE(L, 0, 0, H),
    // U+2513  ┓  HEAVY DOWN AND LEFT
    PG_LINE(H, 0, 0, H),

    // U+2514  └  LIGHT UP AND RIGHT
    PG_LINE(0, L, L, 0),
    // U+2515  ┕  UP LIGHT AND RIGHT HEAVY
    PG_LINE(0, H, L, 0),
    // U+2516  ┖  UP HEAVY AND RIGHT LIGHT
    PG_LINE(0, L, H, 0),
    // U+2517  ┗  HEAVY UP AND RIGHT
    PG_LINE(0, H, H, 0),

    // U+2518  ┘  LIGHT UP AND LEFT
    PG_LINE(L, 0, L, 0),
    // U+2519  ┙  UP LIGHT AND LEFT HEAVY
    PG_LINE(H, 0, L, 0),
    // U+251A  ┚  UP HEAVY AND LEFT LIGHT
    PG_LINE(L, 0, H, 0),
    // U+251B  ┛  HEAVY UP AND LEFT
    PG_LINE(H, 0, H, 0),

    // U+251C  ├  LIGHT VERTICAL AND RIGHT
    PG_LINE(0, L, L, L),
    // U+251D  ┝  VERTICAL LIGHT AND RIGHT HEAVY
    PG_LINE(0, H, L, L),
    // U+251E  ┞  UP HEAVY AND RIGHT DOWN LIGHT
    PG_LINE(0, L, H, L),
    // U+251F  ┟  DOWN HEAVY AND RIGHT UP LIGHT
    PG_LINE(0, L, L, H),

    // U+2520  ┠  VERTICAL HEAVY AND RIGHT LIGHT
    PG_LINE(0, L, H, H),
    // U+2521  ┡  DOWN LIGHT AND RIGHT UP HEAVY
    PG_LINE(0, H, H, L),
    // U+2522  ┢  UP LIGHT AND RIGHT DOWN HEAVY
    PG_LINE(0, H, L, H),
    // U+2523  ┣  HEAVY VERTICAL AND RIGHT
    PG_LINE(0, H, H, H),

    // U+2524  ┤  LIGHT VERTICAL AND LEFT
    PG_LINE(L, 0, L, L),
    // U+2525  ┥  VERTICAL LIGHT AND LEFT HEAVY
    PG_LINE(H, 0, L, L),
    // U+2526  ┦  UP HEAVY AND LEFT DOWN LIGHT
    PG_LINE(L, 0, H, L),
    // U+2527  ┧  DOWN HEAVY AND LEFT UP LIGHT
    PG_LINE(L, 0, L, H),

    // U+2528  ┨  VERTICAL HEAVY AND LEFT LIGHT
    PG_LINE(L, 0, H, H),
    // U+2529  ┩  DOWN LIGHT AND LEFT UP HEAVY
    PG_LINE(H, 0, H, L),
    // U+252A  ┪  UP LIGHT AND LEFT DOWN HEAVY
    PG_LINE(H, 0, L, H),
    // U+252B  ┫  HEAVY VERTICAL AND LEFT
    PG_LINE(H, 0, H, H),

    // U+252C  ┬  LIGHT DOWN AND HORIZONTAL
    PG_LINE(L, L, 0, L),
    // U+252D  ┭  LEFT HEAVY AND RIGHT DOWN LIGHT
    PG_LINE(H, L, 0, L),
    // U+252E  ┮  RIGHT HEAVY AND LEFT DOWN LIGHT
    PG_LINE(L, H, 0, L),
    // U+252F  ┯  DOWN LIGHT AND HORIZONTAL HEAVY
    PG_LINE(H, H, 0, L),

    // U+2530  ┰  DOWN HEAVY AND HORIZONTAL LIGHT
    PG_LINE(L, L, 0, H),
    // U+2531  ┱  RIGHT LIGHT AND LEFT DOWN HEAVY
    PG_LINE(H, L, 0, H),
    // U+2532  ┲  LEFT LIGHT AND RIGHT DOWN HEAVY
    PG_LINE(L, H, 0, H),
    // U+2533  ┳  HEAVY DOWN AND HORIZONTAL
    PG_LINE(H, H, 0, H),

    // U+2534  ┴  LIGHT UP AND HORIZONTAL
    PG_LINE(L, L, L, 0),
    // U+2535  ┵  LEFT HEAVY AND RIGHT UP LIGHT
    PG_LINE(H, L, L, 0),
    // U+2536  ┶  RIGHT HEAVY AND LEFT UP LIGHT
    PG_LINE(L, H, L, 0),
    // U+2537  ┷  UP LIGHT AND HORIZONTAL HEAVY
    PG_LINE(H, H, L, 0),

    // U+2538  ┸  UP HEAVY AND HORIZONTAL LIGHT
    PG_LINE(L, L, H, 0),
    // U+2539  ┹  RIGHT LIGHT AND LEFT UP HEAVY
    PG_LINE(H, L, H, 0),
    // U+253A  ┺  LEFT LIGHT AND RIGHT UP HEAVY
    PG_LINE(L, H, H, 0),
    // U+253B  ┻  HEAVY UP AND HORIZONTAL
    PG_LINE(H, H, H, 0),

    // U+253C  ┼  LIGHT VERTICAL AND HORIZONTAL
    PG_LINE(L, L, L, L),
    // U+253D  ┽  LEFT HEAVY AND RIGHT VERTICAL LIGHT
    PG_LINE(H, L, L, L),
    // U+253E  ┾  RIGHT HEAVY AND LEFT VERTICAL LIGHT
    PG_LINE(L, H, L, L),
    // U+253F  ┿  VERTICAL LIGHT AND HORIZONTAL HEAVY
    PG_LINE(H, H, L, L),

    // U+2540  ╀  UP HEAVY AND DOWN HORIZONTAL LIGHT
    PG_LINE(L, L, H, L),
    // U+2541  ╁  DOWN HEAVY AND UP HORIZONTAL LIGHT
    PG_LINE(L, L, L, H),
    // U+2542  ╂  VERTICAL HEAVY AND HORIZONTAL LIGHT
    PG_LINE(L, L, H, H),
    // U+2543  ╃  LEFT UP HEAVY AND RIGHT DOWN LIGHT
    PG_LINE(H, L, H, L),

    // U+2544  ╄  RIGHT UP HEAVY AND LEFT DOWN LIGHT
    PG_LINE(L, H, H, L),
    // U+2545  ╅  LEFT DOWN HEAVY AND RIGHT UP LIGHT
    PG_LINE(H, L, L, H),
    // U+2546  ╆  RIGHT DOWN HEAVY AND LEFT UP LIGHT
    PG_LINE(L, H, L, H),
    // U+2547  ╇  DOWN LIGHT AND UP HORIZONTAL HEAVY
    PG_LINE(H, H, H, L),

    // U+2548  ╈  UP LIGHT AND DOWN HORIZONTAL HEAVY
    PG_LINE(H, H, L, H),
    // U+2549  ╉  RIGHT LIGHT AND LEFT VERTICAL HEAVY
    PG_LINE(H, L, H, H),
    // U+254A  ╊  LEFT LIGHT AND RIGHT VERTICAL HEAVY
    PG_LINE(L, H, H, H),
    // U+254B  ╋  HEAVY VERTICAL AND HORIZONTAL
    PG_LINE(H, H, H, H),

    // U+254C  ╌  LIGHT DOUBLE DASH HORIZONTAL
    PG_LINED(L, L, 0, 0, DASH2),
    // U+254D  ╍  HEAVY DOUBLE DASH HORIZONTAL
    PG_LINED(H, H, 0, 0, DASH2),
    // U+254E  ╎  LIGHT DOUBLE DASH VERTICAL
    PG_LINED(0, 0, L, L, DASH2),
    // U+254F  ╏  HEAVY DOUBLE DASH VERTICAL
    PG_LINED(0, 0, H, H, DASH2),

    // U+2550  ═  DOUBLE HORIZONTAL
    PG_LINE(D, D, 0, 0),
    // U+2551  ║  DOUBLE VERTICAL
    PG_LINE(0, 0, D, D),

    // U+2552  ╒  DOWN SINGLE AND RIGHT DOUBLE
    PG_LINE(0, D, 0, L),
    // U+2553  ╓  DOWN DOUBLE AND RIGHT SINGLE
    PG_LINE(0, L, 0, D),
    // U+2554  ╔  DOUBLE DOWN AND RIGHT
    PG_LINE(0, D, 0, D),

    // U+2555  ╕  DOWN SINGLE AND LEFT DOUBLE
    PG_LINE(D, 0, 0, L),
    // U+2556  ╖  DOWN DOUBLE AND LEFT SINGLE
    PG_LINE(L, 0, 0, D),
    // U+2557  ╗  DOUBLE DOWN AND LEFT
    PG_LINE(D, 0, 0, D),

    // U+2558  ╘  UP SINGLE AND RIGHT DOUBLE
    PG_LINE(0, D, L, 0),
    // U+2559  ╙  UP DOUBLE AND RIGHT SINGLE
    PG_LINE(0, L, D, 0),
    // U+255A  ╚  DOUBLE UP AND RIGHT
    PG_LINE(0, D, D, 0),

    // U+255B  ╛  UP SINGLE AND LEFT DOUBLE
    PG_LINE(D, 0, L, 0),
    // U+255C  ╜  UP DOUBLE AND LEFT SINGLE
    PG_LINE(L, 0, D, 0),
    // U+255D  ╝  DOUBLE UP AND LEFT
    PG_LINE(D, 0, D, 0),

    // U+255E  ╞  VERTICAL SINGLE AND RIGHT DOUBLE
    PG_LINE(0, D, L, L),
    // U+255F  ╟  VERTICAL DOUBLE AND RIGHT SINGLE
    PG_LINE(0, L, D, D),

    // U+2560  ╠  DOUBLE VERTICAL AND RIGHT
    PG_LINE(0, D, D, D),
    // U+2561  ╡  VERTICAL SINGLE AND LEFT DOUBLE
    PG_LINE(D, 0, L, L),
    // U+2562  ╢  VERTICAL DOUBLE AND LEFT SINGLE
    PG_LINE(L, 0, D, D),
    // U+2563  ╣  DOUBLE VERTICAL AND LEFT
    PG_LINE(D, 0, D, D),

    // U+2564  ╤  DOWN SINGLE AND HORIZONTAL DOUBLE
    PG_LINE(D, D, 0, L),
    // U+2565  ╥  DOWN DOUBLE AND HORIZONTAL SINGLE
    PG_LINE(L, L, 0, D),
    // U+2566  ╦  DOUBLE DOWN AND HORIZONTAL
    PG_LINE(D, D, 0, D),

    // U+2567  ╧  UP SINGLE AND HORIZONTAL DOUBLE
    PG_LINE(D, D, L, 0),
    // U+2568  ╨  UP DOUBLE AND HORIZONTAL SINGLE
    PG_LINE(L, L, D, 0),
    // U+2569  ╩  DOUBLE UP AND HORIZONTAL
    PG_LINE(D, D, D, 0),

    // U+256A  ╪  VERTICAL SINGLE AND HORIZONTAL DOUBLE
    PG_LINE(D, D, L, L),
    // U+256B  ╫  VERTICAL DOUBLE AND HORIZONTAL SINGLE
    PG_LINE(L, L, D, D),
    // U+256C  ╬  DOUBLE VERTICAL AND HORIZONTAL
    PG_LINE(D, D, D, D),

    // U+256D  ╭  LIGHT ARC DOWN AND RIGHT
    PG_LINED(0, L, 0, L, ROUND),
    // U+256E  ╮  LIGHT ARC DOWN AND LEFT
    PG_LINED(L, 0, 0, L, ROUND),
    // U+256F  ╯  LIGHT ARC UP AND LEFT
    PG_LINED(L, 0, L, 0, ROUND),
    // U+2570  ╰  LIGHT ARC UP AND RIGHT
    PG_LINED(0, L, L, 0, ROUND),

    // U+2571  ╱  LIGHT DIAGONAL UPPER RIGHT TO LOWER LEFT
    PG_LINED(0, 0, 0, 0, DIAG),
    // U+2572  ╲  LIGHT DIAGONAL UPPER LEFT TO LOWER RIGHT
    PG_LINED(0, 0, 0, 0, DIAG),
    // U+2573  ╳  LIGHT DIAGONAL CROSS
    PG_LINED(0, 0, 0, 0, DIAG),

    // U+2574  ╴  LIGHT LEFT
    PG_LINE(L, 0, 0, 0),
    // U+2575  ╵  LIGHT UP
    PG_LINE(0, 0, L, 0),
    // U+2576  ╶  LIGHT RIGHT
    PG_LINE(0, L, 0, 0),
    // U+2577  ╷  LIGHT DOWN
    PG_LINE(0, 0, 0, L),

    // U+2578  ╸  HEAVY LEFT
    PG_LINE(H, 0, 0, 0),
    // U+2579  ╹  HEAVY UP
    PG_LINE(0, 0, H, 0),
    // U+257A  ╺  HEAVY RIGHT
    PG_LINE(0, H, 0, 0),
    // U+257B  ╻  HEAVY DOWN
    PG_LINE(0, 0, 0, H),

    // U+257C  ╼  LIGHT LEFT AND HEAVY RIGHT
    PG_LINE(L, H, 0, 0),
    // U+257D  ╽  LIGHT UP AND HEAVY DOWN
    PG_LINE(0, 0, L, H),
    // U+257E  ╾  HEAVY LEFT AND LIGHT RIGHT
    PG_LINE(H, L, 0, 0),
    // U+257F  ╿  HEAVY UP AND LIGHT DOWN
    PG_LINE(0, 0, H, L),

    // === Block Elements (U+2580-U+259F) ===

    // U+2580  ▀  UPPER HALF BLOCK
    PG_RECT(0, 8, 0, 4),
    // U+2581  ▁  LOWER ONE EIGHTH BLOCK
    PG_RECT(0, 8, 7, 8),
    // U+2582  ▂  LOWER ONE QUARTER BLOCK
    PG_RECT(0, 8, 6, 8),
    // U+2583  ▃  LOWER THREE EIGHTHS BLOCK
    PG_RECT(0, 8, 5, 8),
    // U+2584  ▄  LOWER HALF BLOCK
    PG_RECT(0, 8, 4, 8),
    // U+2585  ▅  LOWER FIVE EIGHTHS BLOCK
    PG_RECT(0, 8, 3, 8),
    // U+2586  ▆  LOWER THREE QUARTERS BLOCK
    PG_RECT(0, 8, 2, 8),
    // U+2587  ▇  LOWER SEVEN EIGHTHS BLOCK
    PG_RECT(0, 8, 1, 8),
    // U+2588  █  FULL BLOCK
    PG_RECT(0, 8, 0, 8),
    // U+2589  ▉  LEFT SEVEN EIGHTHS BLOCK
    PG_RECT(0, 7, 0, 8),
    // U+258A  ▊  LEFT THREE QUARTERS BLOCK
    PG_RECT(0, 6, 0, 8),
    // U+258B  ▋  LEFT FIVE EIGHTHS BLOCK
    PG_RECT(0, 5, 0, 8),
    // U+258C  ▌  LEFT HALF BLOCK
    PG_RECT(0, 4, 0, 8),
    // U+258D  ▍  LEFT THREE EIGHTHS BLOCK
    PG_RECT(0, 3, 0, 8),
    // U+258E  ▎  LEFT ONE QUARTER BLOCK
    PG_RECT(0, 2, 0, 8),
    // U+258F  ▏  LEFT ONE EIGHTH BLOCK
    PG_RECT(0, 1, 0, 8),

    // U+2590  ▐  RIGHT HALF BLOCK
    PG_RECT(4, 8, 0, 8),
    // U+2591  ░  LIGHT SHADE
    PG_SHADE(1),
    // U+2592  ▒  MEDIUM SHADE
    PG_SHADE(2),
    // U+2593  ▓  DARK SHADE
    PG_SHADE(3),
    // U+2594  ▔  UPPER ONE EIGHTH BLOCK
    PG_RECT(0, 8, 0, 1),
    // U+2595  ▕  RIGHT ONE EIGHTH BLOCK
    PG_RECT(7, 8, 0, 8),

    // U+2596  ▖  QUADRANT LOWER LEFT
    PG_QUAD(0x4),
    // U+2597  ▗  QUADRANT LOWER RIGHT
    PG_QUAD(0x8),
    // U+2598  ▘  QUADRANT UPPER LEFT
    PG_QUAD(0x1),
    // U+2599  ▙  QUADRANT UPPER LEFT AND LOWER LEFT AND LOWER RIGHT
    PG_QUAD(0xD),
    // U+259A  ▚  QUADRANT UPPER LEFT AND LOWER RIGHT
    PG_QUAD(0x9),
    // U+259B  ▛  QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER LEFT
    PG_QUAD(0x7),
    // U+259C  ▜  QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER RIGHT
    PG_QUAD(0xB),
    // U+259D  ▝  QUADRANT UPPER RIGHT
    PG_QUAD(0x2),
    // U+259E  ▞  QUADRANT UPPER RIGHT AND LOWER LEFT
    PG_QUAD(0x6),
    // U+259F  ▟  QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT
    PG_QUAD(0xE),

    // ===================================================================
    // Braille Patterns (U+2800-U+28FF)
    // 256 entries. Dot mask = codepoint & 0xFF.
    // Standard bit order: dot1=bit0(UL), dot2=bit1(ML), dot3=bit2(LL),
    //   dot4=bit3(UR), dot5=bit4(MR), dot6=bit5(LR), dot7=bit6(BL), dot8=bit7(BR)
    // ===================================================================

    PG_BRAILLE(0x00), PG_BRAILLE(0x01), PG_BRAILLE(0x02), PG_BRAILLE(0x03),
    PG_BRAILLE(0x04), PG_BRAILLE(0x05), PG_BRAILLE(0x06), PG_BRAILLE(0x07),
    PG_BRAILLE(0x08), PG_BRAILLE(0x09), PG_BRAILLE(0x0A), PG_BRAILLE(0x0B),
    PG_BRAILLE(0x0C), PG_BRAILLE(0x0D), PG_BRAILLE(0x0E), PG_BRAILLE(0x0F),
    PG_BRAILLE(0x10), PG_BRAILLE(0x11), PG_BRAILLE(0x12), PG_BRAILLE(0x13),
    PG_BRAILLE(0x14), PG_BRAILLE(0x15), PG_BRAILLE(0x16), PG_BRAILLE(0x17),
    PG_BRAILLE(0x18), PG_BRAILLE(0x19), PG_BRAILLE(0x1A), PG_BRAILLE(0x1B),
    PG_BRAILLE(0x1C), PG_BRAILLE(0x1D), PG_BRAILLE(0x1E), PG_BRAILLE(0x1F),
    PG_BRAILLE(0x20), PG_BRAILLE(0x21), PG_BRAILLE(0x22), PG_BRAILLE(0x23),
    PG_BRAILLE(0x24), PG_BRAILLE(0x25), PG_BRAILLE(0x26), PG_BRAILLE(0x27),
    PG_BRAILLE(0x28), PG_BRAILLE(0x29), PG_BRAILLE(0x2A), PG_BRAILLE(0x2B),
    PG_BRAILLE(0x2C), PG_BRAILLE(0x2D), PG_BRAILLE(0x2E), PG_BRAILLE(0x2F),
    PG_BRAILLE(0x30), PG_BRAILLE(0x31), PG_BRAILLE(0x32), PG_BRAILLE(0x33),
    PG_BRAILLE(0x34), PG_BRAILLE(0x35), PG_BRAILLE(0x36), PG_BRAILLE(0x37),
    PG_BRAILLE(0x38), PG_BRAILLE(0x39), PG_BRAILLE(0x3A), PG_BRAILLE(0x3B),
    PG_BRAILLE(0x3C), PG_BRAILLE(0x3D), PG_BRAILLE(0x3E), PG_BRAILLE(0x3F),
    PG_BRAILLE(0x40), PG_BRAILLE(0x41), PG_BRAILLE(0x42), PG_BRAILLE(0x43),
    PG_BRAILLE(0x44), PG_BRAILLE(0x45), PG_BRAILLE(0x46), PG_BRAILLE(0x47),
    PG_BRAILLE(0x48), PG_BRAILLE(0x49), PG_BRAILLE(0x4A), PG_BRAILLE(0x4B),
    PG_BRAILLE(0x4C), PG_BRAILLE(0x4D), PG_BRAILLE(0x4E), PG_BRAILLE(0x4F),
    PG_BRAILLE(0x50), PG_BRAILLE(0x51), PG_BRAILLE(0x52), PG_BRAILLE(0x53),
    PG_BRAILLE(0x54), PG_BRAILLE(0x55), PG_BRAILLE(0x56), PG_BRAILLE(0x57),
    PG_BRAILLE(0x58), PG_BRAILLE(0x59), PG_BRAILLE(0x5A), PG_BRAILLE(0x5B),
    PG_BRAILLE(0x5C), PG_BRAILLE(0x5D), PG_BRAILLE(0x5E), PG_BRAILLE(0x5F),
    PG_BRAILLE(0x60), PG_BRAILLE(0x61), PG_BRAILLE(0x62), PG_BRAILLE(0x63),
    PG_BRAILLE(0x64), PG_BRAILLE(0x65), PG_BRAILLE(0x66), PG_BRAILLE(0x67),
    PG_BRAILLE(0x68), PG_BRAILLE(0x69), PG_BRAILLE(0x6A), PG_BRAILLE(0x6B),
    PG_BRAILLE(0x6C), PG_BRAILLE(0x6D), PG_BRAILLE(0x6E), PG_BRAILLE(0x6F),
    PG_BRAILLE(0x70), PG_BRAILLE(0x71), PG_BRAILLE(0x72), PG_BRAILLE(0x73),
    PG_BRAILLE(0x74), PG_BRAILLE(0x75), PG_BRAILLE(0x76), PG_BRAILLE(0x77),
    PG_BRAILLE(0x78), PG_BRAILLE(0x79), PG_BRAILLE(0x7A), PG_BRAILLE(0x7B),
    PG_BRAILLE(0x7C), PG_BRAILLE(0x7D), PG_BRAILLE(0x7E), PG_BRAILLE(0x7F),
    PG_BRAILLE(0x80), PG_BRAILLE(0x81), PG_BRAILLE(0x82), PG_BRAILLE(0x83),
    PG_BRAILLE(0x84), PG_BRAILLE(0x85), PG_BRAILLE(0x86), PG_BRAILLE(0x87),
    PG_BRAILLE(0x88), PG_BRAILLE(0x89), PG_BRAILLE(0x8A), PG_BRAILLE(0x8B),
    PG_BRAILLE(0x8C), PG_BRAILLE(0x8D), PG_BRAILLE(0x8E), PG_BRAILLE(0x8F),
    PG_BRAILLE(0x90), PG_BRAILLE(0x91), PG_BRAILLE(0x92), PG_BRAILLE(0x93),
    PG_BRAILLE(0x94), PG_BRAILLE(0x95), PG_BRAILLE(0x96), PG_BRAILLE(0x97),
    PG_BRAILLE(0x98), PG_BRAILLE(0x99), PG_BRAILLE(0x9A), PG_BRAILLE(0x9B),
    PG_BRAILLE(0x9C), PG_BRAILLE(0x9D), PG_BRAILLE(0x9E), PG_BRAILLE(0x9F),
    PG_BRAILLE(0xA0), PG_BRAILLE(0xA1), PG_BRAILLE(0xA2), PG_BRAILLE(0xA3),
    PG_BRAILLE(0xA4), PG_BRAILLE(0xA5), PG_BRAILLE(0xA6), PG_BRAILLE(0xA7),
    PG_BRAILLE(0xA8), PG_BRAILLE(0xA9), PG_BRAILLE(0xAA), PG_BRAILLE(0xAB),
    PG_BRAILLE(0xAC), PG_BRAILLE(0xAD), PG_BRAILLE(0xAE), PG_BRAILLE(0xAF),
    PG_BRAILLE(0xB0), PG_BRAILLE(0xB1), PG_BRAILLE(0xB2), PG_BRAILLE(0xB3),
    PG_BRAILLE(0xB4), PG_BRAILLE(0xB5), PG_BRAILLE(0xB6), PG_BRAILLE(0xB7),
    PG_BRAILLE(0xB8), PG_BRAILLE(0xB9), PG_BRAILLE(0xBA), PG_BRAILLE(0xBB),
    PG_BRAILLE(0xBC), PG_BRAILLE(0xBD), PG_BRAILLE(0xBE), PG_BRAILLE(0xBF),
    PG_BRAILLE(0xC0), PG_BRAILLE(0xC1), PG_BRAILLE(0xC2), PG_BRAILLE(0xC3),
    PG_BRAILLE(0xC4), PG_BRAILLE(0xC5), PG_BRAILLE(0xC6), PG_BRAILLE(0xC7),
    PG_BRAILLE(0xC8), PG_BRAILLE(0xC9), PG_BRAILLE(0xCA), PG_BRAILLE(0xCB),
    PG_BRAILLE(0xCC), PG_BRAILLE(0xCD), PG_BRAILLE(0xCE), PG_BRAILLE(0xCF),
    PG_BRAILLE(0xD0), PG_BRAILLE(0xD1), PG_BRAILLE(0xD2), PG_BRAILLE(0xD3),
    PG_BRAILLE(0xD4), PG_BRAILLE(0xD5), PG_BRAILLE(0xD6), PG_BRAILLE(0xD7),
    PG_BRAILLE(0xD8), PG_BRAILLE(0xD9), PG_BRAILLE(0xDA), PG_BRAILLE(0xDB),
    PG_BRAILLE(0xDC), PG_BRAILLE(0xDD), PG_BRAILLE(0xDE), PG_BRAILLE(0xDF),
    PG_BRAILLE(0xE0), PG_BRAILLE(0xE1), PG_BRAILLE(0xE2), PG_BRAILLE(0xE3),
    PG_BRAILLE(0xE4), PG_BRAILLE(0xE5), PG_BRAILLE(0xE6), PG_BRAILLE(0xE7),
    PG_BRAILLE(0xE8), PG_BRAILLE(0xE9), PG_BRAILLE(0xEA), PG_BRAILLE(0xEB),
    PG_BRAILLE(0xEC), PG_BRAILLE(0xED), PG_BRAILLE(0xEE), PG_BRAILLE(0xEF),
    PG_BRAILLE(0xF0), PG_BRAILLE(0xF1), PG_BRAILLE(0xF2), PG_BRAILLE(0xF3),
    PG_BRAILLE(0xF4), PG_BRAILLE(0xF5), PG_BRAILLE(0xF6), PG_BRAILLE(0xF7),
    PG_BRAILLE(0xF8), PG_BRAILLE(0xF9), PG_BRAILLE(0xFA), PG_BRAILLE(0xFB),
    PG_BRAILLE(0xFC), PG_BRAILLE(0xFD), PG_BRAILLE(0xFE), PG_BRAILLE(0xFF),

    // ===================================================================
    // Sextants (U+1FB00-U+1FB3B)
    // 60 entries. 2x3 grid, skipping masks 0, 21, 42, 63.
    // Grid: bit0=UL, bit1=UR, bit2=ML, bit3=MR, bit4=LL, bit5=LR
    // ===================================================================

    PG_SEXTANT(sextantMask(0)),  PG_SEXTANT(sextantMask(1)),  PG_SEXTANT(sextantMask(2)),
    PG_SEXTANT(sextantMask(3)),  PG_SEXTANT(sextantMask(4)),  PG_SEXTANT(sextantMask(5)),
    PG_SEXTANT(sextantMask(6)),  PG_SEXTANT(sextantMask(7)),  PG_SEXTANT(sextantMask(8)),
    PG_SEXTANT(sextantMask(9)),  PG_SEXTANT(sextantMask(10)), PG_SEXTANT(sextantMask(11)),
    PG_SEXTANT(sextantMask(12)), PG_SEXTANT(sextantMask(13)), PG_SEXTANT(sextantMask(14)),
    PG_SEXTANT(sextantMask(15)), PG_SEXTANT(sextantMask(16)), PG_SEXTANT(sextantMask(17)),
    PG_SEXTANT(sextantMask(18)), PG_SEXTANT(sextantMask(19)), PG_SEXTANT(sextantMask(20)),
    PG_SEXTANT(sextantMask(21)), PG_SEXTANT(sextantMask(22)), PG_SEXTANT(sextantMask(23)),
    PG_SEXTANT(sextantMask(24)), PG_SEXTANT(sextantMask(25)), PG_SEXTANT(sextantMask(26)),
    PG_SEXTANT(sextantMask(27)), PG_SEXTANT(sextantMask(28)), PG_SEXTANT(sextantMask(29)),
    PG_SEXTANT(sextantMask(30)), PG_SEXTANT(sextantMask(31)), PG_SEXTANT(sextantMask(32)),
    PG_SEXTANT(sextantMask(33)), PG_SEXTANT(sextantMask(34)), PG_SEXTANT(sextantMask(35)),
    PG_SEXTANT(sextantMask(36)), PG_SEXTANT(sextantMask(37)), PG_SEXTANT(sextantMask(38)),
    PG_SEXTANT(sextantMask(39)), PG_SEXTANT(sextantMask(40)), PG_SEXTANT(sextantMask(41)),
    PG_SEXTANT(sextantMask(42)), PG_SEXTANT(sextantMask(43)), PG_SEXTANT(sextantMask(44)),
    PG_SEXTANT(sextantMask(45)), PG_SEXTANT(sextantMask(46)), PG_SEXTANT(sextantMask(47)),
    PG_SEXTANT(sextantMask(48)), PG_SEXTANT(sextantMask(49)), PG_SEXTANT(sextantMask(50)),
    PG_SEXTANT(sextantMask(51)), PG_SEXTANT(sextantMask(52)), PG_SEXTANT(sextantMask(53)),
    PG_SEXTANT(sextantMask(54)), PG_SEXTANT(sextantMask(55)), PG_SEXTANT(sextantMask(56)),
    PG_SEXTANT(sextantMask(57)), PG_SEXTANT(sextantMask(58)), PG_SEXTANT(sextantMask(59)),

    // ===================================================================
    // Wedge / Smooth Mosaic Triangles (U+1FB3C-U+1FB6F)
    // 52 entries. Shape ID = offset from U+1FB3C.
    //
    // Shape IDs:
    //   0-12:  Lower-left triangles at 1/8, 2/8, 3/8, 1/2, 5/8, 3/4, 7/8 heights
    //          + lower-left to upper-right fills
    //   13-25: Lower-right triangles (mirrored)
    //   26-38: Upper-left triangles
    //   39-51: Upper-right triangles
    // ===================================================================

    PG_WEDGE(0),  PG_WEDGE(1),  PG_WEDGE(2),  PG_WEDGE(3),  PG_WEDGE(4),
    PG_WEDGE(5),  PG_WEDGE(6),  PG_WEDGE(7),  PG_WEDGE(8),  PG_WEDGE(9),
    PG_WEDGE(10), PG_WEDGE(11), PG_WEDGE(12), PG_WEDGE(13), PG_WEDGE(14),
    PG_WEDGE(15), PG_WEDGE(16), PG_WEDGE(17), PG_WEDGE(18), PG_WEDGE(19),
    PG_WEDGE(20), PG_WEDGE(21), PG_WEDGE(22), PG_WEDGE(23), PG_WEDGE(24),
    PG_WEDGE(25), PG_WEDGE(26), PG_WEDGE(27), PG_WEDGE(28), PG_WEDGE(29),
    PG_WEDGE(30), PG_WEDGE(31), PG_WEDGE(32), PG_WEDGE(33), PG_WEDGE(34),
    PG_WEDGE(35), PG_WEDGE(36), PG_WEDGE(37), PG_WEDGE(38), PG_WEDGE(39),
    PG_WEDGE(40), PG_WEDGE(41), PG_WEDGE(42), PG_WEDGE(43), PG_WEDGE(44),
    PG_WEDGE(45), PG_WEDGE(46), PG_WEDGE(47), PG_WEDGE(48), PG_WEDGE(49),
    PG_WEDGE(50), PG_WEDGE(51),

    // ===================================================================
    // Octants (U+1CD00-U+1CDFE)
    // 255 entries. 2x4 grid, mask = offset + 1 (skipping empty pattern 0).
    // Grid: bit0=R0L, bit1=R0R, bit2=R1L, bit3=R1R,
    //        bit4=R2L, bit5=R2R, bit6=R3L, bit7=R3R
    // ===================================================================

    PG_OCTANT(1),   PG_OCTANT(2),   PG_OCTANT(3),   PG_OCTANT(4),
    PG_OCTANT(5),   PG_OCTANT(6),   PG_OCTANT(7),   PG_OCTANT(8),
    PG_OCTANT(9),   PG_OCTANT(10),  PG_OCTANT(11),  PG_OCTANT(12),
    PG_OCTANT(13),  PG_OCTANT(14),  PG_OCTANT(15),  PG_OCTANT(16),
    PG_OCTANT(17),  PG_OCTANT(18),  PG_OCTANT(19),  PG_OCTANT(20),
    PG_OCTANT(21),  PG_OCTANT(22),  PG_OCTANT(23),  PG_OCTANT(24),
    PG_OCTANT(25),  PG_OCTANT(26),  PG_OCTANT(27),  PG_OCTANT(28),
    PG_OCTANT(29),  PG_OCTANT(30),  PG_OCTANT(31),  PG_OCTANT(32),
    PG_OCTANT(33),  PG_OCTANT(34),  PG_OCTANT(35),  PG_OCTANT(36),
    PG_OCTANT(37),  PG_OCTANT(38),  PG_OCTANT(39),  PG_OCTANT(40),
    PG_OCTANT(41),  PG_OCTANT(42),  PG_OCTANT(43),  PG_OCTANT(44),
    PG_OCTANT(45),  PG_OCTANT(46),  PG_OCTANT(47),  PG_OCTANT(48),
    PG_OCTANT(49),  PG_OCTANT(50),  PG_OCTANT(51),  PG_OCTANT(52),
    PG_OCTANT(53),  PG_OCTANT(54),  PG_OCTANT(55),  PG_OCTANT(56),
    PG_OCTANT(57),  PG_OCTANT(58),  PG_OCTANT(59),  PG_OCTANT(60),
    PG_OCTANT(61),  PG_OCTANT(62),  PG_OCTANT(63),  PG_OCTANT(64),
    PG_OCTANT(65),  PG_OCTANT(66),  PG_OCTANT(67),  PG_OCTANT(68),
    PG_OCTANT(69),  PG_OCTANT(70),  PG_OCTANT(71),  PG_OCTANT(72),
    PG_OCTANT(73),  PG_OCTANT(74),  PG_OCTANT(75),  PG_OCTANT(76),
    PG_OCTANT(77),  PG_OCTANT(78),  PG_OCTANT(79),  PG_OCTANT(80),
    PG_OCTANT(81),  PG_OCTANT(82),  PG_OCTANT(83),  PG_OCTANT(84),
    PG_OCTANT(85),  PG_OCTANT(86),  PG_OCTANT(87),  PG_OCTANT(88),
    PG_OCTANT(89),  PG_OCTANT(90),  PG_OCTANT(91),  PG_OCTANT(92),
    PG_OCTANT(93),  PG_OCTANT(94),  PG_OCTANT(95),  PG_OCTANT(96),
    PG_OCTANT(97),  PG_OCTANT(98),  PG_OCTANT(99),  PG_OCTANT(100),
    PG_OCTANT(101), PG_OCTANT(102), PG_OCTANT(103), PG_OCTANT(104),
    PG_OCTANT(105), PG_OCTANT(106), PG_OCTANT(107), PG_OCTANT(108),
    PG_OCTANT(109), PG_OCTANT(110), PG_OCTANT(111), PG_OCTANT(112),
    PG_OCTANT(113), PG_OCTANT(114), PG_OCTANT(115), PG_OCTANT(116),
    PG_OCTANT(117), PG_OCTANT(118), PG_OCTANT(119), PG_OCTANT(120),
    PG_OCTANT(121), PG_OCTANT(122), PG_OCTANT(123), PG_OCTANT(124),
    PG_OCTANT(125), PG_OCTANT(126), PG_OCTANT(127), PG_OCTANT(128),
    PG_OCTANT(129), PG_OCTANT(130), PG_OCTANT(131), PG_OCTANT(132),
    PG_OCTANT(133), PG_OCTANT(134), PG_OCTANT(135), PG_OCTANT(136),
    PG_OCTANT(137), PG_OCTANT(138), PG_OCTANT(139), PG_OCTANT(140),
    PG_OCTANT(141), PG_OCTANT(142), PG_OCTANT(143), PG_OCTANT(144),
    PG_OCTANT(145), PG_OCTANT(146), PG_OCTANT(147), PG_OCTANT(148),
    PG_OCTANT(149), PG_OCTANT(150), PG_OCTANT(151), PG_OCTANT(152),
    PG_OCTANT(153), PG_OCTANT(154), PG_OCTANT(155), PG_OCTANT(156),
    PG_OCTANT(157), PG_OCTANT(158), PG_OCTANT(159), PG_OCTANT(160),
    PG_OCTANT(161), PG_OCTANT(162), PG_OCTANT(163), PG_OCTANT(164),
    PG_OCTANT(165), PG_OCTANT(166), PG_OCTANT(167), PG_OCTANT(168),
    PG_OCTANT(169), PG_OCTANT(170), PG_OCTANT(171), PG_OCTANT(172),
    PG_OCTANT(173), PG_OCTANT(174), PG_OCTANT(175), PG_OCTANT(176),
    PG_OCTANT(177), PG_OCTANT(178), PG_OCTANT(179), PG_OCTANT(180),
    PG_OCTANT(181), PG_OCTANT(182), PG_OCTANT(183), PG_OCTANT(184),
    PG_OCTANT(185), PG_OCTANT(186), PG_OCTANT(187), PG_OCTANT(188),
    PG_OCTANT(189), PG_OCTANT(190), PG_OCTANT(191), PG_OCTANT(192),
    PG_OCTANT(193), PG_OCTANT(194), PG_OCTANT(195), PG_OCTANT(196),
    PG_OCTANT(197), PG_OCTANT(198), PG_OCTANT(199), PG_OCTANT(200),
    PG_OCTANT(201), PG_OCTANT(202), PG_OCTANT(203), PG_OCTANT(204),
    PG_OCTANT(205), PG_OCTANT(206), PG_OCTANT(207), PG_OCTANT(208),
    PG_OCTANT(209), PG_OCTANT(210), PG_OCTANT(211), PG_OCTANT(212),
    PG_OCTANT(213), PG_OCTANT(214), PG_OCTANT(215), PG_OCTANT(216),
    PG_OCTANT(217), PG_OCTANT(218), PG_OCTANT(219), PG_OCTANT(220),
    PG_OCTANT(221), PG_OCTANT(222), PG_OCTANT(223), PG_OCTANT(224),
    PG_OCTANT(225), PG_OCTANT(226), PG_OCTANT(227), PG_OCTANT(228),
    PG_OCTANT(229), PG_OCTANT(230), PG_OCTANT(231), PG_OCTANT(232),
    PG_OCTANT(233), PG_OCTANT(234), PG_OCTANT(235), PG_OCTANT(236),
    PG_OCTANT(237), PG_OCTANT(238), PG_OCTANT(239), PG_OCTANT(240),
    PG_OCTANT(241), PG_OCTANT(242), PG_OCTANT(243), PG_OCTANT(244),
    PG_OCTANT(245), PG_OCTANT(246), PG_OCTANT(247), PG_OCTANT(248),
    PG_OCTANT(249), PG_OCTANT(250), PG_OCTANT(251), PG_OCTANT(252),
    PG_OCTANT(253), PG_OCTANT(254), PG_OCTANT(255),

    // ===================================================================
    // Powerline Symbols (U+E0B0-U+E0D4)
    // 37 entries. Shape ID = offset from U+E0B0.
    //
    //  0  E0B0  Right solid triangle
    //  1  E0B1  Right thin chevron
    //  2  E0B2  Left solid triangle
    //  3  E0B3  Left thin chevron
    //  4  E0B4  Right semi-circle (filled) → rendered via Slug
    //  5  E0B5  Right thin semi-circle
    //  6  E0B6  Left semi-circle (filled) → rendered via Slug
    //  7  E0B7  Left thin semi-circle
    //  8  E0B8  Lower-left solid triangle
    //  9  E0B9  Back slash (thin)
    // 10  E0BA  Lower-right solid triangle
    // 11  E0BB  Forward slash (thin)
    // 12  E0BC  Upper-left solid triangle
    // 13  E0BD  Forward slash (thin, upper)
    // 14  E0BE  Upper-right solid triangle
    // 15  E0BF  Back slash (thin, upper)
    // 16-36: E0C0-E0D4 extras (flame, pixel, etc.)
    // ===================================================================

    // E0B0-E0B3: core powerline separators
    PG_POWERLINE(0),  PG_POWERLINE(1),  PG_POWERLINE(2),  PG_POWERLINE(3),
    // E0B4-E0B7: semi-circles (filled ones go through Slug pipeline)
    PG_SLUG(0),       PG_POWERLINE(5),  PG_SLUG(1),       PG_POWERLINE(7),
    // E0B8-E0BF: half-cell triangles and diagonals
    PG_POWERLINE(8),  PG_POWERLINE(9),  PG_POWERLINE(10), PG_POWERLINE(11),
    PG_POWERLINE(12), PG_POWERLINE(13), PG_POWERLINE(14), PG_POWERLINE(15),
    // E0C0-E0C7: flame-like shapes
    PG_POWERLINE(16), PG_POWERLINE(17), PG_POWERLINE(18), PG_POWERLINE(19),
    PG_POWERLINE(20), PG_POWERLINE(21), PG_POWERLINE(22), PG_POWERLINE(23),
    // E0C8-E0CF: pixelated / misc
    PG_POWERLINE(24), PG_POWERLINE(25), PG_POWERLINE(26), PG_POWERLINE(27),
    PG_POWERLINE(28), PG_POWERLINE(29), PG_POWERLINE(30), PG_POWERLINE(31),
    // E0D0-E0D4: dotted / misc
    PG_POWERLINE(32), PG_POWERLINE(33), PG_POWERLINE(34), PG_POWERLINE(35),
    PG_POWERLINE(36),
};

// Backward-compat aliases for code that still references BoxDrawing::
namespace BoxDrawingCompat {
    inline constexpr uint32_t kTableSize = ProceduralGlyph::kTableSize;
    inline constexpr uint32_t kBaseCodepoint = ProceduralGlyph::kBoxDrawingBase;
    inline constexpr const uint32_t* kTable = ProceduralGlyph::kTable;
}

#undef PG_NONE
#undef PG_RECT
#undef PG_QUAD
#undef PG_SHADE
#undef PG_LINE
#undef PG_LINED
#undef PG_BRAILLE
#undef PG_SEXTANT
#undef PG_OCTANT
#undef PG_POWERLINE
#undef PG_WEDGE
#undef PG_SLUG
#undef L
#undef H
#undef D
#undef DASH2
#undef DASH3
#undef DASH4
#undef ROUND
#undef DIAG

} // namespace ProceduralGlyph

#pragma once

#include "ColrTypes.h"
#include <functional>
#include <hb.h>
#include <hb-ot.h>

// Callback-based encoder that walks a COLRv1 paint graph via hb_font_paint_glyph()
// and produces a flat instruction buffer for GPU interpretation.
class ColrEncoder {
public:
    // Encode a single glyph's paint graph.
    // The glyphResolver callback maps (font, glyph_id) -> atlas_offset for clip glyphs.
    // It should ensure the glyph is encoded in the Slug atlas and return its offset.
    using GlyphResolver = std::function<uint32_t(hb_font_t* font, hb_codepoint_t glyph,
                                                  float* ext_min_x, float* ext_min_y,
                                                  float* ext_max_x, float* ext_max_y)>;

    static ColrGlyphData encode(hb_font_t* font, hb_codepoint_t glyph,
                                   unsigned int palette_index,
                                   hb_color_t foreground,
                                   GlyphResolver resolver);

private:
    struct State {
        std::vector<uint32_t> instructions;
        std::vector<ColrColorStop> colorStops;
        GlyphResolver resolver;
        hb_font_t* font;
    };

    static void emit(State& s, uint8_t opcode, const uint32_t* payload, uint32_t count);
    static void emitFloat(State& s, uint8_t opcode, const float* payload, uint32_t count);

    // HarfBuzz paint callbacks
    static void pushTransformCb(hb_paint_funcs_t*, void* paint_data,
                                float xx, float yx, float xy, float yy, float dx, float dy,
                                void*);
    static void popTransformCb(hb_paint_funcs_t*, void* paint_data, void*);
    static void pushClipGlyphCb(hb_paint_funcs_t*, void* paint_data,
                                hb_codepoint_t glyph, hb_font_t* font, void*);
    static void pushClipRectCb(hb_paint_funcs_t*, void* paint_data,
                               float xmin, float ymin, float xmax, float ymax, void*);
    static void popClipCb(hb_paint_funcs_t*, void* paint_data, void*);
    static void colorCb(hb_paint_funcs_t*, void* paint_data,
                        hb_bool_t is_foreground, hb_color_t color, void*);
    static void linearGradientCb(hb_paint_funcs_t*, void* paint_data,
                                 hb_color_line_t* color_line,
                                 float x0, float y0, float x1, float y1, float x2, float y2,
                                 void*);
    static void radialGradientCb(hb_paint_funcs_t*, void* paint_data,
                                 hb_color_line_t* color_line,
                                 float x0, float y0, float r0,
                                 float x1, float y1, float r1,
                                 void*);
    static void sweepGradientCb(hb_paint_funcs_t*, void* paint_data,
                                hb_color_line_t* color_line,
                                float cx, float cy,
                                float start_angle, float end_angle,
                                void*);
    static void pushGroupCb(hb_paint_funcs_t*, void* paint_data, void*);
    static void popGroupCb(hb_paint_funcs_t*, void* paint_data,
                           hb_paint_composite_mode_t mode, void*);

    // Helper to encode color stops from a color line
    static void encodeColorStops(State& s, hb_color_line_t* color_line,
                                 uint32_t& stop_offset, uint32_t& stop_count,
                                 uint32_t& extend);

    static uint32_t packColor(hb_color_t c);
};

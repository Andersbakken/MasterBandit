#include "ColrEncoder.h"
#include <cstring>

static inline uint32_t floatBits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

uint32_t ColrEncoder::packColor(hb_color_t c) {
    // hb_color_t: alpha=bits 0-7, red=8-15, green=16-23, blue=24-31
    // Our GPU format: R in bits 0-7, G 8-15, B 16-23, A 24-31
    uint8_t r = hb_color_get_red(c);
    uint8_t g = hb_color_get_green(c);
    uint8_t b = hb_color_get_blue(c);
    uint8_t a = hb_color_get_alpha(c);
    return static_cast<uint32_t>(r)
         | (static_cast<uint32_t>(g) << 8)
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(a) << 24);
}

void ColrEncoder::emit(State& s, uint8_t opcode, const uint32_t* payload, uint32_t count) {
    uint32_t header = static_cast<uint32_t>(opcode) | (count << 8);
    s.instructions.push_back(header);
    for (uint32_t i = 0; i < count; i++)
        s.instructions.push_back(payload[i]);
}

void ColrEncoder::emitFloat(State& s, uint8_t opcode, const float* payload, uint32_t count) {
    uint32_t header = static_cast<uint32_t>(opcode) | (count << 8);
    s.instructions.push_back(header);
    for (uint32_t i = 0; i < count; i++)
        s.instructions.push_back(floatBits(payload[i]));
}

void ColrEncoder::pushTransformCb(hb_paint_funcs_t*, void* paint_data,
                                  float xx, float yx, float xy, float yy, float dx, float dy,
                                  void*)
{
    auto& s = *static_cast<State*>(paint_data);
    float payload[] = { xx, yx, xy, yy, dx, dy };
    emitFloat(s, ColrOp::PushTransform, payload, 6);
}

void ColrEncoder::popTransformCb(hb_paint_funcs_t*, void* paint_data, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    emit(s, ColrOp::PopTransform, nullptr, 0);
}

void ColrEncoder::pushClipGlyphCb(hb_paint_funcs_t*, void* paint_data,
                                  hb_codepoint_t glyph, hb_font_t* font, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    float ext_min_x, ext_min_y, ext_max_x, ext_max_y;
    uint32_t atlas_offset = s.resolver(font, glyph, &ext_min_x, &ext_min_y, &ext_max_x, &ext_max_y);

    uint32_t payload[] = {
        atlas_offset,
        floatBits(ext_min_x), floatBits(ext_min_y),
        floatBits(ext_max_x), floatBits(ext_max_y)
    };
    emit(s, ColrOp::PushClipGlyph, payload, 5);
}

void ColrEncoder::pushClipRectCb(hb_paint_funcs_t*, void* paint_data,
                                 float xmin, float ymin, float xmax, float ymax, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    float payload[] = { xmin, ymin, xmax, ymax };
    emitFloat(s, ColrOp::PushClipRect, payload, 4);
}

void ColrEncoder::popClipCb(hb_paint_funcs_t*, void* paint_data, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    emit(s, ColrOp::PopClip, nullptr, 0);
}

void ColrEncoder::colorCb(hb_paint_funcs_t*, void* paint_data,
                           hb_bool_t /*is_foreground*/, hb_color_t color, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    uint32_t packed = packColor(color);
    emit(s, ColrOp::FillSolid, &packed, 1);
}

void ColrEncoder::encodeColorStops(State& s, hb_color_line_t* color_line,
                                   uint32_t& stop_offset, uint32_t& stop_count,
                                   uint32_t& extend)
{
    // Get total count
    uint32_t total = hb_color_line_get_color_stops(color_line, 0, nullptr, nullptr);
    std::vector<hb_color_stop_t> stops(total);
    uint32_t fetched = total;
    hb_color_line_get_color_stops(color_line, 0, &fetched, stops.data());

    stop_offset = static_cast<uint32_t>(s.colorStops.size());
    stop_count = fetched;
    extend = static_cast<uint32_t>(hb_color_line_get_extend(color_line));

    for (uint32_t i = 0; i < fetched; i++) {
        ColrColorStop cs;
        cs.offset = stops[i].offset;
        cs.color = packColor(stops[i].color);
        s.colorStops.push_back(cs);
    }
}

void ColrEncoder::linearGradientCb(hb_paint_funcs_t*, void* paint_data,
                                   hb_color_line_t* color_line,
                                   float x0, float y0, float x1, float y1, float x2, float y2,
                                   void*)
{
    auto& s = *static_cast<State*>(paint_data);
    uint32_t stop_offset, stop_count, extend;
    encodeColorStops(s, color_line, stop_offset, stop_count, extend);

    float fPayload[] = { x0, y0, x1, y1, x2, y2 };
    uint32_t payload[9];
    for (int i = 0; i < 6; i++) payload[i] = floatBits(fPayload[i]);
    payload[6] = stop_offset;
    payload[7] = stop_count;
    payload[8] = extend;
    emit(s, ColrOp::FillLinearGrad, payload, 9);
}

void ColrEncoder::radialGradientCb(hb_paint_funcs_t*, void* paint_data,
                                   hb_color_line_t* color_line,
                                   float x0, float y0, float r0,
                                   float x1, float y1, float r1,
                                   void*)
{
    auto& s = *static_cast<State*>(paint_data);
    uint32_t stop_offset, stop_count, extend;
    encodeColorStops(s, color_line, stop_offset, stop_count, extend);

    float fPayload[] = { x0, y0, r0, x1, y1, r1 };
    uint32_t payload[9];
    for (int i = 0; i < 6; i++) payload[i] = floatBits(fPayload[i]);
    payload[6] = stop_offset;
    payload[7] = stop_count;
    payload[8] = extend;
    emit(s, ColrOp::FillRadialGrad, payload, 9);
}

void ColrEncoder::sweepGradientCb(hb_paint_funcs_t*, void* paint_data,
                                  hb_color_line_t* color_line,
                                  float cx, float cy,
                                  float start_angle, float end_angle,
                                  void*)
{
    auto& s = *static_cast<State*>(paint_data);
    uint32_t stop_offset, stop_count, extend;
    encodeColorStops(s, color_line, stop_offset, stop_count, extend);

    float fPayload[] = { cx, cy, start_angle, end_angle };
    uint32_t payload[7];
    for (int i = 0; i < 4; i++) payload[i] = floatBits(fPayload[i]);
    payload[4] = stop_offset;
    payload[5] = stop_count;
    payload[6] = extend;
    emit(s, ColrOp::FillSweepGrad, payload, 7);
}

void ColrEncoder::pushGroupCb(hb_paint_funcs_t*, void* paint_data, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    emit(s, ColrOp::PushGroup, nullptr, 0);
}

void ColrEncoder::popGroupCb(hb_paint_funcs_t*, void* paint_data,
                             hb_paint_composite_mode_t mode, void*)
{
    auto& s = *static_cast<State*>(paint_data);
    uint32_t m = static_cast<uint32_t>(mode);
    emit(s, ColrOp::PopGroup, &m, 1);
}

ColrGlyphData ColrEncoder::encode(hb_font_t* font, hb_codepoint_t glyph,
                                     uint32_t palette_index,
                                     hb_color_t foreground,
                                     GlyphResolver resolver)
{
    State state;
    state.resolver = std::move(resolver);
    state.font = font;

    static hb_paint_funcs_t* funcs = [] {
        auto* f = hb_paint_funcs_create();
        hb_paint_funcs_set_push_transform_func(f, pushTransformCb, nullptr, nullptr);
        hb_paint_funcs_set_pop_transform_func(f, popTransformCb, nullptr, nullptr);
        hb_paint_funcs_set_push_clip_glyph_func(f, pushClipGlyphCb, nullptr, nullptr);
        hb_paint_funcs_set_push_clip_rectangle_func(f, pushClipRectCb, nullptr, nullptr);
        hb_paint_funcs_set_pop_clip_func(f, popClipCb, nullptr, nullptr);
        hb_paint_funcs_set_color_func(f, colorCb, nullptr, nullptr);
        hb_paint_funcs_set_linear_gradient_func(f, linearGradientCb, nullptr, nullptr);
        hb_paint_funcs_set_radial_gradient_func(f, radialGradientCb, nullptr, nullptr);
        hb_paint_funcs_set_sweep_gradient_func(f, sweepGradientCb, nullptr, nullptr);
        hb_paint_funcs_set_push_group_func(f, pushGroupCb, nullptr, nullptr);
        hb_paint_funcs_set_pop_group_func(f, popGroupCb, nullptr, nullptr);
        hb_paint_funcs_make_immutable(f);
        return f;
    }();

    hb_font_paint_glyph(font, glyph, funcs, &state, palette_index, foreground);

    return ColrGlyphData{std::move(state.instructions), std::move(state.colorStops)};
}

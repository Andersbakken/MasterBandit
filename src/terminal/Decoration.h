#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Decoration — presentation overlay applied on top of the document's cell
// SGR. Anchored by logical-line id (same model as Selection), so it survives
// reflow / scrollback eviction until the anchor evicts past the archive cap.
//
// Single source of truth for everything overlay-shaped:
//   - DecorationKind::Selection      — single, owned by selection lifecycle
//   - DecorationKind::CommandRegion  — single, OSC 133 selected command
//   - DecorationKind::Hyperlink      — many, one per OSC 8 toggle range
//   - DecorationKind::User           — JS-driven; user-supplied tag for grouping
//
// Composition order at render time:
//   cell SGR → User (by zPriority then insertion) → Hyperlink → CommandRegion
//   → Selection.
// Within composition: fg / bg / strikethrough are last-writer-wins; underline
// composes (any wins; highest priority chooses style+color). `dimOthers` and
// `outline` are region-modifiers extracted to shader uniforms — at most one of
// each is active per frame; ties broken by composition order.

enum class DecorationKind : uint8_t
{
    User          = 0,
    Selection     = 1,
    CommandRegion = 2,
    Hyperlink     = 3,
};

enum class DecorationShape : uint8_t
{
    // [start..end] linear range, walking in reading order across logical
    // lines.
    Range     = 0,
    // Rectangular region: cells with col in [minCol..maxCol] and absRow in
    // [minRow..maxRow] are in. Used by Selection rectangle mode.
    Rectangle = 1,
};

struct UnderlineSpec
{
    uint8_t style { 0 };  // 0=straight, 1=double, 2=curly, 3=dotted
    uint32_t color { 0 }; // 0 = use cell fg; nonzero = packed RGBA8
};

// Outline stroke around the decoration's bounding row range. Edge flags
// mirror the existing OSC 133 outline shader contract.
struct OutlineSpec
{
    uint32_t color { 0 };
    uint32_t edgeFlags { 0x1u | 0x2u }; // bit0: top, bit1: bottom
};

// Style payload — every field optional; "set" overrides the corresponding
// cell SGR or applies a region-modifier uniform.
struct DecorationStyle
{
    std::optional<uint32_t> fg; // packed RGBA8
    std::optional<uint32_t> bg; // packed RGBA8 (alpha ignored if 0xFF)
    std::optional<UnderlineSpec> underline;
    std::optional<bool> strikethrough;
    std::optional<OutlineSpec> outline; // region-modifier (uniform)
    std::optional<float> dimOthers;     // region-modifier: multiply RGB outside

    // Per-side bg rect expansion in pixels. 0 = match cell rect. When
    // non-zero (static or animated), the rendered bg extends symmetrically
    // outward; the expanded area composites under the cell SGR of any
    // cells it now covers.
    int32_t bgInflateX { 0 };
    int32_t bgInflateY { 0 };

    bool empty() const
    {
        return !fg && !bg && !underline && !strikethrough && !outline && !dimOthers &&
            bgInflateX == 0 && bgInflateY == 0;
    }
};

// Easing functions for animated properties. Unit-mapped: f(0)=0, f(1)=1.
enum class Ease : uint8_t
{
    Linear         = 0,
    EaseIn         = 1,
    EaseOut        = 2,
    EaseInOut      = 3,
    EaseInOutCubic = 4,
};

// What kind of object an Animation is targeting. v1 only supports
// Decoration; extending the animation system to popups, panes, etc. is
// adding new variants here + per-target sample/finalize routing.
enum class AnimTargetKind : uint8_t
{
    Decoration = 0,
};

// Animatable decoration property index. Order matches the JS-facing
// property strings: "bg", "fg", "underlineColor", "bgInflateX",
// "bgInflateY".
enum class AnimProp : uint8_t
{
    Bg             = 0,
    Fg             = 1,
    UnderlineColor = 2,
    BgInflateX     = 3,
    BgInflateY     = 4,
    Count          = 5,
};

// Time/value/easing payload shared by every animation regardless of what
// it targets. Immutable once registered. Times are in the same
// monotonic-ms clock as TerminalEmulator::mono(); `startValue` and
// `endValue` are int64 so one descriptor shape covers both color (packed
// RGBA8 in the low 32 bits) and scalar (px int) properties — the
// per-target sampler interprets them per `prop`.
struct AnimDescriptor
{
    uint64_t startMs { 0 };
    uint64_t durationMs { 0 };
    int64_t startValue { 0 };
    int64_t endValue { 0 };
    Ease ease { Ease::Linear };
};

// First-class animation entry. Points at the target it animates. The
// entire animation system is a flat list of these — there's no per-target
// "does this thing have animations?" sidecar; the list IS the registry.
//
// `targetId` is interpreted per `kind`:
//   - AnimTargetKind::Decoration → decoration id (matches Decoration::id)
struct Animation
{
    uint64_t handleId { 0 };
    AnimTargetKind kind { AnimTargetKind::Decoration };
    uint64_t targetId { 0 };
    AnimProp prop { AnimProp::Bg };
    AnimDescriptor desc;
};

// Live storage on TerminalEmulator. Anchors are line-id+cellOffset (same as
// Selection). Mutated under TerminalEmulator::mMutex.
//
// `startCellOffset` and `endCellOffset` are CUMULATIVE cell offsets within
// their respective logical lines (NOT visual-row columns; resolveDecoration
// does `row = firstAbs + off/w; col = off % w` to recover visual position).
// `endCellOffset` is **inclusive** — the column index of the last covered
// cell. The JS `addDecoration` API exposes `endCol` as exclusive (matching
// `pane.selection.endCol`, `getTextFromRows`, `MbMatch.endCol`); the binding
// layer converts at the boundary by storing `ec - 1` here.
struct Decoration
{
    uint64_t id { 0 };
    DecorationKind kind { DecorationKind::User };
    std::string tag; // user grouping; empty for system kinds
    uint64_t startLineId { 0 };
    int startCellOffset { 0 };
    uint64_t endLineId { 0 };
    int endCellOffset { 0 }; // INCLUSIVE — see comment above
    DecorationShape shape { DecorationShape::Range };
    DecorationStyle style;
    int zPriority { 0 };
    uint32_t hyperlinkId { 0 }; // Hyperlink kind only; click-target identity
};

// One queued operation in a decoration batch (see
// TerminalEmulator::applyDecorationBatch). `Add` carries a Decoration spec
// (its `id` is filled in during apply); `Clear` carries a tag to clear, or
// empty for "all User decorations".
struct DecorationBatchOp
{
    enum class Kind : uint8_t
    {
        Add   = 0,
        Clear = 1,
    };
    Kind kind { Kind::Add };
    Decoration spec;      // valid when kind == Add
    std::string clearTag; // valid when kind == Clear; empty = all User
};

// Snapshot mirror: anchors resolved to current absolute rows under the term
// mutex. Render thread reads without locking. Empty when an anchor has
// evicted past the archive cap.
struct ResolvedDecoration
{
    uint64_t id { 0 };
    DecorationKind kind { DecorationKind::User };
    int startCol { 0 };
    int startAbsRow { 0 };
    int endCol { 0 };
    int endAbsRow { 0 };
    DecorationShape shape { DecorationShape::Range };
    DecorationStyle style;
    int zPriority { 0 };
};

// === Animation prop helpers ===

inline bool isColorAnimProp(AnimProp p)
{
    switch (p) {
        case AnimProp::Bg:
        case AnimProp::Fg:
        case AnimProp::UnderlineColor:
            return true;
        case AnimProp::BgInflateX:
        case AnimProp::BgInflateY:
        case AnimProp::Count:
            return false;
    }
    return false;
}

// Write `value` into the appropriate field of `style` for prop `p`. Used
// by completion / cancel paths to land the final static value before
// dropping the descriptor. Animating `UnderlineColor` when no underline is
// configured is a no-op — matches the v1 "permissive" rule that
// preconditions-not-met animations succeed but produce no visible effect.
inline void applyAnimValueToStyle(DecorationStyle &style, AnimProp p, int64_t value)
{
    switch (p) {
        case AnimProp::Bg:
            style.bg = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            return;
        case AnimProp::Fg:
            style.fg = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            return;
        case AnimProp::UnderlineColor:
            if (style.underline) {
                style.underline->color = static_cast<uint32_t>(value & 0xFFFFFFFFu);
            }
            return;
        case AnimProp::BgInflateX:
            style.bgInflateX = static_cast<int32_t>(value);
            return;
        case AnimProp::BgInflateY:
            style.bgInflateY = static_cast<int32_t>(value);
            return;
        case AnimProp::Count:
            return;
    }
}

// === Easing curves ===

inline float easeApply(Ease e, float t)
{
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    switch (e) {
        case Ease::Linear:
            return t;
        case Ease::EaseIn:
            return t * t;
        case Ease::EaseOut: {
            float u = 1.0f - t;
            return 1.0f - u * u;
        }
        case Ease::EaseInOut:
            // smoothstep
            return t * t * (3.0f - 2.0f * t);
        case Ease::EaseInOutCubic:
            if (t < 0.5f) {
                return 4.0f * t * t * t;
            } else {
                float u = 1.0f - t;
                return 1.0f - 4.0f * u * u * u;
            }
    }
    return t;
}

inline float animProgress(const AnimDescriptor &d, uint64_t nowMs)
{
    if (d.durationMs == 0) {
        return 1.0f;
    }
    if (nowMs <= d.startMs) {
        return 0.0f;
    }
    uint64_t elapsed = nowMs - d.startMs;
    if (elapsed >= d.durationMs) {
        return 1.0f;
    }
    return static_cast<float>(elapsed) / static_cast<float>(d.durationMs);
}

// Linear-RGB color lerp on packed RGBA8 (0xAABBGGRR — alpha in MSB,
// matching the existing DecorationStyle::bg encoding). RGB channels go
// through gamma 2.2; alpha is lerped linearly. Approximation is fine for
// short UI fades; swap in an sRGB-curve LUT later if precision needs it.
inline uint32_t lerpRGBA8Linear(uint32_t a, uint32_t b, float t)
{
    auto toLin = [](uint8_t c) -> float
    {
        return std::pow(static_cast<float>(c) / 255.0f, 2.2f);
    };
    auto fromLin = [](float v) -> uint8_t
    {
        if (v <= 0.0f) {
            return 0;
        }
        if (v >= 1.0f) {
            return 255;
        }
        return static_cast<uint8_t>(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f);
    };

    uint8_t ar = static_cast<uint8_t>(a & 0xFF);
    uint8_t ag = static_cast<uint8_t>((a >> 8) & 0xFF);
    uint8_t ab = static_cast<uint8_t>((a >> 16) & 0xFF);
    uint8_t aa = static_cast<uint8_t>((a >> 24) & 0xFF);
    uint8_t br = static_cast<uint8_t>(b & 0xFF);
    uint8_t bg = static_cast<uint8_t>((b >> 8) & 0xFF);
    uint8_t bb = static_cast<uint8_t>((b >> 16) & 0xFF);
    uint8_t ba = static_cast<uint8_t>((b >> 24) & 0xFF);

    float lr = toLin(ar) + (toLin(br) - toLin(ar)) * t;
    float lg = toLin(ag) + (toLin(bg) - toLin(ag)) * t;
    float lb = toLin(ab) + (toLin(bb) - toLin(ab)) * t;
    float la = static_cast<float>(aa) + (static_cast<float>(ba) - static_cast<float>(aa)) * t;

    uint8_t outA = (la <= 0.0f) ? 0 : (la >= 255.0f) ? 255
                                                     : static_cast<uint8_t>(la + 0.5f);

    return static_cast<uint32_t>(fromLin(lr)) |
        (static_cast<uint32_t>(fromLin(lg)) << 8) |
        (static_cast<uint32_t>(fromLin(lb)) << 16) |
        (static_cast<uint32_t>(outA) << 24);
}

// Sample an animated color property at `nowMs`. Returns RGBA8 packed in
// the same byte order as DecorationStyle::bg / ::fg.
inline uint32_t sampleAnimColor(const AnimDescriptor &d, uint64_t nowMs)
{
    float t = easeApply(d.ease, animProgress(d, nowMs));
    return lerpRGBA8Linear(static_cast<uint32_t>(d.startValue & 0xFFFFFFFFu),
                           static_cast<uint32_t>(d.endValue & 0xFFFFFFFFu),
                           t);
}

// Sample an animated scalar (inflate) property at `nowMs`. Caller chooses
// the int width.
inline int32_t sampleAnimScalar(const AnimDescriptor &d, uint64_t nowMs)
{
    float t      = easeApply(d.ease, animProgress(d, nowMs));
    float lerped = static_cast<float>(d.startValue) +
        static_cast<float>(d.endValue - d.startValue) * t;
    return static_cast<int32_t>(std::lround(lerped));
}

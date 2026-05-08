#pragma once

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

    bool empty() const
    {
        return !fg && !bg && !underline && !strikethrough && !outline && !dimOthers;
    }
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

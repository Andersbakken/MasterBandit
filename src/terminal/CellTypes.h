#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

struct CellAttrs {
    // [0] fg R, [1] fg G, [2] fg B
    // [3] bg R, [4] bg G, [5] bg B
    // [6] bit 0: fg mode (0=default, 1=RGB — Indexed was never actually stored,
    //            palette colors resolve to RGB at SGR parse time)
    //     bit 1: bg mode (same)
    //     bits 2-3: semanticType (0=Output, 1=Input, 2=Prompt — OSC 133)
    //     bit 4: bold, bit 5: italic, bit 6: underline, bit 7: strikethrough
    // [7] bit 0: blink, bit 1: inverse, bit 2: dim, bit 3: invisible
    //     bit 4: wide, bit 5: wide spacer
    uint8_t data[8] = {};

    uint8_t fgR() const { return data[0]; }
    uint8_t fgG() const { return data[1]; }
    uint8_t fgB() const { return data[2]; }
    uint8_t bgR() const { return data[3]; }
    uint8_t bgG() const { return data[4]; }
    uint8_t bgB() const { return data[5]; }

    void setFg(uint8_t r, uint8_t g, uint8_t b) { data[0] = r; data[1] = g; data[2] = b; }
    void setBg(uint8_t r, uint8_t g, uint8_t b) { data[3] = r; data[4] = g; data[5] = b; }

    enum ColorMode : uint8_t { Default = 0, RGB = 1 };

    ColorMode fgMode() const { return static_cast<ColorMode>(data[6] & 0x01); }
    ColorMode bgMode() const { return static_cast<ColorMode>((data[6] >> 1) & 0x01); }
    void setFgMode(ColorMode m) { data[6] = (data[6] & ~0x01) | (m & 0x01); }
    void setBgMode(ColorMode m) { data[6] = (data[6] & ~0x02) | ((m & 0x01) << 1); }

    // OSC 133 semantic type (WezTerm-style per-cell tag). Output is the default
    // so cells that never saw an OSC 133 marker read as Output.
    enum SemanticType : uint8_t { Output = 0, Input = 1, Prompt = 2 };
    SemanticType semanticType() const { return static_cast<SemanticType>((data[6] >> 2) & 0x03); }
    void setSemanticType(SemanticType t) { data[6] = (data[6] & ~0x0C) | ((static_cast<uint8_t>(t) & 0x03) << 2); }

    bool bold() const { return data[6] & 0x10; }
    void setBold(bool v) { if (v) data[6] |= 0x10; else data[6] &= ~0x10; }
    bool italic() const { return data[6] & 0x20; }
    void setItalic(bool v) { if (v) data[6] |= 0x20; else data[6] &= ~0x20; }
    bool underline() const { return data[6] & 0x40; }
    void setUnderline(bool v) { if (v) data[6] |= 0x40; else data[6] &= ~0x40; }
    bool strikethrough() const { return data[6] & 0x80; }
    void setStrikethrough(bool v) { if (v) data[6] |= 0x80; else data[6] &= ~0x80; }

    bool blink() const { return data[7] & 0x01; }
    void setBlink(bool v) { if (v) data[7] |= 0x01; else data[7] &= ~0x01; }
    bool inverse() const { return data[7] & 0x02; }
    void setInverse(bool v) { if (v) data[7] |= 0x02; else data[7] &= ~0x02; }
    bool dim() const { return data[7] & 0x04; }
    void setDim(bool v) { if (v) data[7] |= 0x04; else data[7] &= ~0x04; }
    bool invisible() const { return data[7] & 0x08; }
    void setInvisible(bool v) { if (v) data[7] |= 0x08; else data[7] &= ~0x08; }
    bool wide() const { return data[7] & 0x10; }
    void setWide(bool v) { if (v) data[7] |= 0x10; else data[7] &= ~0x10; }
    bool wideSpacer() const { return data[7] & 0x20; }
    void setWideSpacer(bool v) { if (v) data[7] |= 0x20; else data[7] &= ~0x20; }

    // Underline style: 0=straight, 1=double, 2=curly, 3=dotted (stored in data[7] bits 6-7)
    uint8_t underlineStyle() const { return (data[7] >> 6) & 0x03; }
    void setUnderlineStyle(uint8_t s) { data[7] = (data[7] & 0x3F) | ((s & 0x03) << 6); }

    uint32_t packFgAsU32() const {
        if (fgMode() == Default) return 0xFFFFFFFF;
        return (static_cast<uint32_t>(data[0]))
             | (static_cast<uint32_t>(data[1]) << 8)
             | (static_cast<uint32_t>(data[2]) << 16)
             | 0xFF000000u;
    }

    uint32_t packBgAsU32() const {
        if (bgMode() == Default) return 0;
        return (static_cast<uint32_t>(data[3]))
             | (static_cast<uint32_t>(data[4]) << 8)
             | (static_cast<uint32_t>(data[5]) << 16)
             | 0xFF000000u;
    }

    // SGR 0 clears all styling but not the OSC 133 semantic type — semantic mode
    // is orthogonal to SGR and only cycles through A/B/C/D markers.
    void reset() {
        uint8_t sem = data[6] & 0x0C;
        memset(data, 0, sizeof(data));
        data[6] = sem;
    }
};

struct Cell {
    char32_t wc = 0;
    CellAttrs attrs{};
};
static_assert(sizeof(Cell) == 12);

struct CellExtra {
    uint32_t imageId { 0 };
    uint32_t imagePlacementId { 0 }; // kitty placement ID (p=), 0 = default placement
    uint32_t imageStartCol { 0 };    // column where the image starts (for X positioning)
    uint32_t imageOffsetRow { 0 };
    uint32_t hyperlinkId { 0 };      // OSC 8: index into hyperlink registry
    uint32_t underlineColor { 0 };   // SGR 58: packed RGBA8, 0 = use fg color
    std::vector<char32_t> combiningCps; // grapheme cluster continuation codepoints (ZWJ, combiners, VS16, etc.)
};

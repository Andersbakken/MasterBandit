#include <doctest/doctest.h>
#include "TestTerminal.h"
#include "Utils.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstring>
#include <vector>

namespace {

struct ITermTerminal : TestTerminal {
    float cellW { 10.0f };
    float cellH { 20.0f };

    ITermTerminal(int cols = 80, int rows = 24)
        : TestTerminal(cols, rows)
    {
        auto& cbs = term.callbacks();
        cbs.cellPixelWidth  = [this]() -> float { return cellW; };
        cbs.cellPixelHeight = [this]() -> float { return cellH; };
    }

    const CellExtra* extra(int col, int row) const
    {
        return term.grid().getExtra(col, row);
    }
};

// Encode a WxH solid-color RGBA image as a PNG byte vector. Uses stb_image_write
// so tests don't hard-code PNG bytes (format changes across stb revisions).
std::vector<uint8_t> encodePng(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
    }
    std::vector<uint8_t> out;
    auto writer = [](void* ctx, void* data, int len) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        v->insert(v->end(), p, p + len);
    };
    int ok = stbi_write_png_to_func(writer, &out, w, h, 4, rgba.data(), w * 4);
    REQUIRE(ok != 0);
    return out;
}

// Build the full OSC 1337 payload: ESC ] 1337 ; File=<params>:<b64> BEL.
std::string osc1337(const std::string& params, const std::vector<uint8_t>& pngBytes)
{
    std::string b64 = base64::encode(pngBytes.data(), pngBytes.size());
    std::string body = "1337;File=" + params + ":" + b64;
    return "\x1b]" + body + "\x07";
}

} // namespace

// ── Happy path ─────────────────────────────────────────────────────────────

TEST_CASE("OSC 1337: inline PNG registers an image")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 255, 0, 0);

    t.feed(osc1337("inline=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    auto it = t.term.imageRegistry().begin();
    const auto& img = *it->second;
    CHECK(img.pixelWidth == 10);
    CHECK(img.pixelHeight == 20);
    // 10 px / 10 px-per-cell = 1 col; 20 px / 20 px-per-cell = 1 row.
    CHECK(img.cellWidth == 1);
    CHECK(img.cellHeight == 1);
    CHECK(img.rgba.size() == static_cast<size_t>(10 * 20 * 4));
}

TEST_CASE("OSC 1337: image is placed in the grid at the cursor")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(20, 40, 0, 255, 0);

    // Move cursor to (col=5, row=3) before placing.
    t.csi("4;6H");
    t.feed(osc1337("inline=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const uint32_t id = t.term.imageRegistry().begin()->first;

    // 20x40 px at 10x20 px/cell ⇒ 2 cols × 2 rows.
    // Placement marks the start column of each row (column 5) with imageId.
    for (int r = 0; r < 2; ++r) {
        const CellExtra* ex = t.extra(5, 3 + r);
        REQUIRE(ex != nullptr);
        CHECK(ex->imageId == id);
        CHECK(ex->imageStartCol == 5u);
        CHECK(ex->imageOffsetRow == static_cast<uint32_t>(r));
    }
}

TEST_CASE("OSC 1337: sub-cell image pixel dimensions still take at least one cell")
{
    ITermTerminal t(40, 20);
    // 1×1 px image — smaller than a cell in both dimensions.
    auto png = encodePng(1, 1, 0, 0, 255);

    t.feed(osc1337("inline=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 1);
    CHECK(img.cellHeight == 1);
}

TEST_CASE("OSC 1337: non-cell-aligned pixel dimensions round up")
{
    ITermTerminal t(40, 20);
    // 15×25 px at 10×20 px/cell ⇒ ceil(1.5)=2 cols, ceil(1.25)=2 rows.
    auto png = encodePng(15, 25, 200, 200, 200);

    t.feed(osc1337("inline=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 2);
    CHECK(img.cellHeight == 2);
}

TEST_CASE("OSC 1337: successive images get distinct ids")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 1, 2, 3);

    t.feed(osc1337("inline=1", png));
    // Move cursor away so the second image doesn't overwrite the first.
    t.csi("10;1H");
    t.feed(osc1337("inline=1", png));

    CHECK(t.term.imageRegistry().size() == 2);
    auto it = t.term.imageRegistry().begin();
    uint32_t a = it->first;
    ++it;
    uint32_t b = it->first;
    CHECK(a != b);
}

// ── Rejection paths ────────────────────────────────────────────────────────

TEST_CASE("OSC 1337: missing inline=1 is ignored")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 255, 0, 0);

    t.feed(osc1337("name=foo.png;size=1234", png));

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: inline with value other than 1 is ignored")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 255, 0, 0);

    t.feed(osc1337("inline=0", png));

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: missing File= prefix is ignored")
{
    ITermTerminal t(40, 20);
    // Send a payload that doesn't start with "File=" — emulator must bail early.
    std::string b64 = base64::encode(reinterpret_cast<const uint8_t*>("x"), 1);
    t.feed("\x1b]1337;NotFile=inline=1:" + b64 + "\x07");

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: missing colon separator is ignored")
{
    ITermTerminal t(40, 20);
    // "File=inline=1" with no ":<data>" — emulator must bail, not crash.
    t.feed("\x1b]1337;File=inline=1\x07");

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: empty base64 payload is ignored")
{
    ITermTerminal t(40, 20);
    t.feed("\x1b]1337;File=inline=1:\x07");

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: non-image base64 payload is ignored")
{
    ITermTerminal t(40, 20);
    // Valid base64 but not a decodable image — stbi_load_from_memory returns null.
    std::vector<uint8_t> garbage(64, 0xAB);
    t.feed(osc1337("inline=1", garbage));

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: cell size of 0 rejects the image")
{
    ITermTerminal t(40, 20);
    t.cellW = 0.0f;  // simulate missing cell metrics
    auto png = encodePng(10, 20, 255, 0, 0);

    t.feed(osc1337("inline=1", png));

    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("OSC 1337: missing cell-size callback rejects the image")
{
    ITermTerminal t(40, 20);
    // Clear the callbacks a base TestTerminal doesn't set either — emulator
    // must check for null and not crash.
    t.term.callbacks().cellPixelWidth = nullptr;
    t.term.callbacks().cellPixelHeight = nullptr;
    auto png = encodePng(10, 20, 255, 0, 0);

    t.feed(osc1337("inline=1", png));

    CHECK(t.term.imageRegistry().empty());
}

// ── name= metadata ─────────────────────────────────────────────────────────

TEST_CASE("OSC 1337: name is base64-decoded into ImageEntry::name")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 1, 2, 3);

    // base64("foo.png") = "Zm9vLnBuZw=="
    t.feed(osc1337("inline=1;name=Zm9vLnBuZw==", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().begin()->second->name == "foo.png");
}

TEST_CASE("OSC 1337: unset name leaves ImageEntry::name empty")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 1, 2, 3);

    t.feed(osc1337("inline=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().begin()->second->name.empty());
}

TEST_CASE("OSC 1337: unicode name survives base64 roundtrip")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 1, 2, 3);

    // base64("café.png") = "Y2Fmw6kucG5n"
    t.feed(osc1337("inline=1;name=Y2Fmw6kucG5n", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().begin()->second->name == "café.png");
}

TEST_CASE("OSC 1337: malformed base64 name doesn't prevent image registration")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 1, 2, 3);

    // "!!!" contains characters outside the base64 alphabet. The decoder is
    // lenient; what matters is that the image itself still registers rather
    // than the whole sequence being discarded.
    t.feed(osc1337("inline=1;name=!!!", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
}

// ── width=/height= parsing (cells, pixels, percent) ────────────────────────

TEST_CASE("OSC 1337: width in cells overrides natural size")
{
    ITermTerminal t(40, 20);
    // 20×20 px at 10×20 px/cell, natural = 2×1 cells. Ask for width=5 (cells).
    // With aspect preservation default: rows = round(5 / (20/20 * 20/10)) = round(5/2) = 3.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=5", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 5);
    CHECK(img.cellHeight == 3);
}

TEST_CASE("OSC 1337: width in pixels maps via cellPixelWidth")
{
    ITermTerminal t(40, 20);
    // width=50px at 10 px/cell → 5 cols.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=50px", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 5);
}

TEST_CASE("OSC 1337: width in percent is relative to terminal columns")
{
    ITermTerminal t(40, 20);
    // width=50% of 40 cols = 20 cols.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=50%", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 20);
}

TEST_CASE("OSC 1337: width=auto falls back to natural size")
{
    ITermTerminal t(40, 20);
    // 30×20 px at 10×20 px/cell → natural 3×1.
    auto png = encodePng(30, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=auto", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 3);
    CHECK(img.cellHeight == 1);
}

TEST_CASE("OSC 1337: height-only spec derives width from aspect")
{
    ITermTerminal t(40, 20);
    // 20×20 px (square). cellsAspect = (20/20) * (20/10) = 2 (cells-per-row).
    // height=4 → cols = round(4 * 2) = 8.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;height=4", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellHeight == 4);
    CHECK(img.cellWidth == 8);
}

// ── preserveAspectRatio ────────────────────────────────────────────────────

TEST_CASE("OSC 1337: preserveAspectRatio=0 stretches to exact box")
{
    ITermTerminal t(40, 20);
    // 20×20 px image; request 10×10 cells with aspect stretch disabled.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=10;height=10;preserveAspectRatio=0", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 10);
    CHECK(img.cellHeight == 10);
}

TEST_CASE("OSC 1337: preserveAspectRatio=1 fits inside the requested box")
{
    ITermTerminal t(40, 20);
    // 20×20 px image. cellsAspect = 2 (wider than tall in cells).
    // Request 10×10. derivedRows = round(10/2) = 5, fits → 10×5.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=10;height=10;preserveAspectRatio=1", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 10);
    CHECK(img.cellHeight == 5);
}

TEST_CASE("OSC 1337: preserveAspectRatio default is on")
{
    ITermTerminal t(40, 20);
    // Same as above but no explicit preserveAspectRatio param → should match
    // the preserveAspectRatio=1 behavior.
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=10;height=10", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 10);
    CHECK(img.cellHeight == 5);
}

TEST_CASE("OSC 1337: aspect-constrained fit respects the tighter dimension")
{
    ITermTerminal t(40, 20);
    // 20×20 px → cellsAspect 2. Request 4×10. derivedRows = round(4/2)=2
    // (fits in 10), so output is 4×2 (width-constrained).
    auto png = encodePng(20, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=4;height=10", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 4);
    CHECK(img.cellHeight == 2);
}

// ── Malformed width/height ─────────────────────────────────────────────────

TEST_CASE("OSC 1337: malformed width falls back to natural size")
{
    ITermTerminal t(40, 20);
    // 30×20 px → natural 3×1. "garbage" fails to parse → treated as Auto.
    auto png = encodePng(30, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=garbage", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 3);
    CHECK(img.cellHeight == 1);
}

TEST_CASE("OSC 1337: negative width falls back to natural size")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(30, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;width=-5", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
    const auto& img = *t.term.imageRegistry().begin()->second;
    CHECK(img.cellWidth == 3);  // fell back to natural
}

// ── Unknown params are ignored (size=, type=, etc.) ────────────────────────

TEST_CASE("OSC 1337: unknown params don't prevent image from registering")
{
    ITermTerminal t(40, 20);
    auto png = encodePng(10, 20, 0, 0, 0);

    t.feed(osc1337("inline=1;size=1234;type=image/png;unknown=whatever", png));

    REQUIRE(t.term.imageRegistry().size() == 1);
}

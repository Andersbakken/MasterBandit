// Unit tests for TextSystem's dynamic font-fallback machinery.
//
// The live path (PlatformDawn.cpp) wires setSystemFallback / setEmojiFallback
// to CoreText (macOS) or fontconfig (Linux), which dynamically load fonts
// from the OS font database at runtime when a codepoint isn't covered by the
// primary font. The headless/test branch deliberately skips that wiring to
// keep tests deterministic across machines.
//
// These tests use mock lambdas that return bundled, pre-subset test fonts so
// they can exercise the same code paths (lazy fallback invocation, caching,
// COLR vs non-COLR preference, emoji-vs-system routing) without depending on
// whatever fonts the host OS happens to have installed.

#include <doctest/doctest.h>
#include "text.h"
#include <fstream>
#include <vector>
#include <string>

namespace {

// Codepoints chosen by inspecting cmap of each bundled test font:
//   Inconsolata:     broad Latin-1, 882 codepoints. No U+26A0, no U+1F344.
//   DejaVu subset:   just U+26A0 (WARNING SIGN).
//   Noto Color Emoji subset: U+26A0 + U+1F344 (MUSHROOM), both COLRv1 paint.
// So U+26A0 sits in both fallbacks — useful for testing emoji-vs-non-emoji
// routing. U+1F344 is emoji-only. Plain ASCII stays in the primary.
constexpr char32_t kAsciiCp         = U'A';       // primary
constexpr char32_t kNonEmojiFbCp    = 0x26A0;     // ⚠  in both fallbacks
constexpr char32_t kEmojiOnlyCp     = 0x1F344;    // 🍄 in Noto only

std::vector<uint8_t> loadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Build a TextSystem loaded with Inconsolata as primary, with mock fallback
// lambdas that record invocation counts and return the bundled test fonts.
struct FallbackFixture {
    TextSystem ts;
    std::vector<uint8_t> primary;
    std::vector<uint8_t> systemFb;
    std::vector<uint8_t> emojiFb;
    int systemCalls = 0;
    int emojiCalls  = 0;
    char32_t lastSystemCp = 0;
    char32_t lastEmojiCp  = 0;

    bool ready = false;

    FallbackFixture()
    {
        primary  = loadFile(MB_TEST_TEXT_FONT);
        systemFb = loadFile(MB_TEST_FALLBACK_FONT);
        emojiFb  = loadFile(MB_TEST_EMOJI_FONT);
        if (primary.empty() || systemFb.empty() || emojiFb.empty()) return;

        std::vector<std::vector<uint8_t>> primaryList = {primary};
        if (!ts.registerFont("test", primaryList, 48.0f)) return;
        ts.setPrimaryFontPath("test", MB_TEST_TEXT_FONT);

        ts.setSystemFallback([this](const std::string&, char32_t cp) {
            ++systemCalls;
            lastSystemCp = cp;
            return systemFb;
        });
        ts.setEmojiFallback([this](char32_t cp) {
            ++emojiCalls;
            lastEmojiCp = cp;
            return emojiFb;
        });
        ready = true;
    }

    // Helper: decode (fontIndex, glyphId) from a packed ShapedRunGlyph id.
    // glyphKey() in text.cpp packs as (fontIndex << 32) | glyphId.
    static uint32_t fontIndexOf(uint64_t packed) { return static_cast<uint32_t>(packed >> 32); }
    static uint32_t glyphIdOf  (uint64_t packed) { return static_cast<uint32_t>(packed & 0xFFFFFFFFu); }
};

// UTF-8 encode a single codepoint into a std::string (simple, tests only).
std::string u8(char32_t cp)
{
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

} // namespace

TEST_CASE("font fallback: primary-covered codepoint fires no callback")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    auto run = fx.ts.shapeRun("test", u8(kAsciiCp), 20.0f);
    REQUIRE(run.glyphs.size() == 1);
    CHECK(FallbackFixture::glyphIdOf(run.glyphs[0].glyphId) != 0);
    CHECK(FallbackFixture::fontIndexOf(run.glyphs[0].glyphId) == 0); // primary
    CHECK(fx.systemCalls == 0);
    CHECK(fx.emojiCalls == 0);
}

TEST_CASE("font fallback: non-emoji missing codepoint triggers system fallback")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    auto run = fx.ts.shapeRun("test", u8(kNonEmojiFbCp), 20.0f);
    REQUIRE(run.glyphs.size() >= 1);

    // The system fallback callback must have fired exactly once for this cp.
    CHECK(fx.systemCalls == 1);
    CHECK(fx.lastSystemCp == kNonEmojiFbCp);
    // Emoji callback must not have fired — this cp isn't tagged emoji-presentation.
    CHECK(fx.emojiCalls == 0);

    // Resolved glyph must come from a non-primary font index (the freshly
    // loaded DejaVu subset) and be non-zero (actually resolved).
    CHECK(FallbackFixture::glyphIdOf(run.glyphs[0].glyphId) != 0);
    CHECK(FallbackFixture::fontIndexOf(run.glyphs[0].glyphId) != 0);
}

TEST_CASE("font fallback: repeat lookup does not re-invoke callback (caching)")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    fx.ts.shapeRun("test", u8(kNonEmojiFbCp), 20.0f);
    REQUIRE(fx.systemCalls == 1);

    // Second shape of the same codepoint: the fallback font is already in
    // the loaded list, so resolveGlyph must find it without re-invoking the
    // callback (which would be a pathological re-download for live code).
    fx.ts.shapeRun("test", u8(kNonEmojiFbCp), 20.0f);
    CHECK(fx.systemCalls == 1);
}

TEST_CASE("font fallback: emoji-presentation codepoint routes to emoji callback")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    // U+1F344 (mushroom) is widened emoji — TextSystem auto-detects this
    // and routes to the emoji-specific callback, which prefers COLR fonts.
    auto run = fx.ts.shapeRun("test", u8(kEmojiOnlyCp), 20.0f);
    REQUIRE(run.glyphs.size() >= 1);

    CHECK(fx.emojiCalls == 1);
    CHECK(fx.lastEmojiCp == kEmojiOnlyCp);
    CHECK(fx.systemCalls == 0);

    CHECK(FallbackFixture::glyphIdOf(run.glyphs[0].glyphId) != 0);
    CHECK(FallbackFixture::fontIndexOf(run.glyphs[0].glyphId) != 0);
}

TEST_CASE("font fallback: emoji lookup caches across calls")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    fx.ts.shapeRun("test", u8(kEmojiOnlyCp), 20.0f);
    REQUIRE(fx.emojiCalls == 1);

    fx.ts.shapeRun("test", u8(kEmojiOnlyCp), 20.0f);
    CHECK(fx.emojiCalls == 1);
}

TEST_CASE("font fallback: emoji codepoint prefers emoji callback over system callback")
{
    FallbackFixture fx;
    REQUIRE(fx.ready);

    // U+26A0 (warning sign) lives in both fallback fonts. When queried as
    // part of an emoji-presentation context, the emoji route should win even
    // though the system fallback could also resolve it.
    //
    // shapeRun doesn't accept an explicit "treat as emoji" flag — the TextSystem
    // decides based on isWidenedEmoji(). U+26A0 on its own isn't widened, so
    // plain shaping would route to systemFallback_. We combine it with VS16
    // (U+FE0F) to force emoji presentation, which is the real-world way apps
    // convey "render this as the color emoji form".
    std::string text = u8(kNonEmojiFbCp) + u8(0xFE0F);
    auto run = fx.ts.shapeRun("test", text, 20.0f);
    REQUIRE(run.glyphs.size() >= 1);

    // Emoji callback fired (the VS16 selector promoted this to emoji path).
    CHECK(fx.emojiCalls == 1);
    // The resolved glyph must be from a fallback font, not the primary.
    CHECK(FallbackFixture::fontIndexOf(run.glyphs[0].glyphId) != 0);
}

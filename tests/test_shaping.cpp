#include <doctest/doctest.h>
#include "text.h"
#include "FontResolver.h"
#include "FontFallback.h"
#include <fstream>
#include <vector>
#include <string>

static std::vector<uint8_t> loadFontFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static TextSystem* getTextSystem() {
    static TextSystem* ts = nullptr;
    static FontFallback fallback;
    if (ts) return ts;

    // Use the system font resolver to find a monospace font
    std::string fontPath = resolveFontFamily("Menlo");
    if (fontPath.empty()) fontPath = resolveFontFamily("DejaVu Sans Mono");
    if (fontPath.empty()) fontPath = resolveFontFamily("monospace");
    if (fontPath.empty()) return nullptr;

    auto fontData = loadFontFile(fontPath);
    if (fontData.empty()) return nullptr;

    ts = new TextSystem();
    std::vector<std::vector<uint8_t>> fonts = {fontData};
    if (!ts->registerFont("test", fonts, 20.0f)) {
        delete ts;
        ts = nullptr;
        return nullptr;
    }
    ts->setPrimaryFontPath("test", fontPath);
    ts->setSystemFallback([&](const std::string& path, char32_t cp) {
        return fallback.fontDataForCodepoint(path, cp);
    });
    return ts;
}

TEST_CASE("shapeRun: ASCII produces correct glyph count") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "hello", 20.0f);
    CHECK(run.glyphs.size() == 5);
}

TEST_CASE("shapeRun: ASCII glyphs are not substitutions") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "abc", 20.0f);
    REQUIRE(run.glyphs.size() == 3);
    for (const auto& g : run.glyphs) {
        CHECK_FALSE(g.isSubstitution);
    }
}

TEST_CASE("shapeRun: ASCII glyphs are LTR") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "abc", 20.0f);
    REQUIRE(run.glyphs.size() == 3);
    for (const auto& g : run.glyphs) {
        CHECK_FALSE(g.rtl);
    }
}

TEST_CASE("shapeRun: ASCII clusters are sequential") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "abcd", 20.0f);
    REQUIRE(run.glyphs.size() == 4);
    // ASCII: 1 byte per char, clusters should be 0, 1, 2, 3
    CHECK(run.glyphs[0].cluster == 0);
    CHECK(run.glyphs[1].cluster == 1);
    CHECK(run.glyphs[2].cluster == 2);
    CHECK(run.glyphs[3].cluster == 3);
}

TEST_CASE("shapeRun: single character") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "x", 20.0f);
    CHECK(run.glyphs.size() == 1);
    CHECK(run.glyphs[0].cluster == 0);
    CHECK_FALSE(run.glyphs[0].rtl);
    CHECK_FALSE(run.glyphs[0].isSubstitution);
}

TEST_CASE("shapeRun: empty string") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "", 20.0f);
    CHECK(run.glyphs.empty());
}

TEST_CASE("shapeRun: Arabic text is RTL") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    // مرح = three Arabic characters (meem, ra, ha)
    const auto& run = ts->shapeRun("test", "\xd9\x85\xd8\xb1\xd8\xad", 20.0f);
    // May produce different glyph count due to shaping, but all should be RTL
    REQUIRE(run.glyphs.size() > 0);
    for (const auto& g : run.glyphs) {
        CHECK(g.rtl);
    }
}

TEST_CASE("shapeRun: Arabic produces glyphs with RTL flag") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    // مرح — Arabic characters
    const auto& run = ts->shapeRun("test", "\xd9\x85\xd8\xb1\xd8\xad", 20.0f);
    if (run.glyphs.empty()) { MESSAGE("No glyphs produced (font may lack Arabic)"); return; }

    // All Arabic glyphs should be RTL
    for (const auto& g : run.glyphs) {
        CHECK(g.rtl);
    }
    // Whether they are substitutions depends on the fallback font's GSUB tables
}

TEST_CASE("shapeRun: mixed LTR/RTL has correct flags") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    // "hi مر" = Latin + space + Arabic
    const auto& run = ts->shapeRun("test", "hi \xd9\x85\xd8\xb1", 20.0f);
    REQUIRE(run.glyphs.size() >= 4); // at least h, i, space, + Arabic glyphs

    // First glyphs should be LTR (Latin)
    bool foundLtr = false, foundRtl = false;
    for (const auto& g : run.glyphs) {
        if (!g.rtl) foundLtr = true;
        if (g.rtl) foundRtl = true;
    }
    CHECK(foundLtr);
    CHECK(foundRtl);
}

TEST_CASE("shapeRun: advances are positive") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run = ts->shapeRun("test", "hello", 20.0f);
    REQUIRE(run.glyphs.size() == 5);
    for (const auto& g : run.glyphs) {
        CHECK(g.xAdvance > 0.0f);
    }
}

TEST_CASE("shapeRun: cache returns same result") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run1 = ts->shapeRun("test", "test", 20.0f);
    const auto& run2 = ts->shapeRun("test", "test", 20.0f);
    CHECK(&run1 == &run2); // same reference from cache
}

TEST_CASE("shapeRun: different FontStyle produces different cache entry") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    const auto& run1 = ts->shapeRun("test", "abc", 20.0f, FontStyle{});
    const auto& run2 = ts->shapeRun("test", "abc", 20.0f, FontStyle{.bold = true});
    // Different style = different cache key, so different addresses
    // (unless hash collision, which is unlikely)
    CHECK(&run1 != &run2);
}

TEST_CASE("shapeRun: multibyte UTF-8 clusters") {
    auto* ts = getTextSystem();
    if (!ts) { MESSAGE("No system font found, skipping"); return; }

    // "aé" — 'a' is 1 byte, 'é' (U+00E9) is 2 bytes
    const auto& run = ts->shapeRun("test", "a\xc3\xa9", 20.0f);
    REQUIRE(run.glyphs.size() == 2);
    CHECK(run.glyphs[0].cluster == 0); // 'a' at byte 0
    CHECK(run.glyphs[1].cluster == 1); // 'é' at byte 1
}

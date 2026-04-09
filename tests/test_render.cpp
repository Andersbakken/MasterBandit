#include <doctest/doctest.h>
#include "MBConnection.h"

// These tests require a GPU and launch mb --test as a child process.
// A single shared process is reused across tests (reset between each).
//
// Generate reference images:  MB_UPDATE_REFS=1 ./build/bin/mb-tests "[render]"
// Run rendering tests:        ./build/bin/mb-tests "[render]"

TEST_CASE("render: headless startup and screenshot" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.wait(500);

    auto png = rt.screenshotPng();
    REQUIRE(!png.empty());
    REQUIRE(png.size() > 8);
    CHECK(png[0] == 0x89);
    CHECK(png[1] == 'P');
    CHECK(png[2] == 'N');
    CHECK(png[3] == 'G');
}

TEST_CASE("render: text injection and cell rect screenshot" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.reset();
    rt.wait(500);

    rt.sendText("Hello, World!");
    rt.wait(300);

    auto full = rt.screenshotPng();
    REQUIRE(!full.empty());

    auto png = rt.screenshotPaneRect(0, 0, 0, 20, 2);
    REQUIRE(!png.empty());
    CHECK(png[0] == 0x89);
}

TEST_CASE("render: pane screenshot" * doctest::test_suite("render"))
{
    auto& rt = MBConnection::shared();
    REQUIRE(rt.childPid() > 0);

    rt.reset();
    rt.wait(500);

    auto png = rt.screenshotPane(0);
    REQUIRE(!png.empty());
    CHECK(png[0] == 0x89);
}

TEST_CASE("render: VS16 warning emoji renders as color" * doctest::test_suite("render"))
{
    // Use /bin/cat as shell so nothing writes to the terminal between
    // our inject and our screenshot, avoiding zsh prompt interference.
    // Supply the bundled COLRv1 emoji subset so the emoji renders in color.
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.emojiFontPath = MB_TEST_EMOJI_FONT;
    MBConnection rt(opts);
    REQUIRE(rt.connect());

    rt.wait(300);

    // Inject ⚠️ = U+26A0 (warning sign) + U+FE0F (VS16 emoji presentation selector)
    rt.injectData("\xe2\x9a\xa0\xef\xb8\x8f");
    rt.wait(300);

    // Capture just the 2 cells the emoji occupies (col 0-1, row 0)
    auto png = rt.screenshotPaneRect(0, 0, 0, 2, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "vs16_warning_emoji"));
}

TEST_CASE("render: warning sign without VS16 renders as narrow monochrome" * doctest::test_suite("render"))
{
    // Inconsolata as primary (no U+26A0), DejaVu subset as non-COLR fallback (has U+26A0),
    // no emoji font — so ⚠ must render via the non-COLR fallback path.
    MBConnection::Options opts;
    opts.fontPath = MB_TEST_TEXT_FONT;
    opts.fallbackFontPath = MB_TEST_FALLBACK_FONT;
    opts.shell = "/bin/cat";
    MBConnection rt(opts);
    REQUIRE(rt.connect());

    rt.wait(300);

    // Inject ⚠ without VS16 — renders via non-COLR fallback, 1 cell wide
    rt.injectData("\xe2\x9a\xa0");
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 1, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "warning_no_vs16"));
}

// Helper: options with Inconsolata + DejaVu fallback + emoji font, /bin/cat shell
static MBConnection::Options mixedEmojiOpts()
{
    MBConnection::Options opts;
    opts.fontPath = MB_TEST_TEXT_FONT;
    opts.fallbackFontPath = MB_TEST_FALLBACK_FONT;
    opts.emojiFontPath = MB_TEST_EMOJI_FONT;
    opts.shell = "/bin/cat";
    return opts;
}

TEST_CASE("render: text followed by VS16 emoji" * doctest::test_suite("render"))
{
    // "Hi ⚠️" — ASCII text then a VS16-widened COLR emoji (4 cells: H i space ⚠️)
    MBConnection rt(mixedEmojiOpts());
    REQUIRE(rt.connect());
    rt.wait(300);
    rt.injectData("Hi \xe2\x9a\xa0\xef\xb8\x8f");
    rt.wait(300);
    auto png = rt.screenshotPaneRect(0, 0, 0, 5, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "text_then_vs16_emoji"));
}

TEST_CASE("render: text followed by wide emoji" * doctest::test_suite("render"))
{
    // "Hi 🍄" — ASCII text then an inherently wide COLR emoji (5 cells: H i space 🍄🍄)
    MBConnection rt(mixedEmojiOpts());
    REQUIRE(rt.connect());
    rt.wait(300);
    rt.injectData("Hi \xf0\x9f\x8d\x84");
    rt.wait(300);
    auto png = rt.screenshotPaneRect(0, 0, 0, 5, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "text_then_wide_emoji"));
}

TEST_CASE("render: text followed by non-VS16 warning" * doctest::test_suite("render"))
{
    // "Hi ⚠" — ASCII text then a plain monochrome warning sign via fallback (4 cells)
    MBConnection rt(mixedEmojiOpts());
    REQUIRE(rt.connect());
    rt.wait(300);
    rt.injectData("Hi \xe2\x9a\xa0");
    rt.wait(300);
    auto png = rt.screenshotPaneRect(0, 0, 0, 4, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "text_then_no_vs16_warning"));
}

TEST_CASE("render: wide COLRv1 emoji renders in color" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.emojiFontPath = MB_TEST_EMOJI_FONT;
    MBConnection rt(opts);
    REQUIRE(rt.connect());

    rt.wait(300);

    // Inject 🍄 = U+1F344 (mushroom), inherently wide (wcwidth=2), no VS16 needed
    rt.injectData("\xf0\x9f\x8d\x84");
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 2, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "wide_colr_mushroom"));
}

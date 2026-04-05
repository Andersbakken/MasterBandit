#include <doctest/doctest.h>
#include "RenderTest.h"

// These tests require a GPU and launch mb --test as a child process.
// They are slower than unit tests (~1-2s per test case).
//
// Generate reference images:  MB_UPDATE_REFS=1 ./build/bin/mb-tests "[render]"
// Run rendering tests:        ./build/bin/mb-tests "[render]"

TEST_CASE("render: headless startup and screenshot" * doctest::test_suite("render"))
{
    RenderTest rt({.cols = 40, .rows = 10});
    REQUIRE(rt.connect());

    // Wait for the shell to render initial content
    rt.wait(500);

    auto png = rt.screenshotPng();
    REQUIRE(!png.empty());
    // Basic check: PNG magic bytes
    REQUIRE(png.size() > 8);
    CHECK(png[0] == 0x89);
    CHECK(png[1] == 'P');
    CHECK(png[2] == 'N');
    CHECK(png[3] == 'G');
}

TEST_CASE("render: text injection and cell rect screenshot" * doctest::test_suite("render"))
{
    RenderTest rt({.cols = 40, .rows = 10});
    REQUIRE(rt.connect());

    rt.wait(500);

    rt.sendText("Hello, World!");
    rt.wait(300);

    // Grab a composite screenshot first (should work)
    auto full = rt.screenshotPng();
    REQUIRE(!full.empty());

    // Grab a pane screenshot with cell rect
    auto png = rt.screenshotPaneRect(0, 0, 0, 20, 2);
    REQUIRE(!png.empty());
    CHECK(png[0] == 0x89); // PNG magic
}

TEST_CASE("render: pane screenshot" * doctest::test_suite("render"))
{
    RenderTest rt({.cols = 40, .rows = 10});
    REQUIRE(rt.connect());

    rt.wait(500);

    // Pane 0 is the default first pane
    auto png = rt.screenshotPane(0);
    REQUIRE(!png.empty());
    CHECK(png[0] == 0x89);
}

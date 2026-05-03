#include <doctest/doctest.h>
#include "MBConnection.h"
#include "Utils.h"
#include <glaze/glaze.hpp>
#include <vector>
#include <string>

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
    auto& rt = MBConnection::shared(opts);
    rt.reset();

    rt.wait(300);

    // Inject ⚠️ = U+26A0 (warning sign) + U+FE0F (VS16 emoji presentation selector)
    rt.injectData("\xe2\x9a\xa0\xef\xb8\x8f");
    rt.wait(300);

    // Capture just the 2 cells the emoji occupies (col 0-1, row 0)
    auto png = rt.screenshotPaneRect(0, 0, 0, 2, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "vs16_warning_emoji"));
}

TEST_CASE("render: DEC line drawing (ESC ( 0) produces box characters" * doctest::test_suite("render"))
{
    // Use /bin/cat so the shell doesn't emit a prompt before our inject.
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Two-row box: top = ┌──┐, bottom = └──┘. Emit in DEC graphics mode
    // (ESC ( 0 l q q k) and (ESC ( 0 m q q j), with CR+LF between rows.
    // The emulator translates q→─, l→┌, k→┐, m→└, j→┘ before the cell
    // write, so the renderer draws Unicode box characters via the existing
    // procedural-glyph path (U+2500 series).
    rt.injectData("\x1b(0lqqk\r\nmqqj\x1b(B");
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "dec_line_drawing_box"));
}

TEST_CASE("render: warning sign without VS16 renders as narrow monochrome" * doctest::test_suite("render"))
{
    // Inconsolata as primary (no U+26A0), DejaVu subset as non-COLR fallback (has U+26A0),
    // no emoji font — so ⚠ must render via the non-COLR fallback path.
    MBConnection::Options opts;
    opts.fontPath = MB_TEST_TEXT_FONT;
    opts.fallbackFontPath = MB_TEST_FALLBACK_FONT;
    opts.shell = "/bin/cat";
    auto& rt = MBConnection::shared(opts);
    rt.reset();

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
    auto& rt = MBConnection::shared(mixedEmojiOpts());
    rt.reset();
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
    auto& rt = MBConnection::shared(mixedEmojiOpts());
    rt.reset();
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
    auto& rt = MBConnection::shared(mixedEmojiOpts());
    rt.reset();
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
    auto& rt = MBConnection::shared(opts);
    rt.reset();

    rt.wait(300);

    // Inject 🍄 = U+1F344 (mushroom), inherently wide (wcwidth=2), no VS16 needed
    rt.injectData("\xf0\x9f\x8d\x84");
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 2, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "wide_colr_mushroom"));
}

// ── Kitty graphics rendering ────────────────────────────────────────────────

// Helper: build a kitty graphics APC escape with base64-encoded RGBA payload
static std::string kittyGfxEscape(const std::string& params, const std::vector<uint8_t>& payload)
{
    std::string cmd = "\x1b_G" + params;
    if (!payload.empty()) {
        cmd += ";";
        cmd += base64::encode(payload.data(), payload.size());
    }
    cmd += "\x1b\\";
    return cmd;
}

// Helper: solid RGBA image data
static std::vector<uint8_t> solidRGBA(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    std::vector<uint8_t> data(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < data.size(); i += 4) {
        data[i] = r; data[i+1] = g; data[i+2] = b; data[i+3] = a;
    }
    return data;
}

TEST_CASE("render: kitty graphics solid red image" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // 20x20 solid red image → should occupy ~2x1 cells at default font size
    auto px = solidRGBA(20, 20, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=20,v=20,q=2", px));
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_solid_red"));
}

TEST_CASE("render: kitty graphics image with cell scaling" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // 4x4 green image scaled to 10x3 cells
    auto px = solidRGBA(4, 4, 0, 200, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=10,r=3,q=2", px));
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 12, 4);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_scaled_green"));
}

TEST_CASE("render: kitty graphics source rect crop" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // 4x4 image: top-left red, top-right green, bottom-left blue, bottom-right white
    std::vector<uint8_t> px = {
        255,0,0,255, 255,0,0,255, 0,255,0,255, 0,255,0,255,
        255,0,0,255, 255,0,0,255, 0,255,0,255, 0,255,0,255,
        0,0,255,255, 0,0,255,255, 255,255,255,255, 255,255,255,255,
        0,0,255,255, 0,0,255,255, 255,255,255,255, 255,255,255,255,
    };
    // Crop to the top-right 2x2 (green quadrant): x=2,y=0,w=2,h=2
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,x=2,y=0,w=2,h=2,c=4,r=2,q=2", px));
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 6, 3);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_crop_green"));
}

TEST_CASE("render: kitty graphics two images vertically" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // First image: 4x4 solid red, scaled to 4x2 cells
    auto px1 = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=4,r=2,q=2", px1));
    rt.wait(200);

    // Move to next line before second image
    rt.injectData("\r\n");

    // Second image: 4x4 solid blue, scaled to 4x2 cells (placed below first)
    auto px2 = solidRGBA(4, 4, 0, 0, 255);
    rt.injectData(kittyGfxEscape("a=T,i=2,f=32,s=4,v=4,c=4,r=2,q=2", px2));
    rt.wait(300);

    // Screenshot: 6 cols x 5 rows — should show red on top, blue below
    auto png = rt.screenshotPaneRect(0, 0, 0, 6, 5);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_two_vertical"));
}

TEST_CASE("render: kitty graphics multiple placements of same image" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Transmit a 4x4 solid red image (transmit only, don't display)
    auto px = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=t,i=1,f=32,s=4,v=4,q=2", px));
    rt.wait(200);

    // Place at row 0 with placement ID 1 (2x1 cells)
    rt.injectData(kittyGfxEscape("a=p,i=1,p=1,c=2,r=1,q=2", {}));
    rt.wait(200);

    // Move cursor down 2 rows
    rt.injectData("\r\n\n");

    // Place again at row 2 with placement ID 2 (2x1 cells)
    rt.injectData(kittyGfxEscape("a=p,i=1,p=2,c=2,r=1,q=2", {}));
    rt.wait(300);

    // Screenshot: should show two red rectangles at rows 0 and 2
    auto png = rt.screenshotPaneRect(0, 0, 0, 4, 4);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_multiple_placements"));
}

TEST_CASE("render: kitty graphics placement replacement" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Transmit a 4x4 solid blue image
    auto px = solidRGBA(4, 4, 0, 0, 255);
    rt.injectData(kittyGfxEscape("a=t,i=1,f=32,s=4,v=4,q=2", px));
    rt.wait(200);

    // Place at row 0 with placement ID 5
    rt.injectData(kittyGfxEscape("a=p,i=1,p=5,c=3,r=1,q=2", {}));
    rt.wait(200);

    // Move down and replace same placement ID 5 at new position (row 3)
    rt.injectData("\r\n\n\n");
    rt.injectData(kittyGfxEscape("a=p,i=1,p=5,c=3,r=1,q=2", {}));
    rt.wait(300);

    // Screenshot: blue should appear only at row 3, row 0 should be empty
    auto png = rt.screenshotPaneRect(0, 0, 0, 5, 5);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_placement_replace"));
}

TEST_CASE("render: kitty graphics sub-cell pixel offset" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Hide cursor so it doesn't affect the reference image
    rt.injectData("\x1b[?25l");

    // Place two copies of the same image: one without offset, one with.
    // Transmit a solid red 4x4 image
    auto px = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=t,i=1,f=32,s=4,v=4,q=2", px));
    rt.wait(200);

    // Place at row 0 with no sub-cell offset (placement 1)
    rt.injectData(kittyGfxEscape("a=p,i=1,p=1,c=3,r=2,q=2", {}));
    rt.wait(100);

    // Move to row 3
    rt.injectData("\r\n\n");

    // Place at row 3 with X=10 Y=10 sub-cell offset (placement 2)
    rt.injectData(kittyGfxEscape("a=p,i=1,p=2,c=3,r=2,X=10,Y=10,q=2", {}));
    rt.wait(300);

    // Screenshot: two red rectangles, second one visibly shifted right and down
    auto png = rt.screenshotPaneRect(0, 0, 0, 5, 6);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_subcell_offset"));
}

TEST_CASE("render: kitty graphics delete at cursor removes image" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    rt.injectData("\x1b[?25l"); // hide cursor

    // Place a red image at row 0 (2x1 cells)
    auto px1 = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=2,r=1,q=2", px1));
    rt.wait(100);

    // Move down and place a blue image at row 2 (2x1 cells)
    rt.injectData("\r\n\n");
    auto px2 = solidRGBA(4, 4, 0, 0, 255);
    rt.injectData(kittyGfxEscape("a=T,i=2,f=32,s=4,v=4,c=2,r=1,q=2", px2));
    rt.wait(100);

    // Move cursor to row 0 and delete at cursor (should remove red image)
    rt.injectData("\x1b[H");
    rt.injectData(kittyGfxEscape("a=d,d=c", {}));
    rt.wait(300);

    // Screenshot: red should be gone, blue should remain at row 2
    auto png = rt.screenshotPaneRect(0, 0, 0, 4, 4);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_delete_at_cursor"));
}

TEST_CASE("render: kitty graphics frame composition" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    rt.injectData("\x1b[?25l"); // hide cursor

    // Transmit+display a 4x4 solid red image (root frame), scaled to 4x2 cells
    auto px = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=4,r=2,q=2", px));
    rt.wait(200);

    // Add frame 2: 4x4 solid blue
    auto frame2 = solidRGBA(4, 4, 0, 0, 255);
    rt.injectData(kittyGfxEscape("a=f,i=1,f=32,s=4,v=4,z=5000,q=2", frame2));
    rt.wait(100);

    // Compose: copy 2x2 from frame 1 (red) at src (0,0) onto frame 2 (blue) at dst (1,1)
    // Result: frame 2 is blue with a red 2x2 patch at (1,1)
    rt.injectData(kittyGfxEscape("a=c,i=1,r=1,c=2,w=2,h=2,X=0,Y=0,x=1,y=1,C=1,q=2", {}));
    rt.wait(100);

    // Jump to frame 2 via animation control: c= sets current frame (1-based)
    rt.injectData(kittyGfxEscape("a=a,i=1,c=2", {}));
    rt.wait(500);

    // Screenshot: should show the composed frame — mostly blue with a red patch
    auto png = rt.screenshotPaneRect(0, 0, 0, 6, 3);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_frame_composition"));
}

TEST_CASE("render: kitty graphics z-layering below text" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Write white text on default (black) background
    rt.injectData("\x1b[37mHello\x1b[0m");
    rt.wait(100);

    // Move cursor home, place a red image with z=-1 (below text) covering the text area
    rt.injectData("\x1b[H");
    auto px = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=6,r=1,z=-1,q=2", px));
    rt.wait(300);

    // Screenshot: text should be visible on top of the red image
    auto png = rt.screenshotPaneRect(0, 0, 0, 8, 2);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_z_below_text"));
}

TEST_CASE("render: kitty graphics z-layering above text" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // Write white text
    rt.injectData("\x1b[37mHello\x1b[0m");
    rt.wait(100);

    // Move cursor home, place a red image with z=0 (above text, default)
    rt.injectData("\x1b[H");
    auto px = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=6,r=1,q=2", px));
    rt.wait(300);

    // Screenshot: red image should cover the text
    auto png = rt.screenshotPaneRect(0, 0, 0, 8, 2);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_z_above_text"));
}

TEST_CASE("render: italic text" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    rt.injectData("\x1b[?25l"); // hide cursor
    rt.injectData("Normal \x1b[3mItalic\x1b[0m");
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 13, 1);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "italic_text"));
}

// Pull obs.frames_presented out of the queryStats JSON; returns 0 on parse failure.
static uint64_t framesPresented(const std::string& statsJson)
{
    glz::generic j;
    if (glz::read_json(j, statsJson) != 0) return 0;
    auto* root = std::get_if<glz::generic::object_t>(&j.data);
    if (!root) return 0;
    auto it = root->find("obs");
    if (it == root->end()) return 0;
    auto* obs = std::get_if<glz::generic::object_t>(&it->second.data);
    if (!obs) return 0;
    auto fpIt = obs->find("frames_presented");
    if (fpIt == obs->end()) return 0;
    auto* d = std::get_if<double>(&fpIt->second.data);
    if (!d) return 0;
    return static_cast<uint64_t>(*d);
}

TEST_CASE("render: kitty animated image advances frames over time" * doctest::test_suite("render"))
{
    // Regression: tickAnimations() advances the live image's currentFrameIndex
    // and frameShownAt but used not to republish the snapshot, so the renderer
    // kept reading the prior frame's RGBA pointer/index. Visually, GIFs froze
    // until something else (typing, scroll, resize) republished the snapshot.
    //
    // This test plants a 2-frame animation with a small inter-frame gap, then
    // waits long enough that several frames must have advanced. We assert
    // both that frames_presented advanced AND that the rendered pixel data
    // changed at least once during the wait — the latter is the strong check
    // (frames_presented advancing alone could be busy-loop fallout from the
    // scheduler reacting to the stale dueAt; only a real frame swap shows up
    // in pixels).
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    rt.injectData("\x1b[?25l"); // hide cursor — its blink would also tick frames_presented

    // Root frame: solid red 4x4, displayed at 4x2 cells.
    auto red = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=4,r=2,q=2", red));
    rt.wait(150);

    // Frame 2: solid blue, 80ms gap.
    auto blue = solidRGBA(4, 4, 0, 0, 255);
    rt.injectData(kittyGfxEscape("a=f,i=1,f=32,s=4,v=4,z=80,q=2", blue));
    rt.wait(50);

    // Start animation (s=3 → Running with loop).
    rt.injectData(kittyGfxEscape("a=a,i=1,s=3", {}));
    // Don't wait long here — we want to capture the state right after start
    // so the screenshot diff has the full animation window to evolve.
    rt.wait(20);

    auto pngStart = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    REQUIRE(!pngStart.empty());
    uint64_t framesBefore = framesPresented(rt.queryStats());

    // Sample three captures spaced through the animation cycle. With an 80ms
    // gap, the cycle is 160ms — sampling at 80ms / 200ms / 400ms catches the
    // image at distinct phases regardless of timing jitter.
    rt.wait(80);
    auto pngMid1 = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    rt.wait(120);
    auto pngMid2 = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    rt.wait(200);
    auto pngEnd  = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    uint64_t framesAfter = framesPresented(rt.queryStats());

    // frames_presented must have advanced — the animation timer should have
    // fired ~5 times in 400ms with an 80ms gap, plus screenshot-driven renders.
    CHECK(framesAfter > framesBefore + 2);

    // Strong check: pixel data must have flipped at some point during the
    // wait. Red vs blue → channel diff well above the JPEG/AA tolerance.
    int d1 = MBConnection::comparePng(pngStart, pngMid1);
    int d2 = MBConnection::comparePng(pngStart, pngMid2);
    int d3 = MBConnection::comparePng(pngStart, pngEnd);
    int maxDiff = std::max(d1, std::max(d2, d3));
    CHECK(maxDiff > 50);
}

TEST_CASE("render: kitty image survives column resize" * doctest::test_suite("render"))
{
    // Regression: pushVisibleRowToScrollback trimmed image-bearing rows down
    // to their last non-zero wc cell. Image cells stamp a CellExtra without
    // writing wc, so the row was trimmed to length 0, the per-row extras
    // slice (col < rowLen) dropped them, and the image vanished from screen
    // after any reflow. Reproduces by injecting a column-shrink resize
    // through the active terminal's resizeToRect path via a font-size action.
    //
    // Unit-test coverage in test_kitty_graphics.cpp asserts the CellExtra
    // survives at the Document level. This end-to-end check makes sure the
    // pixel actually still gets drawn after the resize lands on the renderer.
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    rt.injectData("\x1b[?25l");

    auto red = solidRGBA(4, 4, 255, 0, 0);
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=4,v=4,c=4,r=2,q=2", red));
    rt.wait(300);

    // Capture the image rendered at the original column count.
    auto pngBefore = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    REQUIRE(!pngBefore.empty());

    // Bumping the font size triggers a pixel-rect reflow on every pane
    // (cellPixelWidth grows → cols shrink). That exercises the same
    // pushVisibleRowToScrollback path that the real window-resize takes.
    rt.sendAction("increase_font_size");
    rt.wait(400);
    rt.sendAction("decrease_font_size");
    rt.wait(400);

    // After the reflow round-trip the image must still be visible. Without
    // the trim fix in Document::pushVisibleRowToScrollback, the cell extras
    // would be dropped on the way through scrollback and the image area
    // would render as the default (black) background.
    auto pngAfter = rt.screenshotPaneRect(0, 0, 0, 4, 2);
    REQUIRE(!pngAfter.empty());

    // Sanity: pre-reflow we had a clearly red image (>>50 diff against a
    // blank black rect). Post-reflow it should still be red (pre/post
    // diff small). Compare to pngBefore which we know is "image present".
    int diff = MBConnection::comparePng(pngBefore, pngAfter);
    CHECK(diff < 30);
}

TEST_CASE("render: kitty graphics two-color checkerboard" * doctest::test_suite("render"))
{
    MBConnection::Options opts;
    opts.shell = "/bin/cat";
    opts.cols = 40;
    opts.rows = 10;
    auto& rt = MBConnection::shared(opts);
    rt.reset();
    rt.wait(300);

    // 2x2 checkerboard: red, blue, blue, red
    std::vector<uint8_t> px = {
        255, 0, 0, 255,   0, 0, 255, 255,
        0, 0, 255, 255,   255, 0, 0, 255,
    };
    rt.injectData(kittyGfxEscape("a=T,i=1,f=32,s=2,v=2,c=4,r=2,q=2", px));
    rt.wait(300);

    auto png = rt.screenshotPaneRect(0, 0, 0, 6, 3);
    REQUIRE(!png.empty());
    CHECK(rt.matchesReference(png, "kitty_checkerboard"));
}

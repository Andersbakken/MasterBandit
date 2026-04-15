#include <doctest/doctest.h>
#include "TestTerminal.h"
#include "Utils.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>

// Helper: TestTerminal with cell pixel size callbacks for kitty graphics
struct GraphicsTerminal : TestTerminal {
    GraphicsTerminal(int cols = 80, int rows = 24)
        : TestTerminal(cols, rows)
    {
        auto& cbs = term.callbacks();
        cbs.cellPixelWidth  = []() -> float { return 10.0f; };
        cbs.cellPixelHeight = []() -> float { return 20.0f; };
    }

    void apc(const std::string& s)
    {
        feed("\x1b_" + s + "\x1b\\");
    }

    // Send a kitty graphics command with optional base64 payload
    void gfx(const std::string& params, const std::vector<uint8_t>& payload = {})
    {
        std::string cmd = "\x1b_G" + params;
        if (!payload.empty()) {
            cmd += ";";
            cmd += base64::encode(payload.data(), payload.size());
        }
        cmd += "\x1b\\";
        feed(cmd);
    }

    // Create a solid-color RGBA image (1 pixel repeated)
    static std::vector<uint8_t> solidRGBA(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        std::vector<uint8_t> data(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < data.size(); i += 4) {
            data[i] = r; data[i+1] = g; data[i+2] = b; data[i+3] = a;
        }
        return data;
    }

    const CellExtra* extra(int col, int row) const
    {
        return term.grid().getExtra(col, row);
    }
};

// ── Query support ───────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: query responds OK for direct transmission")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 255, 0, 0);
    t.gfx("a=q,i=1,f=32,s=1,v=1,t=d", px);
    CHECK(t.output().find("OK") != std::string::npos);
    CHECK(t.output().find("i=1") != std::string::npos);
    // Query should not store the image
    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("kitty graphics: query with q=2 sends no response")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 255, 0, 0);
    t.gfx("a=q,i=1,f=32,s=1,v=1,q=2", px);
    CHECK(t.output().empty());
}

// ── Transmit and display ────────────────────────────────────────────────────

TEST_CASE("kitty graphics: transmit+display places image in grid")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 0, 255, 0);
    t.gfx("a=T,i=5,f=32,s=10,v=20,q=2", px);

    // Image should be in registry
    auto& reg = t.term.imageRegistry();
    REQUIRE(reg.count(5));
    auto& img = *reg.at(5);
    CHECK(img.pixelWidth == 10);
    CHECK(img.pixelHeight == 20);
    CHECK(img.id == 5);

    // Should be placed in grid — cell pixel size is 10x20, so 1 col x 1 row
    CHECK(img.cellWidth == 1);
    CHECK(img.cellHeight == 1);

    // CellExtra should reference the image
    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 5);
}

TEST_CASE("kitty graphics: transmit+display calculates cell dimensions from pixels")
{
    GraphicsTerminal t(40, 20);
    // 25x45 image with 10x20 cell size → ceil(25/10)=3 cols, ceil(45/20)=3 rows
    auto px = GraphicsTerminal::solidRGBA(25, 45, 0, 0, 255);
    t.gfx("a=T,i=1,f=32,s=25,v=45,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.cellWidth == 3);
    CHECK(img.cellHeight == 3);
}

TEST_CASE("kitty graphics: explicit c= and r= override calculated dimensions")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 128, 128, 128);
    t.gfx("a=T,i=1,f=32,s=10,v=10,c=5,r=3,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.cellWidth == 5);
    CHECK(img.cellHeight == 3);
}

TEST_CASE("kitty graphics: transmit-only does not place in grid")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=10,q=2", px);

    // Image should be in registry
    CHECK(t.term.imageRegistry().count(1));

    // But no cell extras placed (cursor didn't move)
    const CellExtra* ex = t.extra(0, 0);
    CHECK((!ex || ex->imageId == 0));
}

// ── Auto-assigned image ID ──────────────────────────────────────────────────

TEST_CASE("kitty graphics: auto-assign ID when i=0")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,f=32,s=1,v=1,q=2", px);

    auto& reg = t.term.imageRegistry();
    CHECK(reg.size() == 1);
    // Should have an auto-assigned ID > 0
    CHECK(reg.begin()->second->id > 0);
}

// ── RGB format ──────────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: RGB24 format converted to RGBA")
{
    GraphicsTerminal t;
    std::vector<uint8_t> rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
    t.gfx("a=t,i=1,f=24,s=2,v=2,q=2", rgb);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.pixelWidth == 2);
    CHECK(img.pixelHeight == 2);
    // First pixel: R=255, G=0, B=0, A=255
    CHECK(img.rgba[0] == 255);
    CHECK(img.rgba[1] == 0);
    CHECK(img.rgba[2] == 0);
    CHECK(img.rgba[3] == 255);
}

// ── Chunked transfer ────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: chunked transfer reassembles correctly")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 200, 100, 50);
    std::string b64 = base64::encode(px.data(), px.size());

    // Split into two chunks
    size_t half = b64.size() / 2;
    std::string chunk1 = b64.substr(0, half);
    std::string chunk2 = b64.substr(half);

    // First chunk with m=1
    t.feed("\x1b_Ga=t,i=7,f=32,s=2,v=2,m=1,q=2;" + chunk1 + "\x1b\\");
    CHECK(t.term.imageRegistry().empty()); // not yet complete

    // Second chunk with m=0
    t.feed("\x1b_Gm=0;" + chunk2 + "\x1b\\");
    REQUIRE(t.term.imageRegistry().count(7));
    auto& img = *t.term.imageRegistry().at(7);
    CHECK(img.pixelWidth == 2);
    CHECK(img.pixelHeight == 2);
}

TEST_CASE("kitty graphics: chunked transfer preserves I= image number")
{
    // Regression test: kitty icat transmits large animated GIFs by chunking
    // the root frame and targeting subsequent a=f/a=a commands via I= (image
    // number). A prior bug dropped I= during chunk reassembly, storing the
    // image with imageNumber=0 and causing every later a=a to resolve to no
    // image — animations loaded but never started.
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 10, 20, 30);
    std::string b64 = base64::encode(px.data(), px.size());
    size_t half = b64.size() / 2;

    // Root frame chunked over two APCs, identified only by I=
    t.feed("\x1b_Ga=T,I=1061220708,f=32,s=2,v=2,m=1,q=2;" + b64.substr(0, half) + "\x1b\\");
    t.feed("\x1b_Gm=0;" + b64.substr(half) + "\x1b\\");

    auto& reg = t.term.imageRegistry();
    REQUIRE(reg.size() == 1);
    auto& img = *reg.begin()->second;
    CHECK(img.imageNumber == 1061220708);
    // findImageByNumber must be able to resolve back to the stored image,
    // which is how a=f / a=a find their target when i= is not set.
    CHECK(t.term.findImageByNumber(1061220708) == img.id);
}

// ── Delete ──────────────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: delete all visible images")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    // Use a=T so images are placed in the grid (visible)
    t.gfx("a=T,i=1,f=32,s=1,v=1,q=2", px);
    t.feed("\r\n");
    t.gfx("a=T,i=2,f=32,s=1,v=1,q=2", px);
    CHECK(t.term.imageRegistry().size() == 2);

    // d=A frees visible image data
    t.gfx("a=d,d=A");
    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("kitty graphics: delete visible does not remove non-visible images")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=T,i=1,f=32,s=1,v=1,q=2", px); // visible (placed in grid)
    t.gfx("a=t,i=2,f=32,s=1,v=1,q=2", px); // not visible (transmit only)
    CHECK(t.term.imageRegistry().size() == 2);

    t.gfx("a=d,d=A");
    CHECK(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().count(2));
}

TEST_CASE("kitty graphics: delete by image ID")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,i=1,f=32,s=1,v=1,q=2", px);
    t.gfx("a=t,i=2,f=32,s=1,v=1,q=2", px);
    CHECK(t.term.imageRegistry().size() == 2);

    t.gfx("a=d,d=i,i=1");
    CHECK(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().count(2));
}

TEST_CASE("kitty graphics: RIS clears image registry")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,i=1,f=32,s=1,v=1,q=2", px);
    t.gfx("a=t,i=2,f=32,s=1,v=1,q=2", px);
    REQUIRE(t.term.imageRegistry().size() == 2);

    t.esc("c");  // RIS

    CHECK(t.term.imageRegistry().empty());

    // After RIS, a=t with i=0 (auto-assign) should start at 1 again.
    t.gfx("a=t,f=32,s=1,v=1,q=2", px);
    REQUIRE(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().count(1));
}

TEST_CASE("kitty graphics: delete animation frames")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.extraFrames.size() == 2);

    // Delete frames
    t.gfx("a=d,d=f,i=1");
    CHECK(img.extraFrames.empty());
    CHECK(img.animationState == TerminalEmulator::ImageEntry::Stopped);
    CHECK(img.currentFrameIndex == 0);
    // Image itself still exists
    CHECK(t.term.imageRegistry().count(1));
}

// ── Put (display previously transmitted image) ──────────────────────────────

TEST_CASE("kitty graphics: put displays previously transmitted image")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 0, 0, 255);
    t.gfx("a=t,i=3,f=32,s=10,v=10,q=2", px); // transmit only

    // No placement yet
    const CellExtra* ex = t.extra(0, 0);
    CHECK((!ex || ex->imageId == 0));

    // Put
    t.clearOutput();
    t.gfx("a=p,i=3");
    CHECK(t.output().find("OK") != std::string::npos);

    ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 3);
}

// ── Cursor movement control ─────────────────────────────────────────────────

TEST_CASE("kitty graphics: C=1 does not move cursor")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 40, 0, 255, 0); // 1x2 cells
    t.gfx("a=T,i=1,f=32,s=10,v=40,C=1,q=2", px);

    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("kitty graphics: C=0 moves cursor to last row of image")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 40, 0, 255, 0); // 1x2 cells
    t.gfx("a=T,i=1,f=32,s=10,v=40,C=0,q=2", px);

    // Cursor on last row of image (row 1), column past right edge
    CHECK(t.term.cursorY() == 1);
    CHECK(t.term.cursorX() == 1); // ceil(10/10) = 1 col
}

// ── Response suppression ────────────────────────────────────────────────────

TEST_CASE("kitty graphics: q=1 suppresses OK but sends errors")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);

    // OK should be suppressed
    t.gfx("a=t,i=1,f=32,s=1,v=1,q=1", px);
    CHECK(t.output().empty());

    // Error should still be sent
    t.clearOutput();
    t.gfx("a=p,i=999,q=1"); // nonexistent image
    CHECK(t.output().find("ENOENT") != std::string::npos);
}

TEST_CASE("kitty graphics: no response when id=0")
{
    GraphicsTerminal t;
    t.clearOutput();
    t.gfx("a=p"); // put with no id
    CHECK(t.output().empty());
}

// ── Animation ───────────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: frame load adds extra frames")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto frame1 = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=100,q=2", frame1);

    auto frame2 = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=50,q=2", frame2);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.extraFrames.size() == 2);
    CHECK(img.extraFrames[0].gap == 100);
    CHECK(img.extraFrames[1].gap == 50);
}

TEST_CASE("kitty graphics: frame load with i=0 uses last image")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=10,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,f=32,s=2,v=2,z=80,q=2", frame); // no i=

    auto& img = *t.term.imageRegistry().at(10);
    CHECK(img.extraFrames.size() == 1);
}

TEST_CASE("kitty graphics: animation control starts/stops animation")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.animationState == TerminalEmulator::ImageEntry::Stopped);

    // Start animation (s=3 → Running)
    t.gfx("a=a,i=1,s=3");
    CHECK(img.animationState == TerminalEmulator::ImageEntry::Running);
    CHECK(img.hasAnimation());

    // Stop animation (s=1 → Stopped)
    t.gfx("a=a,i=1,s=1");
    CHECK(img.animationState == TerminalEmulator::ImageEntry::Stopped);
    CHECK_FALSE(img.hasAnimation());
}

TEST_CASE("kitty graphics: tickAnimations advances frame")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=1,v=1,q=2", px);

    auto f1 = GraphicsTerminal::solidRGBA(1, 1, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=1,v=1,z=1,q=2", f1); // 1ms gap

    auto f2 = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=1,v=1,z=1,q=2", f2); // 1ms gap

    // Set root frame gap to 10ms and start animation
    t.gfx("a=a,i=1,r=1,z=10"); // set gap for frame 1 (root) to 10ms
    t.gfx("a=a,i=1,s=3");      // start running
    const auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.currentFrameIndex == 0);

    // Force frameShownAt into the past to avoid wall-clock timing dependency
    uint64_t shownBefore = TerminalEmulator::mono() - 1000;
    REQUIRE(t.term.setImageFrameShownAtForTest(1, shownBefore));
    REQUIRE(img.hasAnimation());
    t.term.tickAnimations();
    // Ticking an animated image with elapsed > gap must process this image
    // and update its frameShownAt timestamp. (We can't assert which index it
    // lands on — with a 3-frame loop and 1000ms elapsed, the final index is
    // determined by the gap math and may equal the starting index.
    // frameGeneration is no longer bumped on a tick; it tracks content edits.)
    CHECK(img.frameShownAt > shownBefore);
}

// ── Delta frame compositing ─────────────────────────────────────────────────

TEST_CASE("kitty graphics: partial frame composites onto c=1 root frame")
{
    GraphicsTerminal t;
    // 4x4 red base
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    // 2x2 green patch at offset (1,1), composited onto the root frame (c=1)
    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    std::string b64 = base64::encode(patch.data(), patch.size());
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,c=1,x=1,y=1,z=40,q=2;" + b64 + "\x1b\\");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);

    auto& frame = img.extraFrames[0];
    // Pixel at (0,0) should be red (inherited from root)
    CHECK(frame.rgba[0] == 255); // R
    CHECK(frame.rgba[1] == 0);   // G

    // Pixel at (1,1) should be green (from patch)
    size_t idx = (1 * 4 + 1) * 4;
    CHECK(frame.rgba[idx] == 0);     // R
    CHECK(frame.rgba[idx + 1] == 255); // G

    // Pixel at (3,3) should be red (untouched)
    size_t idx2 = (3 * 4 + 3) * 4;
    CHECK(frame.rgba[idx2] == 255);    // R
    CHECK(frame.rgba[idx2 + 1] == 0);  // G
}

TEST_CASE("kitty graphics: partial frame with c=0 treats base as transparent")
{
    // Per kitty spec, c=0 or unset means the new frame is standalone — its
    // base is the Y= background color (transparent black by default), NOT
    // the previous frame or the root.
    GraphicsTerminal t;
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    std::string b64 = base64::encode(patch.data(), patch.size());
    // No c= key: standalone frame
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,x=1,y=1,z=40,q=2;" + b64 + "\x1b\\");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);
    auto& frame = img.extraFrames[0];

    // Pixel at (0,0) should be transparent black (no base) — not red.
    CHECK(frame.rgba[0] == 0); // R
    CHECK(frame.rgba[1] == 0); // G
    CHECK(frame.rgba[2] == 0); // B
    CHECK(frame.rgba[3] == 0); // A

    // Pixel at (1,1) should be green (opaque, from patch)
    size_t idx = (1 * 4 + 1) * 4;
    CHECK(frame.rgba[idx + 0] == 0);   // R
    CHECK(frame.rgba[idx + 1] == 255); // G
    CHECK(frame.rgba[idx + 3] == 255); // A
}

TEST_CASE("kitty graphics: a=f C=1 overwrites base instead of alpha-blending")
{
    // a=f reuses the C= key as composition mode (0=alpha-blend, 1=overwrite),
    // matching kitty's reference (graphics.h unions cursor_movement with
    // compose_mode). icat's SetCompositionMode writes C=.
    GraphicsTerminal t;
    // 2x2 opaque white root frame
    auto base = GraphicsTerminal::solidRGBA(2, 2, 255, 255, 255);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", base);

    // 2x2 half-transparent green patch, composed onto root (c=1) with C=1 (overwrite)
    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0, 128);
    std::string b64 = base64::encode(patch.data(), patch.size());
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,c=1,C=1,z=40,q=2;" + b64 + "\x1b\\");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);
    auto& frame = img.extraFrames[0];

    // With overwrite (C=1), every pixel must be the source verbatim
    // (0, 255, 0, 128) — not blended with the white base.
    for (size_t px = 0; px < 4; ++px) {
        size_t i = px * 4;
        CHECK(frame.rgba[i + 0] == 0);   // R
        CHECK(frame.rgba[i + 1] == 255); // G
        CHECK(frame.rgba[i + 2] == 0);   // B
        CHECK(frame.rgba[i + 3] == 128); // A
    }

    // Contrast: default (C=0, alpha-blend) path composed onto the same root
    // should pick up colour from the white base, not be verbatim source.
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,c=1,z=40,q=2;" + b64 + "\x1b\\");
    auto& blendedFrame = img.extraFrames[1];
    CHECK(blendedFrame.rgba[0] > 0 );   // R picked up from white
    CHECK(blendedFrame.rgba[0] < 255);
    CHECK(blendedFrame.rgba[2] > 0 );   // B picked up from white
    CHECK(blendedFrame.rgba[2] < 255);
}

TEST_CASE("kitty graphics: a=f with r=1 edits the root frame in place")
{
    // Kitty's a=f with r=N specifies an existing frame to edit rather than
    // appending. r=1 targets the root frame (img.rgba); r=2..N+1 targets an
    // entry in extraFrames. See kitty graphics.c:1559.
    GraphicsTerminal t;
    auto rootPx = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0); // red
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", rootPx);
    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.extraFrames.empty());

    // Edit root (r=1) with a 2x2 opaque blue patch, overwrite
    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);
    std::string b64 = base64::encode(patch.data(), patch.size());
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,r=1,C=1,q=2;" + b64 + "\x1b\\");

    // No new frame appended; root content is now blue.
    CHECK(img.extraFrames.empty());
    CHECK(img.rgba[0] == 0);   // R
    CHECK(img.rgba[2] == 255); // B
}

TEST_CASE("kitty graphics: a=f with r=N edits an existing extra frame")
{
    GraphicsTerminal t;
    auto root = GraphicsTerminal::solidRGBA(2, 2, 10, 10, 10);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", root);

    // Add two extra frames (red, green)
    auto frame2 = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame2);
    auto frame3 = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame3);

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 2);

    // Edit frame 2 (extraFrames[0] — the red one) with a blue overwrite
    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);
    std::string b64 = base64::encode(patch.data(), patch.size());
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,r=2,C=1,z=80,q=2;" + b64 + "\x1b\\");

    // Still two extra frames — no new one appended.
    CHECK(img.extraFrames.size() == 2);
    // Frame 2 is now blue and gap updated.
    CHECK(img.extraFrames[0].rgba[0] == 0);   // R
    CHECK(img.extraFrames[0].rgba[2] == 255); // B
    CHECK(img.extraFrames[0].gap == 80);
    // Frame 3 untouched.
    CHECK(img.extraFrames[1].rgba[1] == 255); // G
}

TEST_CASE("kitty graphics: a=f Y= fills base with packed RRGGBBAA")
{
    // When c=0 (no base frame specified), kitty fills the base with the
    // Y= background color (32-bit packed RRGGBBAA) before pasting the new
    // pixels. See kitty graphics.c:1462-1476.
    GraphicsTerminal t;
    auto root = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0); // red (irrelevant — c=0)
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", root);

    // 1x1 opaque green patch pasted at (2, 2); base filled with magenta/0xff
    // packed as 0xFF00FFFF (R=255, G=0, B=255, A=255).
    auto patch = GraphicsTerminal::solidRGBA(1, 1, 0, 255, 0);
    std::string b64 = base64::encode(patch.data(), patch.size());
    // Y=4278255615  == 0xFF00FFFF
    t.feed("\x1b_Ga=f,i=1,f=32,s=1,v=1,x=2,y=2,Y=4278255615,q=2;" + b64 + "\x1b\\");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);
    auto& f = img.extraFrames[0];

    // Pixel at (0,0): untouched base → magenta
    CHECK(f.rgba[0] == 255); // R
    CHECK(f.rgba[1] == 0);   // G
    CHECK(f.rgba[2] == 255); // B
    CHECK(f.rgba[3] == 255); // A

    // Pixel at (2,2): the opaque green patch
    size_t idx = (2 * 4 + 2) * 4;
    CHECK(f.rgba[idx + 0] == 0);   // R
    CHECK(f.rgba[idx + 1] == 255); // G
    CHECK(f.rgba[idx + 2] == 0);   // B
}

TEST_CASE("kitty graphics: chunked transfer preserves t= transmission type")
{
    // Chunked a=T with t=f (file) must preserve the transmission type through
    // reassembly so the terminal reads the file rather than treating the
    // payload as direct base64.
    GraphicsTerminal t;
    // Write raw RGBA pixels to a temp file.
    auto px = GraphicsTerminal::solidRGBA(2, 2, 77, 88, 99, 255);
    char tmpl[] = "/tmp/mb_kitty_chunk_t_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    auto written = write(fd, px.data(), px.size());
    REQUIRE(written == static_cast<ssize_t>(px.size()));
    close(fd);

    // Send the filename base64-encoded, split across two chunks.
    std::string b64 = base64::encode(
        reinterpret_cast<const uint8_t*>(tmpl), std::strlen(tmpl));
    size_t half = b64.size() / 2;
    t.feed("\x1b_Ga=T,i=9,f=32,s=2,v=2,t=f,m=1,q=2;" + b64.substr(0, half) + "\x1b\\");
    CHECK(t.term.imageRegistry().empty()); // not yet complete
    t.feed("\x1b_Gm=0;" + b64.substr(half) + "\x1b\\");

    REQUIRE(t.term.imageRegistry().count(9));
    auto& img = *t.term.imageRegistry().at(9);
    CHECK(img.pixelWidth == 2);
    CHECK(img.pixelHeight == 2);
    // Confirm the file was actually read — first pixel should be 77,88,99,255.
    CHECK(img.rgba[0] == 77);
    CHECK(img.rgba[1] == 88);
    CHECK(img.rgba[2] == 99);
    CHECK(img.rgba[3] == 255);

    unlink(tmpl);
}

// ── Image number (I=) ───────────────────────────────────────────────────────

TEST_CASE("kitty graphics: image number stores and retrieves")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,I=42,f=32,s=1,v=1,q=2", px);

    auto& reg = t.term.imageRegistry();
    CHECK(reg.size() == 1);
    auto& img = *reg.begin()->second;
    CHECK(img.imageNumber == 42);
}

TEST_CASE("kitty graphics: put by image number")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 0, 0, 255);
    t.gfx("a=t,I=99,f=32,s=10,v=10,q=2", px);

    uint32_t imgId = t.term.imageRegistry().begin()->second->id;

    t.clearOutput();
    t.gfx("a=p,I=99");
    CHECK(t.output().find("OK") != std::string::npos);

    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == imgId);
}

TEST_CASE("kitty graphics: image number lookup returns most recent")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,I=7,f=32,s=1,v=1,q=2", px); // first
    t.gfx("a=t,I=7,f=32,s=1,v=1,q=2", px); // second (same number, different auto ID)

    auto& reg = t.term.imageRegistry();
    CHECK(reg.size() == 2);

    // Both have I=7; findImageByNumber should return the higher ID (most recent)
    uint32_t maxId = 0;
    for (auto& [id, img] : reg) {
        CHECK(img->imageNumber == 7);
        maxId = std::max(maxId, id);
    }
    CHECK(t.term.findImageByNumber(7) == maxId);
}

TEST_CASE("kitty graphics: i= and I= together is EINVAL")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,i=1,I=2,f=32,s=1,v=1", px);
    CHECK(t.output().find("EINVAL") != std::string::npos);
    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("kitty graphics: delete by image number")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,I=10,f=32,s=1,v=1,q=2", px);
    t.gfx("a=t,i=5,f=32,s=1,v=1,q=2", px);
    CHECK(t.term.imageRegistry().size() == 2);

    t.gfx("a=d,d=n,I=10");
    CHECK(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().count(5));
}

TEST_CASE("kitty graphics: animation frame load by image number")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,I=50,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,I=50,f=32,s=2,v=2,z=80,q=2", frame);

    auto& reg = t.term.imageRegistry();
    CHECK(reg.size() == 1);
    auto& img = *reg.begin()->second;
    CHECK(img.extraFrames.size() == 1);
}

// ── Source rect cropping ────────────────────────────────────────────────────

TEST_CASE("kitty graphics: crop parameters stored on image")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(10, 10, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=10,x=2,y=3,w=5,h=4,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.cropX == 2);
    CHECK(img.cropY == 3);
    CHECK(img.cropW == 5);
    CHECK(img.cropH == 4);
}

TEST_CASE("kitty graphics: no crop when w=0 h=0")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(10, 10, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=10,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.cropW == 0);
    CHECK(img.cropH == 0);
}

// ── Multiple images coexist ─────────────────────────────────────────────────

TEST_CASE("kitty graphics: two images coexist in grid")
{
    GraphicsTerminal t(40, 20);
    // First image: 10x20 red (1 col x 1 row), placed at column 0
    auto px1 = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px1);

    CHECK(t.term.cursorY() == 0);

    // Move to start of next line (CR+LF like a real app would)
    t.feed("\r\n");

    // Second image: 10x20 green (1 col x 1 row), placed at column 0
    auto px2 = GraphicsTerminal::solidRGBA(10, 20, 0, 255, 0);
    t.gfx("a=T,i=2,f=32,s=10,v=20,q=2", px2);

    CHECK(t.term.cursorY() == 1);

    // Both images should be in registry
    CHECK(t.term.imageRegistry().size() == 2);
    CHECK(t.term.imageRegistry().count(1));
    CHECK(t.term.imageRegistry().count(2));

    // First image at col 0, row 0
    const CellExtra* ex0 = t.extra(0, 0);
    REQUIRE(ex0);
    CHECK(ex0->imageId == 1);

    // Second image at col 0, row 1
    const CellExtra* ex1 = t.extra(0, 1);
    REQUIRE(ex1);
    CHECK(ex1->imageId == 2);
}

TEST_CASE("kitty graphics: delete visible preserves non-visible image")
{
    GraphicsTerminal t(40, 10);
    // Place image 1 at top, then image 2 below
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);
    t.feed("\r\n");
    t.gfx("a=T,i=2,f=32,s=10,v=20,q=2", px);

    CHECK(t.term.imageRegistry().size() == 2);

    // Both should have grid placements
    const CellExtra* ex0 = t.extra(0, 0);
    const CellExtra* ex1 = t.extra(0, 1);
    REQUIRE(ex0);
    REQUIRE(ex1);
    CHECK(ex0->imageId == 1);
    CHECK(ex1->imageId == 2);

    // Delete visible with d=A — both are visible, both should be freed
    t.gfx("a=d,d=A");
    CHECK(t.term.imageRegistry().empty());
}

// ── APC non-graphics silently ignored ───────────────────────────────────────

TEST_CASE("kitty graphics: non-G APC is silently ignored")
{
    GraphicsTerminal t;
    t.apc("Xsomething");
    CHECK(t.output().empty());
    CHECK(t.term.imageRegistry().empty());
}

// ── Image survives reflow ───────────────────────────────────────────────────

TEST_CASE("kitty graphics: image persists through column resize")
{
    GraphicsTerminal t(80, 24);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 0, 0, 255);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);

    // Image should be in registry and grid
    CHECK(t.term.imageRegistry().count(1));
    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 1);

    // Resize columns (triggers reflow)
    t.term.resize(40, 24);

    // Image should still be in registry
    CHECK(t.term.imageRegistry().count(1));
}

// ── Multiple placements ────────────────────────────────────────────────────

TEST_CASE("kitty graphics: put with placement ID creates separate placement")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0); // 1x1 cells
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    // Place at row 0
    t.gfx("a=p,i=1,p=1,q=2");
    const CellExtra* ex0 = t.extra(0, 0);
    REQUIRE(ex0);
    CHECK(ex0->imageId == 1);
    CHECK(ex0->imagePlacementId == 1);

    // Move cursor down and place again with different placement ID
    t.feed("\r\n\n");
    t.gfx("a=p,i=1,p=2,q=2");
    const CellExtra* ex2 = t.extra(0, 2);
    REQUIRE(ex2);
    CHECK(ex2->imageId == 1);
    CHECK(ex2->imagePlacementId == 2);

    // First placement should still be there
    const CellExtra* ex0b = t.extra(0, 0);
    REQUIRE(ex0b);
    CHECK(ex0b->imageId == 1);
    CHECK(ex0b->imagePlacementId == 1);

    // Image has two placements
    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.placements.size() == 2);
    CHECK(img.placements.count(1));
    CHECK(img.placements.count(2));
}

TEST_CASE("kitty graphics: put with same placement ID replaces old placement")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    // Place at row 0
    t.gfx("a=p,i=1,p=5,q=2");
    const CellExtra* ex0 = t.extra(0, 0);
    REQUIRE(ex0);
    CHECK(ex0->imageId == 1);

    // Move down and place again with SAME placement ID — old cells should be cleared
    t.feed("\r\n\n\n");
    t.gfx("a=p,i=1,p=5,q=2");

    // Old position (row 0) should be cleared
    const CellExtra* exOld = t.extra(0, 0);
    CHECK((!exOld || exOld->imageId == 0));

    // New position (row 3) should have the placement
    const CellExtra* exNew = t.extra(0, 3);
    REQUIRE(exNew);
    CHECK(exNew->imageId == 1);
    CHECK(exNew->imagePlacementId == 5);
}

TEST_CASE("kitty graphics: delete specific placement by ID")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    // Create two placements
    t.gfx("a=p,i=1,p=1,q=2");
    t.feed("\r\n\n");
    t.gfx("a=p,i=1,p=2,q=2");

    // Delete only placement 1
    t.gfx("a=d,d=i,i=1,p=1");

    // Placement 1 cells should be gone
    const CellExtra* ex0 = t.extra(0, 0);
    CHECK((!ex0 || ex0->imageId == 0));

    // Placement 2 cells should still exist
    const CellExtra* ex2 = t.extra(0, 2);
    REQUIRE(ex2);
    CHECK(ex2->imageId == 1);
    CHECK(ex2->imagePlacementId == 2);

    // Image still in registry (has remaining placement)
    CHECK(t.term.imageRegistry().count(1));
    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.placements.size() == 1);
    CHECK(img.placements.count(2));
}

TEST_CASE("kitty graphics: delete all placements removes image with uppercase")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    t.gfx("a=p,i=1,p=1,q=2");
    t.feed("\r\n\n");
    t.gfx("a=p,i=1,p=2,q=2");

    // Delete placement 1 with uppercase I (free when empty)
    t.gfx("a=d,d=I,i=1,p=1");
    CHECK(t.term.imageRegistry().count(1)); // still has placement 2

    // Delete placement 2 with uppercase I
    t.gfx("a=d,d=I,i=1,p=2");
    CHECK(t.term.imageRegistry().empty()); // no placements left, image freed
}

TEST_CASE("kitty graphics: transmit+display stores placement ID in cells")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,p=7,f=32,s=10,v=20,q=2", px);

    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 1);
    CHECK(ex->imagePlacementId == 7);
}

TEST_CASE("kitty graphics: placement stores per-placement display params")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(20, 20, 255, 0, 0); // 2x1 cells at 10x20
    t.gfx("a=t,i=1,f=32,s=20,v=20,q=2", px);

    // Place with custom cell dimensions
    t.gfx("a=p,i=1,p=1,c=4,r=3,q=2");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(1));
    auto& pl = img.placements.at(1);
    CHECK(pl.cellWidth == 4);
    CHECK(pl.cellHeight == 3);
}

// ── Position-based delete modes ──────────────────────────���──────────────────

TEST_CASE("kitty graphics: delete at cursor position (d=c)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0); // 1x1 cells
    // Place image at (0, 0)
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);

    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 1);

    // Move cursor to (0, 0) and delete at cursor
    t.feed("\x1b[H"); // cursor home
    t.gfx("a=d,d=c");

    ex = t.extra(0, 0);
    CHECK((!ex || ex->imageId == 0));
    // Lowercase: image data preserved
    CHECK(t.term.imageRegistry().count(1));
}

TEST_CASE("kitty graphics: delete at cursor with uppercase frees image (d=C)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);
    t.feed("\x1b[H");
    t.gfx("a=d,d=C");
    CHECK(t.term.imageRegistry().empty());
}

TEST_CASE("kitty graphics: delete at cursor misses non-intersecting image")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);

    // Move cursor away from the image
    t.feed("\x1b[5;5H"); // row 5, col 5 (1-based)
    t.gfx("a=d,d=c");

    // Image should still be there
    const CellExtra* ex = t.extra(0, 0);
    REQUIRE(ex);
    CHECK(ex->imageId == 1);
}

TEST_CASE("kitty graphics: delete by cell position (d=p)")
{
    GraphicsTerminal t(40, 20);
    // 20x40 image = 2x2 cells at 10x20
    auto px = GraphicsTerminal::solidRGBA(20, 40, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=20,v=40,q=2", px);

    // Delete at cell (2, 1) — 1-based, so (1, 0) 0-based, which is within the 2x2 image
    t.gfx("a=d,d=p,x=2,y=1");

    // Image cells should be cleared
    const CellExtra* ex = t.extra(0, 0);
    CHECK((!ex || ex->imageId == 0));
}

TEST_CASE("kitty graphics: delete by column (d=x)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(20, 40, 255, 0, 0); // 2x2 cells
    t.gfx("a=T,i=1,f=32,s=20,v=40,q=2", px);

    // Place second image at col 10
    t.feed("\x1b[1;11H"); // row 1 col 11 (1-based)
    auto px2 = GraphicsTerminal::solidRGBA(10, 20, 0, 255, 0); // 1x1 cells
    t.gfx("a=T,i=2,f=32,s=10,v=20,q=2", px2);
    CHECK(t.term.imageRegistry().size() == 2);

    // Delete column 1 (1-based = col 0) — should hit image 1 only
    t.gfx("a=d,d=x,x=1");

    const CellExtra* ex0 = t.extra(0, 0);
    CHECK((!ex0 || ex0->imageId == 0));

    // Image 2 at col 10 should survive
    const CellExtra* ex10 = t.extra(10, 0);
    REQUIRE(ex10);
    CHECK(ex10->imageId == 2);
}

TEST_CASE("kitty graphics: delete by row (d=y)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0); // 1x1 cells
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);
    t.feed("\r\n\n");
    t.gfx("a=T,i=2,f=32,s=10,v=20,q=2", px);
    CHECK(t.term.imageRegistry().size() == 2);

    // Delete row 1 (1-based = row 0) — should hit image 1 only
    t.gfx("a=d,d=y,y=1");

    const CellExtra* ex0 = t.extra(0, 0);
    CHECK((!ex0 || ex0->imageId == 0));

    const CellExtra* ex2 = t.extra(0, 2);
    REQUIRE(ex2);
    CHECK(ex2->imageId == 2);
}

TEST_CASE("kitty graphics: delete by ID range (d=r)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=5,f=32,s=10,v=20,q=2", px);
    t.gfx("a=t,i=10,f=32,s=10,v=20,q=2", px);
    t.gfx("a=t,i=15,f=32,s=10,v=20,q=2", px);
    CHECK(t.term.imageRegistry().size() == 3);

    // Delete range [5, 10] with uppercase (free data)
    t.gfx("a=d,d=R,x=5,y=10");
    CHECK(t.term.imageRegistry().size() == 1);
    CHECK(t.term.imageRegistry().count(15));
}

// ── Frame composition (a=c) ─────────────────────────────────────────────────

TEST_CASE("kitty graphics: animation control c= sets current frame")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto f1 = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", f1);

    auto f2 = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", f2);

    auto& img = *t.term.imageRegistry().at(1);
    CHECK(img.currentFrameIndex == 0);

    // Switch to frame 2 (1-based)
    t.gfx("a=a,i=1,c=2");
    CHECK(img.currentFrameIndex == 1);

    // Switch to frame 3
    t.gfx("a=a,i=1,c=3");
    CHECK(img.currentFrameIndex == 2);

    // Frame data should be blue (frame 3)
    const auto& data = img.currentFrameRGBA();
    CHECK(data[0] == 0);
    CHECK(data[2] == 255);
}

// ── Z-layering ──────────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: z-index stored on placement")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    t.gfx("a=p,i=1,p=1,z=-5,q=2");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(1));
    CHECK(img.placements.at(1).zIndex == -5);
}

TEST_CASE("kitty graphics: transmit+display stores z-index")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,z=-1,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(0));
    CHECK(img.placements.at(0).zIndex == -1);
}

TEST_CASE("kitty graphics: default z-index is 0")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(0));
    CHECK(img.placements.at(0).zIndex == 0);
}

TEST_CASE("kitty graphics: delete by z-index (d=z)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    // Place two placements with different z-indices
    t.gfx("a=p,i=1,p=1,z=-1,q=2");
    t.feed("\r\n\n");
    t.gfx("a=p,i=1,p=2,z=5,q=2");

    // Delete z=-1 only
    t.gfx("a=d,d=z,z=-1");

    // Placement 1 (z=-1) should be gone
    const CellExtra* ex0 = t.extra(0, 0);
    CHECK((!ex0 || ex0->imageId == 0));

    // Placement 2 (z=5) should survive
    const CellExtra* ex2 = t.extra(0, 2);
    REQUIRE(ex2);
    CHECK(ex2->imageId == 1);
}

TEST_CASE("kitty graphics: delete by position + z-index (d=q)")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    // Place at row 0 with z=-1
    t.gfx("a=p,i=1,p=1,z=-1,q=2");
    t.feed("\r\n\n");
    // Place at row 2 with z=3
    t.gfx("a=p,i=1,p=2,z=3,q=2");

    // Delete at position (1,3) (1-based = row 2, col 0) with z=3 — should only hit placement 2
    t.gfx("a=d,d=q,x=1,y=3,z=3");

    // Placement 2 should be gone
    const CellExtra* ex2 = t.extra(0, 2);
    CHECK((!ex2 || ex2->imageId == 0));

    // Placement 1 (z=-1) should still exist at row 0
    const CellExtra* ex0 = t.extra(0, 0);
    REQUIRE(ex0);
    CHECK(ex0->imageId == 1);
    CHECK(ex0->imagePlacementId == 1);
}

TEST_CASE("kitty graphics: frame composition copies rectangle between frames")
{
    GraphicsTerminal t;
    // 4x4 red base
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    // Add a 4x4 blue second frame
    auto frame2 = GraphicsTerminal::solidRGBA(4, 4, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=4,v=4,z=40,q=2", frame2);

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);

    // Compose: copy 2x2 from frame 1 (red) at src (0,0) to frame 2 (blue) at dst (1,1)
    // r=1 (source=root), c=2 (dest=frame2), w=2, h=2, X=0, Y=0 (src offset), x=1, y=1 (dst offset)
    t.clearOutput();
    t.gfx("a=c,i=1,r=1,c=2,w=2,h=2,X=0,Y=0,x=1,y=1,C=1");
    CHECK(t.output().find("OK") != std::string::npos);

    // Frame 2 pixel at (0,0) should still be blue (untouched)
    auto& f2 = img.extraFrames[0];
    CHECK(f2.rgba[0] == 0);     // R
    CHECK(f2.rgba[2] == 255);   // B

    // Frame 2 pixel at (1,1) should now be red (copied from frame 1)
    size_t idx = (1 * 4 + 1) * 4;
    CHECK(f2.rgba[idx] == 255);     // R
    CHECK(f2.rgba[idx + 2] == 0);   // B
}

TEST_CASE("kitty graphics: frame composition alpha blends by default")
{
    GraphicsTerminal t;
    // 2x2 white base (opaque)
    auto base = GraphicsTerminal::solidRGBA(2, 2, 255, 255, 255);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", base);

    // Add a 2x2 half-transparent red frame (alpha=128)
    auto frame2 = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0, 128);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame2);

    // Add a 2x2 blue destination frame
    auto frame3 = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame3);

    // Compose frame 2 (semi-transparent red) onto frame 3 (blue), default blend (C=0)
    t.clearOutput();
    t.gfx("a=c,i=1,r=2,c=3");
    CHECK(t.output().find("OK") != std::string::npos);

    // Frame 3 pixel (0,0): should be blended red over blue
    auto& img = *t.term.imageRegistry().at(1);
    auto& f3 = img.extraFrames[1];
    // Red channel: (255 * 128 + 0 * 127) / 255 ≈ 128
    CHECK(f3.rgba[0] > 100); // R should be significant
    // Blue channel: reduced from 255
    CHECK(f3.rgba[2] < 200); // B should be reduced
}

TEST_CASE("kitty graphics: frame composition rejects out of bounds")
{
    GraphicsTerminal t;
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    auto frame2 = GraphicsTerminal::solidRGBA(4, 4, 0, 0, 255);
    t.gfx("a=f,i=1,f=32,s=4,v=4,z=40,q=2", frame2);

    // Try to compose a 3x3 rect at dst offset (2,2) on a 4x4 image — goes out of bounds
    t.clearOutput();
    t.gfx("a=c,i=1,r=1,c=2,w=3,h=3,x=2,y=2");
    CHECK(t.output().find("EINVAL") != std::string::npos);
}

TEST_CASE("kitty graphics: frame composition rejects overlapping same-frame")
{
    GraphicsTerminal t;
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    // Compose frame 1 onto itself with overlapping rectangles
    t.clearOutput();
    t.gfx("a=c,i=1,r=1,c=1,w=3,h=3,X=0,Y=0,x=1,y=1");
    CHECK(t.output().find("EINVAL") != std::string::npos);
}

TEST_CASE("kitty graphics: frame composition nonexistent frame returns ENOENT")
{
    GraphicsTerminal t;
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    t.clearOutput();
    t.gfx("a=c,i=1,r=1,c=5"); // frame 5 doesn't exist
    CHECK(t.output().find("ENOENT") != std::string::npos);
}

// ── S=/O= data size/offset ──────────────────────────────────────────────────

TEST_CASE("kitty graphics: file transmission with S= and O= reads subrange")
{
    GraphicsTerminal t(40, 20);

    // Write a file with two 2x2 RGBA images back-to-back (32 bytes each)
    // First: red, Second: green
    auto red = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    auto green = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);

    std::string path = "/tmp/mb_test_subrange.bin";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f);
        fwrite(red.data(), 1, red.size(), f);
        fwrite(green.data(), 1, green.size(), f);
        fclose(f);
    }

    // Transmit the green portion: offset=16 (skip red), size=16 (one 2x2 image)
    std::string b64path = base64::encode(reinterpret_cast<const uint8_t*>(path.data()), path.size());
    std::string esc = "\x1b_Ga=t,i=1,f=32,s=2,v=2,t=f,S=16,O=16;" + b64path + "\x1b\\";
    t.feed(esc);

    auto& reg = t.term.imageRegistry();
    REQUIRE(reg.count(1));
    auto& img = *reg.at(1);
    CHECK(img.pixelWidth == 2);
    CHECK(img.pixelHeight == 2);
    // First pixel should be green (from offset 16)
    CHECK(img.rgba[0] == 0);
    CHECK(img.rgba[1] == 255);
    CHECK(img.rgba[2] == 0);
    CHECK(img.rgba[3] == 255);

    unlink(path.c_str());
}

TEST_CASE("kitty graphics: file transmission with O= only reads from offset to end")
{
    GraphicsTerminal t(40, 20);

    auto red = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    auto blue = GraphicsTerminal::solidRGBA(2, 2, 0, 0, 255);

    std::string path = "/tmp/mb_test_offset.bin";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f);
        fwrite(red.data(), 1, red.size(), f);
        fwrite(blue.data(), 1, blue.size(), f);
        fclose(f);
    }

    // Transmit from offset 16 to end (no S=)
    std::string b64path = base64::encode(reinterpret_cast<const uint8_t*>(path.data()), path.size());
    std::string esc = "\x1b_Ga=t,i=1,f=32,s=2,v=2,t=f,O=16,q=2;" + b64path + "\x1b\\";
    t.feed(esc);

    auto& img = *t.term.imageRegistry().at(1);
    // Should be blue
    CHECK(img.rgba[0] == 0);
    CHECK(img.rgba[1] == 0);
    CHECK(img.rgba[2] == 255);

    unlink(path.c_str());
}

TEST_CASE("kitty graphics: file transmission with out-of-range offset returns error")
{
    GraphicsTerminal t(40, 20);

    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    std::string path = "/tmp/mb_test_oor.bin";
    {
        FILE* f = fopen(path.c_str(), "wb");
        REQUIRE(f);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
    }

    // Offset past end of file
    t.clearOutput();
    std::string b64path = base64::encode(reinterpret_cast<const uint8_t*>(path.data()), path.size());
    std::string esc = "\x1b_Ga=t,i=1,f=32,s=2,v=2,t=f,O=9999;" + b64path + "\x1b\\";
    t.feed(esc);
    CHECK(t.output().find("EINVAL") != std::string::npos);
    CHECK(t.term.imageRegistry().empty());

    unlink(path.c_str());
}

TEST_CASE("kitty graphics: sub-cell pixel offsets stored on placement")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    t.gfx("a=p,i=1,p=1,X=3,Y=5,q=2");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(1));
    auto& pl = img.placements.at(1);
    CHECK(pl.cellXOffset == 3);
    CHECK(pl.cellYOffset == 5);
}

TEST_CASE("kitty graphics: transmit+display stores sub-cell offsets")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=10,v=20,X=2,Y=4,q=2", px);

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(0));
    auto& pl = img.placements.at(0);
    CHECK(pl.cellXOffset == 2);
    CHECK(pl.cellYOffset == 4);
}

TEST_CASE("kitty graphics: placement with crop params")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(20, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=20,v=20,q=2", px);

    // Place with crop
    t.gfx("a=p,i=1,p=1,x=2,y=3,w=10,h=8,q=2");

    auto& img = *t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(1));
    auto& pl = img.placements.at(1);
    CHECK(pl.cropX == 2);
    CHECK(pl.cropY == 3);
    CHECK(pl.cropW == 10);
    CHECK(pl.cropH == 8);
}

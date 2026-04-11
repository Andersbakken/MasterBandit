#include <doctest/doctest.h>
#include "TestTerminal.h"
#include "Utils.h"
#include <cstring>

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
    auto& img = reg.at(5);
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

    auto& img = t.term.imageRegistry().at(1);
    CHECK(img.cellWidth == 3);
    CHECK(img.cellHeight == 3);
}

TEST_CASE("kitty graphics: explicit c= and r= override calculated dimensions")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 128, 128, 128);
    t.gfx("a=T,i=1,f=32,s=10,v=10,c=5,r=3,q=2", px);

    auto& img = t.term.imageRegistry().at(1);
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
    CHECK(reg.begin()->second.id > 0);
}

// ── RGB format ──────────────────────────────────────────────────────────────

TEST_CASE("kitty graphics: RGB24 format converted to RGBA")
{
    GraphicsTerminal t;
    std::vector<uint8_t> rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 128, 128, 128};
    t.gfx("a=t,i=1,f=24,s=2,v=2,q=2", rgb);

    auto& img = t.term.imageRegistry().at(1);
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
    auto& img = t.term.imageRegistry().at(7);
    CHECK(img.pixelWidth == 2);
    CHECK(img.pixelHeight == 2);
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

TEST_CASE("kitty graphics: delete animation frames")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);

    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(10);
    CHECK(img.extraFrames.size() == 1);
}

TEST_CASE("kitty graphics: animation control starts/stops animation")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(2, 2, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=2,v=2,q=2", px);

    auto frame = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    t.gfx("a=f,i=1,f=32,s=2,v=2,z=40,q=2", frame);

    auto& img = t.term.imageRegistry().at(1);
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
    auto& img = t.term.imageRegistry().at(1);
    CHECK(img.currentFrameIndex == 0);

    // Force frameShownAt into the past to avoid wall-clock timing dependency
    auto& mutImg = t.term.imageRegistryMut().at(1);
    mutImg.frameShownAt = TerminalEmulator::mono() - 1000;
    REQUIRE(mutImg.hasAnimation());
    uint32_t genBefore = mutImg.frameGeneration;
    t.term.tickAnimations();
    // frameGeneration bumps on any tick that processes this image,
    // regardless of which frame it lands on (which depends on exact timing)
    CHECK(mutImg.frameGeneration > genBefore);
}

// ── Delta frame compositing ─────────────────────────────────────────────────

TEST_CASE("kitty graphics: partial frame composites onto previous frame")
{
    GraphicsTerminal t;
    // 4x4 red base
    auto base = GraphicsTerminal::solidRGBA(4, 4, 255, 0, 0);
    t.gfx("a=T,i=1,f=32,s=4,v=4,q=2", base);

    // 2x2 green patch at offset (1,1), composited onto previous frame
    auto patch = GraphicsTerminal::solidRGBA(2, 2, 0, 255, 0);
    std::string b64 = base64::encode(patch.data(), patch.size());
    t.feed("\x1b_Ga=f,i=1,f=32,s=2,v=2,x=1,y=1,z=40,q=2;" + b64 + "\x1b\\");

    auto& img = t.term.imageRegistry().at(1);
    REQUIRE(img.extraFrames.size() == 1);

    auto& frame = img.extraFrames[0];
    // Pixel at (0,0) should be red (from base, inherited from previous = root)
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

// ── Image number (I=) ───────────────────────────────────────────────────────

TEST_CASE("kitty graphics: image number stores and retrieves")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,I=42,f=32,s=1,v=1,q=2", px);

    auto& reg = t.term.imageRegistry();
    CHECK(reg.size() == 1);
    auto& img = reg.begin()->second;
    CHECK(img.imageNumber == 42);
}

TEST_CASE("kitty graphics: put by image number")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 10, 0, 0, 255);
    t.gfx("a=t,I=99,f=32,s=10,v=10,q=2", px);

    uint32_t imgId = t.term.imageRegistry().begin()->second.id;

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
        CHECK(img.imageNumber == 7);
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
    auto& img = reg.begin()->second;
    CHECK(img.extraFrames.size() == 1);
}

// ── Source rect cropping ────────────────────────────────────────────────────

TEST_CASE("kitty graphics: crop parameters stored on image")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(10, 10, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=10,x=2,y=3,w=5,h=4,q=2", px);

    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(1);
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
    auto& img = t.term.imageRegistry().at(1);
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
    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(1);
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

TEST_CASE("kitty graphics: sub-cell pixel offsets stored on placement")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 20, 255, 0, 0);
    t.gfx("a=t,i=1,f=32,s=10,v=20,q=2", px);

    t.gfx("a=p,i=1,p=1,X=3,Y=5,q=2");

    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(1);
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

    auto& img = t.term.imageRegistry().at(1);
    REQUIRE(img.placements.count(1));
    auto& pl = img.placements.at(1);
    CHECK(pl.cropX == 2);
    CHECK(pl.cropY == 3);
    CHECK(pl.cropW == 10);
    CHECK(pl.cropH == 8);
}

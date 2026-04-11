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

TEST_CASE("kitty graphics: delete all images")
{
    GraphicsTerminal t;
    auto px = GraphicsTerminal::solidRGBA(1, 1, 0, 0, 0);
    t.gfx("a=t,i=1,f=32,s=1,v=1,q=2", px);
    t.gfx("a=t,i=2,f=32,s=1,v=1,q=2", px);
    CHECK(t.term.imageRegistry().size() == 2);

    t.gfx("a=d,d=a");
    CHECK(t.term.imageRegistry().empty());
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

TEST_CASE("kitty graphics: C=0 moves cursor past image")
{
    GraphicsTerminal t(40, 20);
    auto px = GraphicsTerminal::solidRGBA(10, 40, 0, 255, 0); // 1x2 cells
    t.gfx("a=T,i=1,f=32,s=10,v=40,C=0,q=2", px);

    CHECK(t.term.cursorY() == 2);
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

    // Set root frame gap to 1ms and start animation
    t.gfx("a=a,i=1,r=1,z=1"); // set gap for frame 1 (root)
    t.gfx("a=a,i=1,s=3");     // start running
    auto& img = t.term.imageRegistry().at(1);
    CHECK(img.currentFrameIndex == 0);

    // Wait and tick — should advance (sleep 20ms to reliably exceed 1ms gap)
    struct timespec ts = {0, 20000000}; // 20ms
    nanosleep(&ts, nullptr);
    t.term.tickAnimations();

    CHECK(img.currentFrameIndex > 0);
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

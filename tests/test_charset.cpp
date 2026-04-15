#include <doctest/doctest.h>
#include "TestTerminal.h"

// ── default state: ASCII passes through ───────────────────────────────────────

TEST_CASE("default charset: ASCII letters unchanged")
{
    TestTerminal t;
    t.feed("qxl");
    CHECK(t.wc(0, 0) == U'q');
    CHECK(t.wc(1, 0) == U'x');
    CHECK(t.wc(2, 0) == U'l');
}

// ── G0 designation (ESC ( 0 / B) ──────────────────────────────────────────────

TEST_CASE("ESC ( 0 switches G0 to DEC graphics; line chars translate")
{
    TestTerminal t;
    t.esc("(0");
    t.feed("qxlkmjn");
    CHECK(t.wc(0, 0) == 0x2500); // ─
    CHECK(t.wc(1, 0) == 0x2502); // │
    CHECK(t.wc(2, 0) == 0x250C); // ┌
    CHECK(t.wc(3, 0) == 0x2510); // ┐
    CHECK(t.wc(4, 0) == 0x2514); // └
    CHECK(t.wc(5, 0) == 0x2518); // ┘
    CHECK(t.wc(6, 0) == 0x253C); // ┼
}

TEST_CASE("ESC ( B restores ASCII")
{
    TestTerminal t;
    t.esc("(0");
    t.feed("q");
    t.esc("(B");
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);
    CHECK(t.wc(1, 0) == U'q');
}

TEST_CASE("ESC ( A switches G0 to UK; # becomes £")
{
    TestTerminal t;
    t.esc("(A");
    t.feed("#");
    CHECK(t.wc(0, 0) == 0x00A3); // £
    t.esc("(B");
    t.feed("#");
    CHECK(t.wc(1, 0) == U'#');
}

TEST_CASE("UK charset only remaps '#'; other letters unchanged")
{
    TestTerminal t;
    t.esc("(A");
    t.feed("Aqk");
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == U'q');
    CHECK(t.wc(2, 0) == U'k');
}

TEST_CASE("Unsupported G0 designator falls back to ASCII")
{
    TestTerminal t;
    t.esc("(0");      // graphics
    t.feed("q");
    t.esc("(F");      // unknown → ASCII
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);
    CHECK(t.wc(1, 0) == U'q');
}

// ── G1 designation + SO/SI locking shifts ─────────────────────────────────────

TEST_CASE("ESC ) 0 loads G1 but G0 still active")
{
    TestTerminal t;
    t.esc(")0");
    t.feed("q");
    CHECK(t.wc(0, 0) == U'q'); // GL=G0, G0 is still ASCII
}

TEST_CASE("SO (0x0E) invokes G1 into GL")
{
    TestTerminal t;
    t.esc(")0");          // G1 = DEC graphics
    t.feed("\x0E");       // SO → GL = G1
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);
}

TEST_CASE("SI (0x0F) returns GL to G0")
{
    TestTerminal t;
    t.esc(")0");
    t.feed("\x0E");       // SO
    t.feed("q");
    t.feed("\x0F");       // SI
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);
    CHECK(t.wc(1, 0) == U'q');
}

TEST_CASE("SO/SI do not advance the cursor or land in cells")
{
    TestTerminal t;
    t.feed("\x0E\x0F");
    CHECK(t.term.cursorX() == 0);
    CHECK(t.wc(0, 0) == 0);
}

// ── range behavior ────────────────────────────────────────────────────────────

TEST_CASE("DEC graphics: characters below 0x5F pass through")
{
    TestTerminal t;
    t.esc("(0");
    t.feed("AZ");          // 0x41, 0x5A — below remap range
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == U'Z');
}

TEST_CASE("DEC graphics: 0x5F is NBSP, 0x7E is middle dot")
{
    TestTerminal t;
    t.esc("(0");
    t.feed("_~");
    CHECK(t.wc(0, 0) == 0x00A0);
    CHECK(t.wc(1, 0) == 0x00B7);
}

// ── DECSC / DECRC save/restore ────────────────────────────────────────────────

TEST_CASE("DECSC saves charset; DECRC restores")
{
    // DECRC restores cursor position too, so write in distinct columns.
    TestTerminal t;
    t.csi("5G");          // cursor at column 5 (x=4)
    t.esc("(0");          // G0 = graphics
    t.esc("7");           // DECSC — saves (4,0), G0=graphics
    t.csi("10G");         // cursor to column 10 (x=9)
    t.esc("(B");          // ASCII
    t.feed("q");          // ASCII 'q' at (9,0)
    CHECK(t.wc(9, 0) == U'q');
    t.esc("8");           // DECRC → cursor=(4,0), G0=graphics
    t.feed("q");          // graphics ─ at (4,0)
    CHECK(t.wc(4, 0) == 0x2500);
}

TEST_CASE("DECSC/DECRC saves and restores shiftOut (GL)")
{
    TestTerminal t;
    t.esc(")0");          // G1 = graphics
    t.csi("5G");
    t.feed("\x0E");       // SO → GL=G1
    t.esc("7");           // DECSC saves cursor=(4,0), shiftOut=true
    t.csi("10G");
    t.feed("\x0F");       // SI → GL=G0
    t.feed("q");          // ASCII at (9,0)
    CHECK(t.wc(9, 0) == U'q');
    t.esc("8");           // DECRC → cursor=(4,0), shiftOut=true
    t.feed("q");          // graphics at (4,0)
    CHECK(t.wc(4, 0) == 0x2500);
}

TEST_CASE("CSI s / CSI u also save/restore charset (shared DECSC slot)")
{
    TestTerminal t;
    t.csi("5G");
    t.esc("(0");
    t.csi("s");           // SCP — shares the DECSC save slot
    t.csi("10G");
    t.esc("(B");
    t.feed("q");
    CHECK(t.wc(9, 0) == U'q');
    t.csi("u");           // RCP
    t.feed("q");
    CHECK(t.wc(4, 0) == 0x2500);
}

// ── alt-screen isolation (per-screen state) ───────────────────────────────────

TEST_CASE("alt screen starts fresh — main's charset does not leak in")
{
    TestTerminal t;
    t.esc("(0");          // main: G0 = graphics
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);

    t.csi("?1049h");      // enter alt
    t.feed("q");
    CHECK(t.wc(0, 0) == U'q'); // alt starts with default ASCII charset
}

TEST_CASE("alt screen's charset does not leak back to main")
{
    TestTerminal t;
    t.csi("?1049h");      // enter alt
    t.esc("(0");          // alt: G0 = graphics
    t.feed("q");
    CHECK(t.wc(0, 0) == 0x2500);

    t.csi("?1049l");      // back to main — main is still ASCII
    t.feed("q");
    // Main should be untouched: ASCII at position (0,0) of main (screen cleared
    // on alt swap in tests? — main content is preserved; cursor returns to
    // original position).
    // Verify by feeding something after exit and checking it's ASCII.
    int cx = t.term.cursorX();
    int cy = t.term.cursorY();
    CHECK(t.term.grid().cell(cx - 1, cy).wc == U'q');
}

// ── RIS resets charset ────────────────────────────────────────────────────────

TEST_CASE("RIS resets charset to ASCII/G0")
{
    TestTerminal t;
    t.esc("(0");
    t.esc(")0");
    t.feed("\x0E");       // SO
    t.esc("c");           // RIS
    t.feed("q");
    CHECK(t.wc(0, 0) == U'q');
}

// ── combined box smoke test ───────────────────────────────────────────────────

TEST_CASE("drawing a box: ESC ( 0 + lqqk produces ┌──┐")
{
    TestTerminal t;
    t.esc("(0");
    t.feed("lqqk");
    t.esc("(B");
    CHECK(t.wc(0, 0) == 0x250C); // ┌
    CHECK(t.wc(1, 0) == 0x2500); // ─
    CHECK(t.wc(2, 0) == 0x2500); // ─
    CHECK(t.wc(3, 0) == 0x2510); // ┐
}

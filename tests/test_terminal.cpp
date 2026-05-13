#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "TestTerminal.h"
#include <doctest/doctest.h>

TEST_CASE("basic text output")
{
    TestTerminal t;
    t.feed("Hello");
    CHECK(t.wc(0, 0) == U'H');
    CHECK(t.wc(1, 0) == U'e');
    CHECK(t.wc(2, 0) == U'l');
    CHECK(t.wc(3, 0) == U'l');
    CHECK(t.wc(4, 0) == U'o');
    CHECK(t.term.cursorX() == 5);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("rowText helper")
{
    TestTerminal t;
    t.feed("Hello");
    CHECK(t.rowText(0) == "Hello");
}

TEST_CASE("carriage return and line feed")
{
    TestTerminal t;
    t.feed("AB\r\nCD");
    CHECK(t.rowText(0) == "AB");
    CHECK(t.wc(0, 1) == U'C');
    CHECK(t.wc(1, 1) == U'D');
    CHECK(t.term.cursorX() == 2);
    CHECK(t.term.cursorY() == 1);
}

TEST_CASE("line wrap")
{
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJKL"); // 12 chars into a 10-wide terminal
    CHECK(t.rowText(0) == "ABCDEFGHIJ");
    CHECK(t.wc(0, 1) == U'K');
    CHECK(t.wc(1, 1) == U'L');
}

TEST_CASE("erase in line - to end")
{
    TestTerminal t;
    t.feed("Hello World");
    t.csi("5G"); // move to col 5 (1-based) → col index 4
    t.csi("0K"); // erase to end of line
    CHECK(t.rowText(0) == "Hell");
}

TEST_CASE("erase in line - whole line")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("2K");
    CHECK(t.rowText(0) == "");
}

TEST_CASE("UTF-8 multibyte text")
{
    TestTerminal t;
    t.feed("caf\xC3\xA9"); // "café"
    CHECK(t.wc(0, 0) == U'c');
    CHECK(t.wc(1, 0) == U'a');
    CHECK(t.wc(2, 0) == U'f');
    CHECK(t.wc(3, 0) == U'\u00e9'); // é
}

// ── Control characters ───────────────────────────────────────────────────────

TEST_CASE("vertical tab acts as line feed")
{
    TestTerminal t;
    t.feed("A\vB");
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.term.cursorY() == 1);
    CHECK(t.wc(1, 1) == U'B'); // column preserved (LF behavior)
}

TEST_CASE("form feed acts as line feed")
{
    TestTerminal t;
    t.feed("A\fB");
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.term.cursorY() == 1);
    CHECK(t.wc(1, 1) == U'B'); // column preserved
}

TEST_CASE("LF does not reset column")
{
    TestTerminal t;
    t.feed("ABC\nD");
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(3, 1) == U'D'); // column 3, not 0
}

// ── Deferred autowrap ────────────────────────────────────────────────────────

TEST_CASE("deferred wrap: cursor stays at last column until next char")
{
    TestTerminal t(5, 3); // 5 columns
    t.feed("ABCDE");      // fill entire row
    // Cursor should be at col 4 (last column) with wrap pending
    CHECK(t.term.cursorX() == 4);
    CHECK(t.term.cursorY() == 0);
    // Writing one more char triggers the wrap
    t.feed("F");
    CHECK(t.term.cursorX() == 1);
    CHECK(t.term.cursorY() == 1);
    CHECK(t.wc(0, 1) == U'F');
}

TEST_CASE("deferred wrap: CR clears pending wrap")
{
    TestTerminal t(5, 3);
    t.feed("ABCDE"); // wrap pending
    t.feed("\r");    // CR clears wrap, cursor to col 0
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("deferred wrap: cursor movement clears pending wrap")
{
    TestTerminal t(5, 3);
    t.feed("ABCDE");              // wrap pending
    t.csi("D");                   // CUB 1 — should clear wrap, move left
    CHECK(t.term.cursorY() == 0); // no wrap happened
}

// === REP (CSI b) tests ===

TEST_CASE("REP repeats last character")
{
    TestTerminal t;
    t.feed("A");
    t.csi("3b"); // repeat 'A' 3 times
    CHECK(t.rowText(0) == "AAAA");
}

TEST_CASE("REP with default count repeats once")
{
    TestTerminal t;
    t.feed("X");
    t.csi("b"); // no count = 1
    CHECK(t.rowText(0) == "XX");
}

TEST_CASE("REP does nothing without prior character")
{
    TestTerminal t;
    t.csi("5b");
    CHECK(t.rowText(0) == "");
}

TEST_CASE("REP wraps at line boundary")
{
    TestTerminal t(5, 3);
    t.feed("A");
    t.csi("6b"); // repeat 6 times, total 7 chars in 5-col terminal
    CHECK(t.rowText(0) == "AAAAA");
    CHECK(t.rowText(1) == "AA");
}

TEST_CASE("REP uses current attributes")
{
    TestTerminal t;
    t.csi("1m"); // bold
    t.feed("B");
    t.csi("2b"); // repeat 2 times
    // All three 'B's should be bold
    CHECK(t.attrs(0, 0).bold());
    CHECK(t.attrs(1, 0).bold());
    CHECK(t.attrs(2, 0).bold());
}

// ── DECAWM (autowrap mode) ──────────────────────────────────────────────────

TEST_CASE("DECAWM off: no wrap at right margin")
{
    TestTerminal t(5, 3);
    t.csi("?7l"); // DECAWM off
    t.feed("ABCDEFGH");
    // All chars after column 4 overwrite the last cell
    CHECK(t.rowText(0) == "ABCDH");
    CHECK(t.term.cursorX() == 4);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("DECAWM on: wraps at right margin (default)")
{
    TestTerminal t(5, 3);
    t.feed("ABCDEFGH");
    CHECK(t.rowText(0) == "ABCDE");
    CHECK(t.rowText(1) == "FGH");
    CHECK(t.term.cursorY() == 1);
}

TEST_CASE("DECAWM off then on: re-enables wrapping")
{
    TestTerminal t(5, 3);
    t.csi("?7l"); // off
    t.csi("?7h"); // on
    t.feed("ABCDEFGH");
    CHECK(t.rowText(0) == "ABCDE");
    CHECK(t.rowText(1) == "FGH");
}

TEST_CASE("DECAWM reset via RIS")
{
    TestTerminal t(5, 3);
    t.csi("?7l"); // off
    t.esc("c");   // RIS — full reset
    t.feed("ABCDEFGH");
    CHECK(t.rowText(0) == "ABCDE");
    CHECK(t.rowText(1) == "FGH");
}

// ── IRM (insert mode) ───────────────────────────────────────────────────────

TEST_CASE("insert mode: shifts existing text right")
{
    TestTerminal t(10, 3);
    t.feed("ABCDE");
    t.csi("1G"); // cursor to col 0
    t.csi("4h"); // SM 4 — insert mode on
    t.feed("XY");
    CHECK(t.rowText(0) == "XYABCDE");
}

TEST_CASE("insert mode off: overwrites (default)")
{
    TestTerminal t(10, 3);
    t.feed("ABCDE");
    t.csi("1G"); // cursor to col 0
    t.feed("XY");
    CHECK(t.rowText(0) == "XYCDE");
}

TEST_CASE("insert mode: text pushed off right edge is lost")
{
    TestTerminal t(5, 3);
    t.feed("ABCDE");
    t.csi("1G");
    t.csi("4h"); // insert mode
    t.feed("XY");
    CHECK(t.rowText(0) == "XYABC"); // D and E pushed off
}

TEST_CASE("insert mode: reset via RM 4")
{
    TestTerminal t(10, 3);
    t.feed("ABCDE");
    t.csi("1G");
    t.csi("4h"); // insert on
    t.feed("X");
    t.csi("4l"); // insert off
    t.feed("Y");
    // X was inserted at col 0, shifting right → XABCDE, cursor at col 1.
    // Y overwrites col 1 (insert off) → XYBCDE.
    CHECK(t.rowText(0) == "XYBCDE");
}

TEST_CASE("insert mode: reset via RIS")
{
    TestTerminal t(10, 3);
    t.csi("4h"); // insert on
    t.esc("c");  // RIS
    t.feed("ABCDE");
    t.csi("1G");
    t.feed("XY");
    CHECK(t.rowText(0) == "XYCDE"); // overwrite, not insert
}

// ── UTF-8 decoder ────────────────────────────────────────────────────────────
//
// These exercise the streaming UTF-8 decoder in ParseToActions.cpp via
// the public `injectData` entry. They're here, not in a parser-only unit
// test, because the boundary case we care about (multi-byte sequence split
// across two injectData calls) only exposes a bug if the decoder's state
// (`mUtf8Buffer`, `mUtf8Index`, `mParserState`) is preserved across calls.

TEST_CASE("UTF-8: 2-byte sequence split across feeds")
{
    TestTerminal t;
    t.feed("\xC3");
    t.feed("\xA9"); // completes 'é'
    CHECK(t.wc(0, 0) == U'\u00E9');
    CHECK(t.term.cursorX() == 1);
}

TEST_CASE("UTF-8: 3-byte sequence split across feeds (every internal boundary)")
{
    // U+65E5 '日' = E6 97 A5. East Asian Wide → col 0 holds the codepoint,
    // col 1 is the wide spacer (wc=0).
    for (int splitAfter : { 1, 2 }) {
        TestTerminal t;
        std::string seq = "\xE6\x97\xA5";
        t.feed(seq.substr(0, splitAfter));
        t.feed(seq.substr(splitAfter));
        CHECK_MESSAGE(t.wc(0, 0) == U'\u65E5', "split after byte ", splitAfter);
        CHECK_MESSAGE(t.term.cursorX() == 2, "cursor split after byte ", splitAfter);
    }
}

TEST_CASE("UTF-8: 4-byte sequence split across feeds (every internal boundary)")
{
    // U+1F600 😀 = F0 9F 98 80. Emoji is East Asian Wide → 2 cells.
    for (int splitAfter : { 1, 2, 3 }) {
        TestTerminal t;
        std::string seq = "\xF0\x9F\x98\x80";
        t.feed(seq.substr(0, splitAfter));
        t.feed(seq.substr(splitAfter));
        CHECK_MESSAGE(t.wc(0, 0) == U'\U0001F600', "split after byte ", splitAfter);
        CHECK_MESSAGE(t.term.cursorX() == 2, "cursor split after byte ", splitAfter);
    }
}

TEST_CASE("UTF-8: 4-byte sequence split byte-by-byte across four feeds")
{
    TestTerminal t;
    const char *seq = "\xF0\x9F\x98\x80"; // U+1F600
    for (int i = 0; i < 4; ++i) {
        t.feed(std::string(1, seq[i]));
    }
    CHECK(t.wc(0, 0) == U'\U0001F600');
    CHECK(t.term.cursorX() == 2); // wide
}

TEST_CASE("UTF-8: malformed - C2 followed by non-continuation (the opencode case)")
{
    // 0xC2 expects one continuation. 0x60 is '`' (0b01100000), top bits 01,
    // not 10 — so the decoder should reject the sequence, reset state, and
    // reprocess 0x60 as ASCII '`'. Reproduces the exact sequence seen in
    // production logs.
    TestTerminal t;
    t.feed("X\xC2\x60Y");
    CHECK(t.wc(0, 0) == U'X');
    CHECK(t.wc(1, 0) == U'`'); // 0x60 emitted as ASCII after rewind
    CHECK(t.wc(2, 0) == U'Y');
}

TEST_CASE("UTF-8: malformed - two lead bytes in a row")
{
    // 0xC2 0xC2: second 0xC2 is a lead byte, not a continuation. First
    // sequence is malformed; second 0xC2 starts a new sequence (still
    // incomplete after this feed). Final 'A' would complete nothing because
    // 'A' is also not a continuation — second sequence aborts too, 'A' is
    // emitted as ASCII.
    TestTerminal t;
    t.feed("\xC2\xC2"
           "A");
    CHECK(t.wc(0, 0) == U'A');
}

TEST_CASE("UTF-8: stray continuation byte in Normal state")
{
    // 0x80 has top bits 10. In Normal state, anything >= 0x80 is treated as
    // a lead byte and enters InUtf8. The next byte (here EOF / next feed)
    // must be a continuation — here we follow with an ASCII 'X' which is
    // not a continuation byte, so the decoder rejects and reprocesses 'X'.
    // The original 0x80 is dropped without emitting anything. Document
    // current behavior: 0x80 alone → no glyph; followed by ASCII →
    // ASCII printed.
    TestTerminal t;
    t.feed("\x80"
           "X");
    CHECK(t.wc(0, 0) == U'X');
}

TEST_CASE("UTF-8: incomplete trailing sequence does not corrupt next feed")
{
    // Stream ends mid-multibyte. The next feed completes the sequence and
    // continues with ASCII; both should land correctly. '日' is wide → col
    // 1 holds the codepoint, col 2 is its spacer, 'B' lands in col 3.
    TestTerminal t;
    t.feed("A\xE6"); // start of 3-byte sequence
    t.feed("\x97\xA5"
           "B"); // complete '日', then 'B'
    CHECK(t.wc(0, 0) == U'A');
    CHECK(t.wc(1, 0) == U'\u65E5');
    CHECK(t.wc(3, 0) == U'B');
    CHECK(t.term.cursorX() == 4);
}

TEST_CASE("UTF-8: many random splits round-trip")
{
    // Build a long mixed string, feed at random byte boundaries, verify the
    // decoded grid matches a single-shot feed of the same bytes.
    std::string text;
    for (int i = 0; i < 50; ++i) {
        text += "ascii ";
        text += "\xC3\xA9";         // é
        text += "\xE6\x97\xA5";     // 日
        text += "\xF0\x9F\x98\x80"; // 😀
        text += "x ";
    }
    // Single-shot reference.
    TestTerminal ref(200, 10);
    ref.feed(text);
    // Chunked feed.
    TestTerminal chunked(200, 10);
    size_t pos     = 0;
    uint32_t seed  = 12345u;
    auto nextSplit = [&]()
    {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % 5 + 1; // 1..5 bytes
    };
    while (pos < text.size()) {
        size_t n = std::min<size_t>(nextSplit(), text.size() - pos);
        chunked.feed(text.substr(pos, n));
        pos += n;
    }
    // Compare every cell across the visible grid.
    for (int row = 0; row < 10; ++row) {
        for (int col = 0; col < 200; ++col) {
            CHECK_MESSAGE(chunked.wc(col, row) == ref.wc(col, row),
                          "mismatch at row=",
                          row,
                          " col=",
                          col);
        }
    }
}

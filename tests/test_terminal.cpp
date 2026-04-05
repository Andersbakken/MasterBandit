#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "TestTerminal.h"

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
    t.csi("5G");   // move to col 5 (1-based) → col index 4
    t.csi("0K");   // erase to end of line
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
    t.feed("ABCDE"); // fill entire row
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
    t.feed("\r");     // CR clears wrap, cursor to col 0
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
}

TEST_CASE("deferred wrap: cursor movement clears pending wrap")
{
    TestTerminal t(5, 3);
    t.feed("ABCDE");  // wrap pending
    t.csi("D");       // CUB 1 — should clear wrap, move left
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

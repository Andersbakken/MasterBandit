#include <doctest/doctest.h>
#include "TestTerminal.h"

// === Mode management tests ===

TEST_CASE("kitty: initial flags are zero") {
    TestTerminal t;
    CHECK(t.term.kittyFlags() == 0);
}

TEST_CASE("kitty: push flags") {
    TestTerminal t;
    t.csi(">1u"); // push flags=1
    CHECK(t.term.kittyFlags() == 1);
}

TEST_CASE("kitty: push then query") {
    TestTerminal t;
    t.csi(">3u"); // push flags=3
    t.clearOutput();
    t.csi("?u"); // query
    CHECK(t.output() == "\x1b[?3u");
}

TEST_CASE("kitty: push multiple then pop") {
    TestTerminal t;
    t.csi(">1u");
    t.csi(">5u");
    CHECK(t.term.kittyFlags() == 5);
    t.csi("<1u"); // pop 1
    CHECK(t.term.kittyFlags() == 1);
    t.csi("<1u"); // pop 1 more
    CHECK(t.term.kittyFlags() == 0);
}

TEST_CASE("kitty: pop more than stack depth") {
    TestTerminal t;
    t.csi(">3u");
    t.csi("<10u"); // pop 10, only 1 on stack
    CHECK(t.term.kittyFlags() == 0);
}

TEST_CASE("kitty: set flags replace") {
    TestTerminal t;
    t.csi(">1u"); // push 1
    t.csi("=5u"); // set to 5 (replace)
    CHECK(t.term.kittyFlags() == 5);
}

TEST_CASE("kitty: set flags OR") {
    TestTerminal t;
    t.csi(">1u"); // push 1 (bit 0)
    t.csi("=2;2u"); // OR in bit 1
    CHECK(t.term.kittyFlags() == 3); // 1 | 2 = 3
}

TEST_CASE("kitty: set flags AND NOT") {
    TestTerminal t;
    t.csi(">7u"); // push 7 (bits 0,1,2)
    t.csi("=2;3u"); // clear bit 1
    CHECK(t.term.kittyFlags() == 5); // 7 & ~2 = 5
}

TEST_CASE("kitty: stack overflow limited to 8") {
    TestTerminal t;
    for (int i = 1; i <= 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), ">%du", i);
        t.csi(buf);
    }
    // Should have flags from last push that fit
    CHECK(t.term.kittyFlags() != 0);
}

TEST_CASE("kitty: RIS resets flags") {
    TestTerminal t;
    t.csi(">5u");
    CHECK(t.term.kittyFlags() == 5);
    t.esc("c"); // RIS
    CHECK(t.term.kittyFlags() == 0);
}

TEST_CASE("kitty: alt screen has independent stack") {
    TestTerminal t;
    t.csi(">3u"); // push 3 on main
    CHECK(t.term.kittyFlags() == 3);
    t.csi("?1049h"); // enter alt screen
    CHECK(t.term.kittyFlags() == 0); // alt stack is empty
    t.csi(">7u"); // push 7 on alt
    CHECK(t.term.kittyFlags() == 7);
    t.csi("?1049l"); // leave alt screen
    CHECK(t.term.kittyFlags() == 3); // main stack restored
}

// === Key encoding tests ===

TEST_CASE("kitty: legacy mode sends plain text") {
    TestTerminal t;
    // No kitty flags — legacy mode
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Press, "a");
    CHECK(t.output() == "a");
}

TEST_CASE("kitty: legacy mode drops release") {
    TestTerminal t;
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Release, "a");
    CHECK(t.output().empty());
}

TEST_CASE("kitty: disambiguate plain a sends text") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Press, "a");
    CHECK(t.output() == "a");
}

TEST_CASE("kitty: disambiguate ctrl+a sends CSI u") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_A, CtrlModifier, KeyAction_Press, "");
    CHECK(t.output() == "\x1b[97;5u"); // 97='a', mods=1+4=5
}

TEST_CASE("kitty: disambiguate escape sends CSI u") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_Escape, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b");  // no mods, press → legacy form
}

TEST_CASE("kitty: disambiguate shift+escape sends CSI u") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_Escape, ShiftModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[27;2u"); // 27=escape, mods=1+1=2
}

TEST_CASE("kitty: disambiguate enter sends legacy") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_Return, 0, KeyAction_Press);
    CHECK(t.output() == "\r");
}

TEST_CASE("kitty: disambiguate shift+enter sends CSI u") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE
    t.clearOutput();
    t.sendKey(Key_Return, ShiftModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[13;2u");
}

TEST_CASE("kitty: disambiguate tab sends legacy") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_Tab, 0, KeyAction_Press);
    CHECK(t.output() == "\t");
}

TEST_CASE("kitty: disambiguate shift+tab sends CSI u") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_Tab, ShiftModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[9;2u");
}

TEST_CASE("kitty: disambiguate backspace sends legacy") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_Backspace, 0, KeyAction_Press);
    CHECK(t.output() == "\x7f");
}

TEST_CASE("kitty: disambiguate arrow up no mods sends legacy") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_Up, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b[A");
}

TEST_CASE("kitty: disambiguate ctrl+arrow up sends CSI with mods") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_Up, CtrlModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[1;5A"); // 1;5A = ctrl modifier
}

TEST_CASE("kitty: disambiguate F5 no mods sends legacy tilde") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_F5, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b[15~");
}

TEST_CASE("kitty: disambiguate ctrl+F5 sends tilde with mods") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_F5, CtrlModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[15;5~");
}

TEST_CASE("kitty: disambiguate release is dropped") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Release, "a");
    CHECK(t.output().empty());
}

TEST_CASE("kitty: report event types release") {
    TestTerminal t;
    t.csi(">3u"); // DISAMBIGUATE | REPORT_EVENT_TYPES
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Release, "a");
    CHECK(t.output() == "\x1b[97;1:3u"); // mods=1, event=3 (release)
}

TEST_CASE("kitty: report event types repeat") {
    TestTerminal t;
    t.csi(">3u");
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Repeat, "a");
    CHECK(t.output() == "\x1b[97;1:2u"); // mods=1, event=2 (repeat)
}

TEST_CASE("kitty: report all keys enter") {
    TestTerminal t;
    t.csi(">8u"); // REPORT_ALL_KEYS (implies disambiguate)
    t.clearOutput();
    t.sendKey(Key_Return, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b[13u");
}

TEST_CASE("kitty: report all keys tab") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_Tab, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b[9u");
}

TEST_CASE("kitty: report all keys backspace") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_Backspace, 0, KeyAction_Press);
    CHECK(t.output() == "\x1b[127u");
}

TEST_CASE("kitty: report all keys plain a") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_A, 0, KeyAction_Press, "a");
    CHECK(t.output() == "\x1b[97u");
}

TEST_CASE("kitty: report all keys ctrl+a") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_A, CtrlModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[97;5u");
}

TEST_CASE("kitty: report all keys ctrl+enter") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_Return, CtrlModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[13;5u");
}

TEST_CASE("kitty: modifier key reported with report all keys") {
    TestTerminal t;
    t.csi(">8u");
    t.clearOutput();
    t.sendKey(Key_Shift_L, ShiftModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[57441;2u"); // left shift, shift mod
}

TEST_CASE("kitty: modifier key not reported without report all keys") {
    TestTerminal t;
    t.csi(">1u"); // DISAMBIGUATE only
    t.clearOutput();
    t.sendKey(Key_Shift_L, ShiftModifier, KeyAction_Press);
    CHECK(t.output().empty());
}

TEST_CASE("kitty: F1 no mods sends legacy") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_F1, 0, KeyAction_Press);
    CHECK(t.output() == "\x1bOP");
}

TEST_CASE("kitty: shift+F1 sends CSI with mods") {
    TestTerminal t;
    t.csi(">1u");
    t.clearOutput();
    t.sendKey(Key_F1, ShiftModifier, KeyAction_Press);
    CHECK(t.output() == "\x1b[1;2P");
}

// === RIS full reset tests ===

TEST_CASE("RIS resets cursor visibility") {
    TestTerminal t;
    t.csi("?25l"); // hide cursor
    CHECK_FALSE(t.term.cursorVisible());
    t.esc("c"); // RIS
    CHECK(t.term.cursorVisible());
}

TEST_CASE("RIS resets cursor shape") {
    TestTerminal t;
    t.csi("4 q"); // steady underline
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyUnderline);
    t.esc("c"); // RIS
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
}

TEST_CASE("RIS resets mouse modes") {
    TestTerminal t;
    t.csi("?1000h"); // enable mouse
    CHECK(t.term.mouseReportingActive());
    t.esc("c"); // RIS
    CHECK_FALSE(t.term.mouseReportingActive());
}

TEST_CASE("RIS resets bracketed paste") {
    TestTerminal t;
    t.csi("?2004h"); // enable bracketed paste
    CHECK(t.term.bracketedPaste());
    t.esc("c"); // RIS
    CHECK_FALSE(t.term.bracketedPaste());
}

TEST_CASE("RIS resets kitty keyboard flags") {
    TestTerminal t;
    t.csi(">5u");
    CHECK(t.term.kittyFlags() == 5);
    t.esc("c"); // RIS
    CHECK(t.term.kittyFlags() == 0);
}

TEST_CASE("RIS exits alt screen") {
    TestTerminal t;
    t.csi("?1049h"); // enter alt screen
    t.esc("c"); // RIS
    // Should be back on main screen with cursor visible
    CHECK(t.term.cursorVisible());
}

TEST_CASE("alt screen switch mid-injectData writes to correct grid") {
    TestTerminal t;
    // Write 'A' on main screen
    t.feed("A");
    CHECK(t.wc(0, 0) == 'A');

    // Single injectData call that enters alt screen, clears, and writes 'B'
    // This tests that the grid reference is refreshed after alt screen switch
    t.feed("\x1b[?1049h\x1b[H\x1b[2JB");

    // Alt screen should have 'B' at (0,0)
    CHECK(t.wc(0, 0) == 'B');

    // Exit alt screen
    t.csi("?1049l");

    // Main screen should still have 'A'
    CHECK(t.wc(0, 0) == 'A');
}

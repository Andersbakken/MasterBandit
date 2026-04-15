#include <doctest/doctest.h>
#include "TestTerminal.h"

// ── RIS - full reset ──────────────────────────────────────────────────────────

TEST_CASE("RIS resets cursor and clears screen")
{
    TestTerminal t;
    t.feed("Hello");
    t.csi("5;10H");
    t.esc("c");  // RIS
    CHECK(t.term.cursorX() == 0);
    CHECK(t.term.cursorY() == 0);
    CHECK(t.rowText(0) == "");
}

TEST_CASE("RIS resets SGR attributes")
{
    TestTerminal t;
    t.csi("1m");   // bold
    t.esc("c");
    t.feed("A");
    CHECK_FALSE(t.attrs(0, 0).bold());
}

// ── cursor visibility ─────────────────────────────────────────────────────────

TEST_CASE("DECTCEM hide cursor (mode 25 reset)")
{
    TestTerminal t;
    CHECK(t.term.cursorVisible());
    t.csi("?25l");
    CHECK_FALSE(t.term.cursorVisible());
}

TEST_CASE("DECTCEM show cursor (mode 25 set)")
{
    TestTerminal t;
    t.csi("?25l");
    t.csi("?25h");
    CHECK(t.term.cursorVisible());
}

// ── alt screen (mode 1049) ────────────────────────────────────────────────────

TEST_CASE("alt screen is clean on entry")
{
    TestTerminal t;
    t.feed("Main content");
    t.csi("?1049h");
    CHECK(t.rowText(0) == "");
}

TEST_CASE("alt screen does not affect main screen content")
{
    TestTerminal t;
    t.feed("Main content");
    t.csi("?1049h");
    t.feed("Alt content");
    t.csi("?1049l");
    CHECK(t.rowText(0) == "Main content");
}

TEST_CASE("alt screen restores cursor position on exit")
{
    TestTerminal t;
    t.csi("5;10H");  // move cursor on main screen
    t.csi("?1049h"); // save cursor, enter alt
    t.csi("1;1H");   // move on alt screen
    t.csi("?1049l"); // restore cursor, exit alt
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

TEST_CASE("alt screen does not leak mode changes back to main")
{
    TestTerminal t;
    // Set a handful of modes on main. An app that enters alt must not be able
    // to leave these modified once it exits.
    t.csi("?1000h");   // mouse reporting
    t.csi("?2004h");   // bracketed paste
    REQUIRE(t.term.mouseReportingActive());
    REQUIRE(t.term.bracketedPaste());

    t.csi("?1049h");
    // Alt starts clean — modes reset to defaults.
    CHECK_FALSE(t.term.mouseReportingActive());
    CHECK_FALSE(t.term.bracketedPaste());
    // App on alt turns these off AND on again (then crashes without cleanup).
    t.csi("?1000l");
    t.csi("?2004h");

    t.csi("?1049l");
    // Main's modes are intact, regardless of what alt did.
    CHECK(t.term.mouseReportingActive());
    CHECK(t.term.bracketedPaste());
}

TEST_CASE("alt screen isolates DECSC save slot")
{
    TestTerminal t;
    // DECSC on main at position (5, 3).
    t.csi("4;6H");
    t.esc("7");  // DECSC
    t.csi("?1049h");
    // DECSC on alt at a different position.
    t.csi("10;20H");
    t.esc("7");  // DECSC on alt — must not clobber main's save slot
    t.csi("?1049l");
    // DECRC on main should restore to (5, 3), not alt's saved position.
    t.esc("8");  // DECRC
    CHECK(t.term.cursorX() == 5);
    CHECK(t.term.cursorY() == 3);
}

// Query a private DEC mode via DECRQM (CSI ? Pm $ p). Response is
// CSI ? <mode> ; <state> $ y where state=1 means set, 2 means reset,
// 0 means unknown. Returns 0/1/2/3/4 per DECRQM spec.
static int queryMode(TestTerminal& t, int mode)
{
    t.clearOutput();
    t.csi("?" + std::to_string(mode) + "$p");
    const std::string& out = t.output();
    std::string needle = "\x1b[?" + std::to_string(mode) + ";";
    auto pos = out.find(needle);
    if (pos == std::string::npos) return -1;
    auto start = pos + needle.size();
    auto end = out.find('$', start);
    if (end == std::string::npos) return -1;
    return std::stoi(out.substr(start, end - start));
}

TEST_CASE("alt screen isolates DECRQM-queryable modes")
{
    TestTerminal t;
    // Modes supported by DECRQM in this terminal: 1049, 2004, 2026, 2031.
    // 1049 is the screen itself — can't meaningfully test it here, so we
    // stick to the app-settable app-mode flags.
    t.csi("?2004h");  // bracketed paste
    t.csi("?2026h");  // synchronized output
    t.csi("?2031h");  // color preference reporting
    REQUIRE(queryMode(t, 2004) == 1);
    REQUIRE(queryMode(t, 2026) == 1);
    REQUIRE(queryMode(t, 2031) == 1);

    t.csi("?1049h");
    CHECK(queryMode(t, 2004) == 2);
    CHECK(queryMode(t, 2026) == 2);
    CHECK(queryMode(t, 2031) == 2);

    // Alt flips them to the opposite of main.
    t.csi("?2004l");
    t.csi("?2026h");
    t.csi("?2031h");

    t.csi("?1049l");
    // Main's state is intact regardless of what alt did.
    CHECK(queryMode(t, 2004) == 1);
    CHECK(queryMode(t, 2026) == 1);
    CHECK(queryMode(t, 2031) == 1);
}

TEST_CASE("alt screen isolates cursor shape (DECSCUSR)")
{
    TestTerminal t;
    // Default is blinking block.
    REQUIRE(t.term.cursorShape() == TerminalEmulator::CursorBlock);

    t.csi("2 q");  // DECSCUSR — steady block on main
    REQUIRE(t.term.cursorShape() == TerminalEmulator::CursorSteadyBlock);

    t.csi("?1049h");
    // Alt starts with the configured default (still block here).
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
    t.csi("5 q");  // steady bar on alt (vim-style)
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBar);

    t.csi("?1049l");
    // Main's cursor shape preserved.
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBlock);
}

TEST_CASE("alt screen isolates SGR pen")
{
    TestTerminal t;
    // Turn on bold on main, write 'A'.
    t.csi("1m");
    t.feed("A");
    REQUIRE(t.attrs(0, 0).bold());

    t.csi("?1049h");
    // Alt pen starts clean — no bold carried over.
    t.feed("B");
    CHECK_FALSE(t.attrs(0, 0).bold());

    t.csi("?1049l");
    // Main is back — previously written 'A' is still bold (cell data
    // preserved), and the pen is still bold so a new write carries bold too.
    CHECK(t.attrs(0, 0).bold());
    t.feed("C");
    CHECK(t.attrs(1, 0).bold());
}

TEST_CASE("RIS clears scrollback history")
{
    TestTerminal t(80, 4);
    // Feed enough lines to push content into scrollback.
    for (int i = 0; i < 20; ++i) t.feed("line\n");
    REQUIRE(t.term.document().historySize() > 0);

    t.esc("c");  // RIS

    CHECK(t.term.document().historySize() == 0);
}

TEST_CASE("RIS resets both main and alt state")
{
    TestTerminal t;
    // Set modes on main.
    t.csi("?2004h");
    t.csi("?2026h");
    REQUIRE(t.term.bracketedPaste());
    REQUIRE(t.term.syncOutputActive());

    // Enter alt, set modes there too.
    t.csi("?1049h");
    t.csi("?2004h");
    t.csi("?2026h");
    REQUIRE(t.term.bracketedPaste());
    REQUIRE(t.term.syncOutputActive());

    // RIS (from alt). Must reset both screens — not just the active one.
    t.esc("c");

    // Active state (now main again because RIS exits alt) is clean.
    CHECK_FALSE(t.term.bracketedPaste());
    CHECK_FALSE(t.term.syncOutputActive());

    // Flip to alt and verify alt is also clean, proving RIS touched both.
    t.csi("?1049h");
    CHECK_FALSE(t.term.bracketedPaste());
    CHECK_FALSE(t.term.syncOutputActive());
}

TEST_CASE("setDefaultCursorShape propagates to both states")
{
    TestTerminal t;
    // Enter alt first so we're operating against mAltState as the active.
    t.csi("?1049h");
    REQUIRE(t.term.cursorShape() == TerminalEmulator::CursorBlock);

    // Config reload while on alt — the live (alt) shape updates immediately
    // AND main's state gets the new default so exit doesn't revert.
    t.term.setDefaultCursorShape(TerminalEmulator::CursorSteadyBar);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBar);

    t.csi("?1049l");
    // Main reflects the new config default, not the pre-reload block.
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBar);
}

// ── mouse modes ───────────────────────────────────────────────────────────────

TEST_CASE("mouse mode 1000 set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.mouseReportingActive());
    t.csi("?1000h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1000l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

TEST_CASE("mouse mode 1002 set/reset")
{
    TestTerminal t;
    t.csi("?1002h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1002l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

TEST_CASE("mouse mode 1003 set/reset")
{
    TestTerminal t;
    t.csi("?1003h");
    CHECK(t.term.mouseReportingActive());
    t.csi("?1003l");
    CHECK_FALSE(t.term.mouseReportingActive());
}

// ── SGR mouse reporting (mode 1006) ──────────────────────────────────────────

TEST_CASE("mouse mode 1006 SGR format press/release")
{
    TestTerminal t;
    t.csi("?1000h"); // enable tracking
    t.csi("?1006h"); // enable SGR format
    t.clearOutput();

    MouseEvent press;
    press.x = 4; press.y = 2;
    press.pixelX = 40; press.pixelY = 30;
    press.button = LeftButton; press.buttons = LeftButton;
    t.term.mousePressEvent(&press);
    CHECK(t.output() == "\x1b[<0;5;3M"); // 1-based: col 5, row 3

    t.clearOutput();
    MouseEvent release;
    release.x = 4; release.y = 2;
    release.pixelX = 40; release.pixelY = 30;
    release.button = LeftButton;
    t.term.mouseReleaseEvent(&release);
    CHECK(t.output() == "\x1b[<0;5;3m"); // lowercase m = release
}

// ── SGR-Pixel mouse reporting (mode 1016) ────────────────────────────────────

TEST_CASE("mouse mode 1016 SGR-Pixel format press/release")
{
    TestTerminal t;
    t.csi("?1000h"); // enable tracking
    t.csi("?1016h"); // enable SGR-Pixel format
    t.clearOutput();

    MouseEvent press;
    press.x = 4; press.y = 2;
    press.pixelX = 40; press.pixelY = 30;
    press.button = LeftButton; press.buttons = LeftButton;
    t.term.mousePressEvent(&press);
    CHECK(t.output() == "\x1b[<0;41;31M"); // 1-based pixel: 41, 31

    t.clearOutput();
    MouseEvent release;
    release.x = 4; release.y = 2;
    release.pixelX = 40; release.pixelY = 30;
    release.button = LeftButton;
    t.term.mouseReleaseEvent(&release);
    CHECK(t.output() == "\x1b[<0;41;31m");
}

TEST_CASE("mouse mode 1016 falls back to 1006 when pixel coords unavailable")
{
    TestTerminal t;
    t.csi("?1000h");
    t.csi("?1016h");
    t.csi("?1006h");
    t.clearOutput();

    MouseEvent press;
    press.x = 4; press.y = 2;
    press.pixelX = -1; press.pixelY = -1; // no pixel info
    press.button = LeftButton; press.buttons = LeftButton;
    t.term.mousePressEvent(&press);
    CHECK(t.output() == "\x1b[<0;5;3M"); // cell coords, not pixel
}

TEST_CASE("mouse mode 1016 set/reset via DECRQM")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("?1016$p");
    CHECK(t.output() == "\x1b[?1016;2$y"); // recognized, reset

    t.clearOutput();
    t.csi("?1016h");
    t.csi("?1016$p");
    CHECK(t.output() == "\x1b[?1016;1$y"); // recognized, set
}

// ── bracketed paste (mode 2004) ───────────────────────────────────────────────

TEST_CASE("bracketed paste mode set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.bracketedPaste());
    t.csi("?2004h");
    CHECK(t.term.bracketedPaste());
    t.csi("?2004l");
    CHECK_FALSE(t.term.bracketedPaste());
}

// ── synchronized output (mode 2026) ──────────────────────────────────────────

TEST_CASE("synchronized output mode set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.syncOutputActive());
    t.csi("?2026h");
    CHECK(t.term.syncOutputActive());
    t.csi("?2026l");
    CHECK_FALSE(t.term.syncOutputActive());
}

// ── Device Attributes ─────────────────────────────────────────────────────────

TEST_CASE("primary DA responds to CSI c")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("c");
    CHECK_FALSE(t.term.capturedOutput.empty());
    // Response starts with ESC [ ?
    CHECK(t.term.capturedOutput.substr(0, 3) == "\x1b[?");
}

TEST_CASE("secondary DA responds to CSI > c")
{
    TestTerminal t;
    t.clearOutput();
    t.feed("\x1b[>c");
    CHECK_FALSE(t.term.capturedOutput.empty());
    CHECK(t.term.capturedOutput.substr(0, 3) == "\x1b[>");
}

TEST_CASE("XTVERSION responds to CSI > q")
{
    TestTerminal t;
    t.clearOutput();
    t.feed("\x1b[>q");
    CHECK_FALSE(t.term.capturedOutput.empty());
    // DCS response: ESC P > | ...
    CHECK(t.term.capturedOutput.find("MasterBandit") != std::string::npos);
}

// ── DECSCUSR (cursor style) ──────────────────────────────────────────────────

TEST_CASE("DECSCUSR default is block")
{
    TestTerminal t;
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
}

TEST_CASE("DECSCUSR 0 resets to block")
{
    TestTerminal t;
    t.csi("5 q");  // bar
    t.csi("0 q");  // reset
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
}

TEST_CASE("DECSCUSR 1 sets blinking block")
{
    TestTerminal t;
    t.csi("1 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
}

TEST_CASE("DECSCUSR 2 sets steady block")
{
    TestTerminal t;
    t.csi("2 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBlock);
}

TEST_CASE("DECSCUSR 3 sets blinking underline")
{
    TestTerminal t;
    t.csi("3 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorUnderline);
}

TEST_CASE("DECSCUSR 4 sets steady underline")
{
    TestTerminal t;
    t.csi("4 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyUnderline);
}

TEST_CASE("DECSCUSR 5 sets blinking bar")
{
    TestTerminal t;
    t.csi("5 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBar);
}

TEST_CASE("DECSCUSR 6 sets steady bar")
{
    TestTerminal t;
    t.csi("6 q");
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBar);
}

// ── color preference notification (mode 2031) ───────────────────────────────

TEST_CASE("mode 2031 set/reset")
{
    TestTerminal t;
    CHECK_FALSE(t.term.colorPreferenceReporting());
    t.csi("?2031h");
    CHECK(t.term.colorPreferenceReporting());
    t.csi("?2031l");
    CHECK_FALSE(t.term.colorPreferenceReporting());
}

TEST_CASE("DSR 996 responds with color preference")
{
    TestTerminal t;
    t.clearOutput();
    t.feed("\x1b[?996n");
    CHECK_FALSE(t.term.capturedOutput.empty());
    // Response: ESC [ ? 997 ; 1 n (dark) or ESC [ ? 997 ; 2 n (light)
    // Default isDarkMode callback is null, so defaults to dark (1)
    CHECK(t.term.capturedOutput == "\x1b[?997;1n");
}

TEST_CASE("notifyColorPreference sends when mode 2031 is set")
{
    TestTerminal t;
    t.csi("?2031h");
    t.clearOutput();
    t.term.notifyColorPreference(false);  // light mode
    CHECK(t.term.capturedOutput == "\x1b[?997;2n");
}

TEST_CASE("notifyColorPreference silent when mode 2031 is not set")
{
    TestTerminal t;
    t.clearOutput();
    t.term.notifyColorPreference(true);
    CHECK(t.term.capturedOutput.empty());
}

// ── DECREQTPARM (CSI Ps x) ───────────────────────────────────────────────────

TEST_CASE("DECREQTPARM Ps=0 returns Psol=2")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("0x");
    CHECK(t.output() == "\x1b[2;1;1;128;128;1;0x");
}

TEST_CASE("DECREQTPARM Ps=1 returns Psol=3")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("1x");
    CHECK(t.output() == "\x1b[3;1;1;128;128;1;0x");
}

TEST_CASE("DECREQTPARM with no parameter defaults to Ps=0")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("x");
    CHECK(t.output() == "\x1b[2;1;1;128;128;1;0x");
}

TEST_CASE("DECREQTPARM with invalid Ps does not respond")
{
    TestTerminal t;
    t.clearOutput();
    t.csi("2x");
    CHECK(t.output().empty());
}

TEST_CASE("CSI x with intermediate is not treated as DECREQTPARM")
{
    TestTerminal t;
    t.clearOutput();
    // CSI 1;1;1;1;1$x — DECFRA-style, has '$' intermediate
    t.csi("1;1;1;1;1$x");
    CHECK(t.output().empty());
}

// ── XTSAVE / XTRESTORE (CSI ? Pm s / CSI ? Pm r) ─────────────────────────────

TEST_CASE("XTSAVE/XTRESTORE round-trips a single mode")
{
    TestTerminal t;
    t.csi("?25h");          // cursor visible
    t.csi("?25s");          // save
    t.csi("?25l");          // hide
    CHECK_FALSE(t.term.cursorVisible());
    t.csi("?25r");          // restore
    CHECK(t.term.cursorVisible());
}

TEST_CASE("XTSAVE/XTRESTORE round-trips multiple modes via list")
{
    TestTerminal t;
    t.csi("?25h");
    t.csi("?2004h");
    t.csi("?25;2004s");     // save both
    t.csi("?25l");
    t.csi("?2004l");
    CHECK_FALSE(t.term.cursorVisible());
    CHECK_FALSE(t.term.bracketedPaste());
    t.csi("?25;2004r");     // restore both
    CHECK(t.term.cursorVisible());
    CHECK(t.term.bracketedPaste());
}

TEST_CASE("XTSAVE with no parameters saves all known modes")
{
    TestTerminal t;
    t.csi("?25h");
    t.csi("?2004h");
    t.csi("?s");            // save all
    t.csi("?25l");
    t.csi("?2004l");
    t.csi("?r");            // restore all
    CHECK(t.term.cursorVisible());
    CHECK(t.term.bracketedPaste());
}

TEST_CASE("XTRESTORE without prior save is a no-op")
{
    TestTerminal t;
    CHECK(t.term.cursorVisible());
    t.csi("?25r");          // nothing was saved
    CHECK(t.term.cursorVisible());
}

TEST_CASE("XTSAVE captures false values too")
{
    TestTerminal t;
    t.csi("?25l");          // hide
    t.csi("?25s");          // save (false)
    t.csi("?25h");          // show
    CHECK(t.term.cursorVisible());
    t.csi("?25r");          // restore -> false
    CHECK_FALSE(t.term.cursorVisible());
}

TEST_CASE("XTRESTORE does not invoke DECSTBM scroll-region path")
{
    // Regression: CSI ? r used to fall through to DECSTBM and reset the cursor.
    TestTerminal t;
    t.csi("5;10H");
    t.csi("?r");
    CHECK(t.term.cursorX() == 9);
    CHECK(t.term.cursorY() == 4);
}

// ── cursor blink (DEC private mode 12) ───────────────────────────────────────

TEST_CASE("cursor blink defaults to enabled")
{
    TestTerminal t;
    CHECK(t.term.cursorBlinkEnabled());
    // Default shape is CursorBlock (blinking variant), so blinking is active.
    CHECK(t.term.cursorBlinking());
}

TEST_CASE("CSI ?12l disables cursor blink flag")
{
    TestTerminal t;
    t.csi("?12l");
    CHECK_FALSE(t.term.cursorBlinkEnabled());
    // Default shape is CursorBlock (blinking variant) — DECSCUSR blinking
    // shapes blink regardless of mode 12.
    CHECK(t.term.cursorBlinking());
}

TEST_CASE("CSI ?12h re-enables cursor blink")
{
    TestTerminal t;
    t.csi("?12l");
    t.csi("?12h");
    CHECK(t.term.cursorBlinkEnabled());
    CHECK(t.term.cursorBlinking());
}

TEST_CASE("steady cursor shape blinks when mode 12 is on")
{
    TestTerminal t;
    t.csi("2 q");  // DECSCUSR steady block
    CHECK(t.term.cursorBlinkEnabled());
    // Mode 12 independently enables blinking even for steady shapes
    CHECK(t.term.cursorBlinking());
}

TEST_CASE("steady cursor shape does not blink when mode 12 is off")
{
    TestTerminal t;
    t.csi("2 q");  // DECSCUSR steady block
    t.csi("?12l"); // mode 12 off
    CHECK_FALSE(t.term.cursorBlinking());
}

TEST_CASE("blinking cursor shape blinks regardless of mode 12")
{
    TestTerminal t;
    t.csi("5 q");    // DECSCUSR blinking bar
    CHECK(t.term.cursorBlinking());
    t.csi("?12l");
    // Blinking shape variant always blinks — mode 12 cannot suppress it
    CHECK(t.term.cursorBlinking());
}

TEST_CASE("XTSAVE/XTRESTORE round-trips mode 12")
{
    TestTerminal t;
    t.csi("?12s");      // save (currently true)
    t.csi("?12l");      // disable
    CHECK_FALSE(t.term.cursorBlinkEnabled());
    t.csi("?12r");      // restore
    CHECK(t.term.cursorBlinkEnabled());
}

TEST_CASE("RIS restores configured cursor defaults")
{
    TestTerminal t;
    CursorConfig cc;
    cc.shape = "underline";
    cc.blink = false;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyUnderline);
    CHECK_FALSE(t.term.cursorBlinkEnabled());

    // App changes cursor at runtime
    t.csi("5 q");        // blinking bar
    t.csi("?12h");       // blink on
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBar);
    CHECK(t.term.cursorBlinkEnabled());

    t.esc("c");          // RIS
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyUnderline);
    CHECK_FALSE(t.term.cursorBlinkEnabled());
}

// ── cursor config mapping ────────────────────────────────────────────────────

TEST_CASE("applyCursorConfig: block + blink → CursorBlock")
{
    TestTerminal t;
    CursorConfig cc; cc.shape = "block"; cc.blink = true;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
    CHECK(t.term.cursorBlinkEnabled());
}

TEST_CASE("applyCursorConfig: block + no blink → CursorSteadyBlock")
{
    TestTerminal t;
    CursorConfig cc; cc.shape = "block"; cc.blink = false;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBlock);
    CHECK_FALSE(t.term.cursorBlinkEnabled());
}

TEST_CASE("applyCursorConfig: underline + blink → CursorUnderline")
{
    TestTerminal t;
    CursorConfig cc; cc.shape = "underline"; cc.blink = true;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorUnderline);
}

TEST_CASE("applyCursorConfig: bar + no blink → CursorSteadyBar")
{
    TestTerminal t;
    CursorConfig cc; cc.shape = "bar"; cc.blink = false;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorSteadyBar);
}

TEST_CASE("applyCursorConfig: unknown shape falls back to block")
{
    TestTerminal t;
    CursorConfig cc; cc.shape = "weird"; cc.blink = true;
    t.term.applyCursorConfig(cc);
    CHECK(t.term.cursorShape() == TerminalEmulator::CursorBlock);
}

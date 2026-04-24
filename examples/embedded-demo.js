// embedded-demo.js — inline widget example
//
// Load with: mb --script embedded-demo.js
// Or via OSC:
//   printf '\e]58237;applet;path=/path/to/embedded-demo.js;permissions=ui,io,io.filter.input\e\\'
//
// Press Ctrl+G (BEL) at the shell prompt to spawn an embedded terminal
// anchored at the cursor row. The embedded renders a small expandable
// widget: click the header to toggle between compact and expanded layouts;
// press 'q' while it's focused to close it.
//
// Why an input filter (io.filter.input): the applet watches keystrokes
// going to the pane and swallows Ctrl+G before the shell sees it. Real
// applets typically use `mb.registerAction` plus a user-bound key, but for
// a self-contained demo the filter approach needs no config.

const pane = mb.activePane;
if (!pane) throw new Error("no active pane");

let counter = 0;

// The shell rarely uses BEL as a binding, so stealing Ctrl+G is safe.
pane.addEventListener("input", (data) => {
    if (data !== "\x07") return; // everything else passes through
    createInlineWidget();
    return ""; // swallow Ctrl+G
});

function createInlineWidget() {
    const em = pane.createEmbeddedTerminal({ rows: 6 });
    if (!em) {
        // Refused (alt-screen, or another embedded already anchored at the cursor row).
        console.log("[embedded-demo] createEmbeddedTerminal refused");
        return;
    }

    const id = ++counter;
    let expanded = false;

    function render() {
        const focused = em.focused;
        const border = focused ? "\x1b[1;36m" : "\x1b[90m";
        const accent = focused ? "\x1b[1;33m" : "\x1b[33m";
        const reset  = "\x1b[0m";

        const rows = em.rows;
        const cols = em.cols;
        const innerW = cols - 2;
        const pad = (s, w) => s.length >= w ? s.substring(0, w) : s + " ".repeat(w - s.length);

        const title = ` widget #${id}  ${expanded ? "(expanded)" : "(compact)"} `;
        const hint  = focused ? " click header to toggle  |  q:close " : " click me to focus ";

        const top    = border + "+" + "-".repeat(innerW) + "+" + reset;
        const bottom = border + "+" + "-".repeat(innerW) + "+" + reset;
        const line = (color, text) =>
            border + "|" + reset + color + pad(text, innerW) + reset + border + "|" + reset + "\r\n";

        // Clear + home, then paint from (0,0).
        let out = "\x1b[2J\x1b[H";
        out += top + "\r\n";
        out += line(accent, title);
        if (expanded) {
            out += line("", " Row " + em.id);
            out += line("", " Cols x Rows = " + cols + " x " + rows);
            out += line("", " Cell px      = " + em.cellWidth + " x " + em.cellHeight);
        } else {
            out += line("", " (click header to expand)");
        }
        // Fill remaining rows with blanks.
        const drawn = expanded ? 5 : 3; // header + N body lines
        for (let i = drawn; i < rows - 1; ++i) out += line("", "");
        out += bottom;
        out += hint;
        em.inject(out);
    }

    em.addEventListener("mouse", (ev) => {
        // Click row 1 (header) toggles expansion. Other clicks just focus.
        if (ev.type === "press" && ev.button === 0 && ev.cellY === 1) {
            expanded = !expanded;
            em.resize(expanded ? 8 : 6); // "resized" event will also re-render
        }
    });

    em.addEventListener("mousemove", (ev) => {
        // Simple hover indicator on the header row: nothing persistent, just
        // demonstrates the event wiring.
        if (ev.cellY === 1) {
            // Move the cursor out to the margin so header highlighting is visible.
            em.inject("\x1b[" + (ev.cellY + 1) + ";" + (ev.cellX + 1) + "H");
        }
    });

    em.addEventListener("input", (data) => {
        // Bare Esc is consumed by the engine (defocuses the embedded); we see
        // everything else. 'q' closes; arrows move the embedded's cursor.
        if (data === "q" || data === "Q") {
            em.close();
            return;
        }
        if (data === "\x1b[A" || data === "\x1b[B" ||
            data === "\x1b[C" || data === "\x1b[D") {
            // Let the emulator move its own cursor — just feed the escape.
            em.inject(data);
        }
    });

    em.addEventListener("resized", (_cols, _rows) => render());
    em.addEventListener("destroyed", () => {
        console.log("[embedded-demo] widget #" + id + " destroyed");
    });

    // Re-render when focus changes (focused cursor vs hollow outline differs
    // visually; the applet can also switch color scheme). FocusPopup is the
    // action that cycles pane → popups → embeddeds → pane.
    mb.addEventListener("action", "FocusPopup", render);

    render();
}

console.log("[embedded-demo] loaded. Press Ctrl+G at the shell to spawn a widget.");

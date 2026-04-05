// popup-demo.js — demo popup with focus-aware rendering and input handling
//
// Load with: mb --script popup-demo.js
// Or via OSC: printf '\e]58237;applet;path=/path/to/popup-demo.js\e\\'
//
// Press Cmd+Shift+I (macOS) or Ctrl+Shift+I (Linux) to focus the popup.
// Press 'q' while focused to close it. Arrow keys move the cursor.

const pane = mb.activePane();
if (!pane) throw new Error("no active pane");

const W = 30, H = 7;
const popup = pane.createPopup({
    id: "demo",
    x: 2, y: 1,
    w: W, h: H
});
if (!popup) throw new Error("failed to create popup");

// Cursor position within the editable area (row 2-4, col 1-28)
let curCol = 1, curRow = 2;
const minCol = 1, maxCol = W - 2;
const minRow = 2, maxRow = H - 3;

function render() {
    const focused = popup.focused;
    const border = focused ? "\x1b[1;36m" : "\x1b[90m";
    const fg     = focused ? "\x1b[1;37m" : "\x1b[37m";
    const hint   = focused ? "\x1b[33m"   : "\x1b[90m";
    const reset  = "\x1b[0m";
    const inner  = W - 2;

    const pad = (s, w) => s.length >= w ? s.substring(0, w) : s + " ".repeat(w - s.length);

    const titleText = focused ? " Popup Demo FOCUSED " : " Popup Demo UNFOCUSED ";
    const hintText  = focused ? " q:close arrows:move"  : " use keybind to focus";

    const top    = border + "+" + "-".repeat(inner) + "+" + reset;
    const bottom = border + "+" + "-".repeat(inner) + "+" + reset;
    const line = (color, text) =>
        border + "|" + reset + color + pad(text, inner) + reset + border + "|" + reset;

    let out = "\x1b[H";
    out += top;
    out += line(fg, titleText);
    for (let r = minRow; r <= maxRow; r++)
        out += line("", "");
    out += line(hint, hintText);
    out += bottom;

    // Position cursor inside the editable area (1-based for ANSI)
    out += "\x1b[" + (curRow + 1) + ";" + (curCol + 1) + "H";

    popup.inject(out);
}

// Re-render on focus changes
mb.addEventListener("action", "FocusPopup", () => render());
mb.addEventListener("action", "FocusPane", () => render());

// Handle input when focused
popup.addEventListener("input", (data) => {
    if (data === "q" || data === "Q") {
        popup.destroy();
        return;
    }

    // Arrow keys come as escape sequences from the terminal emulator
    if (data === "\x1b[A") { // up
        if (curRow > minRow) curRow--;
        render();
    } else if (data === "\x1b[B") { // down
        if (curRow < maxRow) curRow++;
        render();
    } else if (data === "\x1b[C") { // right
        if (curCol < maxCol) curCol++;
        render();
    } else if (data === "\x1b[D") { // left
        if (curCol > minCol) curCol--;
        render();
    } else if (data.length === 1 && data >= " " && data <= "~") {
        // Printable character — place it and advance cursor
        popup.inject("\x1b[" + (curRow + 1) + ";" + (curCol + 1) + "H" + data);
        if (curCol < maxCol) curCol++;
        popup.inject("\x1b[" + (curRow + 1) + ";" + (curCol + 1) + "H");
    }
});

// Initial render
render();

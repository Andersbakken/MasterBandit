// set-title.js — set a custom title override on the focused pane.
//
// Bound to a keybinding via the `title.set` script action. Opens a
// centered popup with a text input pre-filled with the pane's current
// effective title and three buttons: Set (commits the input as the
// override), Cancel (closes without changes), and Clear (removes any
// existing override and falls back to the OSC title).
//
// pane.title is a string getter that returns the effective title
// (override if set, else OSC); the setter accepts a string to install
// an override, or null/undefined to clear it. We use `pane.title = ""`
// to mean "show an empty custom title" (valid) and `pane.title = null`
// to revert to the OSC title.
//
// Re-invoking the action while the popup is open closes it (same
// toggle behavior as scrollback-search / command-palette).

import { signal, render, createTheme, box, text, input, row, button, measure } from "mb:tui";

mb.setNamespace("title");
mb.registerAction("set");

const theme = createTheme({
    bg:     '#1a1b26',
    border: { color: '#7aa2f7' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    text:   {
        color:      'white',
        selectedFg: '#1a1b26', selectedBg: '#7aa2f7',
        hoverFg:    '#c0caf5', hoverBg:    '#414868',
    },
});

let ui = null;

mb.addEventListener("action", "title.set", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (ui) {
        ui.destroy();
        return;
    }

    // Pre-fill with the current effective title — most users want to
    // tweak the existing one rather than start blank. `pane.title`
    // getter returns the override if engaged, else the OSC title,
    // else "".
    const initial = pane.title || "";
    const query   = signal(initial);

    const finish = (commit) => {
        if (commit === "set") {
            pane.title = query.value;
        } else if (commit === "clear") {
            // Null clears the override; the OSC title (or fg process
            // name) shows again on the next tab-bar pull.
            pane.title = null;
        }
        // "cancel" leaves the override untouched.
        if (ui) ui.destroy();
    };

    const labels = {
        set:    "[ Set ]",
        cancel: "[ Cancel ]",
        clear:  "[ Clear ]",
    };

    const setBtn = button({
        label:   labels.set,
        primary: true,
        width:   labels.set.length,
        onClick: () => finish("set"),
    });
    const cancelBtn = button({
        label:   labels.cancel,
        width:   labels.cancel.length,
        onClick: () => finish("cancel"),
    });
    const clearBtn = button({
        label:   labels.clear,
        width:   labels.clear.length,
        onClick: () => finish("clear"),
    });

    const inputNode = input({
        value:    query,
        prompt:   " > ",
        // Enter while the input has focus commits as Set. Enter on a
        // focused button is handled by tui directly via that button's
        // onClick — onSubmit only fires when the input itself is focused.
        onSubmit: () => finish("set"),
    });

    const buildRoot = () => box({ border: "round", padding: 1 }, [
        text({ value: "Set pane title", align: "center", color: "bright-white.bold" }),
        text({ value: "" }),
        inputNode,
        text({ value: "" }),
        row({ gap: 2, justify: "center" }, [setBtn, cancelBtn, clearBtn]),
    ]);

    // Width: enough for the title row, the buttons row, plus border+padding
    // (1 border + 1 padding on each side = 4 cells overhead). The 32-cell
    // floor matches the visual weight of similar dialogs (confirm()).
    const headerW = "Set pane title".length;
    const btnsW   = labels.set.length + labels.cancel.length + labels.clear.length + 4; // two 2-cell gaps
    const w       = Math.max(headerW, btnsW, 32) + 4;

    const root = buildRoot();
    const m    = measure(root, w);
    const h    = m.h;

    const cx = Math.max(0, Math.floor((pane.cols - w) / 2));
    const cy = Math.max(0, Math.floor((pane.rows - h) / 2));

    const popup = pane.createPopup({ id: "mb-set-title", x: cx, y: cy, w, h });
    if (!popup) return;

    // Esc is handled by tui's built-in dismiss path; closing without a
    // button click is treated as cancel (no commit). Enter activates
    // whichever button is focused, via tui's own keyboard handling —
    // no popup-level Enter shortcut here, because it would double-fire
    // alongside the focused button's onClick and re-install the input
    // text as the override on every Clear/Cancel keystroke.

    // If the host pane goes away (PTY exit, tab close), tear down.
    pane.addEventListener("destroyed", () => {
        if (ui) ui.destroy();
    });

    ui = render(popup, root, {
        theme,
        onDestroy: () => {
            ui = null;
        },
    });

    // Focus the input by default — the typical flow is "edit then Enter".
    ui.focus(inputNode);
});

console.log("set-title: initialized");

// popup-tui-mouse-demo.js — exercises mb:tui mouse + hover support.
//
// Load with: mb --script examples/popup-tui-mouse-demo.js
// Or via OSC: printf '\e]58237;applet;path=/path/to/popup-tui-mouse-demo.js;permissions=ui,io\e\\'
//
// What to verify:
//   Click behavior:
//   • Click [ Yes ] / [ No ] — status updates; click fires only on release when
//     press and release land on the same button.
//   • Press one button, drag onto another, release — neither fires.
//   • Click a list row — selection moves; onSelect fires.
//   • Click a list row, drag to a different row, release — no selection change.
//   • Click outside any widget (border / padding) — no action, no error.
//
//   Hover behavior:
//   • Move the cursor over [ Yes ] / [ No ] / [ Close ] — each highlights via
//     theme.text.hoverFg/hoverBg while the cursor is over it.
//   • Move the cursor over list rows — non-selected rows get a subtler
//     highlight via theme.list.hoverStyle; the selected row stays fully
//     highlighted (selection wins over hover).
//   • Move the cursor over [ Yes ] specifically — the description line
//     updates via onMouseEnter; moving away restores it via onMouseLeave.
//   • Move the cursor outside the popup — hover state clears.
//
//   Keyboard parity:
//   • Cmd+Shift+I / Ctrl+Shift+I focuses the popup; Tab cycles focus; arrows
//     move list selection; Esc dismisses.

import {
    signal, render, createTheme,
    box, row, text, list, button,
} from "mb:tui";

const pane = mb.activePane;
if (!pane) throw new Error("no active pane");

const W = 56, H = 18;
const cx = Math.max(0, Math.floor((pane.cols - W) / 2));
const cy = Math.max(0, Math.floor((pane.rows - H) / 2));

const popup = pane.createPopup({ id: "tui-mouse-demo", x: cx, y: cy, w: W, h: H });
if (!popup) throw new Error("failed to create popup");

const theme = createTheme({
    bg:     '#1a1b26',
    border: { color: '#7aa2f7' },
    text:   {
        color:   'white',
        hoverFg: '#1a1b26',
        hoverBg: '#7aa2f7',
    },
    list: {
        selectedStyle: { bg: '#7aa2f7', fg: '#1a1b26', prefix: '▌', prefixFg: '#f7768e' },
        hoverStyle:    { bg: '#2e3148', fg: '#c0caf5', prefix: '·' },
        itemColor:     '#a9b1d6',
    },
});

const status      = signal("ready — hover or click anything");
const description = signal("(hover [ Yes ] for a description here)");
const selected    = signal(0);
const items       = ["apples", "bananas", "cherries", "dates", "elderberries"];

let clickCount = 0;

const root = box({ border: "round", padding: 1 }, [
    text({ value: "mb:tui mouse + hover demo", align: "center", color: "bright-white.bold" }),
    text({ value: "" }),
    text({ value: status,      align: "center" }),
    text({ value: description, align: "center", color: "gray" }),
    text({ value: "" }),
    list({
        items, selected, height: 5,
        onSelect: (idx) => { status.value = "list onSelect: " + items[idx]; },
    }),
    text({ value: "" }),
    row({ gap: 2 }, [
        button({
            label:        "[ Yes ]", primary: true,
            onClick:      () => { status.value = "Yes clicked (#" + (++clickCount) + ")"; },
            onMouseEnter: () => { description.value = "confirms the action and closes"; },
            onMouseLeave: () => { description.value = "(hover [ Yes ] for a description here)"; },
        }),
        button({
            label:   "[ No ]",
            onClick: () => { status.value = "No clicked (#" + (++clickCount) + ")"; },
        }),
        button({
            label:   "[ Close ]",
            onClick: () => { ui.destroy(); },
        }),
    ]),
]);

const ui = render(popup, root, { theme });

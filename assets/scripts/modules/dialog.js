// mb:dialog — reusable popup dialogs built on mb:tui.
//
// Today: confirm({pane, title, message, buttons, defaultIndex, theme})
//        → Promise<number> with a `.dismiss()` method. Buttons support keyboard
//        shortcuts (`key`) and per-button color overrides. Used by
//        applet-loader for the script permission prompt; future home for
//        prompt(), alert().

import { render, createTheme, box, text, row, button, measure } from "mb:tui";

const _defaultTheme = createTheme({
    bg:     '#1a1b26',
    border: { color: '#7aa2f7' },
    text:   {
        color:      'white',
        selectedFg: '#1a1b26', selectedBg: '#7aa2f7',
        hoverFg:    '#c0caf5', hoverBg:    '#414868',
    },
});

// confirm(opts) — show a modal popup with title/message and N buttons. Returns
// a Promise that resolves to the index of the clicked button, or -1 if the
// dialog was dismissed (Esc, host pane destroyed, onDestroy fired, or the
// caller called .dismiss() on the returned promise).
//
// The returned Promise has a `.dismiss()` method attached. Calling it resolves
// the promise with -1 and tears down the popup. Useful when a newer dialog
// supersedes an older one for the same logical request.
//
// opts:
//   pane           Pane object that owns the popup. Required.
//   title          Optional header line.
//   message        Body text. Multi-line via "\n".
//   buttons        [{label, primary?, key?, color?, selectedFg?, selectedBg?,
//                    hoverFg?, hoverBg?}, ...]. Default: [{label: 'OK'}].
//                    `key` — single-character keyboard shortcut (case-insensitive)
//                            that resolves the dialog with this button's index.
//                    color/selectedFg/selectedBg/hoverFg/hoverBg — per-button
//                            color overrides; see mb:tui button().
//   defaultIndex   Which button is initially focused. Default 0.
//                  For destructive flows this should point at the safe option
//                  (e.g. Cancel) so Enter cancels by default.
//   theme          Optional createTheme override.
export function confirm(opts) {
    let dismiss = () => {};
    const promise = new Promise((resolve) => {
        const {
            pane,
            title = '',
            message = '',
            buttons = [{ label: 'OK' }],
            defaultIndex = 0,
            theme,
        } = opts || {};

        if (!pane) { resolve(-1); return; }

        const lines  = String(message).split('\n');
        const labels = buttons.map(b => '[ ' + (b.label ?? '') + ' ]');

        let resolved = false;
        let ui = null;
        const finish = (idx) => {
            if (resolved) return;
            resolved = true;
            // Resolve before destroy so the caller's await unblocks even if
            // destroy throws synchronously for any reason.
            resolve(idx);
            if (ui) ui.destroy();
        };
        dismiss = () => finish(-1);

        // Build the button widgets first so we can reference them for focus.
        // Pin each button's width to its label length so the row layout doesn't
        // floor() the flex split and clip wider labels (e.g. "[ always ]" gets
        // 9 cells when 10 are needed).
        // `defaultIndex` doubles as: initial focus AND the default
        // action fired by Enter from any non-button-focused widget
        // (today the dialog has no inputs/lists, so this only matters
        // if a caller swaps the message body for something focusable;
        // declaring it costs nothing and future-proofs the API).
        const _diNormalized = (typeof defaultIndex === 'number'
                               && defaultIndex >= 0
                               && defaultIndex < buttons.length) ? defaultIndex : 0;
        const buttonNodes = buttons.map((b, i) => button({
            label:      labels[i],
            primary:    !!b.primary,
            default:    i === _diNormalized,
            color:      b.color,
            width:      labels[i].length,
            selectedFg: b.selectedFg,
            selectedBg: b.selectedBg,
            hoverFg:    b.hoverFg,
            hoverBg:    b.hoverBg,
            onClick:    () => finish(i),
        }));

        const children = [];
        if (title) {
            children.push(text({ value: title, align: 'center', color: 'bright-white.bold' }));
            children.push(text({ value: '' }));
        }
        for (const line of lines) children.push(text({ value: line, align: 'center' }));
        children.push(text({ value: '' }));
        children.push(row({ gap: 2, justify: 'center' }, buttonNodes));

        const root = box({ border: 'round', padding: 1 }, children);

        // Width: enough to fit title / longest line / buttons row inside the
        // box's border + padding (2 cells of border + 2 cells of padding =
        // 4 cells of overhead). The minimum 16 is a stylistic floor so
        // single-word dialogs ("OK"/"Cancel") don't shrink to a tiny strip.
        // Height: derived by measure() once we know the width.
        const titleW = title ? title.length : 0;
        const msgW   = lines.reduce((m, l) => Math.max(m, l.length), 0);
        const btnsW  = labels.reduce((s, l) => s + l.length, 0)
                     + Math.max(0, labels.length - 1) * 2;
        const w      = Math.max(titleW, msgW, btnsW, 16) + 4;
        const m      = measure(root, w);
        const h      = m.h;

        const cx = Math.max(0, Math.floor((pane.cols - w) / 2));
        const cy = Math.max(0, Math.floor((pane.rows - h) / 2));

        const id = 'mb-dialog-' + Math.random().toString(36).slice(2, 10);
        const popup = pane.createPopup({ id, x: cx, y: cy, w, h });
        if (!popup) { resolve(-1); return; }

        // Keyboard shortcuts: each button may declare `key` (single char,
        // case-insensitive). Listen on the popup directly — tui's own input
        // handler ignores letter keys on a focused button so both fire safely.
        const keyMap = {};
        for (let i = 0; i < buttons.length; i++) {
            const k = buttons[i].key;
            if (typeof k === 'string' && k.length === 1) keyMap[k.toLowerCase()] = i;
        }
        if (Object.keys(keyMap).length > 0) {
            popup.addEventListener('input', (data) => {
                if (resolved) return;
                if (typeof data !== 'string' || data.length !== 1) return;
                const idx = keyMap[data.toLowerCase()];
                if (idx !== undefined) finish(idx);
            });
        }

        ui = render(popup, root, {
            theme: theme ?? _defaultTheme,
            // Esc-driven dismiss in tui.destroy() lands here too.
            onDestroy: () => finish(-1),
        });

        // Move initial focus to the requested default button (Cancel
        // typically). The same index is wired as the default-on-Enter
        // button above; both pointers stay in sync through
        // _diNormalized.
        ui.focus(buttonNodes[_diNormalized]);

        // Host pane disappearance (PTY exit, tab close from elsewhere) — bail.
        // The popup is destroyed with its pane; tui.onDestroy will fire and
        // call finish(-1) too. Listening here is belt-and-braces.
        pane.addEventListener('destroyed', () => finish(-1));
    });
    promise.dismiss = () => dismiss();
    return promise;
}

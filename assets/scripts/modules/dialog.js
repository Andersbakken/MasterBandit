// mb:dialog — reusable popup dialogs built on mb:tui.
//
// Today: confirm({pane, title, message, buttons, defaultIndex, theme}) → Promise<number>.
// Future home for prompt(), alert(), permission-prompt port.

import { render, createTheme, box, text, row, button } from "mb:tui";

const _defaultTheme = createTheme({
    bg:     '#1a1b26',
    border: { color: '#7aa2f7' },
    text:   { color: 'white', hoverFg: '#1a1b26', hoverBg: '#7aa2f7' },
});

// confirm(opts) — show a modal popup with title/message and N buttons. Returns
// a Promise that resolves to the index of the clicked button, or -1 if the
// dialog was dismissed (Esc, host pane destroyed, or onDestroy fired).
//
// opts:
//   pane           Pane object that owns the popup. Required.
//   title          Optional header line.
//   message        Body text. Multi-line via "\n".
//   buttons        [{label, primary?}, ...]. Default: [{label: 'OK'}].
//   defaultIndex   Which button is initially focused. Default 0.
//                  For destructive flows this should point at the safe option
//                  (e.g. Cancel) so Enter cancels by default.
//   theme          Optional createTheme override.
export function confirm(opts) {
    return new Promise((resolve) => {
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

        // Inner width: max of (title, longest message line, buttons row).
        // Buttons row = sum of label widths + 2-cell gap between buttons.
        const titleW = title ? title.length : 0;
        const msgW   = lines.reduce((m, l) => Math.max(m, l.length), 0);
        const btnsW  = labels.reduce((s, l) => s + l.length, 0)
                     + Math.max(0, labels.length - 1) * 2;
        const innerW = Math.max(titleW, msgW, btnsW, 16);

        // Inner height = (title + blank) + lines + blank + buttons row.
        const innerH = (title ? 2 : 0) + lines.length + 2;

        // Outer dimensions = inner + border (2) + padding (2 * 1 each side).
        const w = innerW + 4;
        const h = innerH + 4;

        const cx = Math.max(0, Math.floor((pane.cols - w) / 2));
        const cy = Math.max(0, Math.floor((pane.rows - h) / 2));

        const id = 'mb-dialog-' + Math.random().toString(36).slice(2, 10);
        const popup = pane.createPopup({ id, x: cx, y: cy, w, h });
        if (!popup) { resolve(-1); return; }

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

        // Build the button widgets first so we can reference them for focus.
        const buttonNodes = buttons.map((b, i) => button({
            label:   labels[i],
            primary: !!b.primary,
            onClick: () => finish(i),
        }));

        const children = [];
        if (title) {
            children.push(text({ value: title, align: 'center', color: 'bright-white.bold' }));
            children.push(text({ value: '' }));
        }
        for (const line of lines) children.push(text({ value: line, align: 'center' }));
        children.push(text({ value: '' }));
        children.push(row({ gap: 2 }, buttonNodes));

        const root = box({ border: 'round', padding: 1 }, children);

        ui = render(popup, root, {
            theme: theme ?? _defaultTheme,
            // Esc-driven dismiss in tui.destroy() lands here too.
            onDestroy: () => finish(-1),
        });

        // Move initial focus to the requested default button (Cancel typically).
        const di = (typeof defaultIndex === 'number'
                   && defaultIndex >= 0
                   && defaultIndex < buttonNodes.length) ? defaultIndex : 0;
        ui.focus(buttonNodes[di]);

        // Host pane disappearance (PTY exit, tab close from elsewhere) — bail.
        // The popup is destroyed with its pane; tui.onDestroy will fire and
        // call finish(-1) too. Listening here is belt-and-braces.
        pane.addEventListener('destroyed', () => finish(-1));
    });
}

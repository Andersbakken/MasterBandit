// mb:action-form — schema-driven argument-collection popup.
//
// Given a host pane and an MbActionEntry (from mb.actions), open a
// centered popup that prompts for each declared arg, then resolve the
// returned promise with the collected ArgsValue. Resolves with `null`
// if the user cancelled (Esc, dismiss button, popup destroyed).
//
// Arg kinds map to widgets:
//   string / int / uuid / (provider-backed without a static value list)
//                            → input (text field)
//   enum / direction
//   provider-backed with a value list
//                            → list (arrow keys to pick)
//   bool                     → list of two entries
//
// Layout: vertical stack of (label, widget) pairs, then a row with
// Cancel / OK buttons at the bottom. Tab/Shift+Tab cycles focus across
// every focusable; Enter on the last field advances to OK; Enter on
// OK submits; Esc cancels.
//
// Used by command-palette.js when the user picks an action whose
// schema has args. Decoupled into a module so other applets that want
// to invoke actions interactively (e.g. a future "rebind a key" UI)
// share the same form.

import {
    signal, computed, effect, render, createTheme,
    box, text, input, list, row, col, button, measure,
} from "mb:tui";

const _defaultTheme = createTheme({
    bg:     '#1a1b26',
    border: { color: '#7aa2f7' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    list: {
        selectedStyle: { bg: '#263d5a', fg: 'white', prefix: '▌' },
        itemColor: '#8aabcf',
    },
    text:   {
        color:      'white',
        selectedFg: '#1a1b26', selectedBg: '#7aa2f7',
        hoverFg:    '#c0caf5', hoverBg:    '#414868',
    },
});

// Resolve a schema arg's value source. Returns an array of { value, label }
// when finite (static enum, direction, provider with results), or null
// when the field is free-form text (provider absent or returned nothing).
function resolveValues(arg) {
    if (Array.isArray(arg.values) && arg.values.length > 0) {
        return arg.values.map(v =>
            (typeof v === 'string') ? { value: v, label: v } : v);
    }
    if (arg.kind === 'direction') {
        return [
            { value: 'left',  label: 'Left' },
            { value: 'right', label: 'Right' },
            { value: 'up',    label: 'Up' },
            { value: 'down',  label: 'Down' },
            { value: 'next',  label: 'Next' },
            { value: 'prev',  label: 'Previous' },
        ];
    }
    if (arg.kind === 'bool') {
        return [
            { value: 'true',  label: 'true' },
            { value: 'false', label: 'false' },
        ];
    }
    if (arg.provider) {
        try {
            const vs = mb.actionValues(arg.provider);
            if (vs && vs.length > 0) return vs;
        } catch (_) {}
    }
    return null;
}

// Coerce a string-typed input to the schema's declared kind. Returns
// the JS-side value (number for int, bool for bool, string otherwise).
function coerce(arg, raw) {
    if (raw === undefined || raw === null) return undefined;
    switch (arg.kind) {
        case 'int': {
            const n = Number(raw);
            return Number.isFinite(n) ? n : undefined;
        }
        case 'bool':
            return raw === 'true' || raw === true;
        default:
            return raw;
    }
}

// Build the form root + per-field state.
function buildForm(schema, fields, focusIdx) {
    const labelW = Math.max(...schema.args.map(a => (a.label || a.name).length), 6) + 2;

    const children = [
        text({ value: ' Set action arguments ', align: 'center', color: 'bright-white.bold' }),
        text({ value: '' }),
    ];

    for (let i = 0; i < schema.args.length; i++) {
        const a   = schema.args[i];
        const lab = (a.label || a.name) + (a.required ? ' *' : '');
        const f   = fields[i];
        const widget = (f.kind === 'list')
            ? list({
                  items:    f.items.map(v => v.label),
                  selected: f.selected,
                  height:   Math.min(5, f.items.length),
              })
            : input({
                  value:  f.value,
                  prompt: ' > ',
              });
        children.push(row({ gap: 1 }, [
            text({ value: lab, width: labelW, color: '#8aabcf' }),
            widget,
        ]));
        // Spacer between fields except after the last.
        if (i < schema.args.length - 1) {
            children.push(text({ value: '' }));
        }
    }

    children.push(text({ value: '' }));
    children.push(row({ gap: 2, justify: 'center' }, [
        button({ label: '[ Cancel ]', width: 10, onClick: fields._cancel }),
        button({ label: '[   OK   ]', width: 10, primary: true, onClick: fields._ok }),
    ]));

    return box({ border: 'round', padding: 1 }, children);
}

export function runActionForm(pane, action, opts) {
    return new Promise((resolve) => {
        if (!pane || !action || !action.schema || !action.schema.args.length) {
            resolve({});
            return;
        }
        const schema = action.schema;

        // Build per-field state. `kind: 'list'` for finite-value fields,
        // 'input' otherwise. Each field carries its own signal so the
        // form's effects only re-render the affected widget.
        const fields = schema.args.map(a => {
            const values = resolveValues(a);
            if (values) {
                const initialIdx = (() => {
                    if (a.default !== undefined) {
                        const i = values.findIndex(v => v.value === String(a.default));
                        if (i >= 0) return i;
                    }
                    return 0;
                })();
                return {
                    kind: 'list',
                    items: values,
                    selected: signal(initialIdx),
                };
            }
            return {
                kind: 'input',
                value: signal(a.default !== undefined ? String(a.default) : ''),
            };
        });

        let resolved = false;
        let ui = null;
        const finish = (out) => {
            if (resolved) return;
            resolved = true;
            resolve(out);
            if (ui) ui.destroy();
        };

        fields._ok = () => {
            const collected = {};
            for (let i = 0; i < schema.args.length; i++) {
                const a = schema.args[i];
                const f = fields[i];
                let raw;
                if (f.kind === 'list') {
                    raw = f.items[f.selected.value]?.value;
                } else {
                    raw = f.value.value;
                }
                if (raw === undefined || raw === '') {
                    if (a.required && a.default === undefined) {
                        // Missing required field: leave focus where it is
                        // and refuse to submit. The form stays open.
                        return;
                    }
                    continue;
                }
                const v = coerce(a, raw);
                if (v !== undefined) collected[a.name] = v;
            }
            finish(collected);
        };
        fields._cancel = () => finish(null);

        const root = buildForm(schema, fields, 0);

        // Width: longest "label  widget" line, plus border/padding.
        // Widgets have no intrinsic preferred width; allocate a flex
        // amount under the labels (40 cells gives a comfortable input
        // field for typical action args).
        const labelW = Math.max(...schema.args.map(a => (a.label || a.name).length), 6) + 2;
        const w      = Math.max(40, labelW + 30) + 4;
        const m      = measure(root, w);
        const h      = m.h;

        const cx = Math.max(0, Math.floor((pane.cols - w) / 2));
        const cy = Math.max(0, Math.floor((pane.rows - h) / 2));

        const id    = 'mb-action-form-' + Math.random().toString(36).slice(2, 10);
        const popup = pane.createPopup({ id, x: cx, y: cy, w, h });
        if (!popup) {
            resolve(null);
            return;
        }

        // Host-pane teardown propagates as cancel.
        pane.addEventListener('destroyed', () => finish(null));

        ui = render(popup, root, {
            theme: opts?.theme ?? _defaultTheme,
            onDestroy: () => { if (!resolved) finish(null); },
        });
    });
}

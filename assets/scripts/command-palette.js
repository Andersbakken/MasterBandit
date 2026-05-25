// command-palette.js — fuzzy command palette, built with mb:tui.
//
// Lists every available action (mb.actions). For each entry, the
// palette decides what to do with the schema's args:
//
//   - No args              -> dispatch on selection.
//   - All args closed-valued (enum / direction / static value list /
//     provider that returns ≤ FINITE_EXPAND_LIMIT entries)
//                          -> expand inline into one palette entry per
//                             combination, each with baked args. Dispatch
//                             on selection.
//   - Any arg open-valued  -> dispatch by opening mb:action-form to
//                             prompt the user, then invoke with the
//                             collected args.
//
// The expansion logic replaces the old C++ argVariants hack (which
// only handled direction-style actions) with a general scheme driven
// by the schema. New actions are pickable from the palette as soon as
// they're registered with a schema; no C++ changes needed.

import {
    signal, computed, effect, render, createTheme,
    box, text, input, list, measure,
} from "mb:tui";
import { runActionForm } from "mb:action-form";

const theme = createTheme({
    bg:     '#0d1b2a',
    border: { color: '#364d6a' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    list: {
        selectedStyle: { bg: '#263d5a', fg: 'white', prefix: '▌' },
        itemColor: '#8aabcf',
    },
});

mb.setNamespace("palette");
mb.registerAction("open");

// Cap for inline expansion. If an action's schema would produce more
// than this many combinations, fall back to the form prompt — a long
// scroll of "Activate Tab: foo / Activate Tab: bar / Activate Tab:
// baz / ..." (one per tab) past 20 entries is more noise than help.
const FINITE_EXPAND_LIMIT = 20;

function fuzzyScore(str, pattern) {
    if (!pattern) return 0;
    const lower = str.toLowerCase();
    const pat = pattern.toLowerCase();
    let score = 0, pi = 0, consecutive = 0;
    for (let i = 0; i < lower.length && pi < pat.length; i++) {
        if (lower[i] === pat[pi]) {
            pi++;
            consecutive++;
            score += consecutive * 2;
            if (i === 0 || str[i - 1] === ' ') score += 5;
        } else {
            consecutive = 0;
        }
    }
    return pi === pat.length ? score : -1;
}

// Return the static value set for an arg, or null when it has none
// (free-form text). Mirrors action-form.js's resolveValues but with
// the FINITE_EXPAND_LIMIT cap baked in: a provider returning 1000
// entries returns null here so the palette falls back to the form
// rather than dumping 1000 palette entries.
function expandableValues(arg) {
    if (Array.isArray(arg.values) && arg.values.length > 0) {
        return arg.values.length <= FINITE_EXPAND_LIMIT
            ? arg.values.map(v => (typeof v === 'string') ? { value: v, label: v } : v)
            : null;
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
            if (vs && vs.length > 0 && vs.length <= FINITE_EXPAND_LIMIT) {
                return vs;
            }
        } catch (_) {}
    }
    return null;
}

// Given an action's schema, return either:
//   { mode: "expand", entries: [{ label, args }, ...] }  (1+ entries)
//   { mode: "prompt" }                                   (open form)
//   { mode: "direct" }                                   (no args; just dispatch)
//
// Inline expansion only fires when EVERY required arg has an
// expandable value set AND the cross-product size is reasonable.
// Optional free-form args don't prevent expansion (they get omitted
// from the baked entries and the action uses their default).
function planActionInvocation(action) {
    const schema = action.schema || { args: [] };
    if (schema.args.length === 0) {
        return { mode: 'direct' };
    }
    // Required args must all be expandable; optional args with no
    // default are skipped (treated as "not present" in expansion).
    const requiredArgs = schema.args.filter(a => a.required);
    const expansions   = [];
    let total          = 1;
    for (const arg of requiredArgs) {
        const vs = expandableValues(arg);
        if (!vs) {
            return { mode: 'prompt' };
        }
        expansions.push({ arg, values: vs });
        total *= vs.length;
        if (total > FINITE_EXPAND_LIMIT) {
            return { mode: 'prompt' };
        }
    }
    // Cross-product the expansions into one entry per combination.
    let combos = [{ args: {}, parts: [] }];
    for (const { arg, values } of expansions) {
        const next = [];
        for (const c of combos) {
            for (const v of values) {
                const args  = { ...c.args, [arg.name]: v.value };
                const parts = c.parts.concat([v.label]);
                next.push({ args, parts });
            }
        }
        combos = next;
    }
    const entries = combos.map(c => ({
        // "Action Label — Val1, Val2"
        label: c.parts.length > 0
            ? `${action.label} — ${c.parts.join(', ')}`
            : action.label,
        args: c.args,
    }));
    return { mode: 'expand', entries };
}

// Build the flat palette list once per palette-open. Each row of the
// list is one of:
//   { kind: "direct",  action, label }
//   { kind: "prompt",  action, label }
//   { kind: "expand",  action, label, args }
// "kind" tells the dispatch handler what to do on Enter.
function buildPaletteEntries(allActions) {
    const out = [];
    for (const action of allActions) {
        const plan = planActionInvocation(action);
        if (plan.mode === 'direct') {
            out.push({ kind: 'direct', action, label: action.label });
        } else if (plan.mode === 'prompt') {
            // Append "…" so users can tell at a glance the action will
            // ask for more input.
            out.push({ kind: 'prompt', action, label: action.label + ' …' });
        } else {
            for (const e of plan.entries) {
                out.push({ kind: 'expand', action, label: e.label, args: e.args });
            }
        }
    }
    return out;
}

let ui = null;

mb.addEventListener("action", "palette.open", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (ui) {
        ui.destroy(); // toggles closed; onDestroy sets ui = null
        return;
    }

    const allActions = mb.actions;
    const entries    = buildPaletteEntries(allActions);
    const query      = signal("");
    const selected   = signal(0);

    const filtered = computed(() => {
        const q = query.value;
        if (!q) return entries.slice();
        return entries
            .map(e => ({ entry: e, score: fuzzyScore(e.label, q) }))
            .filter(x => x.score >= 0)
            .sort((a, b) => b.score - a.score)
            .map(x => x.entry);
    });

    // Reset selection to top whenever the filtered list changes
    effect(() => {
        filtered.value,
        selected.value = 0;
    });

    function dispatch(entry) {
        if (!entry) return;
        if (entry.kind === 'direct') {
            mb.invokeAction(entry.action.name);
            return;
        }
        if (entry.kind === 'expand') {
            mb.invokeAction(entry.action.name, entry.args);
            return;
        }
        // kind === 'prompt' — open the form for user input. The form
        // runs in its own popup; we wait for it via the returned
        // promise. The palette popup was already destroyed by the
        // outer onSelect path (we tore down ui before calling
        // dispatch), so the form is the only popup on screen.
        runActionForm(pane, entry.action).then(args => {
            if (args === null) return; // cancelled
            mb.invokeAction(entry.action.name, args);
        });
    }

    function buildRoot(listH) {
        return box({ border: "round" }, [
            input({ value: query, prompt: " > " }),
            box({ borderTop: "line" }),
            list({
                items: computed(() => filtered.value.map(e => e.label)),
                selected,
                height: listH,
                onSelect: (idx) => {
                    const entry = filtered.value[idx];
                    ui.destroy();
                    dispatch(entry);
                },
            }),
            text({
                value: computed(() => ` ${filtered.value.length}/${entries.length}`),
                align: "right",
                color: "#5a7b9f",
            }),
        ]);
    }

    function popupRect(cols, rows) {
        const w     = Math.min(80, Math.max(40, Math.floor(cols * 0.6)));
        const listH = Math.min(20, Math.max(5,  Math.floor(rows * 0.5)));
        const m     = measure(buildRoot(listH), w);
        const x     = Math.max(0, Math.floor((cols - w) / 2));
        const y     = Math.max(0, Math.floor((rows - m.h) / 2));
        return { x, y, w: m.w, h: m.h, listH };
    }

    const d     = popupRect(pane.cols, pane.rows);
    const popup = pane.createPopup({ id: "palette", x: d.x, y: d.y, w: d.w, h: d.h });
    if (!popup) return;

    const resizeCb = (cols, rows) => {
        if (!ui) return;
        const nd = popupRect(cols, rows);
        popup.resize({ x: nd.x, y: nd.y, w: nd.w, h: nd.h });
        ui.resize(nd.w, nd.h, buildRoot(nd.listH));
    };
    pane.addEventListener("resized", resizeCb);

    ui = render(popup, buildRoot(d.listH), {
        theme,
        onDestroy: () => {
            pane.removeEventListener("resized", resizeCb);
            ui = null;
        },
    });
});

console.log("command-palette: initialized");

// scrollback-search.js — in-pane scrollback search applet.
//
// Bound to Cmd+F (macOS) / Ctrl+Shift+F (Linux) via the `search.open`
// script action. Opens a small input bar at the top of the focused pane;
// each keystroke runs `pane.findText(needle, opts)` and paints highlight
// decorations for every match, plus a higher-zPriority "current match"
// decoration that the user steps through with `n` / `N` (or Up/Down).
// Esc clears decorations and closes the popup.
//
// Decoration tags:
//   "search"         — every match (yellow background)
//   "current-match"  — the active hit (orange background, higher zPriority)
//
// `pane.findText` and `pane.scrollToRow` require `pane.read`. This script
// runs as a built-in controller (full Perm::All), so the gating is invisible.
//
// Open-while-open re-opens fresh on the active pane (toggle off → on with
// the same keystroke is the natural close-then-search flow).

import { signal, computed, effect, render, createTheme, box, text, input, measure } from "mb:tui";

mb.setNamespace("search");
mb.registerAction("open");

// Decoration colors (packed 0xAABBGGRR — alpha in MSB).
//
//   yellow background, black foreground for bulk matches.
//   orange background, black foreground for the active match.
//
// These are byte-swapped from "natural" RGB: 0xAABBGGRR = (alpha<<24) |
// (B<<16) | (G<<8) | R. So yellow (R=255, G=215, B=0) packs as 0xFF00D7FF;
// orange (R=255, G=140, B=0) packs as 0xFF008CFF.
const HIGHLIGHT_BG       = 0xFF00D7FF; // yellow
const HIGHLIGHT_FG       = 0xFF000000; // black text over yellow
const CURRENT_BG         = 0xFF008CFF; // orange
const CURRENT_FG         = 0xFF000000; // black text over orange
// Hard cap on rendered decorations. Beyond this, results are findable in
// principle but not all painted — keeps the snapshot composition cheap.
const MAX_DECORATIONS    = 1000;

const theme = createTheme({
    bg:     '#0d1b2a',
    border: { color: '#364d6a' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    text:   { color: '#8aabcf' },
});

let ui = null;

// Clear-and-recompute the decoration set. Replaces the bulk "search"
// decorations and the single "current-match" decoration in one pass.
function paintDecorations(pane, matches, currentIdx) {
    pane.clearDecorations("search");
    pane.clearDecorations("current-match");
    if (!matches || matches.length === 0) return;

    const cap = Math.min(matches.length, MAX_DECORATIONS);
    for (let i = 0; i < cap; i++) {
        const m = matches[i];
        // The active match gets the "current-match" tag with higher
        // zPriority so it composites on top of the bulk highlight when
        // they overlap (they always do for the active hit, since both
        // are added). Bulk matches use zPriority 0; current uses 10.
        if (i === currentIdx) continue; // skip; emitted below as "current-match"
        pane.addDecoration({
            startRowId: m.startRowId, startCol: m.startCol,
            endRowId:   m.endRowId,   endCol:   m.endCol,
            style:      { fg: HIGHLIGHT_FG, bg: HIGHLIGHT_BG },
            tag:        "search",
            zPriority:  0,
        });
    }
    if (currentIdx >= 0 && currentIdx < matches.length) {
        const m = matches[currentIdx];
        pane.addDecoration({
            startRowId: m.startRowId, startCol: m.startCol,
            endRowId:   m.endRowId,   endCol:   m.endCol,
            style:      { fg: CURRENT_FG, bg: CURRENT_BG },
            tag:        "current-match",
            zPriority:  10,
        });
    }
}

mb.addEventListener("action", "search.open", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (ui) {
        ui.destroy(); // onDestroy clears decorations and sets ui = null
        return;
    }

    const query    = signal("");
    const matches  = signal([]);
    const current  = signal(0);
    const status   = computed(() => {
        const n = matches.value.length;
        if (!query.value) return "";
        if (n === 0) return " 0 matches";
        return ` ${current.value + 1}/${n}` + (n >= MAX_DECORATIONS ? "+" : "");
    });

    // Per-invocation "alive" flag so effects survive the popup's destruction
    // gracefully: tui has no public hook to dispose user-created effects, so
    // any signal mutation that lands after destroy would otherwise re-fire
    // pane.findText / addDecoration / scrollToRow on the dead popup's
    // sibling pane (harmless but wasteful, and addDecoration with a stale
    // tag would re-paint highlights we just cleared in onDestroy).
    let alive = true;

    // Pick the initial active match: the bottom-most match whose anchor
    // line is already visible (so a user typing "s" doesn't get yanked to
    // the oldest occurrence in scrollback if the live viewport already
    // shows several `s`s). Falls back to the last match overall when none
    // are in the viewport — that's the user searching for something not
    // currently visible, where "jump to the most recent occurrence in
    // scrollback" is the least surprising default.
    //
    // `pane.rowIdAt(N)` returns the lineId at visible-screen row N. The
    // viewport spans rows 0..pane.rows-1; matches' startRowId values are
    // 64-bit logical line ids. Visible matches are the ones whose
    // startRowId lies in [topId..bottomId] inclusive. lineIds are
    // monotonic (assigned at write time, never reused), so a numeric
    // range comparison is correct as long as both endpoints resolve.
    function pickInitialIndex(ms) {
        if (ms.length === 0) return 0;
        const topId    = pane.rowIdAt(0);
        const bottomId = pane.rowIdAt(pane.rows - 1);
        if (topId == null || bottomId == null) {
            // Defensive fallback — viewport row not addressable for some
            // reason; just pick the last match.
            return ms.length - 1;
        }
        // Walk newest-to-oldest, pick the first hit whose lineId lands in
        // [topId..bottomId]. Matches are returned oldest-first, so reverse
        // iteration finds the bottom-most visible hit cheaply.
        for (let i = ms.length - 1; i >= 0; --i) {
            const r = ms[i].startRowId;
            if (r >= topId && r <= bottomId) return i;
        }
        return ms.length - 1; // none visible → most recent in scrollback
    }

    // Re-search whenever the query text changes. Keeping this as an effect
    // (rather than wiring an onInput) means we don't have to know about the
    // input widget's internals.
    effect(() => {
        const q = query.value;
        if (!alive) return;
        if (!q) {
            matches.value = [];
            current.value = 0;
            return;
        }
        const opts = {
            regex: false,
            caseSensitive: false,
            wholeWord: false,
            limit: MAX_DECORATIONS,
        };
        // Detect simple regex-ish patterns: needle starts/ends with `/`.
        // Lightweight heuristic — full regex toggle UI is a follow-up.
        let needle = q;
        if (q.length >= 2 && q.startsWith("/") && q.endsWith("/")) {
            opts.regex = true;
            needle = q.slice(1, -1);
        }
        let ms = [];
        try {
            ms = pane.findText(needle, opts);
        } catch (e) {
            ms = [];
        }
        matches.value = ms;
        current.value = pickInitialIndex(ms);
    });

    // Repaint whenever matches or current change. Running both as the same
    // effect collapses the redraw burst when current advances inside a
    // changing match set.
    effect(() => {
        const ms = matches.value;
        const cur = current.value;
        if (!alive) return;
        paintDecorations(pane, ms, ms.length > 0 ? cur : -1);
        if (ms.length > 0 && cur < ms.length) {
            const m = ms[cur];
            // Bring the current match on screen; scrollToRow is a no-op if
            // already visible.
            pane.scrollToRow(m.startRowId);
        }
    });

    function step(delta) {
        const n = matches.value.length;
        if (n === 0) return;
        current.value = ((current.value + delta) % n + n) % n;
    }

    function buildRoot() {
        return box({ border: "round" }, [
            // Prompt is intentionally not "/" — `/foo/` is the regex-mode
            // syntax, and rendering it as " / /foo/" reads as duplicated.
            input({ value: query, prompt: " > " }),
            text({
                value: status,
                align: "right",
                color: "#8aabcf",
            }),
        ]);
    }

    function popupRect(cols, _rows) {
        const w    = Math.min(60, Math.max(30, Math.floor(cols * 0.5)));
        // measure() reports the height the box needs at width w (top
        // border + input row + status row + bottom border = 4) — no
        // hand-counting of border/padding rows.
        const dims = measure(buildRoot(), w);
        const x    = Math.max(0, cols - w - 1);
        const y    = 0;
        return { x, y, w: dims.w, h: dims.h };
    }

    const d     = popupRect(pane.cols, pane.rows);
    const popup = pane.createPopup({ id: "scrollback-search", x: d.x, y: d.y, w: d.w, h: d.h });
    if (!popup) return;

    // Custom input listener intercepts navigation keys (Up/Down/Enter).
    // tui's built-in input widget swallows printables + backspace into
    // `query`, and forwards other keys (arrows, Enter) to "the first list
    // in the tree" — we don't have a list, so those keys are effectively
    // dropped by tui and our listener is the sole consumer. Esc reaches
    // tui's RenderInstance handler which calls destroy().
    const inputCb = (data) => {
        if (data === "\r" || data === "\n" || data === "\x1b[B") {
            step(+1); // Enter / Down → next match
            return;
        }
        if (data === "\x1b[A") {
            step(-1); // Up → previous match
            return;
        }
    };
    popup.addEventListener("input", inputCb);

    ui = render(popup, buildRoot(), {
        theme,
        // tui's destroy() runs popup.close() before invoking this hook, so
        // by the time we get here the popup is already !alive — calling
        // popup.removeEventListener would throw "popup is destroyed". The
        // listener registration is gone with the popup anyway. Likewise,
        // clear ui and `alive` FIRST so a subsequent search.open invocation
        // can re-open even if a later step throws.
        onDestroy: () => {
            alive = false;
            ui = null;
            try { pane.clearDecorations("search"); } catch (_) {}
            try { pane.clearDecorations("current-match"); } catch (_) {}
        },
    });
});

console.log("scrollback-search: initialized");

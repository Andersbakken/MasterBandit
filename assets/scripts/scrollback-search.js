// scrollback-search.js — in-pane scrollback search applet.
//
// Bound to Cmd+F (macOS) / Ctrl+Shift+F (Linux) via the `search.toggle`
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
mb.registerAction("toggle");

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

const theme = createTheme({
    bg:     '#0d1b2a',
    border: { color: '#364d6a' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    text:   { color: '#8aabcf' },
});

let ui = null;

// Repaint the bulk highlight set ("search" tag). All matches get this
// tag — including the active one. The active match also gets a separate
// "current-match" decoration (higher zPriority) so it composites on top.
// Atomic via createDecorationBatch: a single snapshot publish replaces
// the previous "search" set, so the renderer never samples a partial
// state between clear-and-readd.
function paintBulk(pane, matches) {
    const batch = pane.createDecorationBatch();
    batch.clearDecorations("search");
    if (matches && matches.length > 0) {
        for (let i = 0; i < matches.length; i++) {
            const m = matches[i];
            batch.addDecoration({
                startRowId: m.startRowId, startCol: m.startCol,
                endRowId:   m.endRowId,   endCol:   m.endCol,
                style:      { fg: HIGHLIGHT_FG, bg: HIGHLIGHT_BG },
                tag:        "search",
                zPriority:  0,
            });
        }
    }
    batch.submit();
}

// Repaint just the active match ("current-match" tag). One clear + at
// most one add, also atomic. Stepping with n / N hits this path only —
// the bulk set is untouched, which is the whole point of the split.
//
// After re-adding, fire a quick "pulse" animation: the bg rect expands
// outward by PULSE_PX, then contracts back to the cell rect. Visually
// flags the cursor's new location without touching the bulk yellow set.
// The animation is fire-and-forget; if the current-match is replaced
// mid-pulse (the user keeps stepping), the prior decoration is cleared
// and its in-flight animations auto-cancel, so the new handle starts a
// fresh pulse from 0 inflate.
const PULSE_PX        = 4;
const PULSE_EXPAND_MS = 110;
const PULSE_RETURN_MS = 180;

async function pulseHandle(handle) {
    try {
        // Expand on both axes in parallel; await one — they have the same
        // duration and share a fate (cancellation, completion).
        handle.animate("bgInflateY", { endValue: PULSE_PX, durationMs: PULSE_EXPAND_MS, ease: "easeOut" });
        const result = await handle.animate("bgInflateX", { endValue: PULSE_PX, durationMs: PULSE_EXPAND_MS, ease: "easeOut" }).onEnd();
        if (result !== "completed") return; // decoration was cleared mid-expand
        handle.animate("bgInflateY", { endValue: 0, durationMs: PULSE_RETURN_MS, ease: "easeIn" });
        handle.animate("bgInflateX", { endValue: 0, durationMs: PULSE_RETURN_MS, ease: "easeIn" });
    } catch (_) {}
}

function paintCurrent(pane, matches, currentIdx) {
    const batch = pane.createDecorationBatch();
    batch.clearDecorations("current-match");
    if (matches && currentIdx >= 0 && currentIdx < matches.length) {
        const m = matches[currentIdx];
        batch.addDecoration({
            startRowId: m.startRowId, startCol: m.startCol,
            endRowId:   m.endRowId,   endCol:   m.endCol,
            style:      { fg: CURRENT_FG, bg: CURRENT_BG },
            tag:        "current-match",
            zPriority:  10,
        });
    }
    const handles = batch.submit();
    if (handles.length > 0) {
        pulseHandle(handles[0]);
    }
}

mb.addEventListener("action", "search.toggle", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (ui) {
        ui.destroy(); // onDestroy clears decorations and sets ui = null
        return;
    }

    // Find searches the Document (main-screen scrollback + visible grid);
    // alt-screen contents live in a separate grid that findText cannot see.
    // Running on alt screen would highlight whatever main-screen text
    // happens to lie underneath the vim/less/htop UI, which is wrong.
    // Alt-screen apps typically have their own in-app search anyway.
    if (pane.usingAltScreen) return;

    const query    = signal("");
    const matches  = signal([]);
    const current  = signal(0);
    const status   = computed(() => {
        const n = matches.value.length;
        if (!query.value) return "";
        if (n === 0) return " 0 matches";
        return ` ${current.value + 1}/${n}`;
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
            limit: 0, // 0 = no cap; bulk paint is capped separately in paintBulk.
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

    // Bulk repaint — depends on `matches` only. A pure cursor step (n / N)
    // doesn't change the match set, so this effect doesn't re-fire and the
    // 600 yellow highlights stay in place.
    effect(() => {
        const ms = matches.value;
        if (!alive) return;
        paintBulk(pane, ms);
    });

    // Current-match repaint + scroll — depends on `matches` and `current`.
    // On a cursor step this is the only effect that runs: one clear + one
    // add for the orange decoration, one snapshot publish.
    effect(() => {
        const ms  = matches.value;
        const cur = current.value;
        if (!alive) return;
        paintCurrent(pane, ms, ms.length > 0 ? cur : -1);
        if (ms.length > 0 && cur < ms.length) {
            // Bring the current match on screen; scrollToRow is a no-op if
            // already visible.
            pane.scrollToRow(ms[cur].startRowId);
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
            input({
                value:    query,
                prompt:   " > ",
                // Enter advances to the next match. Routed through
                // tui's input.onSubmit hook rather than a popup-level
                // input listener so we don't ride alongside tui's own
                // focused-button Enter handling — important now that
                // dialogs can declare default buttons (the search bar
                // doesn't have one, but adding one later wouldn't
                // double-fire this).
                onSubmit: () => step(+1),
            }),
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

    // Popup-level listener for arrow Up/Down only. Enter is now
    // routed through input.onSubmit (see the input() call above);
    // arrows still go here because tui's input branch forwards
    // arrows to "the first list in the tree" — we have no list, so
    // tui's path is a no-op and this listener is the sole consumer.
    // Esc reaches tui's RenderInstance handler which calls destroy().
    const inputCb = (data) => {
        if (data === "\x1b[B") {
            step(+1); // Down → next match
            return;
        }
        if (data === "\x1b[A") {
            step(-1); // Up → previous match
            return;
        }
    };
    popup.addEventListener("input", inputCb);

    // If the pane switches to alt screen while search is open (user pops
    // into vim/less without closing the bar first), tear down: findText is
    // searching the wrong buffer at that point and any highlights painted
    // against the main-screen Document are no longer what the user sees.
    const altCb = (usingAltScreen) => {
        if (usingAltScreen && ui) {
            ui.destroy();
        }
    };
    pane.addEventListener("altScreenChanged", altCb);

    ui = render(popup, buildRoot(), {
        theme,
        // tui's destroy() runs popup.close() before invoking this hook, so
        // by the time we get here the popup is already !alive — calling
        // popup.removeEventListener would throw "popup is destroyed". The
        // listener registration is gone with the popup anyway. Likewise,
        // clear ui and `alive` FIRST so a subsequent search.toggle invocation
        // can re-open even if a later step throws.
        onDestroy: () => {
            alive = false;
            ui = null;
            try { pane.removeEventListener("altScreenChanged", altCb); } catch (_) {}
            try { pane.clearDecorations("search"); } catch (_) {}
            try { pane.clearDecorations("current-match"); } catch (_) {}
        },
    });
});

console.log("scrollback-search: initialized");

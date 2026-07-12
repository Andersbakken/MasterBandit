// scrollback-search.js — in-pane scrollback search applet.
//
// Bound to Cmd+F (macOS) / Ctrl+Shift+F (Linux) via the `search.toggle`
// script action. Opens a small input bar at the top of the focused pane.
// Typing is debounced; the search then runs as a chain of bounded
// `pane.findText` slices (newest → oldest, MAX_LINES_PER_CHUNK logical
// lines each) driven through setTimeout(0), so a huge scrollback never
// blocks a keystroke — each slice paints its matches incrementally and
// yields back to the event loop. Total highlights are capped at
// MAX_MATCHES; the cap truncates the OLDEST matches because the walk is
// newest-first. `n` / `N` (or Up/Down) step through matches; Esc clears
// decorations and closes the popup.
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

// Total highlight cap. Painting 10k decorations is cheap (per-frame cost
// is bounded by visible rows, not decoration count); the cap exists to
// bound match-array and decoration memory on pathological needles.
const MAX_MATCHES = 10000;
// Logical lines per findText slice. Keeps each synchronous call to a few
// milliseconds even on very wide lines.
const MAX_LINES_PER_CHUNK = 20000;
// Keystroke debounce before a new search starts.
const DEBOUNCE_MS = 350;

// Decoration colors (packed 0xAABBGGRR — alpha in MSB).
//
//   yellow background, black foreground for bulk matches.
//   orange background, black foreground for the active match.
//
// These are byte-swapped from "natural" RGB: 0xAABBGGRR = (alpha<<24) |
// (B<<16) | (G<<8) | R. So yellow (R=255, G=215, B=0) packs as 0xFF00D7FF;
// orange (R=255, G=140, B=0) packs as 0xFF008CFF.
const HIGHLIGHT_BG = 0xFF00D7FF; // yellow
const HIGHLIGHT_FG = 0xFF000000; // black text over yellow
const CURRENT_BG   = 0xFF008CFF; // orange
const CURRENT_FG   = 0xFF000000; // black text over orange

const theme = createTheme({
    bg:     '#0d1b2a',
    border: { color: '#364d6a' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    text:   { color: '#8aabcf' },
});

let ui = null;

// Append highlight decorations for one slice's matches. Adds only — the
// bulk "search" set is cleared once when a new search starts, then grows
// as slices stream in. One atomic batch per slice.
function paintChunk(pane, matches) {
    if (!matches || matches.length === 0) return;
    const batch = pane.createDecorationBatch();
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
    batch.submit();
}

// Repaint just the active match ("current-match" tag). One clear + at
// most one add, atomic. Stepping with n / N hits this path only — the
// bulk set is untouched.
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

    const query     = signal("");
    // Matches accumulate newest-line-first (the findText walk order), so
    // slice appends never shift existing indices and the "current" index
    // stays valid while a search streams in.
    const matches   = signal([]);
    const current   = signal(0);
    const searching = signal(false);
    const capped    = signal(false);
    const status    = computed(() => {
        const n = matches.value.length;
        if (!query.value) return "";
        if (n === 0) return searching.value ? " …" : " 0 matches";
        // Number top-down: the oldest match is 1, the newest is n —
        // stepping "down" through newer matches counts up.
        const pos = n - current.value;
        return ` ${pos}/${n}${capped.value ? "+" : ""}${searching.value ? "…" : ""}`;
    });

    // Per-invocation "alive" flag so effects survive the popup's destruction
    // gracefully: tui has no public hook to dispose user-created effects, so
    // any signal mutation that lands after destroy would otherwise re-fire
    // pane.findText / addDecoration / scrollToRow on the dead popup's
    // sibling pane (harmless but wasteful, and addDecoration with a stale
    // tag would re-paint highlights we just cleared in onDestroy).
    let alive = true;

    // Search-generation token: bumped on every query change and on
    // destroy. In-flight debounce timers and slice callbacks check it and
    // drop out when stale, so a superseded search cancels itself.
    let generation    = 0;
    let debounceTimer = 0;
    let chunkTimer    = 0;

    // Whether `current` points at a match that was visible in the viewport
    // when picked. Until one is found, later (older) slices may upgrade
    // the pick: the user's viewport can be scrolled deep into scrollback,
    // where the visible matches only arrive several slices in.
    let pickedVisible = false;

    // Prefer the bottom-most match already visible in the viewport (so a
    // user typing "s" doesn't get yanked away if the viewport already
    // shows several `s`s). Matches are newest-first, so the first match
    // whose line falls inside [topId..bottomId] is the bottom-most visible
    // one. Fallback while none are visible: the newest match overall
    // (index 0) — the most recent occurrence, least surprising jump.
    function maybePick(startIdx) {
        if (pickedVisible) return;
        const ms = matches.value;
        if (ms.length === 0) return;
        const topId    = pane.rowIdAt(0);
        const bottomId = pane.rowIdAt(pane.rows - 1);
        if (topId != null && bottomId != null) {
            for (let i = startIdx; i < ms.length; i++) {
                const r = ms[i].startRowId;
                if (r >= topId && r <= bottomId) {
                    current.value = i;
                    pickedVisible = true;
                    return;
                }
            }
        }
        if (startIdx === 0) {
            current.value = 0; // fallback until something visible shows up
        }
    }

    function stopSearch() {
        ++generation;
        if (debounceTimer) { clearTimeout(debounceTimer); debounceTimer = 0; }
        if (chunkTimer) { clearTimeout(chunkTimer); chunkTimer = 0; }
        searching.value = false;
    }

    // One findText slice; reschedules itself via setTimeout(0) until the
    // walk completes, the match cap fills, or the generation goes stale.
    function runChunk(gen, needle, opts, fromRowId) {
        chunkTimer = 0;
        if (!alive || gen !== generation) return;
        let res;
        try {
            opts.fromRowId = fromRowId; // undefined on the first slice
            opts.limit     = MAX_MATCHES - matches.value.length;
            res = pane.findText(needle, opts);
        } catch (e) {
            searching.value = false;
            return;
        }
        if (res.matches.length > 0) {
            paintChunk(pane, res.matches);
            const prevLen = matches.value.length;
            matches.value = matches.value.concat(res.matches);
            maybePick(prevLen);
        }
        if (res.resumeRowId != null && matches.value.length < MAX_MATCHES) {
            chunkTimer = setTimeout(() => runChunk(gen, needle, opts, res.resumeRowId), 0);
        } else {
            capped.value    = res.resumeRowId != null;
            searching.value = false;
        }
    }

    function startSearch(q) {
        stopSearch();
        const gen = generation;
        // Clear previous highlights in one publish; the new set streams in
        // per-slice on top of a clean slate.
        const batch = pane.createDecorationBatch();
        batch.clearDecorations("search");
        batch.submit();
        matches.value = [];
        current.value = 0;
        capped.value  = false;
        pickedVisible = false;
        if (!q) return;

        const opts = { regex: false, caseSensitive: false, wholeWord: false,
                       maxLines: MAX_LINES_PER_CHUNK };
        // Detect simple regex-ish patterns: needle starts/ends with `/`.
        // Lightweight heuristic — full regex toggle UI is a follow-up.
        let needle = q;
        if (q.length >= 2 && q.startsWith("/") && q.endsWith("/")) {
            opts.regex = true;
            needle = q.slice(1, -1);
        }
        searching.value = true;
        runChunk(gen, needle, opts, undefined);
    }

    // Kick a debounced search whenever the query text changes. Keeping
    // this as an effect (rather than wiring an onInput) means we don't
    // have to know about the input widget's internals.
    effect(() => {
        const q = query.value;
        if (!alive) return;
        stopSearch();
        if (!q) {
            startSearch(""); // immediate clear, no debounce
            return;
        }
        const gen     = generation;
        debounceTimer = setTimeout(() => {
            debounceTimer = 0;
            if (!alive || gen !== generation) return;
            startSearch(q);
        }, DEBOUNCE_MS);
    });

    // Current-match repaint + scroll — depends on `matches` and `current`.
    // On a cursor step this is the only effect that runs: one clear + one
    // add for the orange decoration, one snapshot publish. It also re-fires
    // on slice appends, but the current match is unchanged then and
    // scrollToRow is a no-op for a visible row.
    effect(() => {
        const ms  = matches.value;
        const cur = current.value;
        if (!alive) return;
        paintCurrent(pane, ms, ms.length > 0 ? cur : -1);
        if (ms.length > 0 && cur < ms.length) {
            pane.scrollToRow(ms[cur].startRowId);
        }
    });

    // Matches are newest-first: +1 walks OLDER (up the screen), -1 walks
    // NEWER (down). Wraps around.
    function step(delta) {
        const n = matches.value.length;
        if (n === 0) return;
        current.value = ((current.value + delta) % n + n) % n;
        pickedVisible = true; // manual navigation owns the cursor now
    }

    function buildRoot() {
        return box({ border: "round" }, [
            // Prompt is intentionally not "/" — `/foo/` is the regex-mode
            // syntax, and rendering it as " / /foo/" reads as duplicated.
            input({
                value:    query,
                prompt:   " > ",
                // Enter advances downward (toward newer matches). Routed
                // through tui's input.onSubmit hook rather than a
                // popup-level input listener so we don't ride alongside
                // tui's own focused-button Enter handling — important now
                // that dialogs can declare default buttons (the search bar
                // doesn't have one, but adding one later wouldn't
                // double-fire this).
                onSubmit: () => step(-1),
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

    // Popup-level listener for arrow Up/Down only. Enter is routed
    // through input.onSubmit (see the input() call above); arrows still
    // go here because tui's input branch forwards arrows to "the first
    // list in the tree" — we have no list, so tui's path is a no-op and
    // this listener is the sole consumer. Esc reaches tui's
    // RenderInstance handler which calls destroy().
    const inputCb = (data) => {
        if (data === "\x1b[B") {
            step(-1); // Down → newer match
            return;
        }
        if (data === "\x1b[A") {
            step(+1); // Up → older match
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
            stopSearch();
            try { pane.removeEventListener("altScreenChanged", altCb); } catch (_) {}
            try { pane.clearDecorations("search"); } catch (_) {}
            try { pane.clearDecorations("current-match"); } catch (_) {}
        },
    });
});

console.log("scrollback-search: initialized");

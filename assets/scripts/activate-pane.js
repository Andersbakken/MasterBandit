// activate-pane.js — find a pane by its (effective) title and activate it.
//
// Bound via the `pane.activate_by_name` script action. The action takes a
// single string argument:
//
//   action = "pane.activate_by_name"
//   args   = ["<name>"]
//
// `<name>` is matched against each pane's effective title (the same
// string returned by `pane.title` — override if set, else OSC 0/2,
// else ""). Two modes:
//
//   - "/regex/flags"  — JS regex; surrounding slashes + optional flags.
//                       Empty flags default to "i". Example: "/^claude$/"
//                       or "/api.*test/i" (i is implicit if you omit it).
//   - "name"          — exact, case-insensitive equality.
//
// Walks tabs/panes in tree order; the first hit wins. Activates every
// Stack ancestor along the way so panes living inside sub-bars get their
// containing top-level tab activated too. Finally calls focusPane on
// the matched terminal so the focusedPaneChanged event fires.
//
// Invocation paths:
//   - TOML keybinding:    keys = ["ctrl+shift+1"], action = "...", args = ["claude"]
//   - From JS:            mb.invokeAction("pane.activate_by_name", "claude")
//   - From C++ Bindings:  Action::ScriptAction { "pane.activate_by_name", { "claude" } }

mb.setNamespace("pane");
mb.registerAction("activate_by_name", {
    args: [
        {
            // `name` accepts either an exact (case-insensitive) match
            // against a pane's effective title, or "/regex/flags" for
            // regex matching. The "panes" value provider feeds the
            // command palette with current pane titles so the user can
            // pick from a list without typing; bypass the list by
            // submitting a "/.../" regex via the action form's text
            // entry.
            name:     "name",
            label:    "Pane name",
            kind:     "string",
            required: true,
            provider: "panes",
        },
    ],
});

// Parse "/pattern/flags" into a RegExp, or null if the input isn't in
// regex form. Flags default to "i" (case-insensitive) — matches the
// "exact, case-insensitive" semantics of the literal branch.
function parseRegexArg(s) {
    if (typeof s !== "string" || s.length < 2) return null;
    if (!s.startsWith("/")) return null;
    // Find the last "/" — flags live after it. Reject if there's no
    // closing slash or the trailing segment isn't all valid flag chars.
    const lastSlash = s.lastIndexOf("/");
    if (lastSlash === 0) return null;
    const body  = s.slice(1, lastSlash);
    const flags = s.slice(lastSlash + 1);
    if (!/^[gimsuy]*$/.test(flags)) return null;
    try {
        return new RegExp(body, flags || "i");
    } catch (_) {
        return null;
    }
}

function buildMatcher(arg) {
    const re = parseRegexArg(arg);
    if (re) {
        return (title) => re.test(title);
    }
    const needle = String(arg).toLowerCase();
    return (title) => title.toLowerCase() === needle;
}

// Clear zoomTarget on any ancestor Stack of `targetId` whose current
// zoomTarget would hide `targetId`. Mirrors default-ui's helper of the
// same name — kept inline because script files don't share scope.
function unzoomToReveal(targetId) {
    if (!targetId) return;
    for (let cur = targetId; cur; ) {
        const n = mb.layout.node(cur);
        if (!n) break;
        if (n.kind === "stack" && n.zoomTarget) {
            if (!mb.layout.contains(n.zoomTarget, targetId)) {
                mb.layout.setStackZoom(cur, null);
            }
        }
        cur = n.parent;
    }
}

// Walk the parent chain from `nodeId` up to (and excluding) the layout
// root. For every Stack encountered, set its activeChild to the
// previously-seen child so the pane becomes visible. activateTab fires
// the proper tab-switched events for top-level Stack children; for
// nested Stacks (sub-bars) setActiveChild is what makes them visible.
function activateAncestors(nodeId) {
    let prev = nodeId;
    let cur  = mb.layout.node(nodeId)?.parent;
    while (cur) {
        const node = mb.layout.node(cur);
        if (!node) break;
        if (node.kind === "stack" && node.activeChild !== prev) {
            // activateTab handles tab-bar updates + window title.
            // It accepts any direct Stack child (top-level or sub-bar).
            mb.layout.activateTab(prev);
        }
        prev = cur;
        cur  = node.parent;
    }
}

mb.addEventListener("action", "pane.activate_by_name", ({ name }) => {
    if (typeof name !== "string" || name.length === 0) {
        console.log("pane.activate_by_name: missing or empty name arg");
        return;
    }
    const match = buildMatcher(name);

    // queryNodes("terminal") returns every Terminal leaf in tree-walk
    // order — top-to-bottom, left-to-right. First match wins.
    const termIds = mb.layout.queryNodes("terminal");
    for (const tid of termIds) {
        const pane = mb.pane(tid);
        if (!pane) continue;
        const title = pane.title || "";
        if (!match(title)) continue;

        activateAncestors(tid);
        unzoomToReveal(tid);
        mb.layout.focusPane(tid);
        return;
    }
});

console.log("activate-pane: initialized");

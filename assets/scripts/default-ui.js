// Default UI controller. Owns the JS-policy actions (tab/pane lifecycle,
// structural mutations) AND the startup tree-shape construction. Mandatory
// — mb refuses to start without it.

import { confirm } from "mb:dialog";

// JS-only state: the set of foreground process names that count as "the
// shell at a prompt" — i.e. not busy. config.js can mutate via the
// default-ui.add-shell / default-ui.remove-shell actions below.
const SHELLS = new Set(["zsh", "bash", "fish", "dash", "sh", "ksh", "mksh", "ash"]);

function _isShell(name)  { return !!name && SHELLS.has(name); }
function _isPaneBusy(p)  { return !!(p && p.foregroundProcess && !_isShell(p.foregroundProcess)); }
function _confirmMode()  {
    const m = mb.config?.confirm_close;
    return (m === "never" || m === "always") ? m : "if_busy";
}

// Collect non-shell foreground process names across a set of pane nodeIds.
// Order preserved; duplicates kept (they show up as "vim, vim, ssh" in the
// aggregated dialog message — informative without dedup gymnastics).
function _busyProcessesIn(termNodeIds) {
    const out = [];
    for (const id of termNodeIds) {
        const p = mb.pane(id);
        if (p && p.foregroundProcess && !_isShell(p.foregroundProcess))
            out.push(p.foregroundProcess);
    }
    return out;
}

// Per-pane focus history stack — top = most recently focused popup. Updated
// from `focusedPopupChanged`. When a popup is destroyed (visible as a
// transition to "" with the prior popup gone from `pane.popups`), default-ui
// pops the stack and refocuses the next surviving entry. This keeps modal
// dialog dismissal feeling natural (close confirm → palette regains focus)
// without any per-popup glue from the script that opened the popup.
//
// Registered as the FIRST thing default-ui does so the IIFE's
// `createTerminal` below triggers `paneCreated` after the listener exists.
const _focusStacks = new Map(); // PaneId(string) → string[] (popup ids)
function _stackFor(paneId) {
    let s = _focusStacks.get(paneId);
    if (!s) { s = []; _focusStacks.set(paneId, s); }
    return s;
}
mb.addEventListener('paneCreated', (pane) => {
    const stack = _stackFor(pane.id);
    pane.addEventListener('focusedPopupChanged', (newId) => {
        if (newId) {
            const i = stack.indexOf(newId);
            if (i >= 0) stack.splice(i, 1);
            stack.push(newId);
            return;
        }
        // Focus cleared. Drop any dead entries from the top, then refocus
        // the most-recent surviving popup if there is one.
        const liveIds = new Set(pane.popups.map(p => p.id));
        while (stack.length > 0 && !liveIds.has(stack[stack.length - 1])) {
            stack.pop();
        }
        if (stack.length > 0) {
            const next = pane.popups.find(p => p.id === stack[stack.length - 1]);
            if (next) next.focus();   // built-ins have ui.focus
        }
    });
    pane.addEventListener('destroyed', () => {
        _focusStacks.delete(pane.id);
    });
});
//
// At load time, the tree is empty except for Engine::layoutRootStack_, the
// Stack that holds each tab as a direct child. We build:
//
//   Container (newRoot, vertical, tree root)
//   ├── TabBar (fixedCells=1)
//   └── Stack (layoutRootStack, stretch=1)       // the tabs Stack
//       └── Stack (tab 1 subtreeRoot, activeChild)
//           └── Container (content, activeChild)
//               └── Terminal (first pane, spawned by mb.layout.createTerminal)
//
// mb.layout.createTerminal spawns the native PTY + initial resize; we focus
// the resulting Terminal so keyboard input lands there immediately.

// The root Container is built once with the tab bar on either the "top" or
// "bottom" side depending on [tab_bar].position. These Uuids are captured
// so the configChanged listener below can swap the children's order on
// hot-reload without rebuilding the tree.
let _rootContainer = null;
let _tabBarNode    = null;
let _tabsStackNode = null;

// stackUuid → previously-active child Uuid. Updated on every
// _activateTabAndFocus so activate_last_tab can flip back.
const _lastActiveByStack = new Map();

(() => {
    const tabsStack = mb.layout.getRoot();
    if (!tabsStack) {
        console.error('default-ui: no root Stack at load time — aborting tree construction');
        return;
    }
    // Wrap the tabs Stack in a vertical root Container with a TabBar sibling.
    const newRoot = mb.layout.createContainer('vertical');
    const tabBar  = mb.layout.createTabBar();
    mb.layout.setRoot(newRoot);

    const position = mb.config?.tab_bar?.position || 'bottom';
    if (position === 'top') {
        mb.layout.appendChild(newRoot, tabBar, {fixedCells: 1});
        mb.layout.appendChild(newRoot, tabsStack, {stretch: 1});
    } else {
        mb.layout.appendChild(newRoot, tabsStack, {stretch: 1});
        mb.layout.appendChild(newRoot, tabBar, {fixedCells: 1});
    }
    mb.layout.setTabBarStack(tabBar, tabsStack);

    _rootContainer = newRoot;
    _tabBarNode    = tabBar;
    _tabsStackNode = tabsStack;

    // Build the first tab: Stack → content Container. appendChild
    // auto-sets the Stack's activeChild to its first child, so both
    // `tabsStack.activeChild = firstTab` and
    // `firstTab.activeChild = content` fall out naturally.
    const firstTab = mb.layout.createStack();
    const content  = mb.layout.createContainer('horizontal');
    mb.layout.appendChild(firstTab, content, {stretch: 1});
    mb.layout.appendChild(tabsStack, firstTab, {stretch: 1});

    // Spawn the first Terminal. Native handles PTY + initial resize; the
    // returned nodeId is the Terminal's tree Uuid. Focus it so keyboard
    // input lands there on the first frame.
    const termNodeId = mb.layout.createTerminal(content);
    if (termNodeId) mb.layout.focusPane(termNodeId);
})();

// Hot-reload support: if the user toggles [tab_bar].position between "top"
// and "bottom" at runtime, swap the root Container's children so the tab
// bar moves side without a restart. Use moveChild (in-place rotate) rather
// than removeChild + appendChild because appendChild constructs a fresh
// ChildSlot from the JS defaults ({fixedCells: 1, stretch: 1}), which
// overwrites whatever initTabBar's setBarSlot last wrote — in particular
// the {fixedCells: 0, stretch: 0} "hidden" state used by style="auto" with
// a single tab. moveChild is a structural reorder that leaves every
// ChildSlot field untouched, so auto-hide survives a position swap.
mb.addEventListener('configChanged', () => {
    if (!_rootContainer || !_tabBarNode || !_tabsStackNode) return;
    const want = mb.config?.tab_bar?.position || 'bottom';

    const rootNode = mb.layout.node(_rootContainer);
    if (!rootNode || !rootNode.children) return;
    const ids = rootNode.children.map(c => c.id);
    const barIdx  = ids.indexOf(_tabBarNode);
    const stackIdx = ids.indexOf(_tabsStackNode);
    if (barIdx < 0 || stackIdx < 0) return;
    const currentlyTop = barIdx < stackIdx;
    if ((want === 'top') === currentlyTop) return;

    mb.layout.moveChild(_rootContainer, _tabBarNode, want === 'top' ? -1 : +1);
});

// Resolve `idx` to a tab UUID by indexing into the chrome TabBar's bound
// Stack. Used by keybinding-driven actions whose payload is a positional
// integer (`meta+1..9`, mouse clicks on the tab bar). Returns null on
// out-of-range or if the bar has no boundStack.
function _tabUuidByIndex(idx) {
    if (!_tabBarNode) return null;
    const bar = mb.layout.node(_tabBarNode);
    if (!bar || !bar.boundStack) return null;
    const stack = mb.layout.node(bar.boundStack);
    if (!stack || !stack.children) return null;
    if (idx < 0 || idx >= stack.children.length) return null;
    return stack.children[idx].id;
}

// Switch the enclosing Stack's activeChild to `tabUuid` and route keyboard
// focus to a live pane inside it (preferring the remembered focus, else
// the first visible leaf). C++ activateTabByUuid only handles the Stack
// switch + GPU teardown; the focusPane call is what fires
// notifyPaneFocusChange (CSI ?1004, paneFocusChanged listeners, title).
function _activateTabAndFocus(tabUuid) {
    if (!tabUuid) return;
    const node = mb.layout.node(tabUuid);
    if (node && node.parent) {
        const parent = mb.layout.node(node.parent);
        if (parent && parent.activeChild && parent.activeChild !== tabUuid) {
            _lastActiveByStack.set(node.parent, parent.activeChild);
        }
    }
    mb.layout.activateTab(tabUuid);
    let focusTarget = mb.layout.rememberedFocusInSubtree(tabUuid);
    if (!focusTarget) {
        const leaves = mb.layout.terminalLeavesIn(tabUuid, true);
        if (leaves.length > 0) focusTarget = leaves[0];
    }
    if (focusTarget) mb.layout.focusPane(focusTarget);
}

// Currently active tab UUID (the chrome TabBar's bound Stack's activeChild).
function _activeTabUuid() {
    if (!_tabBarNode) return null;
    const bar = mb.layout.node(_tabBarNode);
    if (!bar || !bar.boundStack) return null;
    const stack = mb.layout.node(bar.boundStack);
    return stack ? stack.activeChild : null;
}

// Read the focused pane's effective CWD (OSC 7 if known; falls back to
// /proc/<pgid>/cwd or proc_pidpath on the C++ side). Empty string means
// "no inheritance" — C++ then uses TerminalOptions.cwd → $HOME.
function _focusedPaneCwd() {
    const fp = mb.layout.focusedPane();
    if (!fp) return '';
    const pane = mb.pane(fp.nodeId);
    return pane ? (pane.cwd || '') : '';
}

mb.actions.register('newTab', () => {
    const cwd = _focusedPaneCwd();
    const tabUuid = mb.layout.createTab();
    if (!tabUuid) return;
    const opts = cwd ? { cwd } : undefined;
    mb.layout.createTerminal(tabUuid, opts);
    _activateTabAndFocus(tabUuid);
});

mb.actions.register('closeTab', async ({target, index}) => {
    let targetId = null;
    if (target) {
        targetId = target;
    } else if (typeof index === 'number' && index >= 0) {
        targetId = _tabUuidByIndex(index);
    } else {
        targetId = _activeTabUuid();
    }
    if (!targetId) return;

    // Determine if target is a top-level tab (direct child of _tabsStackNode)
    // purely for dialog wording. C++ closeTab handles both uniformly.
    const node = mb.layout.node(targetId);
    if (!node || !node.parent) return;
    const isTopLevel = (node.parent === _tabsStackNode);

    const mode = _confirmMode();
    if (mode !== "never") {
        const termIds = mb.layout.queryNodes('terminal', targetId);
        const busy    = _busyProcessesIn(termIds);
        const need    = mode === "always" || busy.length > 0;
        if (need) {
            const fp    = mb.layout.focusedPane();
            const pane  = fp ? mb.pane(fp.nodeId) : mb.activePane;
            const label = isTopLevel ? "tab" : "sub-tab";
            const msg  = busy.length
                ? `${busy.length} process${busy.length === 1 ? '' : 'es'} running: ${busy.slice(0, 5).join(', ')}${busy.length > 5 ? '…' : ''}.\nClose ${label} anyway?`
                : `Close ${label}?`;
            const choice = await confirm({
                pane,
                title: `Close ${label}?`,
                message: msg,
                buttons: [{ label: "Cancel" }, { label: "Close", primary: true }],
                defaultIndex: 0,
            });
            if (choice !== 1) return;
        }
    }
    // Kill live terminals first; C++ closeTab refuses to destroy a subtree
    // containing live Terminals (panes need PTY teardown before the tree
    // node goes away). After kill, the synchronous closeTab call removes
    // the now-empty subtree and activates a surviving sibling in the same
    // Stack.
    for (const termId of mb.layout.queryNodes('terminal', targetId)) {
        mb.layout.killTerminal(termId);
    }
    mb.layout.closeTab(targetId);
});

mb.actions.register('activateTab', ({target, index}) => {
    let targetId = target;
    if (!targetId && typeof index === 'number' && index >= 0) {
        targetId = _tabUuidByIndex(index);
    }
    _activateTabAndFocus(targetId);
});

mb.actions.register('activateTabRelative', ({stack: stackArg, delta}) => {
    // Cycle the activeChild of `stackArg` (when provided by the dispatcher).
    // Otherwise walk up from the focused pane to the nearest enclosing
    // *tabs-list* Stack — the Stack whose children are tab Stacks. The tree
    // shape for a normal tab is:
    //
    //   tabsStack (Stack)            ← what we want to cycle
    //     tab      (Stack)           ← Stack-child; the tab itself
    //       content (Container)
    //         terminal
    //
    // `nearestAncestorOfKind(fp, "Stack")` returns the *inner* `tab` Stack,
    // not `tabsStack`, so cycling its activeChild is a no-op (its only
    // child is `content`, never a sibling). The correct lookup walks up
    // looking for the first Stack-child whose PARENT is a Stack — that
    // parent is the tabs-list (top-level tabs, or a sub-tab Stack inside
    // a sub-bar). Falls back to the primary tabs Stack if no pane is
    // focused.
    // The tree shape for a normal top-level tab is:
    //
    //   tabsStack (Stack)            ← _tabsStackNode; we want to cycle this
    //     tab      (Stack)           ← Stack-child of tabsStack; the tab
    //       content (Container)
    //         terminal
    //
    // Sub-tab bars add another layer:
    //
    //   tabsStack (Stack)
    //     tab (Stack)
    //       wrapper (Container)
    //         subTabBar (TabBar)
    //         subTabsStack (Stack)    ← cycle this if focus is inside it
    //           subTab (Stack)
    //             content (Container)
    //               terminal
    //
    // Strategy: walk from the focused pane up to the root, collecting every
    // Stack-of-Stacks (a Stack whose first child is also a Stack) we pass
    // through. The INNERMOST such Stack is the right one to cycle —
    // `subTabsStack` if focus is in a sub-bar, `_tabsStackNode` otherwise.
    // Falls back to `_tabsStackNode` for callers without a focused pane
    // (e.g. palette invocation with no pane focus context).
    let stackId = stackArg || null;
    if (!stackId) {
        const fp = mb.layout.focusedPane();
        if (fp) {
            for (let cur = fp.nodeId; cur; ) {
                const n = mb.layout.node(cur);
                if (!n) break;
                if (n.kind === 'stack' && n.children && n.children.length > 0) {
                    const firstChild = mb.layout.node(n.children[0].id);
                    if (firstChild && firstChild.kind === 'stack') {
                        stackId = cur;
                        // keep walking — outer matches override inner ones
                        // only when no inner match was found (innermost wins
                        // because the first assignment along the walk-up is
                        // the innermost; later assignments would overwrite).
                        // Actually we want INNERMOST, so break here.
                        break;
                    }
                }
                cur = n.parent;
            }
        }
    }
    if (!stackId) stackId = _tabsStackNode;
    if (!stackId) return;
    const stack = mb.layout.node(stackId);
    if (!stack || !stack.children || stack.children.length === 0) return;
    const active = stack.activeChild;
    let curIdx = -1;
    for (let i = 0; i < stack.children.length; i++) {
        if (stack.children[i].id === active) { curIdx = i; break; }
    }
    if (curIdx < 0) return;
    const newIdx = curIdx + delta;
    if (newIdx < 0 || newIdx >= stack.children.length) return;
    _activateTabAndFocus(stack.children[newIdx].id);
});

mb.actions.register('activateLastTab', ({stack: stackArg}) => {
    const stackId = stackArg || _tabsStackNode;
    if (!stackId) return;
    const prev = _lastActiveByStack.get(stackId);
    if (!prev) return;
    const stack = mb.layout.node(stackId);
    if (!stack || !stack.children) return;
    const stillExists = stack.children.some(c => c.id === prev);
    if (!stillExists) {
        _lastActiveByStack.delete(stackId);
        return;
    }
    _activateTabAndFocus(prev);
});

mb.actions.register('splitPane', ({dir}) => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;
    const pane = mb.pane(fp.nodeId);
    const cwd = pane ? (pane.cwd || '') : '';
    const opts = cwd ? { cwd } : undefined;
    const newNodeId = mb.layout.splitPane(fp.nodeId, dir, opts);
    if (newNodeId) mb.layout.focusPane(newNodeId);
});

mb.actions.register('closePane', async () => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;

    const pane = mb.pane(fp.nodeId);
    const mode = _confirmMode();
    const need = mode === "always" || (mode === "if_busy" && _isPaneBusy(pane));
    if (need) {
        const fg     = pane?.foregroundProcess || "process";
        const choice = await confirm({
            pane,
            title:        "Close pane?",
            message:      `'${fg}' is running. Close anyway?`,
            buttons:      [{ label: "Cancel" }, { label: "Close", primary: true }],
            defaultIndex: 0,
        });
        if (choice !== 1) return;
    }

    // Kill the Terminal; the `terminalExited` listener below drives the
    // tree removal and any tab/quit cascade. Keeping the user-keybind and
    // shell-exit paths on the same flow means there's only one place where
    // the empty-tab / last-tab policy is expressed.
    mb.layout.killTerminal(fp.nodeId);
});

mb.actions.register('zoomPane', () => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;
    // Walk up from the focused pane to the nearest enclosing Stack — that's
    // the tab's subtreeRoot. Its `zoomTarget` is the tree-native override;
    // we toggle between "zoomed to fp" and "cleared".
    let tabStack = null;
    for (let cur = fp.nodeId; cur; ) {
        const n = mb.layout.node(cur);
        if (!n) break;
        if (n.kind === 'stack') { tabStack = cur; break; }
        cur = n.parent;
    }
    if (!tabStack) return;
    const stackNode = mb.layout.node(tabStack);
    if (!stackNode) return;
    mb.layout.setStackZoom(tabStack, stackNode.zoomTarget ? null : fp.nodeId);
});

mb.actions.register('adjustPaneSize', ({dir, amount}) => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;
    mb.layout.adjustPaneSize(fp.nodeId, dir, amount);
});

mb.addEventListener('terminalExited', ({paneId, paneNodeId}) => {
    // Invariant at entry: Terminal is graveyarded; its tree node is still
    // present. Walk up the chain of Stack-child ancestors INNERMOST first.
    // At each level, if the subtree has no surviving Terminal, close it
    // through the unified path (mb.layout.closeTab → C++ closeTab handles
    // surviving-sibling activation + remembered-focus restore for any Stack,
    // top-level or sub-bar). Sub-stack about to become empty: dismantle the
    // wrapper Container so we don't leave an orphan TabBar + empty Stack
    // visible. Root tabs Stack about to be empty → quit.
    //
    // The chain is collected BEFORE removing the terminal so we have
    // valid parent pointers; the cascade then walks them in order.
    const node = mb.layout.node(paneNodeId);
    if (!node) return;
    const chain = []; // [{ stackChildId, parentStackId }, ...] innermost first
    for (let cur = node.parent; cur; ) {
        const n = mb.layout.node(cur);
        if (!n) break;
        if (n.parent) {
            const p = mb.layout.node(n.parent);
            if (p && p.kind === 'stack') {
                chain.push({ stackChildId: cur, parentStackId: n.parent });
            }
        }
        cur = n.parent;
    }

    mb.layout.removeNode(paneNodeId);

    for (const { stackChildId, parentStackId } of chain) {
        // If this Stack-child still has any live Terminal, the ancestors
        // above it definitely do too. Done walking.
        if (mb.layout.queryNodes('terminal', stackChildId).length !== 0) {
            break;
        }
        const parentStack = mb.layout.node(parentStackId);
        if (!parentStack) break;
        const isTabsStack = (parentStackId === _tabsStackNode);

        if (parentStack.children.length > 1) {
            // Multiple siblings remain. C++ closeTab removes this child +
            // activates a surviving sibling + restores its remembered focus.
            mb.layout.closeTab(stackChildId);
            // Fire focus-change events for the new focus so CSI ?1004 +
            // paneFocusChanged listeners + window-title refresh.
            const fp = mb.layout.focusedPane();
            if (fp) mb.layout.focusPane(fp.nodeId);
            break;
        }

        // Stack would be empty after removing this child.
        if (isTabsStack) {
            mb.quit();
            return;
        }

        // Sub-stack about to be empty: dismantle the wrapper Container that
        // holds [TabBar | this Stack]. wrapInStack created that wrapper as
        // the sub-stack's parent. Removing the wrapper drops the orphan
        // TabBar + empty Stack from the layout. The cascade continues so
        // the wrapper's enclosing levels can collapse too.
        const wrapperId = parentStack.parent;
        if (!wrapperId) break;
        mb.layout.removeNode(wrapperId);
        // Continue loop — outer Stack-child ancestors may also be empty
        // now (e.g. the top-level tab's content Container had only the
        // wrapper).
    }

    // After cascade, restore focus to a surviving leaf in whatever subtree
    // remains. C++ closeTab handled this within each Stack it closed; the
    // wrapper-dismantle branch did not. Pick the focused pane (C++
    // removeNode falls back to first live pane when focus was inside the
    // removed subtree) and re-fire focusPane so listeners observe the
    // transition.
    const fp = mb.layout.focusedPane();
    if (fp) mb.layout.focusPane(fp.nodeId);
});

// JS-only mutator actions for the SHELLS set. config.js calls
// `mb.invokeAction('default-ui.add-shell', {name: 'xonsh'})` to extend the
// list without rewriting closePane/closeTab. Idempotent so config.js
// hot-reload reapplies cleanly.
mb.actions.register('default-ui.add-shell',    ({name}) => { if (name) SHELLS.add(name); });
mb.actions.register('default-ui.remove-shell', ({name}) => { if (name) SHELLS.delete(name); });
mb.actions.register('default-ui.list-shells',  ()       => [...SHELLS]);

// OS window-close (X button / NSApp termination) — C++ fires this only when
// at least one listener is registered. With no listener, the C++ fallback
// quits immediately. So registering this here unconditionally is safe;
// `mode=never` short-circuits the dialog and quits in a single tick.
mb.addEventListener('quit-requested', async () => {
    const mode = _confirmMode();
    if (mode === "never") { mb.quit(); return; }

    // Aggregate busy processes across every Terminal in the tabs Stack.
    const allTerms = _tabsStackNode ? mb.layout.queryNodes('terminal', _tabsStackNode) : [];
    const busy     = _busyProcessesIn(allTerms);
    if (mode === "if_busy" && busy.length === 0) { mb.quit(); return; }

    const pane = mb.activePane;
    if (!pane) { mb.quit(); return; }   // no place to host the dialog — just quit
    const msg  = busy.length
        ? `${busy.length} process${busy.length === 1 ? '' : 'es'} running: ${busy.slice(0, 5).join(', ')}${busy.length > 5 ? '…' : ''}.\nQuit anyway?`
        : `Quit?`;
    const choice = await confirm({
        pane,
        title:        "Quit?",
        message:      msg,
        buttons:      [{ label: "Cancel" }, { label: "Quit", primary: true }],
        defaultIndex: 0,
    });
    if (choice === 1) mb.quit();
});

console.log('default-ui: loaded');

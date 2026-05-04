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
// bar moves side without a restart. LayoutTree has no "reorder children"
// API; instead we detach and re-append in the new order.
mb.addEventListener('configChanged', () => {
    if (!_rootContainer || !_tabBarNode || !_tabsStackNode) return;
    const want = mb.config?.tab_bar?.position || 'bottom';

    // Determine current order by walking children. If already correct,
    // skip the churn.
    const rootNode = mb.layout.node(_rootContainer);
    if (!rootNode || !rootNode.children) return;
    const ids = rootNode.children.map(c => c.id);
    const barIdx  = ids.indexOf(_tabBarNode);
    const stackIdx = ids.indexOf(_tabsStackNode);
    if (barIdx < 0 || stackIdx < 0) return;
    const currentlyTop = barIdx < stackIdx;
    if ((want === 'top') === currentlyTop) return;

    // Reorder: detach both, re-append in new order. The inner subtree
    // (_tabsStackNode) isn't destroyed — removeChild is structural-only.
    mb.layout.removeChild(_rootContainer, _tabBarNode);
    mb.layout.removeChild(_rootContainer, _tabsStackNode);
    if (want === 'top') {
        mb.layout.appendChild(_rootContainer, _tabBarNode,    {fixedCells: 1});
        mb.layout.appendChild(_rootContainer, _tabsStackNode, {stretch: 1});
    } else {
        mb.layout.appendChild(_rootContainer, _tabsStackNode, {stretch: 1});
        mb.layout.appendChild(_rootContainer, _tabBarNode,    {fixedCells: 1});
    }
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
    const termNodeId = mb.layout.createTerminal(tabUuid, opts);
    if (termNodeId) mb.layout.focusPane(termNodeId);
    mb.layout.activateTab(tabUuid);
});

mb.actions.register('closeTab', async ({index}) => {
    let target = null;
    if (typeof index === 'number' && index >= 0) {
        target = _tabUuidByIndex(index);
    } else {
        target = _activeTabUuid();
    }
    if (!target) return;

    const mode = _confirmMode();
    if (mode !== "never") {
        const termIds = mb.layout.queryNodes('terminal', target);
        const busy    = _busyProcessesIn(termIds);
        const need    = mode === "always" || busy.length > 0;
        if (need) {
            const fp   = mb.layout.focusedPane();
            const pane = fp ? mb.pane(fp.nodeId) : mb.activePane;
            const msg  = busy.length
                ? `${busy.length} process${busy.length === 1 ? '' : 'es'} running: ${busy.slice(0, 5).join(', ')}${busy.length > 5 ? '…' : ''}.\nClose tab anyway?`
                : `Close tab?`;
            const choice = await confirm({
                pane,
                title: "Close tab?",
                message: msg,
                buttons: [{ label: "Cancel" }, { label: "Close", primary: true }],
                defaultIndex: 0,
            });
            if (choice !== 1) return;
        }
    }
    mb.layout.closeTab(target);
});

mb.actions.register('activateTab', ({index}) => {
    const target = _tabUuidByIndex(index);
    if (target) mb.layout.activateTab(target);
});

mb.actions.register('activateTabRelative', ({delta}) => {
    if (!_tabBarNode) return;
    const bar = mb.layout.node(_tabBarNode);
    if (!bar || !bar.boundStack) return;
    const stack = mb.layout.node(bar.boundStack);
    if (!stack || !stack.children || stack.children.length === 0) return;
    const active = stack.activeChild;
    let curIdx = -1;
    for (let i = 0; i < stack.children.length; i++) {
        if (stack.children[i].id === active) { curIdx = i; break; }
    }
    if (curIdx < 0) return;
    const newIdx = curIdx + delta;
    if (newIdx < 0 || newIdx >= stack.children.length) return;
    mb.layout.activateTab(stack.children[newIdx].id);
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
    // present. Remove the node (structural-only — the Terminal is already
    // gone so the "no live Terminals beneath" guard is satisfied). If its
    // enclosing tab ends up empty, close the tab. If that was the last
    // tab, quit.
    mb.layout.removeNode(paneNodeId);

    // Find a tab subtree that contains zero live Terminal nodes. The killed
    // pane's tree node is gone (removeNode above); queryNodes("Terminal", subtree)
    // returns only what's left. We walk the chrome TabBar's bound Stack since
    // that's the canonical tabs list.
    if (!_tabBarNode) return;
    const bar = mb.layout.node(_tabBarNode);
    if (!bar || !bar.boundStack) return;
    const stack = mb.layout.node(bar.boundStack);
    if (!stack || !stack.children) return;

    let emptyTab = null;
    for (const child of stack.children) {
        if (mb.layout.queryNodes('terminal', child.id).length === 0) {
            emptyTab = child.id;
            break;
        }
    }
    if (!emptyTab) return;
    if (stack.children.length <= 1) {
        mb.quit();
    } else {
        mb.layout.closeTab(emptyTab);
    }
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

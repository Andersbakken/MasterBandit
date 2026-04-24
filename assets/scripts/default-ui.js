// Default UI controller. Owns the JS-policy actions (tab/pane lifecycle,
// structural mutations) AND the startup tree-shape construction. Mandatory
// — mb refuses to start without it.
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

    const position = mb.tabBarPosition || 'bottom';
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
    const term = mb.layout.createTerminal(content);
    if (term && term.nodeId) mb.layout.focusPane(term.nodeId);
})();

// Hot-reload support: if the user toggles [tab_bar].position between "top"
// and "bottom" at runtime, swap the root Container's children so the tab
// bar moves side without a restart. LayoutTree has no "reorder children"
// API; instead we detach and re-append in the new order.
mb.addEventListener('configChanged', () => {
    if (!_rootContainer || !_tabBarNode || !_tabsStackNode) return;
    const want = mb.tabBarPosition || 'bottom';

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

mb.actions.register('newTab', () => {
    const tab = mb.layout.createTab();
    if (!tab) return;
    const term = mb.layout.createTerminal(tab.nodeId);
    if (term) mb.layout.focusPane(term.id);
    mb.layout.activateTab(tab.id);
});

mb.actions.register('closeTab', ({index}) => {
    if (typeof index === 'number' && index >= 0) {
        mb.layout.closeTab(index);
    } else {
        const t = mb.layout.activeTab();
        if (t) mb.layout.closeTab(t.id);
    }
});

mb.actions.register('activateTab', ({index}) => {
    mb.layout.activateTab(index);
});

mb.actions.register('activateTabRelative', ({delta}) => {
    const t = mb.layout.activeTab();
    if (!t) return;
    mb.layout.activateTab(t.id + delta);
});

mb.actions.register('splitPane', ({dir}) => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;
    const result = mb.layout.splitPane(fp.nodeId, dir);
    if (result) mb.layout.focusPane(result.id);
});

mb.actions.register('closePane', () => {
    const fp = mb.layout.focusedPane();
    if (!fp) return;
    // Kill the Terminal; the `terminalExited` listener below drives the
    // tree removal and any tab/quit cascade. Keeping the user-keybind and
    // shell-exit paths on the same flow means there's only one place where
    // the empty-tab / last-tab policy is expressed.
    mb.layout.killTerminal(fp.id);
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
        if (n.kind === 'Stack') { tabStack = cur; break; }
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

    // Walk tabs: find the one containing no live panes. (The killed pane
    // is already filtered out of mb.tabs[i].panes — `panes` reflects live
    // Terminals only.)
    const tabs = mb.tabs;
    let emptyIdx = -1;
    for (let i = 0; i < tabs.length; i++) {
        if (tabs[i].panes.length === 0) { emptyIdx = i; break; }
    }
    if (emptyIdx < 0) return;
    if (tabs.length <= 1) {
        mb.quit();
    } else {
        mb.layout.closeTab(emptyIdx);
    }
});

console.log('default-ui: loaded');

// Default UI controller. Owns the JS-policy actions (tab/pane lifecycle,
// structural mutations). Mandatory — mb refuses to start without it.

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
    mb.layout.setZoom(fp.nodeId);
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

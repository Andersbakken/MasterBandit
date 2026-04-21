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
    mb.layout.closePane(fp.id);
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
    // Observation only for now; native cascade handles the close. Once the
    // controller fully owns pane lifecycle, call mb.layout.closePane(paneId).
    console.log(`default-ui: terminalExited paneId=${paneId} nodeId=${paneNodeId}`);
});

console.log('default-ui: loaded');

// Built-in controller script: handles applet launch via OSC 58237.
//
// Shell usage:
//   printf '\e]58237;applet;path=/path/to/script.js\e\\'

function parsePayload(payload) {
    const parts = payload.split(";");
    if (parts.length < 1) return null;

    const verb = parts[0];
    const kv = {};
    for (let i = 1; i < parts.length; i++) {
        const eq = parts[i].indexOf("=");
        if (eq >= 0) {
            kv[parts[i].substring(0, eq)] = parts[i].substring(eq + 1);
        }
    }
    return { verb, kv };
}

function registerPane(pane) {
    pane.addEventListener("osc:58237", (payload) => {
        const parsed = parsePayload(payload);
        if (!parsed) return;

        if (parsed.verb === "applet") {
            const path = parsed.kv.path;
            if (!path) {
                console.error("applet-loader: missing path in applet OSC");
                return;
            }
            const permissions = parsed.kv.permissions || "";
            const id = mb.loadScript(path, permissions);
            if (id) {
                console.log("applet-loader: loaded script:", path, "id:", id);
            } else {
                console.log("applet-loader: script pending approval or denied:", path);
            }
        } else {
            console.log("applet-loader: unknown verb:", parsed.verb);
        }
    });
}

// Listen for OSC 58237 on all new panes
mb.addEventListener("paneCreated", (pane) => {
    registerPane(pane);
});

// Also register on existing panes at startup
for (const tab of mb.tabs) {
    for (const pane of tab.panes) {
        registerPane(pane);
    }
}

console.log("applet-loader: initialized");

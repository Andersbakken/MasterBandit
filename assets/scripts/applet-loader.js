// Built-in controller script: handles applet launch via OSC 58237
// and permission prompts for untrusted scripts.
//
// Shell usage:
//   printf '\e]58237;applet;path=/path/to/script.js;permissions=ui,io\e\\'

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

// ============================================================================
// Permission prompt popup
// ============================================================================

const W = 50, H = 8;
let permPopupCounter = 0;
const activePermPopups = {}; // path → popup

function showPermissionPrompt(path, permissions, hash) {
    // Close any existing prompt for this path
    if (activePermPopups[path]) {
        activePermPopups[path].close();
        delete activePermPopups[path];
    }
    const pane = mb.activePane;
    if (!pane) {
        console.error("applet-loader: no active pane for permission prompt");
        return;
    }

    const x = Math.max(0, Math.floor((pane.cols - W) / 2));
    const y = Math.max(0, Math.floor((pane.rows - H) / 2));
    const popupId = "__perm_" + (++permPopupCounter);
    const popup = pane.createPopup({ id: popupId, x, y, w: W, h: H });
    if (!popup) {
        console.error("applet-loader: failed to create permission popup");
        return;
    }

    // Extract filename from path
    let filename = path;
    const slash = path.lastIndexOf("/");
    if (slash >= 0) filename = path.substring(slash + 1);

    const inner = W - 2;
    const pad = (s, w) => s.length >= w ? s.substring(0, w) : s + " ".repeat(w - s.length);
    const border = "\x1b[90m";
    const reset = "\x1b[0m";
    const hline = border + "+" + "-".repeat(inner) + "+" + reset;
    const line = (color, text) =>
        border + "|" + reset + color + pad(text, inner) + reset + border + "|" + reset;

    // Button positions (0-indexed columns within popup):
    //  col 2-8: [allow]   col 11-16: [deny]   col 19-26: [always]   col 29-35: [never]
    const btnAllow  = { x1: 2, x2: 8 };
    const btnDeny   = { x1: 11, x2: 16 };
    const btnAlways = { x1: 19, x2: 26 };
    const btnNever  = { x1: 29, x2: 35 };
    const btnRow = 6; // 0-indexed

    function render() {
        let out = "\x1b[H";
        out += "\x1b[1;1H" + hline;
        out += "\x1b[2;1H" + line("\x1b[1;33m", " Script Permission Request");
        out += "\x1b[3;1H" + line("\x1b[1m", " Path: " + filename);
        out += "\x1b[4;1H" + line("\x1b[36m", " Perms: " + permissions);
        out += "\x1b[5;1H" + line("\x1b[90m", " Hash: " + hash.substring(0, 16) + "...");
        out += "\x1b[6;1H" + line("", "");
        // Button line — use cursor positioning to place each button precisely
        const r = 7;
        out += "\x1b[" + r + ";1H" + border + "|" + reset;
        out += "\x1b[" + r + ";" + (btnAllow.x1 + 1) + "H\x1b[1;32m[allow]\x1b[0m";
        out += "\x1b[" + r + ";" + (btnDeny.x1 + 1) + "H\x1b[1;31m[deny]\x1b[0m";
        out += "\x1b[" + r + ";" + (btnAlways.x1 + 1) + "H\x1b[1;36m[always]\x1b[0m";
        out += "\x1b[" + r + ";" + (btnNever.x1 + 1) + "H\x1b[1;35m[never]\x1b[0m";
        out += "\x1b[" + r + ";" + W + "H" + border + "|" + reset;
        out += "\x1b[8;1H" + hline;
        popup.inject(out);
    }

    activePermPopups[path] = popup;

    function respond(response) {
        delete activePermPopups[path];
        popup.close();
        mb.approveScript(path, response);
    }

    // Keyboard input (focus popup first with Cmd+Shift+I)
    popup.addEventListener("input", (data) => {
        if (data === "y" || data === "Y") respond("y");
        else if (data === "n" || data === "N") respond("n");
        else if (data === "a" || data === "A") respond("a");
        else if (data === "d" || data === "D") respond("d");
    });

    // Mouse clicks on buttons
    popup.addEventListener("mouse", (ev) => {
        if (ev.type !== "press" || ev.button !== 0) return;
        if (ev.cellY !== btnRow) return;

        const cx = ev.cellX;
        if (cx >= btnAllow.x1 && cx <= btnAllow.x2)   respond("y");
        else if (cx >= btnDeny.x1 && cx <= btnDeny.x2) respond("n");
        else if (cx >= btnAlways.x1 && cx <= btnAlways.x2) respond("a");
        else if (cx >= btnNever.x1 && cx <= btnNever.x2)   respond("d");
    });

    render();
    console.log("applet-loader: showing permission prompt for", path);
}

// ============================================================================
// Event listeners
// ============================================================================

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

// Handle permission prompts from the engine
mb.addEventListener("scriptPermissionRequired", (path, permissions, hash) => {
    showPermissionPrompt(path, permissions, hash);
});

console.log("applet-loader: initialized");

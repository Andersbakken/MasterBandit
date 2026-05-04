// Built-in controller script: handles applet launch via OSC 58237
// and permission prompts for untrusted scripts.
//
// Shell usage:
//   printf '\e]58237;applet;path=/path/to/script.js;permissions=ui,io\e\\'
//
// Acknowledgement: for every OSC 58237 "applet" request, the shell receives
// exactly one terminal response on its stdin:
//   \e]58237;result;status=loaded;id=<n>;path=<path>\e\\    — script running
//   \e]58237;result;status=denied;path=<path>\e\\           — allowlist-denied or user denied
//   \e]58237;result;status=error;path=<path>;error=<url-encoded>\e\\
// "pending" (prompt shown) produces no immediate ack; the final loaded/denied
// ack arrives only after the user responds. Shells should use a generous
// timeout (approval can take 30s+).

import { confirm } from "mb:dialog";

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

// path → pane awaiting permission-prompt resolution. The final ack is written
// from respond() once the user picks allow / deny / always / never.
const pendingPanes = new Map();

function writeAck(pane, path, result) {
    if (!pane || !pane.hasPty) return;
    let msg = `\x1b]58237;result;status=${result.status};path=${path}`;
    if (result.status === "loaded" && typeof result.id === "number") {
        msg += `;id=${result.id}`;
    } else if (result.status === "error" && result.error) {
        msg += `;error=${encodeURIComponent(result.error)}`;
    }
    msg += "\x1b\\";
    pane.write(msg);
}

function registerPane(pane) {
    pane.addEventListener("osc:58237", (payload) => {
        const parsed = parsePayload(payload);
        if (!parsed) return;

        if (parsed.verb !== "applet") {
            // Not our verb — either stray OSC, or another listener's concern.
            return;
        }

        const path = parsed.kv.path;
        if (!path) {
            console.error("applet-loader: missing path in applet OSC");
            return;
        }
        const permissions = parsed.kv.permissions || "";
        const res = mb.loadScript(path, permissions);

        switch (res.status) {
            case "loaded":
                console.log("applet-loader: loaded script:", path, "id:", res.id);
                writeAck(pane, path, res);
                break;
            case "pending":
                console.log("applet-loader: script awaiting approval:", path);
                pendingPanes.set(path, pane);
                // No ack yet; respond() will write it after the user picks.
                break;
            case "denied":
                console.log("applet-loader: script permanently denied:", path);
                writeAck(pane, path, res);
                break;
            case "error":
                console.error("applet-loader: script load error:", path, res.error);
                writeAck(pane, path, res);
                break;
        }
    });
}

// ============================================================================
// Permission prompt popup — built on mb:dialog.confirm()
// ============================================================================

// path → opaque token of the latest dialog for that path. If a duplicate
// scriptPermissionRequired arrives, we dismiss the prior dialog and install
// a new token; the prior dialog's resolution checks the token and skips its
// ack if it's been superseded.
const activePermTokens = new Map();
// path → dismiss fn for the active dialog, so a duplicate request can tear
// the prior popup down.
const activePermDismiss = new Map();

function showPermissionPrompt(path, permissions, hash) {
    const prevDismiss = activePermDismiss.get(path);
    if (prevDismiss) prevDismiss();

    const pane = mb.activePane;
    if (!pane) {
        console.error("applet-loader: no active pane for permission prompt");
        return;
    }

    let filename = path;
    const slash = path.lastIndexOf("/");
    if (slash >= 0) filename = path.substring(slash + 1);

    const myToken = {};
    activePermTokens.set(path, myToken);

    const dialog = confirm({
        pane,
        title: 'Script Permission Request',
        message:
            'Path: '  + filename + '\n' +
            'Perms: ' + permissions + '\n' +
            'Hash: '  + hash.substring(0, 16) + '...',
        buttons: [
            { label: 'allow',  key: 'y',
              color: 'bright-green.bold',
              selectedFg: 'black', selectedBg: 'green',
              hoverFg:    'black', hoverBg:    'bright-green' },
            { label: 'deny',   key: 'n',
              color: 'bright-red.bold',
              selectedFg: 'black', selectedBg: 'red',
              hoverFg:    'black', hoverBg:    'bright-red' },
            { label: 'always', key: 'a',
              color: 'bright-cyan.bold',
              selectedFg: 'black', selectedBg: 'cyan',
              hoverFg:    'black', hoverBg:    'bright-cyan' },
            { label: 'never',  key: 'd',
              color: 'bright-magenta.bold',
              selectedFg: 'black', selectedBg: 'magenta',
              hoverFg:    'black', hoverBg:    'bright-magenta' },
        ],
        defaultIndex: 1, // deny — Enter denies by default
    });
    activePermDismiss.set(path, dialog.dismiss);
    dialog.then((idx) => {
        // Superseded: a newer dialog took over; that one will write the ack.
        if (activePermTokens.get(path) !== myToken) return;
        activePermTokens.delete(path);
        activePermDismiss.delete(path);
        // Esc / pane-destroyed / onDestroy → -1: treat as "deny once" so the
        // shell receives an ack instead of hanging on the original request.
        const response = idx >= 0 ? ['y', 'n', 'a', 'd'][idx] : 'n';
        const res = mb.approveScript(path, response);
        const originPane = pendingPanes.get(path);
        pendingPanes.delete(path);
        if (originPane) writeAck(originPane, path, res);
    });

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
for (const nodeId of mb.layout.queryNodes("terminal")) {
    const pane = mb.pane(nodeId);
    if (pane) registerPane(pane);
}

// Handle permission prompts from the engine
mb.addEventListener("scriptPermissionRequired", (path, permissions, hash) => {
    showPermissionPrompt(path, permissions, hash);
});

console.log("applet-loader: initialized");

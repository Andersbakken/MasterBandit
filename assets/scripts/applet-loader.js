// Built-in controller script: handles applet launch and pty-backed popup
// requests via OSC 58237, plus permission prompts for untrusted scripts.
//
// Shell usage:
//   printf '\e]58237;applet;path=/path/to/script.js;permissions=ui,io\e\\'
//   printf '\e]58237;popup;id=foo;w=40;h=10\e\\'        (x/y/w/h optional)
//   printf '\e]58237;popup-close;id=foo\e\\'
//
// Acknowledgement: for every OSC 58237 "applet" request, the shell receives
// exactly one terminal response on its stdin:
//   \e]58237;result;status=loaded;id=<n>;path=<path>\e\\    — script running
//   \e]58237;result;status=denied;path=<path>\e\\           — allowlist-denied or user denied
//   \e]58237;result;status=error;path=<path>;error=<url-encoded>\e\\
// "pending" (prompt shown) produces no immediate ack; the final loaded/denied
// ack arrives only after the user responds. Shells should use a generous
// timeout (approval can take 30s+).
//
// "popup" requests ack the same way (one response per request; none while a
// permission prompt is showing, except the pairing PIN ack):
//   \e]58237;popup-result;status=created;id=<id>;tty=/dev/pts/N\e\\
//   \e]58237;popup-result;status=pairing;id=<id>;pin=<pin>\e\\
//   \e]58237;popup-result;status=paired;id=<id>;tty=...;key=<secret>\e\\
//   \e]58237;popup-result;status=closed;id=<id>\e\\
//   \e]58237;popup-result;status=denied;id=<id>[;error=...]\e\\
//   \e]58237;popup-result;status=error;id=<id>;error=<url-encoded>\e\\
// Trust model: possession of a popup key (key=<secret>) creates with no
// prompt; keys are minted by the user (popup-keys.create action) or via
// the pair=1 PIN flow, and stored hashed in
// <configDir>/popup-permissions.json. Keyless requests always prompt
// (allow / deny / never) keyed on the foreground exe path; "never"
// suppresses future prompts. Prompt spam is rate-limited: one dialog per
// process and per pane, a cooldown after each deny, session-wide mute
// after repeated denials, and a cap on live OSC popups per pane.

import { confirm } from "mb:dialog";
import { readFileSync, writeFileSync } from "mb:fs";

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

function handleAppletLoad(pane, kv) {
    const path = kv.path;
    if (!path) {
        console.error("applet-loader: missing path in applet OSC");
        return;
    }
    const permissions = kv.permissions || "";
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
}

// ============================================================================
// PTY-backed popups via OSC — permission gated on the foreground process
// ============================================================================

function writePopupAck(pane, id, result) {
    if (!pane || !pane.hasPty) return;
    let msg = `\x1b]58237;popup-result;status=${result.status}`;
    if (id) msg += `;id=${id}`;
    if (result.pin) msg += `;pin=${result.pin}`;
    if (result.tty) msg += `;tty=${result.tty}`;
    if (result.key) msg += `;key=${result.key}`;
    if (result.error) msg += `;error=${encodeURIComponent(result.error)}`;
    msg += "\x1b\\";
    pane.write(msg);
}

// Persisted popup-permission store:
//   processVerdicts: { "<process key>": "never" } — prompt suppression,
//     keyed on popupPermKey() (best-effort foreground identity).
//   keys: [{ hash, label, created, source: "manual" | "paired" }] — trusted
//     capabilities. A request presenting a matching key= secret creates a
//     popup with no prompt. Only sha256 hashes are stored; the secret
//     lives with the app (paired) or the user (manual, e.g. an env var).
function popupPermFile() {
    return mb.configDir + "/popup-permissions.json";
}

function loadPopupPerms() {
    let parsed = null;
    try {
        parsed = JSON.parse(readFileSync(popupPermFile()));
    } catch {
        // missing or malformed — start empty
    }
    if (!parsed || typeof parsed !== "object") parsed = {};
    return {
        processVerdicts:
            parsed.processVerdicts && typeof parsed.processVerdicts === "object"
                ? parsed.processVerdicts
                : {},
        keys: Array.isArray(parsed.keys) ? parsed.keys : [],
    };
}

function savePopupPerms(perms) {
    try {
        writeFileSync(popupPermFile(), JSON.stringify(perms, null, 2) + "\n");
    } catch (e) {
        console.error("applet-loader: cannot persist popup permissions:", e);
    }
}

function saveProcessVerdict(processKey, verdict) {
    const perms = loadPopupPerms();
    perms.processVerdicts[processKey] = verdict;
    savePopupPerms(perms);
}

// Mint a popup key: returns the secret; only its hash is persisted.
function addPopupKey(label, source) {
    const secret = mb.createSecureToken(16);
    const perms = loadPopupPerms();
    perms.keys.push({
        hash: mb.sha256(secret),
        label,
        created: new Date().toISOString(),
        source,
    });
    savePopupPerms(perms);
    console.log("applet-loader: popup key created:", label, `(${source})`);
    return secret;
}

function revokePopupKeys(label) {
    const perms = loadPopupPerms();
    const before = perms.keys.length;
    perms.keys = perms.keys.filter((k) => k.label !== label);
    if (perms.keys.length !== before) savePopupPerms(perms);
    return before - perms.keys.length;
}

// Permission identity: the foreground process's resolved executable path
// (kernel-maintained, immune to argv0/prctl name spoofing), falling back
// to the comm name when /proc/<pid>/exe is unreadable.
function popupPermKey(pane) {
    return pane.foregroundExe || pane.foregroundProcess || "(unknown)";
}

// Rate limits. All of these deny with an immediate ack instead of
// prompting, so a request loop can't spam dialogs:
// - one visible prompt per process key AND per pane (no stacking/supersede)
// - a cooldown after each user deny/dismiss
// - after POPUP_DENIALS_TO_NEVER denials, no more prompts this session
// - at most MAX_OSC_POPUPS_PER_PANE live OSC-created popups per pane
const POPUP_DENY_COOLDOWN_MS = 60000;
const POPUP_DENIALS_TO_NEVER = 3;
const MAX_OSC_POPUPS_PER_PANE = 4;

const activePopupPrompts = new Set();   // key → prompt currently showing
const popupDenyCooldown = new Map();    // key → ms timestamp cooldown ends
const popupSessionDenials = new Map();  // key → user denials this session
const popupSessionNever = new Set();    // keys escalated to session-never

const POPUP_BTN_STYLES = {
    y: { color: 'bright-green.bold',
         selectedFg: 'black', selectedBg: 'green',
         hoverFg:    'black', hoverBg:    'bright-green' },
    n: { color: 'bright-red.bold',
         selectedFg: 'black', selectedBg: 'red',
         hoverFg:    'black', hoverBg:    'bright-red' },
    d: { color: 'bright-magenta.bold',
         selectedFg: 'black', selectedBg: 'magenta',
         hoverFg:    'black', hoverBg:    'bright-magenta' },
};

function popupBtn(label, key) {
    return Object.assign({ label, key }, POPUP_BTN_STYLES[key]);
}

// Show a popup-permission dialog. onDecision receives one of the button
// keys ("y" | "n" | "d"), with "n" for Esc/dismissed. Bookkeeps
// activePopupPrompts for the one-prompt-per-process limit.
function showPopupPrompt(pane, processKey, message, buttons, onDecision) {
    activePopupPrompts.add(processKey);
    const dialog = confirm({
        pane,
        title: 'Popup Permission Request',
        message,
        buttons,
        defaultIndex: 1, // deny — Enter denies by default
    });
    dialog.then((idx) => {
        activePopupPrompts.delete(processKey);
        onDecision(idx >= 0 ? buttons[idx].key : 'n');
    });
}

function handlePopupCreate(pane, kv, oscPopupIds, paneState) {
    const id = kv.id;
    if (!id) {
        writePopupAck(pane, "", { status: "error", error: "missing-id" });
        return;
    }

    const doCreate = (ackExtra) => {
        const cols = pane.cols, rows = pane.rows;
        let w = kv.w !== undefined ? parseInt(kv.w, 10) : 40;
        let h = kv.h !== undefined ? parseInt(kv.h, 10) : 10;
        if (!Number.isInteger(w) || w <= 0 || !Number.isInteger(h) || h <= 0) {
            writePopupAck(pane, id, { status: "error", error: "bad-params" });
            return;
        }
        w = Math.min(w, cols);
        h = Math.min(h, rows);
        // Default position: centered.
        let x = kv.x !== undefined ? parseInt(kv.x, 10) : Math.floor((cols - w) / 2);
        let y = kv.y !== undefined ? parseInt(kv.y, 10) : Math.floor((rows - h) / 2);
        if (!Number.isInteger(x) || !Number.isInteger(y)) {
            writePopupAck(pane, id, { status: "error", error: "bad-params" });
            return;
        }
        x = Math.max(0, Math.min(x, cols - w));
        y = Math.max(0, Math.min(y, rows - h));

        let popup = null;
        try {
            popup = pane.createPopup({ id, x, y, w, h, pty: true });
        } catch (e) {
            writePopupAck(pane, id, { status: "error", error: String(e && e.message || e) });
            return;
        }
        if (!popup || !popup.tty) {
            writePopupAck(pane, id, { status: "error", error: "create-failed" });
            return;
        }
        oscPopupIds.add(id);
        console.log("applet-loader: popup", id, "created on", popup.tty);
        writePopupAck(
            pane, id,
            Object.assign({ status: "created", tty: popup.tty }, ackExtra));
    };

    const capReached = () => {
        // Cap live OSC popups per pane. Prune ids whose popup was closed
        // by other means (script close, pane teardown) first.
        for (const staleId of [...oscPopupIds]) {
            if (!pane.popups.some((p) => p.id === staleId)) oscPopupIds.delete(staleId);
        }
        return oscPopupIds.size >= MAX_OSC_POPUPS_PER_PANE;
    };

    // Key-authenticated request: possession of a stored key is the grant —
    // no prompt, no foreground guessing. Unknown key is denied outright
    // (no prompt either: a revoked app must re-pair, not nag).
    if (kv.key !== undefined) {
        const known = loadPopupPerms().keys.some(
            (k) => k.hash === mb.sha256(kv.key));
        if (!known) {
            writePopupAck(pane, id, { status: "denied", error: "bad-key" });
            return;
        }
        if (capReached()) {
            writePopupAck(pane, id, { status: "error", error: "too-many-popups" });
            return;
        }
        doCreate();
        return;
    }

    const processKey = popupPermKey(pane);
    if (loadPopupPerms().processVerdicts[processKey] === "never"
        || popupSessionNever.has(processKey)) {
        console.log("applet-loader: popup denied by allowlist for", processKey);
        writePopupAck(pane, id, { status: "denied" });
        return;
    }
    const cooldownEnd = popupDenyCooldown.get(processKey);
    if (cooldownEnd && Date.now() < cooldownEnd) {
        writePopupAck(pane, id, { status: "denied" });
        return;
    }
    if (activePopupPrompts.has(processKey) || paneState.promptShowing) {
        writePopupAck(pane, id, { status: "denied", error: "prompt-pending" });
        return;
    }
    if (capReached()) {
        writePopupAck(pane, id, { status: "error", error: "too-many-popups" });
        return;
    }

    const slash = processKey.lastIndexOf("/");
    const display = slash >= 0 ? processKey.substring(slash + 1) : processKey;
    const pathLine = slash >= 0 ? 'Path: ' + processKey + '\n' : '';

    // Pairing (pair=1): grant mints a persistent key delivered in the ack.
    // The PIN binds this dialog to the requesting instance — the app
    // displays the PIN from the pairing ack, the user checks it matches.
    const pairing = kv.pair === "1";
    let message, buttons, pin = null;
    if (pairing) {
        const label = kv.label || display;
        pin = String(parseInt(mb.createSecureToken(4), 16) % 10000).padStart(4, '0');
        writePopupAck(pane, id, { status: "pairing", pin });
        message =
            '«' + label + '» wants to pair for popup access\n' +
            'Process: ' + display + '\n' + pathLine +
            'PIN (must match the app\'s): ' + pin + '\n' +
            'Grant issues a key valid until revoked';
        buttons = [popupBtn('grant', 'y'), popupBtn('deny', 'n'), popupBtn('never', 'd')];
    } else {
        message =
            'Process: ' + display + '\n' + pathLine +
            'Wants to open a popup in this pane';
        buttons = [popupBtn('allow', 'y'), popupBtn('deny', 'n'), popupBtn('never', 'd')];
    }

    // No ack until the user picks (beyond the pairing PIN ack above),
    // mirroring the applet approval flow.
    paneState.promptShowing = true;
    showPopupPrompt(pane, processKey, message, buttons, (resp) => {
        paneState.promptShowing = false;
        if (resp === 'y') {
            if (pairing) {
                const secret = addPopupKey(kv.label || display, "paired");
                doCreate({ status: "paired", key: secret });
            } else {
                doCreate();
            }
            return;
        }
        if (resp === 'd') saveProcessVerdict(processKey, "never");
        writePopupAck(pane, id, { status: "denied" });
        if (resp === 'n') {
            // "deny once" (or Esc): back off, and stop asking entirely
            // after repeated denials this session.
            popupDenyCooldown.set(processKey, Date.now() + POPUP_DENY_COOLDOWN_MS);
            const denials = (popupSessionDenials.get(processKey) || 0) + 1;
            popupSessionDenials.set(processKey, denials);
            if (denials >= POPUP_DENIALS_TO_NEVER) {
                console.log("applet-loader: popup prompts muted this session for", processKey);
                popupSessionNever.add(processKey);
            }
        }
    });
}

function handlePopupClose(pane, kv, oscPopupIds) {
    const id = kv.id;
    if (!id) {
        writePopupAck(pane, "", { status: "error", error: "missing-id" });
        return;
    }
    // Only popups created through this OSC channel can be closed by it —
    // script-owned popups are not the shell's to dismiss.
    const popup = oscPopupIds.has(id)
        ? pane.popups.find((p) => p.id === id)
        : undefined;
    oscPopupIds.delete(id);
    if (!popup) {
        writePopupAck(pane, id, { status: "error", error: "not-found" });
        return;
    }
    popup.close();
    writePopupAck(pane, id, { status: "closed" });
}

function registerPane(pane) {
    // Popup ids created via OSC on this pane; lifetime matches the
    // listener closure (i.e. the pane). promptShowing enforces one
    // popup-permission dialog per pane.
    const oscPopupIds = new Set();
    const paneState = { promptShowing: false };
    pane.addEventListener("osc:58237", (payload) => {
        const parsed = parsePayload(payload);
        if (!parsed) return;

        switch (parsed.verb) {
            case "applet":
                handleAppletLoad(pane, parsed.kv);
                break;
            case "popup":
                handlePopupCreate(pane, parsed.kv, oscPopupIds, paneState);
                break;
            case "popup-close":
                handlePopupClose(pane, parsed.kv, oscPopupIds);
                break;
            default:
                // Not our verb — stray OSC, or another listener's concern.
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

// ============================================================================
// Popup-key management actions (command palette / keybindings)
// ============================================================================

mb.setNamespace("popup-keys");
mb.registerAction("create", {
    args: [{ name: "label", label: "Key label", kind: "string", required: true }],
});
mb.registerAction("revoke", {
    args: [{ name: "label", label: "Key label", kind: "string", required: true }],
});

mb.addEventListener("action", "popup-keys.create", ({ label }) => {
    if (typeof label !== "string" || !label) return;
    const secret = addPopupKey(label, "manual");
    mb.setClipboard(secret);
    const pane = mb.activePane;
    if (!pane) return;
    confirm({
        pane,
        title: 'Popup Key Created',
        message:
            'Label: ' + label + '\n' +
            secret + '\n' +
            'Copied to clipboard. Shown once —\n' +
            'MB stores only its hash.',
        buttons: [popupBtn('ok', 'y')],
        defaultIndex: 0,
    });
});

mb.addEventListener("action", "popup-keys.revoke", ({ label }) => {
    if (typeof label !== "string" || !label) return;
    const n = revokePopupKeys(label);
    console.log("applet-loader: revoked", n, "popup key(s) labeled", label);
    const pane = mb.activePane;
    if (!pane) return;
    confirm({
        pane,
        title: 'Popup Key Revoke',
        message: n > 0
            ? 'Revoked ' + n + ' key(s) labeled "' + label + '"'
            : 'No key labeled "' + label + '"',
        buttons: [popupBtn('ok', 'y')],
        defaultIndex: 0,
    });
});

console.log("applet-loader: initialized");

# Writing MasterBandit Applets

Practical notes for writing JS/TS applets that run inside MasterBandit's
QuickJS-ng engine. Focuses on sharp edges and non-obvious behavior, not a
reference — that's [`types/mb.d.ts`](types/mb.d.ts) and DESIGN §19.

## Loading

An applet is a JS module loaded via `mb.loadScript(path, perms)`. Three ways to
trigger it:

1. **OSC 58237 from a shell** — `printf '\e]58237;applet;path=/path/to/applet.js;permissions=ui,shell\e\\'` is parsed by `applet-loader.js`, which calls `mb.loadScript`. First load prompts for permission (popup with allow / deny / always / never); subsequent loads use the allowlist.
2. **Command palette / config / built-in script** — any script with `scripts.load` permission can call `mb.loadScript` directly.
3. **Built-in scripts** — `assets/scripts/*.js` loaded at MB startup; fully trusted (all permissions, no prompt).

`mb.loadScript` returns a `MbLoadResult` discriminated union:
`{status:"loaded", id}` | `{status:"pending"}` | `{status:"denied"}` | `{status:"error", error?}`.
A `"pending"` return means the user prompt was raised and the caller will get
the final outcome via `mb.approveScript`'s return value when the user picks.

### OSC 58237 acknowledgement

For every `applet` verb OSC 58237, `applet-loader.js` writes exactly one
response to the originating pane's PTY:

```
\e]58237;result;status=loaded;id=<n>;path=<path>\e\\       — script running
\e]58237;result;status=denied;path=<path>\e\\              — allowlist-denied or user denied
\e]58237;result;status=error;path=<path>;error=<url-encoded>\e\\
```

No ack is written while the permission prompt is showing (`pending` state) —
the final `loaded` / `denied` ack is written after the user picks. Shells
should treat "no ack within a generous timeout" (30s+) as "no integration
available" and run un-integrated; humans take time on prompts.

## PTY-backed popups (OSC `popup` verb)

Any CLI app can open a popup on its pane and get back a filesystem endpoint —
a pty slave device — that any process may then open and read/write. Bytes
written to it render in the popup with full VT semantics (colors, cursor
movement, TUI redraws); when the popup is focused (Cmd/Ctrl+Shift+I cycles),
keyboard input is readable from the same device.

```
\e]58237;popup;id=<id>[;x=<col>][;y=<row>][;w=<cols>][;h=<rows>][;key=<secret>][;pair=1][;label=<name>]\e\\
\e]58237;popup-close;id=<id>\e\\
```

`id` is required and unique per pane. `w`/`h` default to 40×10, `x`/`y` to
centered; everything is clamped to the pane. Acks arrive on the requesting
pane's PTY, one per request:

```
\e]58237;popup-result;status=created;id=<id>;tty=/dev/pts/N\e\\
\e]58237;popup-result;status=pairing;id=<id>;pin=<4 digits>\e\\
\e]58237;popup-result;status=paired;id=<id>;tty=/dev/pts/N;key=<secret>\e\\
\e]58237;popup-result;status=closed;id=<id>\e\\
\e]58237;popup-result;status=denied;id=<id>[;error=bad-key|prompt-pending]\e\\
\e]58237;popup-result;status=error;id=<id>;error=<url-encoded>\e\\
```

Shell example:

```sh
printf '\e]58237;popup;id=log;w=60;h=12\e\\'
# parse the ack for tty=..., then:
tail -f build.log > /dev/pts/N &
printf '\e]58237;popup-close;id=log\e\\'
```

Semantics worth knowing:

- **Popup keys — the persistent trust mechanism.** A request presenting
  `key=<secret>` matching a stored key creates the popup with no prompt.
  Possession is the identity: whoever holds the key is the grantee,
  regardless of which process is foreground. MB stores only sha256 hashes
  (in `<configDir>/popup-permissions.json`, with a label and creation
  date); an unknown/revoked key gets `status=denied;error=bad-key` with no
  prompt (re-pair instead of nagging). Two ways to get a key:
  - **User-created**: the `popup-keys.create` action (command palette)
    prompts for a label, copies the secret to the clipboard, and shows it
    once. Drop it in your shell env (`export MB_POPUP_KEY=...`) and
    one-off scripts get promptless popups:
    `printf '\e]58237;popup;id=x;key='"$MB_POPUP_KEY"'\e\\'`.
    Revoke by label with `popup-keys.revoke`.
  - **Pairing** (`pair=1[;label=<name>]`): MB immediately acks
    `status=pairing;pin=<4 digits>`; the app displays the PIN; MB shows a
    dialog with the same PIN. Matching codes tell the user the dialog
    belongs to the app they just ran (a concurrent hostile request would
    show a different PIN). Grant mints a key, creates the popup, and acks
    `status=paired;tty=...;key=<secret>` — the app stores the key for
    next time.
- **Keyless requests always prompt** (allow / deny / never — no persistent
  "allow always"; that's what keys are for). The prompt is keyed on the
  foreground process's resolved executable path (`/proc/<pid>/exe` /
  `proc_pidpath` — immune to argv0/prctl name spoofing; comm-name fallback)
  and `never` persists, silently denying future prompts. Foreground
  attribution is best-effort: the pty channel cannot prove *which* in-pane
  process emitted the OSC, which is exactly why persistent trust rides on
  keys instead. Keys are bearer tokens — same-user processes can steal
  them; in-model per "TTY is not a security boundary" below.
- **Prompt spam limits**: at most one permission dialog per process and per
  pane (concurrent requests get `status=denied;error=prompt-pending`); a
  60s cooldown after each user deny; after 3 denials the process is muted
  for the rest of the session; at most 4 live OSC-created popups per pane
  (`status=error;error=too-many-popups`, applies to keyed requests too).
- MB holds its own fd on the slave, so writers can open and close freely —
  `echo hi > /dev/pts/N` per line works; the popup never sees EOF.
- Default termios is canonical + echo. Programs *reading* popup input from
  the device should raw-mode it first (`stty -F /dev/pts/N raw -echo`).
- The device reports the popup's size via `TIOCGWINSZ` and tracks
  `popup.resize` / OSC-side resizes.
- The popup dies on `popup-close`, when its pane or tab closes, or with MB.
  `popup-close` only closes popups created via this OSC channel.
- Capability discovery: `mb-query-popup` via XTGETTCAP (DCS).
- JS equivalent: `pane.createPopup({id, x, y, w, h, pty: true})`; the slave
  path is `popup.tty`. The JS `"input"` event does not fire for pty popups —
  keystrokes go to the pty.

## Register everything at top level, synchronously

**The single most important rule.** QuickJS runs the module's top-level code
synchronously from the event loop's perspective — nothing else (PTY reads,
timers, other scripts) interleaves until it completes. Anything registered at
top level is live by the time `mb.loadScript` returns.

```ts
// GOOD — handler is registered before anything else runs
mb.addEventListener("paneCreated", onPane);
for (const tab of mb.tabs) for (const p of tab.panes) register(p);
```

```ts
// BAD — handler registration deferred; queries that race the applet load
// get "not registered" before setTimeout fires
setTimeout(() => mb.addEventListener("paneCreated", onPane), 0);
```

Same applies to `pane.addEventListener("osc:NNNN", ...)` — if the applet
intends to service an OSC, register the listener at top level *before* any
code that might yield. Top-level `await`, `setTimeout`, `fetch`-style promises,
or anything that hands control back to the event loop opens a window where
OSCs emitted by shells already running will not be delivered.

If async init is unavoidable, do it *after* registering handlers, and use
unsolicited announce (walk `mb.tabs` in a `paneCreated` listener and/or on
applet load) so late shells still discover the applet.

## Events are async, not synchronous

`addEventListener` callbacks fire as microtasks, not inline with the C++ side
that triggered them. Consequences:

- Multiple events that fire from one native operation may land in a predictable
  order but not interleave with other JS code the way inline callbacks would.
- Callbacks can freely call back into `mb.*` — no re-entrancy concerns.
- The object a callback receives may already have been torn down natively by
  the time the callback runs. The JS wrapper's `*.alive` state goes dead and
  methods become no-ops silently. Check `pane.hasPty` / related state if it
  matters.

The exception: input/output filters (`pane.addEventListener("input"/"output", fn)`)
are called **synchronously** from the PTY read path so they can mutate the
data stream. Keep them fast.

## Same-path reload is idempotent if nothing changed

Calling `mb.loadScript(path, perms)` when an instance is already loaded:

- **Content + perms identical** → no-op. Returns the existing instance id.
- **Content or perms changed** → unload old instance (full cleanup: timers,
  popups, overlays, filters, actions, WS servers), load fresh.

Practical implication: shells can re-trigger the same OSC 58237 on every
startup without churning the applet. Editing the applet on disk and re-loading
via command palette replaces it cleanly (hot-reload).

## Permissions

Declared at load time as a comma-separated string (`"ui,shell,net.listen.local"`).
Groups expand to all member bits. See `types/mb.d.ts` for the full list.

Key facts:

- The declared permission set + the script's content + its directory's `.js`
  files are all hashed into the allowlist entry. Change any one and the user
  is re-prompted.
- **Requesting fewer perms doesn't silently downgrade.** If an already-loaded
  instance has `shell,ui` and you reload requesting only `shell`, the existing
  instance's superset doesn't match exactly → full reload with the narrower
  set. This is by design.
- Unauthorized API calls throw a TypeError **and** schedule the script for
  termination on a zero-delay timer. Built-in scripts bypass all checks.

## Shell integration (OSC 133) and the `commandComplete` event

When the shell emits OSC 133 semantic-prompt markers (FinalTerm / Per Bothner
spec), MB assembles them into `MbCommand` records and fires a
`commandComplete` event per completed command.

Wire format shells should emit:

```
\e]133;A\e\\          start of prompt           (options: k=s/c for continuation, k=r for right)
\e]133;B\e\\          end of prompt / input begins
\e]133;C\e\\          end of input / output begins
\e]133;D;<exit>\e\\   command finished (exit code parsed and stored)
\e]133;N[;aid=...]\e\\  like A but closes the in-flight command
\e]133;P[;k=...]\e\\    explicit prompt start (optional after A/N)
\e]133;I\e\\          end of prompt / input begins (ends at EOL)
\e]133;L\e\\          fresh-line (emit \r\n if not at column 0)
```

MB records stable row IDs for each OSC 133 zone boundary — the command
object's `promptStart`, `commandStart`, `outputStart`, and `outputEnd` each
carry a `rowId` (plus convenience `absRow` at query time and `col`). A
`rowId` identifies the logical line, so it's stable across width-change
reflow. The `command` and `output` text fields are extracted lazily from the
document at query time — they are never truncated and reflect the live cell
content. The `cwd` field snaps the most recent OSC 7 value at `A` time.

```ts
pane.addEventListener("commandComplete", (cmd) => {
    if (cmd.exitCode !== 0) {
        shellWs.send(JSON.stringify({
            kind: "failed-command",
            command: cmd.command,   // extracted lazily, no size cap
            output: cmd.output,
            exitCode: cmd.exitCode,
            cwd: cmd.cwd,
        }));
    }
});
```

### Document query API

You can extract text from any row-id range, not just command zones:

```ts
// Get text from a command's full span (prompt + input + output)
const fullText = pane.getTextFromRows(
    cmd.promptStart.rowId, cmd.promptStart.col,
    cmd.outputEnd.rowId,   cmd.outputEnd.col);

// Get the row ID at a given screen row (null if out of range)
const rowId = pane.rowIdAt(0); // top of visible screen
```

Row IDs are stable monotonic integers identifying logical lines. They survive
scrolling AND width-change reflow — a soft-wrapped logical line keeps one ID
shared across all its physical rows. Use them to anchor a position in the
document and re-read text at any later time (as long as the line hasn't been
evicted from the archive).

**Permission required: `shell.commands`.** Reading `pane.lastCommand`,
iterating `pane.commands`, and subscribing to `commandComplete` all require
this bit. Command records frequently contain secrets (typed passwords, API
tokens in argv, file contents in output) — gate carefully. Declare it
alongside `shell.write` in your applet header if you need both.

**Collapse case** (shell rewrites a multi-line header/footer prompt down to a
single command line on Enter): if another `A` arrives before a `D` has
finalized the previous command, MB treats it as a relocation of the same
logical command — `promptStart` is updated, not a new record created. `N`
with a matching `aid` explicitly closes the in-flight command.

**Caveats**
- Rows evicted from the archive are gone; recent commands are always accessible.
- Exit code is parsed from `D;<n>` or `D;err=<v>` (non-empty `err` wins per spec).
- Alt-screen (e.g. vim, less) doesn't participate — its output isn't in the record.

## OSC handler routing

Applets register OSC listeners per-pane:

```ts
pane.addEventListener("osc:58237", (payload) => {
    // payload is the raw OSC body with leading "N;" stripped
});
```

Unhandled OSCs fall through to the script engine; if any instance has a
matching listener, it receives the payload. If none does, the OSC is discarded
(not an error).

Responses travel the opposite direction via `pane.write("\x1b]...\x1b\\")` —
bytes land on the shell's **stdin**. The shell must have an OSC parser on
stdin to interpret them. Standard shells don't; custom shells can.

## TTY is not a security boundary

Anything in a pane (child processes of the shell, pipes, etc.) can emit OSC
sequences and read the responses MB writes back. This is intrinsic to how
terminals work — not a bug. Tokens or other secrets that flow through a
PTY are only as protected as any other program running under that PTY.

For the `mb:ws` shell-integration pattern: the token granting access to the
WS server is exposed via PTY stdin. Any process in the pane can harvest it
and open a connection. Acceptable because all processes in the pane are
assumed to be running as the user and already have equivalent access.

## WebSocket servers (`mb:ws`)

Shared single `lws_context` per MB process, lazy-created on first
`createServer`. Each server is a new vhost bound to `127.0.0.1:port` (use
`port: 0` and read `server.port` back). Auth: the `token` IS the protocol
name (`mb-shell.<token>`) — clients must connect with
`Sec-WebSocket-Protocol: mb-shell.<token>`. lws's built-in matching rejects
mismatches during the handshake; no custom validation logic runs.

Lifecycle: a server's owning instance is the script that called `createServer`.
Script unload closes every server owned by that instance and fires `close`
events on live connections. Explicit `server.close()` does the same.

For the shell-integration pattern (shell ↔ MB via WS):

1. Applet loads at MB startup (via config) so it exists before any shell starts.
2. Applet calls `mb.createSecureToken()`, then `ws.createServer({host: "127.0.0.1", port: 0, token})`.
3. Applet registers `pane.addEventListener("osc:NNNN", ...)` on every existing pane *and* `mb.addEventListener("paneCreated", ...)` for future panes.
4. Handler writes `\x1b]NNNN;port=...;token=...\x1b\\` to the pane's PTY when the shell asks.

The shell parses unsolicited announces on stdin and re-handshakes if the
applet is reloaded (server port + token change).

## Imports

Two trusted sources for `import`:

- **Built-in modules**: `import ws from "mb:ws"`, `import fs from "mb:fs"`, `import tui from "mb:tui"`, etc.
- **Own directory**: relative imports under the script's own directory tree. No `../` escapes.

Anything else is rejected by the module loader.

## TypeScript

`types/mb.d.ts` at repo root. Reference via tsconfig `types` or a file-level
`/// <reference path="..." />`. Does NOT declare `console`, `setTimeout`,
etc. — those conflict with `lib.dom.d.ts`. Cover them in your project's
tsconfig `lib` or provide your own shim.

Transpile to ES2022+ JS before `mb.loadScript` picks it up. No bundler
required for single-file applets; multi-file applets can use any bundler
since `mb.loadScript` only sees the final .js.

## Cleanup on unload

`Engine::unload(id)` tears everything down for that instance:

- Timers (`setTimeout`/`setInterval`).
- Owned popups (`pane.createPopup`).
- Owned overlays (`tab.createOverlay`).
- I/O filters (input/output on pane and overlay).
- Registered actions (`mb.registerAction`).
- WS servers and all their connections (`mb:ws`).

What **isn't** cleaned up automatically: data the applet wrote to external
systems (files via `mb:fs`, OSC bytes already sent to shells). That's the
applet's responsibility if reversal is needed.

## Debugging

- `console.log`/`info`/`warn`/`error` route to spdlog (same log file as MB's
  other output). `mb --ctl logs` streams live.
- Permission violations log the denied permission name and terminate the
  script; look for `permission denied: <name>` in logs.
- `applet-loader.js` logs load/approve decisions — grep for `applet-loader:`.
- Script engine logs load/replace/unload at info level — grep for `ScriptEngine:`.
- Uncaught exceptions in event listeners are logged with the exception message
  but don't terminate the script. Check logs if events silently stop firing.

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

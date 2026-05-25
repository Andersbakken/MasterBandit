# Daemon / Frontend Attach Protocol (sketch)

Status: design sketch, not implemented. Drafted to evaluate scope and pin
down the protocol surface before any code lands.

## 1. Goals

- A headless `mb` daemon owns PTYs, terminal emulation, scrollback,
  scripting, layout tree, and config.
- One or more `mb` frontends attach over a transport, render the current
  state, deliver input, and detach cleanly. The daemon outlives any
  individual frontend.
- Multi-attach is **shared view**: all frontends see the same focused
  pane, the same selection, the same scroll position. A frontend is a
  window onto the session, not an independent client.
- Transport is abstracted. SHM-backed Unix-socket transport is the
  primary local transport; TCP (eventually TCP+TLS) is the remote
  transport. Protocol code is transport-agnostic.

## 2. Non-goals (v1)

- Per-frontend viewports, selections, or focus.
- Remote-only operation without local SHM optimization (TCP works, but
  bulk transfers are slower).
- Wire-format compatibility across builds. Strict version match in
  handshake; mismatch rejects the attach.
- Cell-delta encoding. Viewport is sent whole at up to ~120 Hz; daemon
  drops intermediate snapshots when the frontend falls behind.

## 3. Binary surface

Single binary, mode flag:

- `mb` — current behavior. In-process daemon + frontend, IPC bypassed
  via an in-proc transport.
- `mb --daemon [--socket PATH]` — daemon only. Default socket
  `$XDG_RUNTIME_DIR/mb/<sessionid>.sock` (Linux) /
  `$TMPDIR/mb-<sessionid>.sock` (macOS).
- `mb --attach [--socket PATH] [--tcp HOST:PORT]` — frontend only.
- `mb --attach` with no args picks the newest socket owned by the user
  matching the glob (same approach as `CLIClient.cpp:170` today).

In-proc mode uses a transport implementation that hands messages
directly between an in-process daemon thread and the frontend, with no
serialization (passes structs by move). Same protocol surface, same
message types, no syscalls. This keeps the single-process launch fast
and exercises the same control flow as out-of-proc.

## 4. Transport abstraction

```cpp
// src/attach/Transport.h
class Transport {
public:
    virtual ~Transport() = default;

    // Control / event messages. Bounded size (< 64 KiB typical).
    // Lossless, in-order, framed.
    virtual void send(MessageId id, std::span<const std::byte> payload) = 0;
    virtual std::optional<Message> recv(/* timeout */) = 0;

    // Bulk transfers. Caller hands ownership of a byte buffer or an
    // already-mapped SHM segment. Transport decides on the wire format.
    // BulkHandle is opaque on the recv side and resolves to bytes via
    // openBulk(); the transport may map an fd, copy from socket, or
    // hand back an internal pointer.
    virtual BulkHandle sendBulk(BulkPayload&&) = 0;
    virtual std::span<const std::byte> openBulk(const BulkHandle&) = 0;
    virtual void releaseBulk(const BulkHandle&) = 0;

    // Lifecycle.
    virtual void close() = 0;
    virtual bool isAlive() const = 0;
    virtual int pollFd() const = 0; // for EventLoop integration; -1 for in-proc
};
```

Implementations:

- `InProcTransport` — both ends in same process. `send` is a mutex +
  deque move; `sendBulk` just moves a `std::shared_ptr<const Bytes>`.
  `pollFd()` returns an eventfd for EventLoop wakeups.
- `UnixSocketTransport` — `SOCK_STREAM` over `AF_UNIX`. Frames are
  length-prefixed (4-byte LE length, 2-byte message id, payload).
  `sendBulk` uses `memfd_create` (Linux) / POSIX `shm_open` (macOS),
  `ftruncate`, `mmap`, then `SCM_RIGHTS` fd-pass. Receiver `mmap`s the
  fd, holds it for the BulkHandle's lifetime.
- `TcpTransport` — same framing, plus a TLS layer for non-loopback.
  `sendBulk` is just inline framed bytes (no fd-pass possible).
  Receiver materializes into a heap buffer.

The protocol layer always references `BulkHandle`. Whether that's an
mmap'd shm region or a heap-allocated buffer is the transport's
business. `openBulk` returns a `span` valid until `releaseBulk`.

### Bulk payload typing

Every `BulkPayload` carries a `BulkKind` so the transport can apply
content-aware optimizations (image RGBA → consider compression on TCP;
viewport cell array → never compress; raw bytes → opaque). Kinds:
`Viewport`, `ScrollbackRows`, `ImageRgba`, `Opaque`.

## 5. Session model

```
Daemon
├── Session (1)
│   ├── LayoutTree (authoritative)
│   ├── Panes: Uuid → Terminal (TerminalEmulator + PTY + state)
│   ├── Popups: Uuid → child Terminal (daemon-side, was script-driven)
│   ├── Embeddeds: lineId-anchored child Terminals
│   ├── Selection state: per pane (Uuid → SelectionAnchor)
│   ├── Scroll position: per pane (Uuid → row offset)
│   ├── Focused: Uuid (single)
│   ├── Config (loaded + hot-reload owner)
│   ├── ScriptEngine (QuickJS, all scripts)
│   └── AttachedFrontends: { FrontendId → SubscriberState }
└── EventLoop (epoll/kqueue, no Window)
```

One daemon, one session, N attached frontends. Multi-session in one
daemon is a future extension (would require splitting the singletons
listed above by session id, not in v1).

`SubscriberState` per frontend:

- transport handle
- last-acknowledged snapshot version per pane (for backpressure
  coalescing; daemon drops a queued snapshot if a newer one arrives
  before the frontend reads)
- pending RPC replies queue
- liveness timestamp (last activity, for keepalive)

## 6. Frontend model

```
Frontend
├── Transport (one)
├── LayoutTree (mirror, daemon-driven)
├── PaneView: Uuid → { lastSnapshot, GPU PaneRenderPrivate, local input state }
├── PopupView, EmbeddedView (snapshot-only, no Terminal instance)
├── ImageCache: hash → mmap'd SHM region or heap bytes (via BulkHandle)
├── ScrollbackCache: Uuid → bounded ring (viewport + N adjacent rows)
├── InputController (all state: drag, click, sequence, OSC 22 cursor cache)
├── Bindings table (daemon-pushed on config reload)
├── Window, RenderEngine, RenderThread, TextSystem
└── EventLoop (window-toolkit-bound: xcb / NSApp / wayland-future)
```

The frontend resolves keybindings locally for latency. Actions either
execute locally (cosmetic — scroll, copy, font size) or RPC daemon-side
(structural, PTY-bound, script-invoking). The daemon ships the current
binding table on config reload.

## 7. Message types

All messages are tagged with a `MessageId`. Two directions:
**daemon→frontend** (D2F) and **frontend→daemon** (F2D). Bulk handles
are sent in-band as opaque references; the receiver fetches via
`openBulk`.

### 7.1 Handshake

| Direction | Message            | Payload                                                                                |
|-----------|--------------------|----------------------------------------------------------------------------------------|
| F2D       | `Hello`            | `{ protocolVersion, buildId, capabilities[] }`                                         |
| D2F       | `HelloAck`         | `{ protocolVersion, buildId, sessionId, accepted: bool, reason? }`                     |

Strict version match: if `protocolVersion` or `buildId` mismatch, daemon
sends `HelloAck{accepted=false}` and closes. v1 has no negotiation.

### 7.2 Initial state push (on attach)

After `HelloAck{accepted=true}`, daemon pushes a state burst:

| D2F message       | Payload                                                                          |
|-------------------|----------------------------------------------------------------------------------|
| `ConfigState`     | Full `Config` (palette, font name+size, divider, key bindings, scrollback cap, …) |
| `LayoutFull`      | LayoutTree serialized — nodes, children, slots, focused Uuid                     |
| `PaneState[]`     | One per pane: Uuid, cols, rows, cursor, modes, palette overrides, title, icon, cwd, progress, OSC 133 command-ring head, decorations summary, image-ids in viewport |
| `ViewportSnapshot[]` | One per pane: bulk handle to viewport cell array, scroll offset, dirty mask = full |
| `PopupState[]`, `EmbeddedState[]` | If present                                                            |
| `ReadyToRender`   | Marker — frontend may now paint                                                  |

Wire-cost: viewport at 200×60 cells × 12 B = 144 KiB per pane, plus
small structured payloads. Bounded by pane count. No scrollback in this
burst.

### 7.3 Steady-state daemon→frontend

| Message                     | Trigger                                            | Payload                                                                       |
|-----------------------------|----------------------------------------------------|-------------------------------------------------------------------------------|
| `ViewportSnapshot`          | Per-pane, on state change only, rate-limited to ~120 Hz | `{ paneId, version, scrollOffset, bulk:viewportCells, dirtyRowsMask, cursor, syncOutputActive }` |
| `ScrollbackResponse`        | Reply to `ScrollbackRequest`                       | `{ requestId, paneId, startRow, rowCount, bulk:rows }`                        |
| `DecorationsDelta`          | Decoration add/clear                               | `{ paneId, ops[] }`                                                           |
| `ImagesDelta`               | Image add/del/animate; sends new hashes only       | `{ paneId, added: [{id, hash, bulk:rgba?}], removed: [id], placements: [...]}` |
| `ImageReleased`             | All panes dropped image with hash                  | `{ hash }` — frontend may evict from cache                                    |
| `LayoutDelta`               | Pane created/destroyed/split/swapped, focus change | `{ ops[] }`                                                                   |
| `PaneStateDelta`            | Title/icon/cwd/progress/cursor-style/mode change   | `{ paneId, fields[] }`                                                        |
| `Selection`                 | Selection start/extend/clear                       | `{ paneId, anchor, extent }`                                                  |
| `BellEvent`                 | OSC 9 / BEL                                        | `{ paneId, kind }`                                                            |
| `NotificationRequest`       | OSC 99                                             | `{ paneId, id, title, body, urgency, buttons[], icon }`                       |
| `ClipboardWrite`            | OSC 52 set, `mb.clipboard.write` from JS           | `{ target: clipboard|primary, bulk?: bytes, inline?: string }`                |
| `ClipboardReadRequest`      | OSC 52 get                                         | `{ requestId, target, paneId }`                                               |
| `WindowAction`              | `Window::raise`, title set, urgency hint, dock badge | `{ kind, args }`                                                            |
| `OpenUrl`                   | `platformOpenURL` (hyperlink click reached daemon, applet, etc.) | `{ url }`                                                       |
| `BindingsChanged`           | Config reload, `addKeybinding` from JS             | full or delta binding table                                                   |
| `ConfigDelta`               | Frontend-facing config fields changed              | `{ fields[] }`                                                                |
| `Pong`                      | Reply to `Ping`                                    | `{ nonce, daemonClock }`                                                      |
| `Goodbye`                   | Daemon shutting down or evicting this frontend     | `{ reason }`                                                                  |

### 7.4 Steady-state frontend→daemon

| Message                   | Payload                                                                        |
|---------------------------|--------------------------------------------------------------------------------|
| `Key`                     | `{ paneId, keysym, mods, text, repeat, isPress }`                              |
| `Mouse`                   | `{ paneId, kind: press/release/motion/wheel, button, mods, cellX, cellY, pxX, pxY }` |
| `Paste`                   | `{ paneId, bulk:bytes, bracketed }`                                            |
| `Resize`                  | `{ paneId, cols, rows, pixelW, pixelH }` (TIOCSWINSZ on daemon side)           |
| `Scroll`                  | `{ paneId, deltaRows | absoluteRow }`                                          |
| `ScrollbackRequest`       | `{ requestId, paneId, startRow, rowCount }`                                    |
| `ActionInvoke`            | `{ action: enum or string, args }` — for daemon-side actions                   |
| `SelectionSet`            | `{ paneId, anchor, extent, kind: char/word/line }` — frontend commits selection drag |
| `ClipboardReadResponse`   | `{ requestId, bulk?:bytes, inline?:string, denied }`                           |
| `WindowFocusEvent`        | `{ focused }` (frontend gained/lost OS focus) — affects cursor-blink, hover    |
| `NotificationEvent`       | `{ id, action: closed/clicked/buttonId }` — relays Linux DBus / macOS UNUser callbacks back |
| `Ping`                    | `{ nonce, frontendClock }`                                                     |
| `Detach`                  | `{ reason }`                                                                   |

## 8. Snapshot delivery & backpressure

A `ViewportSnapshot` is produced **only when pane state actually
changed**, capped at ~120 Hz. Idle panes produce zero snapshots; a
session with no active output and no input is silent on the wire.
This matches today's in-process `TerminalEmulator::injectData`
behaviour (`TerminalEmulator.h:970–998`).

Things that produce a new snapshot:

- Parser mutated the grid, modes, cursor, palette, selection, or
  decorations.
- Daemon-side scroll position changed (frontend `Scroll` RPC, JS
  scroll action, search jump-to-match).
- Frontend `SelectionSet` updated the selection.
- An image animation frame advanced (`tickAnimations`, daemon-side
  post-split).
- A mode change toggled visual state (alt-screen switch, sync-output
  begin/end).

Things that do **not** produce a snapshot:

- Cursor blink edges. Blink is a frontend-side render concern; the
  snapshot carries `cursor.style` + `cursor.blinkMode` + the cursor
  position, and the frontend animates the visibility locally between
  snapshots.
- Frontend window focus gain/loss (cursor render flips between
  filled/hollow; handled frontend-side from `WindowFocusEvent` state).
- Frontend hover-over-hyperlink (renders an underline decoration
  locally; the daemon already publishes hyperlink ids in the cell
  data).
- Selection drag motion. The frontend tracks the in-progress drag
  locally and renders the in-progress selection rectangle; it sends
  `SelectionSet` only at meaningful boundaries (drag-start, throttled
  intermediate commits if the daemon-side selection model needs them
  for, e.g., scroll-on-edge auto-scroll, and drag-release).

Backpressure: per attached frontend the daemon holds at most one
pending snapshot per pane in the send queue. A newer snapshot replaces
the pending one (last-write-wins for snapshots; events/RPC replies are
reliable and queued separately).

Implementation: each `SubscriberState` has
`map<Uuid, ViewportSnapshotMsg> pendingViewport_`. The daemon's send
loop drains both queues — pending viewports map and the reliable
message queue — in interleaved fashion, draining the reliable queue
fully between viewport publishes for a given pane.

A slow frontend therefore sees lower frame rate but never stale
out-of-order state. The parse worker is never blocked by a slow
frontend.

## 9. Scrollback transport

Frontend keeps a local ring per pane: viewport rows plus a bounded
window of adjacent rows (e.g. ±1000) for instant micro-scroll. Beyond
that:

1. Frontend issues `ScrollbackRequest{requestId, paneId, startRow:
   absRow, rowCount: N}`.
2. Daemon serializes rows N..M from `LineBuffer` into a bulk payload,
   replies with `ScrollbackResponse{requestId, paneId, startRow,
   rowCount, bulk}`. Bulk is a packed array of cells plus a small
   per-row `CellExtra` index (only rows with non-empty `CellExtra`
   carry extra bytes).
3. Frontend appends to ring, repaints.

A multi-attach scroll-up by frontend A also sends `Scroll{paneId,
absoluteRow}` to the daemon; daemon updates the global scroll position
and pushes a new `ViewportSnapshot` to all attached frontends from the
new offset. Frontend B sees the same scrolled view.

Search across full scrollback is daemon-side: a future
`ScrollbackSearchRequest` returns matches with absolute row positions;
decorations get pushed via `DecorationsDelta`. (Sketch only;
out-of-scope for v1.)

### 9.1 Cell wire format

Cells are `sizeof(Cell) == 12` bytes (`src/terminal/CellTypes.h:223`).
The bulk payload is a packed `cols * rows * 12` byte array, row-major.
A separate `CellExtraIndex` block follows: `vector<{row, col,
CellExtra}>` for cells with non-default extras (image refs, hyperlink
ids, grapheme continuations). Default-zero `CellExtra` is implicit.

For 80×24 viewport: 23 KiB cells + tiny extras index.
For 200×60 viewport: 144 KiB cells + tiny extras index.

Per-frame at 60 Hz that's 1.4–8.6 MiB/s per pane uncompressed — fine
for SHM (zero-copy), tolerable for loopback TCP, painful for remote.
Remote attach is a v2 problem; document and move on.

## 10. Image transport

Daemon assigns each image a content hash (e.g. xxh3-64 of the RGBA
bytes plus dimensions). The frontend maintains an `ImageCache: hash →
BulkHandle`.

Flow on first sight of an image in a viewport:

1. Daemon checks `frontend.knownImageHashes_.contains(hash)`. If not:
2. Daemon includes `ImagesDelta{ added: [{id, hash, bulk: rgba}] }` in
   the message stream.
3. Frontend opens the bulk handle (mmap for SHM, heap for TCP), caches
   by hash, marks `knownImageHashes_` daemon-side via implicit ack
   (next viewport snapshot referencing the id is the ack).

On animation: each frame is its own RGBA buffer with its own hash. The
`AnimationState` ships per-frame hash references; the frontend cycles
through cached handles per frame.

On image deletion (`d=`/`d=I`): when no pane references the hash on the
daemon side, daemon sends `ImageReleased{hash}`. Frontend may evict.
Eviction is advisory; frontend cache also LRU-evicts to a size cap.

Wire-cost reality: a 1080×1920 RGBA image is ~7.9 MiB. SHM transport
passes one fd; TCP transport ships 7.9 MiB. Animated kitty stickers
with 20 frames at full resolution are ~160 MiB. SHM hands over 20 fds;
TCP would saturate the link. The transport abstraction lets the
protocol stay the same; remote-attach image performance is a known
weakness for v1, addressable later via per-frame zstd or downscaling
on transport.

## 11. Input

Frontend captures keys/mouse via its `Window` and processes them
through its local `InputController` (selection drag, click detector,
sequence matcher, OSC 22 cursor cache, auto-scroll timer all live
here). Binding match is local against the daemon-pushed table.

Resolved outcomes flow daemon-side as:

- `Key { paneId, keysym, mods, text, repeat, isPress }` — daemon's
  `Terminal::keyPressEvent` runs on the corresponding `TerminalEmulator`,
  writes encoded bytes to PTY.
- `Mouse { ... }` — same path; daemon's mouse-mode state decides
  whether to write SGR mouse bytes to PTY, fire script listeners, or
  do nothing.
- `Paste { bulk, bracketed }` — daemon writes paste, with bracketing
  if mode set.
- `Scroll { absoluteRow | deltaRows }` — daemon updates pane scroll
  offset, publishes new snapshots.
- `SelectionSet { anchor, extent, kind }` — daemon updates the pane's
  `Selection`, publishes snapshot delta. Selection is global; all
  attached frontends see it.
- `ActionInvoke { action, args }` — daemon's `ActionRouter` executes.
  For actions resolvable purely frontend-side (font-size, full-screen
  toggle), the frontend executes locally and does not send.

Action ownership table (excerpt):

| Action                | Side       |
|-----------------------|------------|
| SendString, SendKey   | Daemon     |
| Scroll*               | Daemon (publishes new snapshot to all)  |
| SelectionCopy         | Daemon reads selection bytes, sends ClipboardWrite |
| Paste                 | Frontend collects bytes (clipboard read), sends `Paste` |
| NewTab, SplitPane, ClosePane, FocusPane, SwapPanes | Daemon (LayoutTree mutation) |
| ToggleFullscreen, FontSizeAdjust | Frontend  |
| InvokeJsAction        | Daemon (JS) |
| AnyCustom (`mb.actions.register`) | Daemon (JS) |

## 12. Layout authority

Daemon owns `LayoutTree`. Frontend mirrors via `LayoutDelta` ops:

```
op AddNode    { parentId, slot, nodeKind, uuid, params }
op RemoveNode { uuid }
op MoveNode   { uuid, newParentId, newSlot }
op SetFocused { uuid }
op SetStackActive { stackUuid, activeChildUuid }
op SetSplitRatio  { containerUuid, ratio }
```

Frontend-originated mutations (keybinding for SplitPane, drag-to-move
pane) issue `ActionInvoke`; daemon mutates, broadcasts `LayoutDelta`.
Frontend repaints on receipt. No frontend-authoritative mutation —
single source of truth.

## 13. Window resize

The pane grid size is a property of the rendered viewport, which
depends on font metrics (frontend-side). On window resize:

1. Frontend computes new cols×rows per pane from the new pixel size +
   font metrics + layout split ratios (mirrored from daemon).
2. Frontend sends `Resize{paneId, cols, rows, pixelW, pixelH}` per
   affected pane.
3. Daemon runs `Terminal::resize(cols, rows)` (TIOCSWINSZ + emulator
   resize + reflow). Publishes new `ViewportSnapshot` at the new
   geometry.

Multi-attach hazard: two frontends with different window sizes/font
sizes compute different cell counts. Possible policies:

- (v1) **Smallest attached frontend wins.** Daemon picks the min
  cols/rows across all attached frontends per pane. Larger frontends
  letterbox or up-scale. Simple, never overflows.
- (future) Per-frontend rendering with per-frontend cell grid — would
  require per-frontend snapshots at per-frontend dimensions, which
  breaks the shared-view simplicity. Out of scope.

## 14. Detach detection & reconnect

- Clean detach: frontend sends `Detach{reason}`, then closes the
  transport.
- Unclean detach: transport `recv` returns EOF / error / keepalive
  timeout. Daemon removes the frontend from `attachedFrontends_`,
  releases any bulk handles owned by that frontend, drops queued
  messages.
- Periodic `Ping`/`Pong` every N seconds (e.g. 10 s) detects half-open
  sockets on TCP. Local Unix socket relies on EOF.
- Session survives. State is intact. A subsequent `mb --attach` reruns
  the handshake and initial state push (§7.2).

Daemon shutdown: `mb --daemon` with no attached frontend continues to
run. Explicit kill (`mb --kill-daemon` or signal) sends `Goodbye{reason}`
to all attached frontends, closes their transports, exits.

## 15. Scripting on the daemon

`ScriptEngine` lives entirely daemon-side. `Engine::terminals_`
continues to own `unique_ptr<Terminal>`. Output/input/OSC filters keep
their synchronous parse-time semantics (no IPC hops). Popups and
embedded terminals are created daemon-side as full child
`TerminalEmulator`s and ship snapshots to the frontend like any other
pane.

JS surface changes from in-proc:

- `mb.window.raise()`, `mb.window.setTitle()`, `mb.clipboard.read()`,
  `mb.clipboard.write()`, notification APIs, `platformOpenURL` — all
  become daemon→frontend RPCs internally. Async semantics where they
  weren't before. `mb.clipboard.read()` already had a `Promise`-shaped
  API (per types/mb.d.ts), so this is invisible. `mb.window.raise()`
  becomes fire-and-forget.
- `popup.create({ rows, cols, ... })`, `pane.createEmbeddedTerminal()`
  unchanged in JS — daemon-side it still instantiates a child
  Terminal. Rendering goes via the snapshot stream.
- Mouse listeners (`pane.addEventListener("mouse", fn)`): the frontend
  ships mouse events to the daemon; if no listener is registered for a
  pane (the same `paneMouseFlag_` short-circuit that exists today),
  the daemon doesn't post to JS. If a listener is registered, JS fires
  daemon-side. The handler may call `pane.inject` / structural
  mutations — all local to the daemon.

The result: the JS API surface is unchanged from a script author's
perspective. Frontend gains no scripting capability; it is a renderer.

## 16. Action queue & concurrency

All structural mutations (LayoutTree, ScriptEngine state, config) run
on a single daemon thread — the existing "main" thread in `PlatformDawn`,
renamed daemon-thread. Sources of mutation:

- Frontend RPCs (`ActionInvoke`, `Resize`, `Scroll`, `SelectionSet`,
  `Key`, `Mouse`, `Paste`).
- JS callbacks (mouse listeners, OSC handlers, action handlers).
- Config file watcher reload.
- Parser callbacks delivered via `eventLoop_->post`.

All converge on the daemon EventLoop's posted-task queue. Single-writer
discipline on LayoutTree and ScriptEngine. No locks beyond the existing
`mMutex` between parser and render-snapshot capture (which now lives
daemon-side too — `TerminalSnapshot` is built daemon-side and
serialized over the transport, not handed to a render thread).

## 17. Render thread on the frontend

The frontend retains its existing `RenderThread` / `RenderEngine` /
`PaneRenderPrivate` structure. Source of truth for `RenderFrameState`
is no longer a daemon-thread shadow copy — it's reconstructed from
incoming `LayoutDelta` + `ViewportSnapshot` + `PaneStateDelta`
messages. The shape of `RenderFrameState` is unchanged; the producer is
the message handler instead of `applyPendingMutations`.

Per-pane GPU resources (`PaneRenderPrivate`) are created on first
`LayoutDelta:AddNode(terminal)` and destroyed on `RemoveNode`.

## 18. Persistence & restart

Out of scope for v1. Daemon state is in-memory. Daemon process death =
session loss. A future addition could snapshot LineBuffer / mode /
palette to disk periodically and restore on daemon restart; the
protocol does not require it.

## 19. Open questions for implementation phase

These are decisions deferred until the protocol is being implemented,
not now:

- Wire encoding: hand-rolled struct packing vs flatbuffers vs
  capnproto. Hand-rolled is simplest and matches the rest of the
  codebase; flatbuffers gains zero-copy reads on TCP. Recommend
  hand-rolled for v1 to keep dependencies flat.
- TLS dependency. mbedTLS is already in some Dawn paths; check before
  adding. Wait until TCP attach is actually being implemented.
- macOS event loop in headless daemon mode. Verify whether a daemon
  needs `NSApp` for anything (probably not — no Cocoa surfaces, no
  pasteboard ownership). If not, daemon uses plain `kqueue` loop and
  skips `NSApp` entirely.
- Selection-on-multi-attach race: two frontends drag at the same time.
  v1 policy: last `SelectionSet` wins. Document and move on.
- Sound / bell. OSC 9 → `BellEvent` → all frontends play their own
  bell. Or daemon picks one frontend. Open.
- Per-frontend window-title from the same pane title. The pane title
  is global; each frontend may want its own decoration. Out of scope —
  v1 sets all frontends' titles to the focused pane title.

## 20. Scope estimate

Counting touch points:

- New: `src/attach/Transport.h` + `InProc.{h,cpp}` + `UnixSocket.{h,cpp}`
  + `Tcp.{h,cpp}` + `Message.{h,cpp}` (codecs) + `DaemonServer.{h,cpp}`
  + `FrontendClient.{h,cpp}`.
- Modified: `main.cpp` (mode flag dispatch), `PlatformDawn` (split into
  `DaemonCore` and `FrontendShell` — the existing seven subsystems
  remain, but `TabManager` / `ActionRouter` / `ConfigLoader` /
  `ScriptEngine` move daemon-side; `RenderEngine` / `RenderThread` /
  `InputController` move frontend-side; `AnimationScheduler` splits —
  cursor blink frontend, image animation daemon).
- `Terminal` callbacks (`Platform_Tabs.cpp:1043–1276`): each callback
  that currently `eventLoop_->post`s to the main thread now translates
  to a daemon→frontend message. The current indirection is the natural
  insertion point.
- `RenderEngine.cpp:1442, 1481, 1539, 1546`: replace raw `term`
  dereferences with snapshot fields (cols/rows stamped on snapshot,
  `tickAnimations` moves daemon-side, `loadSnapshot` becomes
  "latest received snapshot" from the message handler).
- `ScriptEngine.cpp:2141 resolveEmulatorFromVal` and all
  `Platform_EventLoop.cpp` script callbacks: unchanged (scripts and
  Terminal instances are colocated daemon-side).

Estimated LOC, very rough: 5–8 KLOC new (transport + protocol +
client/server), 2–3 KLOC modified (PlatformDawn split, callback
re-routing). Touches every subsystem at least lightly. Rendering tests
need a way to inject snapshots without a real daemon — likely an
`InProcTransport` plus a synthetic-snapshot helper.

## 21. Phasing

A reasonable implementation order, not committed to:

1. Define `Transport`, `Message`, codecs. Implement `InProcTransport`.
   Wire it into a no-op pass-through so daemon and frontend live in
   the same process but communicate over the transport.
2. Move `Engine::terminals_` and `ScriptEngine` notionally to the
   daemon side; refactor render thread's raw `term` reads to use only
   snapshot fields (cols/rows on snapshot; move `tickAnimations`).
3. Split `PlatformDawn` into `DaemonCore` + `FrontendShell`.
4. Implement `UnixSocketTransport` with SHM bulk channel. `mb --daemon`
   / `mb --attach`.
5. Detach/reattach handshake + initial state push.
6. Multi-attach: shared view; replicate viewport push to N frontends.
7. `TcpTransport`. Defer TLS to a separate phase.
8. Scrollback search RPC. Image content-hash dedup. Compression toggle
   for remote attach.

Steps 1–3 are the largest and yield no user-visible change. They are
prerequisites for any out-of-process work to be testable.

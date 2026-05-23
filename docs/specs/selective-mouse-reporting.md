# Selective Mouse Reporting

A terminal-emulator protocol extension for fine-grained control over which
mouse events are forwarded to the application.

**Status:** draft v0.1
**Slot:** `CSI = w` (set), `CSI ? w` (query)
**Origin:** MasterBandit

## Motivation

The existing xterm mouse-reporting modes (`?1000`, `?1002`, `?1003`) are
all-or-nothing: enabling any of them causes the terminal to forward every
press, release, motion, and wheel event to the application, which then has
to ignore what it doesn't want. This is fine for full-screen TUIs that
implement their own selection (vim, tmux, emacs), but a poor fit for TUIs
that want a narrow subset — e.g. "the wheel for repaginating, but let the
terminal handle clicks for native text selection."

The conventional workaround is to require Shift-modified drags for
selection. That works but burdens the user and is not discoverable.

This extension lets the application declare *exactly* which buttons and
event types it wants reported. The terminal handles everything else
locally — selection, scrolling, link clicks, etc. — as if no mouse
reporting were enabled.

## Sequences

### Set state

```
CSI = <button_mask> ; <event_mask> w
```

Both parameters are decimal integers. Either may be omitted (defaults to
`0`). `CSI = 0 ; 0 w` (equivalently `CSI = w`) disables the protocol.

### Query state

```
CSI ? w
```

The terminal replies with:

```
CSI ? <button_mask> ; <event_mask> w
```

with the current values. A terminal that does not implement the protocol
sends no reply.

## Bit definitions

### Button mask (16-bit field, 11 defined)

Each bit corresponds to a button code as encoded by the xterm mouse-reporting
protocol.

| Bit (hex) | Button code | Name |
|----------:|------------:|------|
| `0x0001`  | 0   | Left press      |
| `0x0002`  | 1   | Middle press    |
| `0x0004`  | 2   | Right press     |
| `0x0008`  | 64  | Wheel up        |
| `0x0010`  | 65  | Wheel down      |
| `0x0020`  | 66  | Wheel left      |
| `0x0040`  | 67  | Wheel right     |
| `0x0080`  | 128 | Button 8 (back) |
| `0x0100`  | 129 | Button 9 (forward) |
| `0x0200`  | 130 | Button 10       |
| `0x0400`  | 131 | Button 11       |

Bits `0x0800`–`0x8000` are reserved for future xterm extensions; terminals
MUST ignore them on input and MUST clear them on output.

### Event mask (4-bit field)

| Bit | Event |
|----:|-------|
| `0x1` | Press   |
| `0x2` | Release |
| `0x4` | Motion (any-motion, like `?1003`) |
| `0x8` | Drag (motion-while-held, like `?1002`) |

An event is reported only if both (a) its source button has a set bit in
the button mask, and (b) its event type has a set bit in the event mask.

## Wire encoding

Events are reported using the existing SGR mouse-reporting format
(`CSI < Cb ; Cx ; Cy M` for press, `m` for release). If `?1016` is also
enabled, the SGR-Pixels encoding is used instead. No new wire format is
introduced — consumers that already parse SGR mouse reports get this for
free.

## Precedence

If any of `?1000`, `?1002`, or `?1003` is enabled, the legacy protocol
fully governs reporting and the masks defined here are ignored. The
extension activates only when no legacy mouse-tracking mode is set.

Rationale: applications that opt into the legacy modes already declared
"send me everything"; honoring a narrower mask would surprise them.

## State scope

Each of the main and alternate screens maintains an independent pair of
masks. Entry into the alternate screen (`?1049h`) inherits the current
masks from the main screen, matching the convention used by `?1000`/
`?1002`/`?1003`. Exiting the alternate screen restores the main screen's
unchanged state.

## Reset behavior

- **RIS** (`ESC c`) — both masks cleared to `0`.
- **DECSTR** (`CSI ! p`) — both masks preserved. (Matches the
  VT510/xterm convention for mouse-tracking modes.)

## Feature detection

The protocol does not register a DECSET mode and therefore is not
discoverable through DECRQM. Use the standard probe-and-barrier pattern
established by the kitty keyboard protocol:

1. Send `CSI ? w` (query).
2. Send `CSI c` (DA1, primary device attributes).
3. Read replies. If a `CSI ? <bmask> ; <emask> w` arrives before the DA1
   reply, the terminal supports the protocol. If only the DA1 reply
   arrives, it does not.

DA1 is universally implemented, so it serves as a synchronization
barrier: once the DA1 reply has been received, any selective-mouse-reporting
reply that was going to come has already come.

## Worked example: hydra wheel-only

A TUI that wants wheel-up and wheel-down events forwarded (so it can
repaginate or scroll its own content), but wants the terminal to handle
all click and drag operations natively for text selection:

```
button_mask = 0x0008 | 0x0010 = 0x18  (wheel up + wheel down)
event_mask  = 0x1                     (press only — wheels never release)
```

On startup, after probing for support:

```
CSI = 24 ; 1 w
```

On exit:

```
CSI = 0 ; 0 w
```

The wheel events will arrive as standard SGR reports:

```
CSI < 64 ; <col> ; <row> M    # wheel up
CSI < 65 ; <col> ; <row> M    # wheel down
```

Clicks, drags, motion, and the back/forward buttons are not reported and
remain available for the terminal's native selection, scrollback, and
link-click handling.

## Interaction with other modes

| Mode | Interaction |
|------|-------------|
| `?1000`, `?1002`, `?1003` | Legacy wins. Selective masks ignored while any is enabled. |
| `?1006` (SGR encoding) | Format choice for selective-mode reports follows `?1006` as normal. |
| `?1016` (SGR-Pixels encoding) | Same — `?1016` swaps cell coordinates for pixel coordinates. |
| `?1004` (focus reporting) | Independent. |
| `?2004` (bracketed paste) | Independent. |
| Shift-modified press | Shift bypasses reporting (terminal does selection), same convention as legacy modes. |

## Non-goals

- **No push/pop / stack semantics.** Applications are expected to issue an
  explicit `CSI = 0 ; 0 w` on exit, matching the cleanup pattern for
  `?1000` and friends. Adding a stack would double the protocol surface
  without a clear second use case.
- **No new wire format.** This is purely a filter over events that the
  existing xterm protocol already encodes. Anything xterm cannot encode
  (button 12+, sub-cell wheel deltas, etc.) is out of scope.
- **No application-side coordinate transforms.** The mask filters; it
  does not modify reported coordinates, encodings, or button codes.

## Implementation notes

A reference implementation in MasterBandit consists of:

- Two `uint16_t` / `uint8_t` fields per `TerminalState` (main and alt).
- A new CSI dispatch case for final byte `w` with `=` and `?`
  intermediates, parsing two numeric parameters with defaults.
- A predicate in `mousePressEvent` / `mouseReleaseEvent` /
  `mouseMoveEvent` that gates emission on the masks (only when legacy
  modes are off).
- An atomic mirror of "is the selective mode currently doing anything" so
  off-thread callers (e.g. the wheel router in the platform layer) can
  read it without locking.

The total addition is roughly 200 lines across four files plus tests.

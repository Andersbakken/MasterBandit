# TODO

## Terminal Protocol Support

- [ ] Mode 2031 — Color preference notification (light/dark mode). Send `CSI ? 997 ; 1/2 n` on macOS appearance change. Apps query via `CSI ? 996 n`.
- [ ] Kitty graphics protocol — APC-based image protocol. Chunked transfer, persistent image IDs, virtual placement, z-layering, animation.
- [ ] DECSCUSR (`CSI Ps SP q`) — Set cursor style: block, underline, bar, blinking variants. Currently silently ignored.
- [ ] OSC 7 — Current working directory reporting. Shell sends CWD for new tab/split support.
- [ ] OSC 8 — Hyperlinks. `OSC 8 ; params ; url ST`. Clickable links from ls, gcc, grep, cargo, gh, etc.
- [ ] OSC 10/11/12 — Query/set default fg/bg/cursor colors. Apps query to detect light vs dark theme.
- [ ] OSC 22 — Set mouse cursor shape (pointer, text, etc.)
- [ ] OSC 133 — Shell integration prompt/command/output markers. Smart scrolling between prompts, command re-run.
- [ ] Sixel graphics — DEC-era raster image protocol. Broad legacy tool support.
- [ ] REP (`CSI b`) — Repeat preceding character N times.
- [ ] Cursor blink (`CSI ? 12 h/l`) — Toggle cursor blinking.

## Multi-Tab / Multi-Pane

- [ ] Pane splits — keyboard shortcuts to split current pane horizontally/vertically.
- [ ] Pane focus navigation — keyboard shortcuts to move focus between panes.
- [ ] Pane resize — drag split dividers or keyboard shortcuts to adjust split ratio.
- [ ] Pane close — keyboard shortcut to close focused pane (currently only tabs are closeable via middle-click).
- [ ] Zoom — keyboard shortcut to zoom focused pane to fill the window (Layout::zoomPane is implemented, needs keybinding).
- [ ] Full-screen overlays — Tab::pushOverlay / popOverlay are implemented; need a way to trigger (e.g. Cmd+Shift+Enter kitty-style).
- [ ] Popup panes (OSC 999) — Pane::handleOSCMB and TerminalEmulator dispatch are implemented; needs end-to-end testing.
- [ ] Tab bar: tab title from OSC 0/2 is wired but tab titles display as empty until shell sets them.
- [ ] Tab bar: cursor-blink optimization — re-render to held texture when only cursor changes (500 ms interval guarantees GPU completion).
- [ ] Tab bar: color packing uses BGR order (parseHexColor packs R<<16|G<<8|B but shader reads R from bits 0-7). Colors are visually incorrect (R/B swapped) — fix to match packFgAsU32 byte order.
- [ ] Tab bar: configurable keybindings for new tab (Ctrl+Shift+T) and tab close.
- [ ] Non-powerline tab bar styles (fade, slant, separator, round) from config.

## Configuration

- [ ] Color scheme — `[colors]` config section for ANSI palette, foreground, background, cursor.
- [ ] Keybindings — configurable key mappings for tab/pane operations.
- [ ] Cursor style — block/underline/bar, blink on/off, blink interval.

## Infrastructure

- [ ] Split PlatformDawn.cpp — it's grown large; separate platform/event loop, font loading, cell resolution into own files.
- [ ] Log level — currently defaulting to Error; `-v` flags lower it. Debug level enables pool allocation logs.
- [ ] macOS font fallback — CoreText two-pass strategy implemented; parity with Linux fontconfig pass needs verification.

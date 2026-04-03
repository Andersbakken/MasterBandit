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

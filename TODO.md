# TODO

## Terminal Protocol Support

- [x] Mode 2031 — Color preference notification (light/dark mode).
- [x] DECSCUSR (`CSI Ps SP q`) — Set cursor style: block, underline, bar, blinking variants.
- [x] Kitty keyboard protocol (`CSI > Ps u`) — Unambiguous key encoding. Distinguishes ESC from Alt+[, Ctrl+I from Tab, reports key release, supports non-Latin layouts. Used by Neovim, Vim, crossterm, textual, Ink.
- [ ] Kitty graphics protocol — APC-based image protocol. Chunked transfer, persistent image IDs, virtual placement, z-layering, animation.
- [x] Underline styles (`CSI 4:N m`) — Curly, dotted, dashed, double underlines + colored underlines (`CSI 58;...m`).
- [x] OSC 7 — Current working directory reporting. Pane stores CWD for new splits.
- [x] OSC 8 — Hyperlinks. Cmd/Ctrl+click opens URL. Auto-underlines linked text.
- [ ] OSC 10/11/12 — Query/set default fg/bg/cursor colors. Apps query to detect light vs dark theme.
- [ ] OSC 22 — Set mouse cursor shape (pointer, text, etc.)
- [x] OSC 99 — Desktop notifications (kitty). Title/body accumulation, macOS UNUserNotification.
- [x] OSC 10/11/12 — Query/set default fg/bg/cursor colors. Responses in `rgb:RRRR/GGGG/BBBB` format.
- [x] OSC 133 — Shell integration prompt/command/output markers. Stored per-row. Used for jump-to-prompt and command output selection.
- [x] REP (`CSI b`) — Repeat preceding character N times.
- [x] Mode 2026 — Synchronized output. Defers rendering while active so intermediate states aren't shown.
- [ ] Color stack (OSC 30001/30101) — Push/pop entire color state. Apps can safely change colors and restore.
- [ ] Sixel graphics — DEC-era raster image protocol. Broad legacy tool support.
- [ ] Cursor blink (`CSI ? 12 h/l`) — Toggle cursor blinking.

## Multi-Tab / Multi-Pane

- [x] Pane splits — Ctrl+Shift+E (right) / Ctrl+Shift+O (down); Left/Up variants also supported.
- [x] Pane focus navigation — Ctrl+Shift+Arrows (spatial), Ctrl+Shift+N/P (cyclic). Focus-in/out sequences sent to terminals (mode 1004).
- [x] Pane resize — AdjustPaneSize action adjusts split ratio by cell increments.
- [x] Pane close — Ctrl+Shift+W; focus transfers to sibling. Shell exit (`exit`) also closes pane/tab gracefully.
- [x] Zoom — Ctrl+Shift+Z toggles zoom on focused pane.
- [x] Pane dividers — colored separator rendered between panes via GPU rect pipeline. Color and width configurable (`divider_color`, `divider_width`). Divider buffers are per-pane persistent GPU buffers; inactive tabs release textures and divider buffers on switch.
- [x] Per-pane title tracking — each pane stores its own OSC title/icon; tab bar shows the focused pane's title and updates on focus change.
- [x] Cursor rendering — solid cursor on focused pane, hollow outline on unfocused panes. Cursor type and position passed as UBO params to compute shader (no cell mutation).
- [x] PTY exit handling — `terminalExited` closes pane, or tab if last pane, or quits if last tab.
- [ ] Pane resize — drag split dividers with mouse (needs mouse binding system first).
- [ ] Pane swap/rotate — swap focused pane with another, rotate panes clockwise/counterclockwise.
- [ ] Move pane to new tab or new window.
- [ ] Full-screen overlays — Tab::pushOverlay / popOverlay are implemented; need a way to trigger (e.g. Cmd+Shift+Enter kitty-style).
- [ ] Popup panes (OSC 999) — Pane::handleOSCMB and TerminalEmulator dispatch are implemented; needs end-to-end testing.
- [ ] Tab bar: cursor-blink optimization — re-render to held texture when only cursor changes (500 ms interval guarantees GPU completion).
- [ ] Tab bar: color packing uses BGR order (parseHexColor packs R<<16|G<<8|B but shader reads R from bits 0-7). Colors are visually incorrect (R/B swapped) — fix to match packFgAsU32 byte order.
- [x] Tab bar: configurable keybindings for new tab (Ctrl+Shift+T) and tab close.
- [ ] Non-powerline tab bar styles (fade, slant, separator, round) from config.
- [ ] Per-tab color rules — color tabs based on title, active process, or index. Requires foreground process tracking and a rule matching engine in the tab bar renderer. Config via `[[tab_bar.color_rules]]` with match patterns (e.g. `title:*ssh*`, `process:python`, `index:0`).
- [ ] Foreground process tracking — poll `tcgetpgrp()` on the master PTY fd to detect the foreground process group, then look up the process name (`proc_pidpath` on macOS, `/proc/<pid>/cmdline` on Linux). Update tab title when the foreground process changes (like iTerm2/kitty). Needed for per-tab color rules, accurate tab titles, and jump-to-tab-by-process.
- [ ] Jump to tab by title/process — keybinding action that activates the first tab matching a title or process pattern. E.g. `action = "activate_tab_matching"`, `args = ["process:claude", "forward"]`. Repeating cycles to the next match; a second binding with `"backward"` cycles in reverse. Combined with foreground process tracking, allows quick switching to specific running apps.
- [ ] Tab reordering — drag tabs in the tab bar to rearrange, or keybindings to move current tab left/right. Drag UX: render dragged tab at low opacity following the cursor, draw a vertical insertion bar (via rect pipeline, same as pane dividers) at the drop position.
- [ ] Tab close confirmation — prompt when closing a tab that has a foreground process other than the shell (requires foreground process tracking). Skip prompt for idle shell.
- [ ] New tab/pane inherits CWD — new tabs and pane splits start in the focused pane's working directory (already tracked via OSC 7).
- [ ] Automatic tab ordering — sort rules that pin tabs to preferred positions based on title/process. E.g. `match = "process:clangd"`, `position = 1`. Multiple matches for the same position group together adjacent. Unmatched tabs fill in around pinned groups. Shares pattern matching infrastructure with color rules and jump-to-tab.

## Mouse Bindings

- [ ] Mouse binding system — extend Action/Binding to cover mouse triggers: button, event type (press/release/click/doubleclick), modifiers. Mirror WezTerm/Kitty: location (tab bar vs pane) determined by hit-test before binding lookup, not encoded in the trigger. Tab bar bindings (left-click → switch tab, middle-click → close tab) and pane bindings resolved separately.

## Keybindings / Actions

- [x] Action/binding system — `Action::Any` variant, sequence-based key bindings (Kitty-style `key1 > key2`), TOML config via `[[keybinding]]`. Default bindings for all tab and pane operations.
- [x] SGR inverse (reverse video) — correctly swaps fg/bg in cell resolution; fixes TUI app cursors (Claude Code, htop, vim selection, etc.).
- [x] `$COLORTERM=truecolor` — set in PTY environment so apps detect 24-bit color support.
- [x] Platform-appropriate default bindings — `#ifdef __APPLE__` uses Cmd-based bindings (Cmd+T, Cmd+W, Cmd+C/V, Cmd+D split), Linux uses Ctrl+Shift (Ctrl+Shift+T, Ctrl+Shift+W, Ctrl+Shift+C/V).
- [x] Jump to prompt — Cmd+Up/Down (macOS), Ctrl+Alt+Z/X (Linux). Scans history for OSC 133 PromptStart markers.
- [x] Scrollback pager — Cmd+F (macOS), Ctrl+Shift+F (Linux). Opens `less -R` in overlay with serialized scrollback.
- [ ] Prompt navigation mode — enter a mode that highlights the current command's output (dim everything else, like iTerm). Previous/next keybinds to cycle between prompts. Copy output of highlighted command. Requires per-row tinting in the compute shader. Exit on Escape.
- [ ] Select command output — select output of a specific command (between OSC 133 C and next A). Needs a way to target which command (click, or previous/next navigation).

## Configuration

- [ ] Color scheme — `[colors]` config section for ANSI palette, foreground, background, cursor.
- [x] Keybindings — configurable key mappings for tab/pane operations.
- [x] Divider — `divider_color` (hex) and `divider_width` (pixels) in config.
- [ ] Cursor style — block/underline/bar, blink on/off, blink interval.
- [ ] Minimum contrast — auto-adjust foreground colors when too close to background (like iTerm2). Uses perceived brightness difference with configurable threshold. Would allow ANSI black to be true `#000000` while remaining visible.

## Text Shaping

- [x] Run-based shaping — adjacent cells grouped into runs, shaped via HarfBuzz. Replaces per-cell shaping. Enables ligatures, Arabic contextual forms, combining marks.
- [x] RTL support — SheenBidi BiDi analysis, HarfBuzz RTL direction, cell mirroring within contiguous RTL segments. Mixed LTR/RTL lines render correctly.
- [x] Indirect glyph list — ResolvedCell stores offset+count into GlyphEntry buffer. Supports multiple glyphs per cell (combining marks) and zero glyphs (ligature trailing cells).
- [x] Substitution detection — shaped glyph ID compared against nominal lookup. Substituted glyphs use HarfBuzz advance positioning; normal glyphs anchor at cell origin.
- [x] Per-row shaping cache — only dirty rows re-shaped. Clean rows reuse cached glyph data.
- [ ] Ligature verification — test with ligature fonts (Fira Code, JetBrains Mono).
- [ ] Arabic cursive connection verification — test with proper Arabic fonts (GSUB contextual forms).

## Emoji / Color Fonts

- [ ] Color emoji rendering — current pipeline is outline-only (hb-gpu/Slug encodes Bézier paths; fragment shader applies a single fg tint). Color fonts (COLR layered vector, CBDT/CBLC bitmap, sbix) silently produce empty glyphs. Options: COLRv0 via multi-pass layered outline rendering; CBDT/sbix via bitmap texture upload; or ship a monochrome outline emoji fallback font.

## Observability

- [x] `mb --ctl stats` — reports texture pool, compute pool, and per-pane GPU memory usage as JSON. Inactive tab textures are released to pool on tab switch.

## Infrastructure

- [x] Split TerminalEmulator.cpp — extracted into `terminal/` directory: TerminalEmulator.cpp (core state machine + CSI + onAction), KittyKeyboard.cpp, MouseAndSelection.cpp, SGR.cpp, OSC.cpp.
- [x] Split PlatformDawn.cpp — extracted into `platform/` directory: PlatformDawn.cpp (init), EventLoop.cpp, Input.cpp, Actions.cpp, Render.cpp, Tabs.cpp, TabBar.cpp, Debug.cpp.
- [x] Removed Platform abstract class — Terminal uses `PlatformCallbacks` (onTerminalExited, quit) instead of virtual base. main.cpp uses PlatformDawn directly.
- [x] Object library build — `terminal` and `platform` are CMake OBJECT libraries with their own CMakeLists.txt. Tests link `terminal` directly.
- [x] Reflow on resize — soft-wrap tracking via per-row continued flag. Full reflow on column change with cursor tracking. SIGWINCH coalesced to once per render frame.
- [x] Overlay rendering — overlays render as full-screen terminals over pane layout. Used by scrollback pager. Per-tab render state. Deferred cleanup on exit.
- [ ] Input delay — buffer PTY input for ~3ms before rendering (like kitty's `input_delay`) to avoid showing intermediate redraw states in apps that don't use Mode 2026.
- [ ] Shell integration injection — auto-source OSC 133 hooks for zsh/bash/fish (like kitty's `ZDOTDIR` hijack) so prompt markers work without user configuration.
- [ ] Log level — currently defaulting to Error; `-v` flags lower it. Debug level enables pool allocation logs.
- [ ] macOS font fallback — CoreText two-pass strategy implemented; parity with Linux fontconfig pass needs verification.
- [ ] Config colors as floats — tab bar colors use packed uint32 (parseHexColor), progress bar uses floats. Unify to float storage throughout so no unpacking is needed at render time.
- [ ] Wayland support — `NativeSurface.cpp` only handles X11 (`GLFW_PLATFORM_X11`). Add `GLFW_PLATFORM_WAYLAND` branch using `glfwGetWaylandDisplay()` / `glfwGetWaylandWindow()` and `wgpu::SurfaceSourceWaylandSurface`. Requires GLFW built with `-DGLFW_BUILD_WAYLAND=ON`.

## Testing

- [x] Unit test suite — doctest-based, tests `TerminalEmulator` in isolation. Covers: text output, cursor movement, SGR (all attributes, 16/256/truecolor colors, inverse), screen operations (ED, EL, SU/SD, DECSTBM, CNL/CPL/VPA, DCH/ICH, IL/DL, ECH), terminal modes (alt screen, mouse, bracketed paste, sync output, DA/XTVERSION, RIS), scrollback viewport, OSC title/icon, wide characters, SGR inverse color swap.
- [x] shapeRun() unit tests — ASCII clusters/advances/flags, Arabic RTL detection, mixed LTR/RTL, cache behavior, multibyte UTF-8 clusters. Uses system font resolver + fallback.
- [x] Kitty keyboard protocol tests — mode management (push/pop/set/query/stack overflow/alt screen/RIS), key encoding for all flag modes, legacy key compatibility, modifier-only keys, RIS comprehensive reset, alt screen grid reference.
- [x] Reflow tests — shrink/grow, newline preservation, trailing blank trimming, cursor tracking, SGR preservation, wide chars, history reflow, height-only change.
- [x] OSC 10/11/12 tests — query/set default colors, format parsing.
- [x] REP tests — basic repeat, default count, no prior char, wrapping, wide chars, attribute preservation.
- [x] Prompt tests — OSC 133 markers, jump to prompt, command output selection, scrollback serialization.
- [ ] Rendering tests — pixel comparison against reference images. Launch `mb` as a child process, drive it via the existing debug IPC (`mb --ctl screenshot --format png`, `mb --ctl key`), compare PNG output against reference images. No headless device needed — uses the real render pipeline. Needs: test harness that launches/connects/drives, reference image storage, comparison with tolerance.

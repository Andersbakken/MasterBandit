# TODO

## Terminal Protocol Support

- [x] Mode 2031 — Color preference notification (light/dark mode).
- [x] DECSCUSR (`CSI Ps SP q`) — Set cursor style: block, underline, bar, blinking variants.
- [x] Kitty keyboard protocol (`CSI > Ps u`) — Unambiguous key encoding. Distinguishes ESC from Alt+[, Ctrl+I from Tab, reports key release, supports non-Latin layouts. Used by Neovim, Vim, crossterm, textual, Ink.
- [x] Kitty graphics protocol — APC-based image protocol. Chunked transfer, persistent image IDs, PNG/RGB/RGBA formats, zlib decompression, query/delete, response messages, image numbers (`I=`), source rect cropping.
- [x] Kitty graphics: animation — frame load (`a=f`), animation control (`a=a`), delta compositing with base frame references, render-loop driven frame advancement.
- [x] Kitty graphics: file/shm transmission — `t=f` (file), `t=t` (temp file), `t=s` (shared memory).
- [x] Kitty graphics: multiple placements — one image displayed at multiple positions via `a=p` with placement IDs (`p=`). Per-placement cell dimensions and crop. Re-placing with same `p=` replaces old placement. Delete by placement ID via `d=i`/`d=n` with `p=`.
- [x] Kitty graphics: sub-cell pixel offsets — `X=` and `Y=` offset image within the starting cell. Per-placement.
- [x] Kitty graphics: position-based delete — `d=c`/`d=C` (cursor), `d=p`/`d=P` (cell position), `d=x`/`d=X` (column), `d=y`/`d=Y` (row), `d=r`/`d=R` (ID range). Uppercase frees image data.
- [x] Kitty graphics: frame composition (`a=c`) — explicit pixel-level blit between animation frames. Source/dest frame selection, rectangle region, alpha blend or overwrite.
- [x] Kitty graphics: z-layering — `z=` index per placement. `z >= 0` renders above text (default), `z < 0` below text. Sorted render with split-point. Delete by z-index (`d=z`/`d=Z`) and position+z (`d=q`/`d=Q`). Frame selection via `a=a,c=`.
- [x] Kitty graphics: `S=`/`O=` data size/offset — subrange reads for `t=f` (file), `t=t` (temp file), `t=s` (shared memory).
- [x] Kitty graphics: transparent border on GPU textures — 1px transparent border around image textures for soft edge blending matching kitty's `GL_CLAMP_TO_BORDER` behavior.
- [ ] Kitty graphics: Unicode placeholders — `U+10EEEE` virtual placements. Neither WezTerm nor most terminals support this.
- [ ] Kitty graphics: relative placements — `P=`/`Q=` parent image/placement references. Neither WezTerm nor most terminals support this.
- [x] Underline styles (`CSI 4:N m`) — Curly, dotted, dashed, double underlines + colored underlines (`CSI 58;...m`).
- [x] OSC 7 — Current working directory reporting. Pane stores CWD for new splits.
- [x] OSC 8 — Hyperlinks. Cmd/Ctrl+click opens URL. Auto-underlines linked text.
- [x] OSC 22 — Set mouse cursor shape. Full kitty-style stack ops (`=`/`>`/`<`/`?`), per-screen stacks (main vs alt), CSS3 + X11 alias name table, Cocoa + XCB cursor mappings. Per-pane CursorStyle cache; cursor follows the hovered pane (not focused), refreshed on tab/pane/overlay lifecycle events.
- [x] OSC 99 — Desktop notifications (kitty). Title/body accumulation, macOS UNUserNotification.
- [x] OSC 10/11/12 — Query/set default fg/bg/cursor colors. Responses in `rgb:RRRR/GGGG/BBBB` format.
- [x] OSC 4/104 — Set/query individual ANSI palette entries at runtime (`OSC 4 ; index ; color` to set, `OSC 4 ; index ; ?` to query, `OSC 104` to reset). Used by Neovim, Vim, and tmux to detect or adapt to the terminal's color scheme.
- [x] OSC 110/111/112 — Reset default fg/bg/cursor colors to built-in defaults. Complement to the already-implemented OSC 10/11/12 set/query.
- [x] OSC 133 — Shell integration prompt/command/output markers. Stored per-row. Used for jump-to-prompt and command output selection.
- [x] REP (`CSI b`) — Repeat preceding character N times.
- [x] Mode 2026 — Synchronized output. Defers rendering while active so intermediate states aren't shown.
- [x] DECRQSS (`DCS $ q ... ST`) — Query current terminal state. Supports `" q"` (cursor shape), `m` (current SGR), `r` (scroll margins / DECSTBM). Used by Vim/Neovim to restore cursor shape on exit.
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
- [x] Mouse selection on macOS — drag-to-select.
- [ ] Pane resize — drag split dividers with mouse (needs mouse binding system first).
- [ ] Refactor MouseSelection and OpenHyperlink — these are mouse-input-path concepts that need mouse coordinates, not general actions. They should be removed from the `Action` variant and handled purely as mouse binding result types. Similarly, PushOverlay needs parameters (command/shell) so it can't work as a parameterless action — it's script-only.
- [ ] Pane swap/rotate — swap focused pane with another, rotate panes clockwise/counterclockwise.
- [ ] Move pane to new tab or new window.
- [ ] Full-screen overlays — Tab::pushOverlay / popOverlay are implemented; need a way to trigger (e.g. Cmd+Shift+Enter kitty-style).
- [x] Popup panes (OSC 58237) — fully implemented with JS-driven API: `pane.createPopup({id, x, y, w, h})`, `popup.inject(data)`, `popup.close()`, `popup.resize({x, y, w, h})`, `popup.addEventListener("input"/"mouse")`. Popup properties: `cols`, `rows`, `x`, `y`, `focused`.
- [x] Popup focus cycling — `FocusPopup` action (Cmd+Shift+I / Ctrl+Shift+I) cycles focus between main terminal and popups. When focused, keystrokes route to the popup's emulator → JS input listeners. Popup cursor renders at popup position; main cursor hidden behind popups.
- [x] JS-driven popup API — `pane.createPopup({id, x, y, w, h})` returns a Popup object. `popup.inject(data)` writes to it, `popup.close()` removes it, `popup.addEventListener("input", fn)` receives keystrokes when focused. JS properties: `pane.focused`, `pane.focusedPopupId`, `pane.popups`, `popup.focused`.
- [ ] Tab bar: cursor-blink optimization — re-render to held texture when only cursor changes (500 ms interval guarantees GPU completion).
- [x] Tab bar: color packing uses BGR order (parseHexColor packs R<<16|G<<8|B but shader reads R from bits 0-7). Colors are visually incorrect (R/B swapped) — fix to match packFgAsU32 byte order.
- [x] Tab bar: configurable keybindings for new tab (Ctrl+Shift+T) and tab close.
- [ ] Non-powerline tab bar styles (fade, slant, separator, round) from config.
- [ ] Per-tab color rules — color tabs based on title, active process, or index. Requires foreground process tracking and a rule matching engine in the tab bar renderer. Config via `[[tab_bar.color_rules]]` with match patterns (e.g. `title:*ssh*`, `process:python`, `index:0`).
- [x] Foreground process tracking — `Terminal::foregroundProcess()` via `tcgetpgrp` on PTY fd + `proc_pidpath` (macOS) / `/proc/<pid>/comm` (Linux). Checked on each PTY read. `pane.foregroundProcess` JS property, `foregroundProcessChanged` event on change. Tab title shows foreground process name when no OSC 2 title set.
- [ ] Jump to tab by title/process — keybinding action that activates the first tab matching a title or process pattern. E.g. `action = "activate_tab_matching"`, `args = ["process:claude", "forward"]`. Repeating cycles to the next match; a second binding with `"backward"` cycles in reverse. Combined with foreground process tracking, allows quick switching to specific running apps.
- [ ] Tab reordering — drag tabs in the tab bar to rearrange, or keybindings to move current tab left/right. Drag UX: render dragged tab at low opacity following the cursor, draw a vertical insertion bar (via rect pipeline, same as pane dividers) at the drop position.
- [ ] Tab close confirmation — prompt when closing a tab that has a foreground process other than the shell (requires foreground process tracking). Skip prompt for idle shell.
- [x] New tab/pane inherits CWD — new tabs and pane splits start in the focused pane's working directory (already tracked via OSC 7).
- [ ] Automatic tab ordering — sort rules that pin tabs to preferred positions based on title/process. E.g. `match = "process:clangd"`, `position = 1`. Multiple matches for the same position group together adjacent. Unmatched tabs fill in around pinned groups. Shares pattern matching infrastructure with color rules and jump-to-tab.

## Mouse Bindings

- [x] Scripting: mouse events for popups, panes, and overlays — `popup.addEventListener("mouse", fn)`, `pane.addEventListener("mouse", fn)`, and `overlay.addEventListener("mouse", fn)` receive press/release events with cell coordinates, pixel coordinates, and button. Gated on `ui` group permission. Click inside popup delivers to JS and doesn't propagate to terminal selection.
- [x] Mouse binding system — MouseStroke (button, event type, modifiers, mode, region) + MouseBinding + ClickDetector for multi-click. Default bindings: character/word/line/extend/rectangle selection, tab bar clicks, hyperlink open, middle-click paste, shift-override in grabbed mode. Configurable via `[[mousebinding]]` in TOML. Reloadable.
- [ ] Scripting: pane-level mouse events on overlays — `overlay.addEventListener("mouse", fn)` is registered but the C++ Input.cpp overlay path doesn't deliver events yet (only pane and popup mouse events are wired up).

## Keybindings / Actions

- [x] Action/binding system — `Action::Any` variant, sequence-based key bindings (Kitty-style `key1 > key2`), TOML config via `[[keybinding]]`. Default bindings for all tab and pane operations.
- [x] SGR inverse (reverse video) — correctly swaps fg/bg in cell resolution; fixes TUI app cursors (Claude Code, htop, vim selection, etc.).
- [x] SGR inverse bug — only the background is reversed, not the foreground. Text with green fg on default bg should render as default fg on green bg when inverse is set, but currently renders green on green (unreadable).
- [x] `$COLORTERM=truecolor` — set in PTY environment so apps detect 24-bit color support.
- [x] Platform-appropriate default bindings — `#ifdef __APPLE__` uses Cmd-based bindings (Cmd+T, Cmd+W, Cmd+C/V, Cmd+D split), Linux uses Ctrl+Shift (Ctrl+Shift+T, Ctrl+Shift+W, Ctrl+Shift+C/V).
- [x] Jump to prompt — Cmd+Up/Down (macOS), Ctrl+Alt+Z/X (Linux). Scans history for OSC 133 PromptStart markers.
- [x] Scrollback pager — Cmd+F (macOS), Ctrl+Shift+F (Linux). Opens `less -R` in overlay with serialized scrollback.
- [ ] Prompt navigation mode — enter a mode that highlights the current command's output (dim everything else, like iTerm). Previous/next keybinds to cycle between prompts. Copy output of highlighted command. Requires per-row tinting in the compute shader. Exit on Escape.
- [ ] Select command output — select output of a specific command (between OSC 133 C and next A). Needs a way to target which command (click, or previous/next navigation).

## Configuration

- [x] Color scheme — `[colors]` config section for ANSI palette, foreground, background, cursor.
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
- [x] Italic font rendering — real italic variant resolved via CoreText/fontconfig (`kCTFontItalicTrait` / `FC_SLANT_ITALIC`), falls back to synthetic slant via `hb_font_set_synthetic_slant`. Same pattern as bold.
- [ ] Ligature verification — test with ligature fonts (Fira Code, JetBrains Mono).
- [ ] Arabic cursive connection verification — test with proper Arabic fonts (GSUB contextual forms).

## Emoji / Color Fonts

- [x] COLRv1 color emoji — GPU-side paint graph interpreter rasterizes COLRv1 glyphs into a bucketed tile atlas (32–512px). Supports solid fills, linear/radial/sweep gradients, nested transforms, clip glyphs/rects, and SRC_OVER compositing. Generation-based eviction with atlas auto-grow up to 8192px.
- [ ] COLRv1 compositing modes — only CLEAR, SRC, SRC_OVER, and DEST are implemented in the rasterizer shader. The spec defines ~13 Porter-Duff modes plus blend modes (screen, multiply, overlay, etc.). Some fonts use DEST_IN/DEST_OUT for masking.

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
- [x] Animated kitty graphics CPU usage — reduced from ~30% to ~3.5% per visible animated image (below kitty's ~6%). Fixes: (1) `Renderer::useImageFrame` caches one GPU texture per `(imageId, frameIndex)` slot; animation playback is just a sampler swap after the first cycle. (2) `tickAnimations()` now returns whether a frame index actually advanced, and `rs.dirty` is only set when it did — eliminates full-pane re-renders on every event-loop tick. (3) `scheduleAnimationWakeup()` replaces spin-rendering at vsync with a one-shot timer at the next `frameShownAt + gap`, collapsing ~120 Hz event-loop churn to the animation's own rate. (4) Drift-free rebase: `frameShownAt = now - elapsed` rather than `= now`, so timer jitter can't accumulate and silently cross a gap boundary. (5) `retainImagesOnly(union of per-pane lastVisibleImageIds)` evicts textures for images scrolled into scrollback; re-uploads lazily on scroll-back.
- [ ] Shell integration injection — auto-source OSC 133 hooks for zsh/bash/fish (like kitty's `ZDOTDIR` hijack) so prompt markers work without user configuration. Also needed to fix: closing a second tab produces a spurious newline in tab 1 because `updateTabBarVisibility` resizes the surviving pane (tab bar hidden → pane grows), triggering SIGWINCH → shell reprints prompt. With OSC 133 markers the terminal could suppress the redraw or preserve prompt position.
- [ ] Log level — currently defaulting to Error; `-v` flags lower it. Debug level enables pool allocation logs.
- [ ] macOS font fallback — CoreText two-pass strategy implemented; parity with Linux fontconfig pass needs verification.
- [ ] Config colors as floats — tab bar colors use packed uint32 (parseHexColor), progress bar uses floats. Unify to float storage throughout so no unpacking is needed at render time.
- [ ] Wayland support — `NativeSurface.cpp` only handles X11 (`GLFW_PLATFORM_X11`). Add `GLFW_PLATFORM_WAYLAND` branch using `glfwGetWaylandDisplay()` / `glfwGetWaylandWindow()` and `wgpu::SurfaceSourceWaylandSurface`. Requires GLFW built with `-DGLFW_BUILD_WAYLAND=ON`.
- [x] PTY write EAGAIN — `writeToPTY` fails with EAGAIN when pasting large text (PTY buffer full). Should retry with backoff or use non-blocking write with a write queue.
- [ ] Prompt-aware reflow — when OSC 133 markers are present, blank the rprompt region of prompt lines before reflow to prevent rprompt padding from wrapping into extra rows. The shell redraws the current prompt after SIGWINCH; history prompts could use the marker to identify and strip the rprompt portion.
- [x] Reflow strips spaces — cursor gap detection treated spaces as null cells, truncating content after cursor.
- [x] Underlines not cleared on scroll — deleteChars/insertChars didn't shift extras map entries, leaving stale underline colors at wrong columns.
- [x] Scripting engine (QuickJS-ng) — embedded with Pane/Overlay/Tab JS classes, synchronous output/input filters (zero-copy when no listeners), async lifecycle events via microtasks, action listener system. Two script types: controller (global app control) and applet (headless terminal overlay).
- [x] Scripting: OSC handler routing — `pane.addEventListener("osc:NNNN", fn)` registers JS handlers for specific OSC numbers. Terminal emulator routes unhandled OSC codes to script engine. Enables applet launch via escape sequence.
- [x] Scripting: applet launch confirmation — implemented in JS (applet-loader.js). Creates a popup with clickable [allow]/[deny]/[always]/[never] buttons. Supports both keyboard and mouse click input. Allowlist persisted in config dir.
- [ ] Scripting: command line loading — `--script <path>` loads user scripts at startup; permissions read from script header (see below).
- [ ] Scripting: permission declaration in script header — move permission declaration from OSC/caller into the script file itself as a structured comment, e.g. `// @mb {"permissions": "io,shell", "netHosts": ["api.example.com"]}`. C++ parses this before loading, presents it to the user, and enforces it at the API layer. Keeps the OSC lean, ensures the declaration is part of the hashed content (cannot be changed post-approval), and removes the caller as a trust surface. Naturally extends to host allowlists when the net module lands.
- [ ] Scripting: TypeScript definitions — generate a `.d.ts` file covering the full `mb.*` JS API surface (`mb`, `Pane`, `Popup`, `Tab`, `Overlay`, events, permissions). Serves as both documentation and enables type-checking for applet authors.
- [x] Scripting: `console.log` — route QuickJS console output to spdlog.
- [x] Scripting: timers — `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` backed by libuv timers.
- [x] Scripting: overlay creation — `tab.createOverlay()` creates headless terminal, `overlay.inject()` renders, `overlay.addEventListener("input", fn)` receives keystrokes, `overlay.close()` pops it.
- [ ] Scripting: overlay resize event — `overlay.addEventListener("resized", (cols, rows) => {...})`. Overlays don't have a fixed pane association (panes can come and go while an overlay is active), so they need their own resize notification rather than relying on the underlying pane's `resized` event. Fire when the tab's framebuffer geometry changes.
- [ ] Scripting: `mb.*`-level destroyed events — every `Created` event fan-out on `mb` needs a symmetric `Destroyed` counterpart. Currently `paneCreated`/`tabCreated` (ScriptEngine.cpp:2262/2283) fan out to JS listeners, but `notifyPaneDestroyed`/`notifyTabDestroyed` (2278/2299) only call `cleanupPane`/`cleanupTab` with no JS fan-out. Add `mb.__evt_paneDestroyed` / `mb.__evt_tabDestroyed` fan-out mirroring the `Created` pattern. Listener signatures deliver the id (number), not the object, since the object is already dead by the time listeners fire. Useful for scripts tracking pane/tab-keyed state without pre-registering per-object `destroyed` listeners. Audit other `Created`/`Destroyed` pairs for the same gap.
- [x] Scripting: applet loading via OSC 58237 — `printf '\e]58237;applet;path=/path/to/script.js\e\\'` triggers built-in applet-loader.js controller.
- [x] Scripting: JS module imports — `JS_EVAL_TYPE_MODULE` + custom module loader. Two trusted import sources: (1) script's own directory + subdirectories (no `../` escape), (2) built-in modules directory shipped alongside the binary (e.g. `scripts/modules/tui.js`). Built-in modules always allowed regardless of script permissions. Needed before `mb:fs` and other native modules.
- [ ] Scripting: `mb:fs` module — Node-style sync file API: `readFileSync`, `writeFileSync`, `readdirSync`, `statSync`, `existsSync`, `mkdirSync`, `unlinkSync`, `renameSync`. Permission-gated.
- [ ] Scripting: `mb:http` module — async HTTP client backed by libwebsockets (already a dependency). Node-style `http.get(url, cb)` / `http.request(opts, cb)`. Permission-gated. Requires `net` permission with declared host allowlist (see below).
- [ ] Scripting: network permission with host scoping — when `mb:http` (and future net modules) land, introduce a `net` permission where scripts declare allowed hosts upfront (e.g. `net:api.example.com,cdn.example.com`). Enforced at connection time; redirects to undeclared hosts are blocked. Post-DNS resolution, connections to RFC1918/loopback addresses are blocked unless a separate `net.local` permission is granted (prevents DNS rebinding). The host list must be declared in a structured script header comment so it is covered by the content hash and cannot be changed post-approval. The combination `io.filter.input + net` should trigger a distinct high-risk warning in the permission prompt.
- [x] Scripting: `mb:tui` module — bundled TUI toolkit for applets. `box()`, `text()`, `input()`, `list()`, etc. Built on escape sequence generation, targets the applet overlay API directly.
- [ ] Scripting: `Buffer` polyfill — needed for binary data in fs/http modules.
- [ ] Scripting: WebSocket server module — backed by libwebsockets. Would allow replacing the C++ DebugIPC with a JS applet. Needs `mb:ws` module with `ws.createServer({path})` returning a server object with connection/message events.
- [ ] Scripting: replace DebugIPC with JS applet — WebSocket server applet that exposes grid content, screenshots, stats, key injection, action dispatch. Requires `mb:ws` module and grid/screenshot APIs on `mb.*`.
- [ ] Scripting: execution guardrails — `JS_SetInterruptHandler` with ~5s wall-time budget to kill runaway loops. `JS_SetMemoryLimit` per script to cap memory leaks. Covers accidental infinite loops and unbounded allocations in user scripts.
- [ ] Scripting: internal property hardening — `__input_filters`, `__output_filters`, `__evt_*`, `__pane_registry`, `__popup_registry` etc. are regular JS properties accessible to any script. A script could bypass permission checks by directly reading/writing these. Fix by using Symbols or C-side storage instead of string-keyed JS properties.
- [ ] Scripting: revisit module hash generation for script approval — currently globs all `.js` files in the script's directory tree and hashes them all. Tradeoffs: unrelated files in the same directory trigger re-approval; changing a file that isn't actually imported by the script is a false positive; doesn't track transitive imports across directories. Alternatives: track only files actually loaded by the QuickJS module loader during execution, or require one script per directory by convention. Needs more thought.
- [ ] Refactor: `buildTerminalCallbacks()` lives in `Tabs.cpp` but has nothing tab-specific about it — it wires all platform-level callbacks (OSC, CWD, foreground process, clipboard, custom tcap, etc.). Move it to a more appropriate location (e.g. `PlatformDawn.cpp` or a dedicated `TerminalFactory.cpp`).
- [ ] Scripting: gate OSC listener registration — `pane.addEventListener("osc:NNNN", fn)` currently requires no permission. Should be gated (e.g. `IoFilterOutput`) since it receives raw terminal output including sensitive content.
- [ ] Scripting: gate XTGETTCAP registration — `mb.registerTcap` / `mb.unregisterTcap` currently gate on `ActionsInvoke`. Consider a dedicated `terminal` permission group covering both tcap registration and OSC listener registration.
- [ ] Scripting: pane/tab query permissions — hierarchical access levels: `panes.current` (active pane only), `panes.tab` (all panes in active tab), `panes.all` (all tabs/panes). Currently all pane/tab queries are ungated. `ui` should imply `panes.current`. `mb.activePane()` requires `panes.current`, `mb.activeTab()` requires `panes.tab`, `mb.tabs()` requires `panes.all`. Pane property getters (title, cwd, foreground process) gated by the level that granted access to the pane object.
- [x] Scripting: permission system — granular bitmask permissions (ui.overlay.create, io.filter.input, shell.write, actions.invoke, tabs.create, scripts.load, etc.) with group aliases for OSC brevity (ui, io, shell, actions, tabs, scripts). Built-in scripts fully trusted. User scripts checked against allowlist (`allowed-scripts.toml` with SHA-256 content hash and version field). Permission prompt is fully JS-driven (applet-loader.js creates the popup, handles keyboard + mouse clicks). `mb.approveScript(path, response)` API and `scriptPermissionRequired` event. `actions.invoke` cross-checks action names against relevant permissions (NewTab→tabs.create, etc.). Erases applet/controller distinction.
- [x] Scripting: script actions & namespaces — `mb.setNamespace(name)`, `mb.registerAction(name)` registers `namespace.action`. Config binds via `action = "namespace.action"`. `ScriptAction` variant in Action.h. `ReloadConfig` action rebuilds bindings at runtime.
- [x] Scripting: script lifecycle — same-path replacement on content or permission change (unload old instance, load fresh). Identical path + content hash + permissions is a no-op — returns the existing instance id without churn. `mb.exit()` for self-unload via zero-delay timer, full cleanup on unload (timers, popups, overlays, filter counts, registered actions).
- [x] Scripting: foreground process detection — `Terminal::foregroundProcess()` via tcgetpgrp + proc_pidpath/proc/pid/comm. `pane.foregroundProcess` JS property. `foregroundProcessChanged` event on PTY read. Tab title fallback when no OSC 2 title set.
- [x] Scripting: `mb.actions` property — returns all built-in + registered script actions, derived from compile-time Action variant reflection. Includes arg variants for directional actions.
- [x] Scripting: command palette — built-in JS applet (`command-palette.js`). Fuzzy search, arrow navigation, Enter to execute. Default binding Cmd+Shift+P / Ctrl+Shift+P. Uses `mb.actions` and `mb.invokeAction`.
- [x] Scripting: API consistency — `mb.tabs`, `mb.activePane`, `mb.activeTab`, `mb.actions` are getter properties (not functions). `popup.close()` (not destroy). `popup.cols/rows/x/y` properties. Removed `pane.destroyPopup`, `mb.loadApplet`, `mb.loadController` from JS API.
- [x] Scripting: permission violation terminates script — scripts that try to use unpermitted APIs are scheduled for termination via zero-delay timer. Built-in scripts exempt.
- [ ] Move higher-level UI to JS — the C++ side should provide basic primitives (workspace containers with layout trees, pane creation, show/hide/stack ordering) and let JS handle the semantics. Tabs and overlays could be unified as "workspaces" — a tab is a workspace with panes and a tab bar entry, an overlay is a transient workspace with a single pane pushed on top. The tab bar itself becomes a JS script. Built-in actions like NewTab/CloseTab become JS-registered actions backed by C++ primitive API calls (`mb.createWorkspace()`, `mb.activateWorkspace(id)`, etc.). This simplifies the C++ layer and makes UI behavior fully customizable.
- [ ] Configuration UI — first bundled script. QuickJS script that reads config, draws a TUI form in an overlay pane via escape sequences, writes changes back. Replaces manual TOML editing.
- [ ] Built-in UI theming — expose a `[ui]` config section (colors, border style) that all built-in scripts (command palette, permission dialog, future TUI overlays) read from a single place. Implement via `mb.config` JS property populated from the loaded Config. Scripts call `createTheme(mb.config.ui)` rather than hardcoding colors. Covers command-palette.js, applet-loader.js permission prompt, and any future built-in popups/overlays.
- [ ] GPU buffer pool — divider and popup border vertex buffers are created/destroyed directly. A pool (like TexturePool/ComputeStatePool) would avoid per-frame GPU allocations.
- [ ] Image vertex buffer growth — `imageVertexBuffer_` is fixed at 256 image slots. Should grow dynamically or guard against overflow when >256 images are visible in one frame.
- [ ] mmap font loading — large fonts (64 MB+) are currently read into a malloc'd buffer. Use `mmap` so pages can be faulted in on demand and reclaimed under memory pressure. HarfBuzz accepts pointer+length so this is a drop-in change.

## Platform (Linux)

- [ ] Notifications via D-Bus — replace `execvp notify-send` with direct libdbus calls. Wire libdbus watches/timeouts into libuv (`uv_poll_t`/`uv_timer_t`) via `dbus_connection_set_watch_functions` / `dbus_connection_set_timeout_functions`. Subscribe to `ActionInvoked`, `NotificationClosed`, `ActivationToken` signals for click-to-focus support (like kitty/wezterm). kitty's `glfw/dbus_glfw.c` + `glfw/linux_notify.c` (~500 lines, no extra deps beyond libdbus) is a useful reference for the watch integration pattern.

## Procedural Glyph Rendering

- [x] Box drawing + block elements (U+2500–259F) — rect, quadrant, shade, box line types.
- [x] Braille patterns (U+2800–28FF) — 8-dot 2×4 grid.
- [x] Sextants (U+1FB00–1FB3B) — 2×3 grid fill.
- [x] Octants (U+1CD00–1CDFE) — 2×4 grid fill.
- [x] Wedge triangles (U+1FB3C–1FB6F) — smooth mosaic triangles.
- [x] Powerline core (E0B0–E0B3) — solid triangles and thin chevrons.
- [x] Powerline half-cell triangles and diagonals (E0B8–E0BF).
- [x] Powerline semi-circles (E0B4–E0B7) — tessellated with 16 segments.
- [ ] Powerline extras (E0C0–E0D4) — flame, pixel, trapezoid, and misc shapes. Need per-shape geometry.
- [ ] Slug/bezier rendering for semi-circles (E0B4, E0B6) — generate Slug-format atlas data for the 2 filled semi-circles, route through text pipeline for analytical anti-aliased curves. Currently using tessellation fallback.
- [x] Diagonal edge AA for procedural glyphs — `vec2f edge_dist` in `RectVertexStorage` (32 bytes). Compute shader sets signed distance to nearest diagonal edge per vertex via `edge_dist_to()` + `tri_aa_d0()` helpers (large positive sentinel for axis-aligned shapes). Fragment shader applies `smoothstep(0.0, 1.0, min(d0, d1))` to alpha. Covers powerline triangles/chevrons (shapes 0-3, 8-15) and wedge triangles (type 9). Semi-circles unchanged (tessellation fallback).
- [ ] Shade dithering — type 3 currently renders as a semi-transparent rect. Should use a dither pattern (checkerboard / noise) for accurate ░▒▓ rendering.
- [ ] Compute shader split for procedural glyphs — procedural glyph logic (~560 lines of branching) increases register pressure and may reduce occupancy even when no procedural glyphs are on screen. If profiling shows this matters, split into two dispatches (text cells vs procedural cells) with a CPU-side partition.

## Testing

- [x] Unit test suite — doctest-based, tests `TerminalEmulator` in isolation. Covers: text output, cursor movement, SGR (all attributes, 16/256/truecolor colors, inverse), screen operations (ED, EL, SU/SD, DECSTBM, CNL/CPL/VPA, DCH/ICH, IL/DL, ECH), terminal modes (alt screen, mouse, bracketed paste, sync output, DA/XTVERSION, RIS), scrollback viewport, OSC title/icon, wide characters, SGR inverse color swap.
- [x] shapeRun() unit tests — ASCII clusters/advances/flags, Arabic RTL detection, mixed LTR/RTL, cache behavior, multibyte UTF-8 clusters. Uses system font resolver + fallback.
- [x] Kitty keyboard protocol tests — mode management (push/pop/set/query/stack overflow/alt screen/RIS), key encoding for all flag modes, legacy key compatibility, modifier-only keys, RIS comprehensive reset, alt screen grid reference.
- [x] Reflow tests — shrink/grow, newline preservation, trailing blank trimming, cursor tracking, SGR preservation, wide chars, history reflow, height-only change.
- [x] OSC 10/11/12 tests — query/set default colors, format parsing.
- [x] REP tests — basic repeat, default count, no prior char, wrapping, wide chars, attribute preservation.
- [x] Prompt tests — OSC 133 markers, jump to prompt, command output selection, scrollback serialization.
- [ ] Script engine tests — JS-level tests loaded via `Engine::loadController` with mock callbacks. Test scripts exercise APIs and assert via console output. Covers: permission checks, popup lifecycle, action registration, event delivery, mouse events, allowlist, foreground process.
- [ ] IPC-driven script tests — extend `mb --ctl` with `--script <path>` to load scripts via IPC. Enables end-to-end testing of script loading, permission prompts, and applet behavior. Requires IPC.
- [x] IPC security — debug IPC socket restricted to `--test` mode only. No socket created in normal operation.
- [ ] Rendering tests — pixel comparison against reference images. Launch `mb` as a child process, drive it via the existing debug IPC (`mb --ctl screenshot --format png`, `mb --ctl key`), compare PNG output against reference images. No headless device needed — uses the real render pipeline. Needs: test harness that launches/connects/drives, reference image storage, comparison with tolerance.

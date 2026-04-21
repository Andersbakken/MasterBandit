# Tree Cutover — Status & Plan

Successor to `CUTOVER.md`. The prior cutover landed the shared `LayoutTree` + UUID
identity + `mb.layout.*` structural API + JS ownership of the *action dispatch
path*. This doc covers the next (and final) layer: dissolving the native `Tab`,
`Layout`, and `Overlay` concepts so the tree is the sole source of UI truth.

The user has expressed that this is the second attempt at this refactor; the first
stalled halfway. The "Done criteria" at the bottom is a contract — we don't ship
until every item is green.

---

## What shipped this session (branch state)

All 689 tests pass on the branch. These are additive to the prior cutover:

### Action dispatch is JS-owned

- `Engine::registerActionHandler` / `unregisterActionHandler` /
  `invokeActionHandler(name, buildArgs)` on `Script::Engine`.
- `mb.actions.register(name, fn)` / `unregister(name)` — attached to the array
  returned by the existing `mb.actions` getter. Gated on `Perm::LayoutModify`.
  Per-instance cleanup on unload.
- `Platform_Actions.cpp`: the 8 JS-owned actions (`newTab`, `closeTab`,
  `activateTab`, `activateTabRelative`, `splitPane`, `closePane`, `zoomPane`,
  `adjustPaneSize`) call `invokeActionHandler` *only* — no native fallback.
  `spdlog::error` on missing handler. `FocusPopup` still native (no popup
  primitive yet; will move later).

### Events

- `terminalExited` JS event with `{paneId, paneNodeId}` payload. Fired from
  `TabManager::terminalExited` before the native cleanup cascade (observation
  only today — the cascade still runs; see "What's wrong" below).

### Lifecycle + layout primitives on `mb.layout`

All are thin shims over `AppCallbacks` which fan out to `TabManager` methods:

- `createTab() → {id, nodeId}` — empty Tab hooked under root Stack. Does not activate.
- `closeTab(idOrNodeId)` / `activateTab(idOrNodeId)` / `focusPane(idOrNodeId)` / `closePane(idOrNodeId)`
- `createTerminal(parentContainerNodeId, opts?) → {id, nodeId}` — spawns PTY,
  appends under parent, inserts into Layout's `mPanes`, fires `paneCreated`.
- `splitPane(existingNodeId, dir, newIsFirst?) → {id, nodeId}` — wraps existing
  in Container, places new alongside, inherits slot sizing.
- `setZoom(paneNodeIdOrEmptyString)`
- `setSlotStretch/MinCells/MaxCells/FixedCells(parentId, childId, value)`
- `adjustPaneSize(paneNodeId, dir, amount)` — converts cells→pixels native-side.
- `focusedPane() → {id, nodeId, tabId, tabNodeId} | null`
- `activeTab() → {id, nodeId} | null`
- `createTerminalNode()` — bare tree-node creator, currently unused.

### `TabManager` additions

- `createEmptyTab(outNodeId*) → tabIdx` — empty Tab + Layout, hooks under root Stack.
- `activateTabByIdx(idx)` — wraps UI chrome.
- `createTerminalInContainer(parentUuid, cwd, outPaneId*, outNodeId*)`
- `splitPaneByNodeId(existingUuid, dir, ratio, newIsFirst, outPaneId*, outNodeId*)` — `ratio` is currently ignored; slot stretch inherits.
- `closePaneById(paneId)` — full ClosePane teardown (poll remove, graveyard, refresh).
- `focusPaneById(paneId)` / `findTabBySubtreeRoot` / `findTabForNode` / `findPaneIdByNodeId`.

### `LayoutTree` additions

- `setSlotStretch/MinCells/MaxCells/FixedCells(parent, child, value)`.
- On `Layout`: `allocatePaneNode(outNodeId*)` — orphan Terminal node + paneId + maps.
- On `Layout`: `splitByNodeId(existingUuid, dir, newUuid, newIsFirst)` — UUID-based split, inherits slot sizing.

### `default-ui.js`

Mandatory — `Platform_EventLoop.cpp` does `std::exit(1)` on load failure.
Registers handlers for all 8 JS-owned actions and listens for `terminalExited`
(observation only for now).

### Bug fixes along the way

- **`Uuid::toString()`** was placing hyphens at byte indices `4/6/8/10` instead
  of string positions `8/13/18/23`, producing malformed UUIDs like
  `86d3055d-c2-7f-5f-f105d9e65f--------` that round-tripped back as nil.
  Silent bug — no round-trip test existed. Fixed; **still needs a regression
  test in `tests/test_uuid.cpp`**.
- **`mb.actions.register` collision** with pre-existing `mb.actions` array
  getter. Resolved by attaching `register`/`unregister` onto the array the
  getter returns. Has the quirk that `mb.actions !== mb.actions` (fresh array
  each access); capture into a local if you need identity.

---

## Design decisions (locked in this session)

Reference for next session — do not re-debate without reason.

**Q1 — Action split.** JS-owned: `NewTab`, `CloseTab`, `SplitPane`, `ClosePane`,
`ZoomPane`, `AdjustPaneSize`, `ActivateTab`, `ActivateTabRelative`, `FocusPopup`.
Native: `FocusPane` (spatial nav, latency), scroll, clipboard, font size, mouse
region, selection, `ReloadConfig`, etc. **Status**: shipped except `FocusPopup`
which needs a popup primitive before it can migrate.

**Q2 — Preemption model.** **(b) JS-first.** Native only provides primitives.
Controller owns every named action. No native fallback. Missing handler →
`spdlog::error`. **Status**: shipped for 8 actions.

**Q3 — Tree mutations vs. side effects.** **(a) Tree is data; side effects are
separate APIs.** `mb.layout.*` = structural. Lifecycle via distinct primitives
(`killTerminal`, `removeNode`, etc.). **Status**: partially shipped — current
`closePane` still does lifecycle + tree removal in one call; needs splitting.

**Q4 — Default controller.** `assets/scripts/default-ui.js`. Mandatory
(refuse-to-start on load failure). **Status**: shipped as stub-with-handlers;
needs expansion to own tree construction at startup.

**Q5 — Lifecycle naming.** `mb.layout.killTerminal` (Terminal death) vs.
`mb.layout.removeNode` (tree mutation). `closePane` is being renamed.

**Q6 — Terminal lifetime.** There is no "dead but not graveyarded" state.
Terminals are `live` or `graveyarded`. `killTerminal` is synchronous (close master
fd, SIGHUP process group, remove from `mPanes`, graveyard object, fire
`terminalExited`). Shell-exit path (native EOF detection) does the same thing.
Both paths produce the invariant at event-fire time: *Terminal is graveyarded,
tree node is still present*.

**Q7 — Overlay dissolution.** Overlay as a native C++ concept dies. Overlays are
just additional Terminal (or Stack) children of a tab's Stack, swapped via
`Stack.setActiveChild`. The "tab" is a Stack whose `activeChild` is the normal
content; siblings are "overlays".

**Q8 — Native UI surface.** The **only** two native leaf-content kinds are
`Terminal` and `TabBar`. Everything else (tabs, overlays, splits, active-tab
selection) is expressed as tree shape under JS control.

**Q9 — `Tab` C++ object.** Dies entirely. No bookkeeping class. What used to
live there:
- Title → Stack's `label` (already on the tree node).
- Overlay stack → Stack's children (siblings of the content node).
- Layout ownership → tree itself; Terminals owned by an engine-wide
  `unordered_map<Uuid, unique_ptr<Terminal>>`.

**Q10 — `Layout` C++ class.** Dies. Replaced by: the tree itself, plus
engine-wide state for focus (one `Uuid focusedTerminalNodeId`), zoom (one
`Uuid zoomedNodeId`), and the terminal map.

**Q11 — Scrollback pager.** `Action::ShowScrollback` currently spawns a
`less`-style overlay. Rewrites to spawn a Terminal as a Stack child, same
mechanism as any other "overlay."

**Q12 — `mb.quit()`.** New primitive for app exit. Distinct from existing
`mb.exit()` which unloads the calling script instance.

---

## Target model

```
Root Container (vertical)
├── TabBar node                    ← bound to root Stack; renders labels
└── Stack "tabs"                   ← root Stack; activeChild = current tab
    ├── Stack "tab-1"              ← label = tab title; activeChild = content
    │   ├── Container "splits"     ← normal splits (or a single Terminal)
    │   │   ├── Terminal A
    │   │   └── Terminal B
    │   └── Terminal "palette"     ← "overlay" sibling; setActiveChild to show
    ├── Stack "tab-2"
    │   └── Terminal C
    └── ...
```

Constructed by `default-ui.js` at startup. Native contributes: tree primitives,
`Terminal` lifecycle, `TabBar` rendering, `Terminal` rendering, input/focus
routing, `mb.quit()`.

---

## What dies (complete kill list)

### C++ classes / member fields
- `class Tab` (Tab.h, Tab.cpp) — entire class.
- `class Layout` (Layout.h, Layout.cpp) — entire class. 115 `tab->layout()->...`
  callsites disappear.
- `class Overlay` (wherever it lives) and any `Tab::mOverlay` vector.
- `TabManager::tabs_`, `TabManager::activeTabIdx_`.
- `TabManager::createTab/closeTab/addInitialTab/setActiveTabIdx/attachLayoutSubtree`.
- `TabManager::findTabForPane/findTabBySubtreeRoot/findTabForNode/findPaneIdByNodeId`
  (replaced by engine-wide terminal map lookups).
- `TabManager::createEmptyTab/activateTabByIdx/createTerminalInContainer/splitPaneByNodeId/closePaneById/focusPaneById`
  (this session's bridge APIs — most vanish since the Tab/Layout layer is gone).
- `TabManager::resizeAllPanesInTab/refreshDividers/clearDividers/releaseTabTextures/updateTabTitleFromFocusedPane/updateWindowTitle/notifyPaneFocusChange`.

### Actions
- `Action::PushOverlay` (currently a TODO stub).
- `Action::PopOverlay` — `closePane` on the overlay node replaces it.
- `Action::ShowScrollback` — **refactored** (not deleted): spawns a Terminal as
  a Stack sibling of the focused tab's content node.

### RenderEngine / RenderSync
- `RenderFrameState::hasOverlay`, `overlay`, `destroyedOverlay`.
- `PendingMutations::CreateOverlayState`, `DestroyOverlayState`.
- `RenderEngine::overlayRenderPrivate_`.
- Overlay-specific render pass in `RenderEngine::renderFrame`.
- Per-tab rendering loop — becomes a tree walk.

### InputController
- ~16 `hasOverlay` branches in `onKey`, `onMouse`, `onMouseMove`, pane delivery.
- `tab->topOverlay()` readers.

### AppCallbacks (ScriptEngine.h)
- `createOverlay`, `popOverlay`, `overlayHasPty`, `overlayInfo`, `overlayGetText`,
  `overlayLineIdAt`, `injectOverlayData`, `writeOverlayToShell`, `pasteOverlayText`,
  `filterOverlayOutput`, `filterOverlayInput`.
- `createTab`, `closeTab` (legacy top-level), `createEmptyTab`, `activateTab`,
  `splitPaneByNodeId`, `createTerminalInContainer` (bridge APIs from this session).
- `OverlayInfo` struct.
- `TabInfo::hasOverlay` field.

### Engine (ScriptEngine.cpp)
- `notifyOverlayCreated`, `notifyOverlayDestroyed`.
- `filterOverlayOutput`, `filterOverlayInput`, `hasOverlayOutputFilters`,
  `hasOverlayInputFilters`, `addOverlayOutputFilter`, `addOverlayInputFilter`.
- `deliverOverlayMouseEvent`, `deliverPopupMouseEvent` for overlays.
- `__evt_overlayCreated`, `__evt_overlayDestroyed` plumbing.
- `cleanupTab` (or gets simpler — becomes nodeId-keyed).

### JS surface
- `mb.layout.createTab`, `mb.layout.closeTab`, `mb.layout.activateTab` (controller
  composes from `createStack` + `appendChild`).
- `mb.layout.closePane` → renamed `mb.layout.removeNode`.
- `mb.layout.createTerminalNode` (dead since composite shipped).
- Tab handles' `hasOverlay`, `createOverlay`, `closeOverlay`.
- `overlayCreated`, `overlayDestroyed` events.

---

## What remains (the native UI kernel, post-refactor)

| Component | Role |
|---|---|
| `Terminal` | PTY + emulator + per-pane render private. Owned by the engine-wide `unordered_map<Uuid, unique_ptr<Terminal>>` keyed by tree nodeId. |
| `TabBar` tree node kind | Reads labels from `boundStack`'s children, renders tab strip. Only other native leaf-content kind. |
| `LayoutTree` + `Node` (Terminal, Container, Stack, TabBar) | Single source of structural truth. Owns rects, active-child, sizing, hierarchy. |
| `RenderEngine` (rewritten) | Walks tree top-down, draws each Terminal at its rect, draws TabBar strip. No per-tab loop. |
| `InputController` (simplified) | Routes input to the focused Terminal via the Stack.activeChild / Container focus chain. |
| `TabManager` (renamed, e.g. `TerminalService`) | Terminal map owner. Provides `spawnTerminal(parentNodeId, cwd)`, PTY poll registry, kill path, graveyard staging. That is all. |
| `Graveyard` | Generic — unchanged. |
| `Uuid` | Unchanged. |
| `Terminal::mExited` + auto-graveyard on EOF | Fires `terminalExited` event. |
| Engine-level state: `focusedTerminalNodeId`, `zoomedNodeId` | Replaces `Layout::mFocusedPaneId`, `Layout::mZoomedPaneId`. |

### New JS primitives
- `mb.layout.killTerminal(idOrNodeId)` — live Terminal only; synchronous
  graveyard; fires `terminalExited`.
- `mb.layout.removeNode(nodeId)` — tree removal. Guard: no descendant Terminal
  has a live object. Works for any kind.
- `mb.layout.setFocus(terminalNodeId)` — sets focus chain to root.
- `mb.layout.focused() → {nodeId, ...} | null`
- `mb.quit()` — app exit.

### Existing primitives that survive
- `mb.layout.createTerminal(parentContainerNodeId, opts)` — but now accepts any
  parent container or Stack. Unchanged shape; the "containerness" of tab-Stacks
  is handled by the controller composing `createStack` + `appendChild`.
- `mb.layout.createContainer/createStack/createTabBar` — unchanged.
- `mb.layout.appendChild/removeChild/replaceChild/setActiveChild/setTabBarStack/setLabel`
  — unchanged.
- `mb.layout.splitPane` — survives. Unchanged semantics.
- `mb.layout.setZoom` — survives. Becomes engine-wide zoom state.
- `mb.layout.setSlotStretch/MinCells/MaxCells/FixedCells` — unchanged.
- `mb.layout.adjustPaneSize` — unchanged.
- `mb.layout.computeRects` — unchanged.

---

## Bundled JS impact

- **`default-ui.js`** — **rewritten**. At startup constructs: root Container
  (vertical) → TabBar + root Stack → one tab Stack → one Terminal. All action
  handlers rewritten to use `createStack`/`appendChild`/`setActiveChild` rather
  than `createTab`/`activateTab`. `terminalExited` handler becomes the full
  cascade (closeNode, quit-if-last-cell-in-last-tab).
- **`command-palette.js`** — unchanged (uses popups, not overlays).
- **`applet-loader.js`** — unchanged.
- **`modules/tui.js`** — unchanged.

---

## Tests impact

- **`test_layout_tree`** (20 cases) — unchanged; likely gains
  overlay-as-Stack-child + zoom-as-tree-property cases.
- **`test_cwd`**, **`test_render`** — rewritten. Currently use `queryStats`
  IPC which walks `TabManager::tabs_`. That vector is gone; `queryStats`
  walks the tree instead. Action-dispatching tests (`sendAction("new_tab")`)
  flow through the controller; assertions become tree-shape assertions.
- **`test_bindings`** — unchanged (tests binding parsing, not dispatch).
- **`test_tabs`** — unchanged (terminal tab-*stops*, not our Tab).
- **`test_terminal`**, others — mostly unchanged; possible fixture tweaks
  where they construct a `Tab` or `Layout` directly.
- **NEW: `test_uuid.cpp`** — round-trip test to regression-guard the
  `toString`/`fromString` pair.

---

## Suggested sequencing (next session)

Order chosen so each step compiles and passes tests before the next. Do not
commit partial steps — push as a branch and iterate.

1. **`test_uuid.cpp`** — round-trip test. Quick.
2. **Engine terminal map + Terminal lifetime refactor.** Move
   `unique_ptr<Terminal>` ownership from `Layout::mPanes` into an engine-wide
   `unordered_map<Uuid, unique_ptr<Terminal>>`. Every current `panes()`
   iteration becomes either a tree walk or an iteration over the map.
   `findPaneIdByNodeId` becomes a simple lookup. Layout's `mPanes` gets
   deleted.
3. **`mb.layout.killTerminal` + synchronous graveyard.** Strips layout work
   from `TabManager::terminalExited`. Terminal death → remove from engine
   map → graveyard → fire event. Default controller's `terminalExited`
   listener calls `removeNode` + quit-check.
4. **`mb.layout.removeNode` (rename from `closePane` + add container guard).**
   Enforce "no live Terminals beneath" invariant. Wire action handlers.
5. **`mb.quit()` primitive.** Trivial binding.
6. **Dissolve `class Tab`.** Title → Stack label (plumb through `setLabel`
   from the Terminal's title callback). Delete `class Tab`. `TabManager`
   shrinks: loses `tabs_`, `activeTabIdx_`, all tab-shaped methods.
7. **Dissolve `class Layout`.** `subtreeRoot` concept moves to the controller
   (it owns the tree shape). Focus state → engine-wide
   `focusedTerminalNodeId`. Zoom → engine-wide `zoomedNodeId`. Divider
   rendering and geometry helpers migrate to free functions over the tree.
   Delete `class Layout`.
8. **Dissolve overlay.** Rewrite `Action::ShowScrollback` to spawn a Terminal
   as a Stack sibling. Delete every `hasOverlay`, `topOverlay`, `mOverlay`,
   `notifyOverlay*`, overlay-shaped `AppCallbacks` field, overlay render
   pass. InputController loses its overlay branches. Render engine loses its
   overlay pass.
9. **Rewrite `default-ui.js`** to construct the root tree at startup
   (Container → TabBar + root Stack + initial tab Stack + first Terminal).
   Update handlers to compose from primitives. Update `terminalExited`
   listener to do the full cascade.
10. **Rewrite renderer pane loop** to walk the tree from root instead of
    iterating `TabManager::tabs_`. Delete the overlay render pass.
11. **Rewrite `InputController`** to route via the tree's focus chain.
12. **Update tests.** `test_cwd`, `test_render`, IPC stats protocol.
13. **Manual GUI smoke** — `build/bin/mb-tests` drives the child mb binary;
    verify new_tab / split / close / tab-switch / overlay (as Stack sibling)
    / shell-exit all work.
14. **Audit against the Done Criteria.** Every item must be green.

Estimated: 3–5 sessions. All changes live on one branch; no partial commits
to main.

---

## Done criteria

The refactor is complete when all of these hold:

1. `grep -rn "class Tab\b" src/` returns nothing.
2. `grep -rn "class Layout\b" src/` returns nothing.
3. `grep -rn "Overlay\|mOverlay\|overlay_" src/` returns nothing (excluding
   third-party).
4. `TabManager::tabs_` and `activeTabIdx_` do not exist.
5. `RenderFrameState` has no `overlay`/`hasOverlay`/`destroyedOverlay` fields.
6. `PendingMutations` has no `*OverlayState` variants.
7. `InputController` has no `hasOverlay` branches.
8. `AppCallbacks` has no `createOverlay`/`popOverlay`/`overlay*` fields.
9. `Engine` has no `notifyOverlay*`/`filterOverlay*`/`deliverOverlayMouseEvent`
   methods, no overlay event plumbing.
10. No callsite reads `tab->layout()->...`.
11. `Action::PushOverlay`, `Action::PopOverlay` removed from `Action.h`.
    `Action::ShowScrollback` spawns a Stack child.
12. `default-ui.js` constructs the full tree at startup including TabBar.
13. Full test suite (`build/bin/mb-tests`) passes.
14. Manual GUI smoke passes: create tab, split, close pane (keybind), close
    tab, switch tab, shell-exit (`exit` in shell), show scrollback, command
    palette open/close.

When all 14 are green → merge. If any is yellow → keep going. No half-ship.

---

## Known gotchas for next session

- **`Uuid::toString()`** — already fixed but has no regression test. Step 1
  adds it.
- **`mb.actions !== mb.actions`** — the getter re-builds each call. Scripts
  that care about identity must capture into a local. If this becomes a
  hot-path issue, cache the array on the context and invalidate on
  `registeredActions_` change.
- **`LayoutTree::setActiveChild` on empty Stack** — when a tab closes its
  last content node, need to decide: does the Stack's `activeChild` clear,
  or does it stay dangling? Today's `removeChild` has logic for this. Verify
  it still holds under the new "empty tab is allowed" invariant.
- **TabBar rendering** — today assumes a tab-level abstraction. Under the
  new model, TabBar reads labels from its bound Stack's children. Currently
  reads them from `Tab::title()`. That path goes through `Tab` and will need
  to be redirected to `node->label` on the Stack child.
- **Destruction order** — `Script::Engine` must outlive `TabManager` (the
  tree outlives the terminal map). Currently enforced by member order in
  `PlatformDawn`. When `TabManager` is reduced to just the Terminal map,
  this still needs to hold (terminals reference the tree indirectly via their
  nodeId being looked up; direct references may not exist, but graveyard
  ordering matters).
- **Render thread mutex** — the current shadow-copy pattern (snapshot state
  under `renderThread_->mutex()` before each frame) stays valid; just
  snapshotted against the tree instead of `tabs_`.
- **Exit code / signal** not plumbed on `terminalExited`. Child PID not
  tracked in `Terminal`. Current payload is `{paneId, paneNodeId}` only.
  Add later if wanted.
- **`FocusPopup` action** still native (not migrated). Pop it into JS only
  after a popup primitive surface exists on `mb.layout.*`.

---

## One-paragraph summary

After this cutover, the only native UI abstractions are `Terminal` (PTY +
emulator) and `TabBar` (strip-rendered tree leaf). Everything else — what a
"tab" is, what an "overlay" is, which tab is active, what's zoomed, how splits
are arranged, what happens when a shell exits — is expressed as tree shape and
driven by JS. Native's job is: own Terminal PTY lifecycle, walk the tree to
render, route input to the focused Terminal node. The 14 Done Criteria above
are the contract.

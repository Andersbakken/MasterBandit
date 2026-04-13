# Render Threading & Snapshot Architecture

Design for moving rendering off the event-loop thread and decoupling VT parsing from frame rendering.

## Motivation

Today, MasterBandit runs PTY reads, VT parsing, worker-pool-driven row shaping, and GPU command submission on a single event loop thread. Each tick calls `flushReadBuffer()` (parse) then `renderFrame()` (render) sequentially (`Platform_EventLoop.cpp:420`, `:426`). A slow frame stalls parse progress; a burst of parse work delays the next frame. Under output floods (`yes`, `cat largefile`, progress-bar-heavy builds) this couples throughput and latency in both directions.

Goal: dedicate a thread to rendering, drive it via wakeups from the parser thread, and let the two make independent progress under their own backpressure mechanisms.

Reference implementation studied: Ghostty (`src/renderer/Thread.zig`, `src/terminal/render.zig`, `src/renderer/generic.zig`). Ghostty's design — mutex-guarded Terminal + per-frame snapshot into a render-owned `RenderState` + semaphore-gated swap chain — is the pattern adopted here.

## Current State Summary

From the coupling audit:

- **No synchronization on `Terminal` or `TerminalEmulator`.** Single-threaded assumption throughout.
- **`Cell` is POD, 12 bytes**, memcpy-friendly (`CellTypes.h:93`). `CellExtra` likewise.
- **Per-row dirty bits already exist** (`CellGrid::dirty_`, `Document::dirty_`, `markRowDirty()` in `Document.cpp:203`).
- **Renderer reads are localized to `Platform_Render.cpp`** — ~35 sites touching cells, cursor, colors, grid, image registry, extras.
- **`flushReadBuffer()` is a natural parse/render seam** — parser fully drains before render runs.
- **WorkerPool already reads live Terminal state** from worker threads during `resolveRow()`. Today this is safe because the main thread is blocked in `dispatch()` during that window; post-refactor, workers read from the snapshot.
- **`FontData` uses `std::shared_mutex`** (`text.cpp:82`) — precedent for the reader/writer pattern.
- **Scripts (QuickJS) run synchronously on the event loop thread** and call `Terminal::injectData()` / `paneCommands()` directly.
- **`Document` has monotonic, reflow-stable row IDs** — usable as stable keys for image placements.

## Target Architecture

Two threads per process (not per terminal — multi-pane shares both):

**Event loop thread** (existing): PTY reads, VT parsing, script execution, IPC, input event dispatch, resize intake. Owns the canonical `Terminal` / `TerminalEmulator` / `Document` / `CellGrid`.

**Render thread** (new): Snapshot capture, cell shaping via WorkerPool, GPU command build + submit, atlas upload, animation frame advance. Owns per-frame GPU resources, the image registry, and the font atlas state.

Communication:

- Event loop → render: `renderWakeup.notify()` after `flushReadBuffer()` finishes parsing a chunk. Edge-triggered; many notifies between frames collapse to one snapshot.
- Render → event loop: none on the hot path. Render thread may post input-focus/cursor-visibility hints via the existing event loop's cross-thread post mechanism (if one exists; otherwise add one).
- Parser → render data channel (images): SPSC message queue with move-only payloads. See §Image Registry.

### Why Not a Separate Write Thread

Ghostty uses three threads per terminal — read, write, render. MasterBandit adopts the snapshot/render split but deliberately does **not** split read and write. Rationale:

- **Ghostty's "write thread" is misnamed.** Reading `src/termio/Thread.zig:1-10`, its real job is housekeeping that isn't parsing: PTY writes, resize coalescing (25 ms), sync-output reset timer (1000 ms), selection-scroll timer (15 ms), message coalescing. The thread comment "offload as much from the reader thread as possible since it is the hot path" is about keeping housekeeping off the parse thread, not isolating writes from reads.
- **PTY writes are tiny and non-blocking.** Keystrokes are bytes; bracketed-paste of a large file is bounded by pipe capacity and drains in microseconds. `write(fd, buf, n)` on a non-blocking PTY master doesn't meaningfully block, and the event loop already handles EAGAIN via its fd watches.
- **The housekeeping timers already live on our event loop.** Resize coalescing, sync-output timers, selection-scroll — natural event-loop work in MasterBandit. There's no third concern to peel off.
- **The throughput-blocking interaction is parse vs. render.** That's what the snapshot solves. PTY writes aren't on that path; adding a third thread buys nothing for the cost of another message queue and more race surface.

If profiling ever shows PTY write latency hurting input responsiveness during a parse burst — unlikely — split at that point. Until then, two threads.

## Terminal Mutex & Snapshot

### Mutex

One `std::mutex` on `Terminal`. Held by:
- Parser thread during `injectData()` and related mutation paths.
- Render thread only during `TerminalSnapshot::update()` — a viewport-sized memcpy plus scalar field reads. Sub-millisecond.

Rationale (vs. a diff/message-stream design): VT state is too broad to serialize — palette, modes, scroll region, alt screen, tab stops, selection, hyperlinks, DEC flags, OSC title, scrollback growth. A diff protocol forces two authoritative representations and special cases for resize, alt-screen, and scroll-jump that snapshotting handles for free. The mutex critical section runs at frame cadence (not per parsed byte); under realistic workloads the parser is never meaningfully blocked by the renderer.

Rejected alternatives:
- **Seqlock / lock-free snapshot.** Complex and still needs a coherent "what constitutes a snapshot" definition.
- **Double-buffered Terminal.** Doubles memory, awkward with scrollback and archival.

### Snapshot Type

```cpp
struct TerminalSnapshot {
    // Dimensions
    int rows, cols;
    int viewportOffset;             // for scrollback position
    uint64_t firstVisibleRowId;     // stable row ID at viewport row 0
    // (remaining row IDs = firstVisibleRowId + y; or stored per-row)

    // Cursor
    int cursorX, cursorY;
    CursorShape cursorShape;
    bool cursorVisible, cursorBlinking, passwordInput;

    // Colors
    DefaultColors defaults;
    std::array<Color, 256> palette;
    bool reverseVideo;

    // Cells (viewport-only, row-major)
    std::vector<Cell> cells;            // size = rows * cols, POD memcpy
    std::vector<uint8_t> rowDirty;      // size = rows; 1 if re-copied this frame

    // Per-row extras, only for dirty rows (others carry prior state)
    struct RowExtras {
        uint64_t rowId;
        std::vector<std::pair<int, CellExtra>> extras;   // (col, extra), sorted
    };
    std::vector<RowExtras> rowExtras;   // size = rows

    // Selection / highlights (cheap, compute on every snapshot)
    std::optional<Selection> selection;
    std::vector<HighlightRange> hyperlinkHover;

    // Sync output gate
    bool syncOutputActive;

    // Version / cookie for debugging
    uint64_t version;
};
```

### Snapshot Update Path

`TerminalSnapshot::update(const Terminal&)` runs under the terminal mutex. It:

1. Checks global dirty bits: terminal dirty flag, screen dirty flag, dimensions change, viewport pin change. Any of these → full re-copy.
2. Copies cheap fields unconditionally (cursor, colors, palette, mode flags).
3. Walks viewport rows; for each row:
   - If not globally dirty AND `rowDirty[y]` is clear AND the row's underlying dirty flag is clear → skip (retain last frame's data for this row).
   - Else: `memcpy` the row's cells (`fastmem`-style), rebuild `rowExtras[y]` from the live `unordered_map<int, CellExtra>` as a sorted vector, mark `rowDirty[y]`.
4. Clears the row's live dirty flag so the next frame won't re-copy unchanged rows.
5. Recomputes selection and hyperlink-hover ranges (cheap, not dirty-tracked).

Memory discipline: the snapshot is retained across frames (not reallocated). `std::vector` capacity is reused via `resize()` + assignment. No allocator traffic in the steady state.

### Scroll Handling

Any change to `firstVisibleRowId` forces a full re-copy (every row is now different content). No incremental shift-and-fill optimization. The CPU cost is a viewport-sized memcpy — tens of KB, one cache-friendly pass. The GPU has to repaint anyway, so the incremental version buys nothing observable.

### Synchronized Output (Mode 2026)

If `syncOutputActive` is true at snapshot time, `update()` returns early without touching row data. The render thread re-presents the prior frame instead of rebuilding. This makes mode 2026 behave correctly with zero extra plumbing.

## Image Registry (Kitty Graphics)

The image registry moves entirely to the render thread. The parser retains a metadata-only side index for protocol responses.

### Why Move It

- PNG decode + RGBA buffers are the heaviest payloads in the system. Moving them across a channel via `std::move` avoids holding the terminal mutex through GPU upload.
- GPU textures already live on the render side; the registry is naturally adjacent.
- `tickAnimations()` belongs on the render thread (frame-cadence advance, not per-parsed-byte).
- Frame composition (`a=c`) is pixel-level work native to the render thread.

### Ownership Split

**Parser-side** (`ImageIndex`, lives in `TerminalEmulator`):
```cpp
struct ImageMeta {
    int width, height;
    int frameCount;
    bool hasAnimation;
    // No pixels, no GPU handles, no placements.
};
std::unordered_map<uint32_t /*imageId*/, ImageMeta> index_;
```
Maintained in lockstep with sent messages. Used to answer synchronous kitty protocol queries (`q=1` responses, duplicate-ID detection, error codes). Cheap, never copied across threads.

**Render-side** (`ImageRegistry`):
```cpp
struct Frame {
    std::vector<uint8_t> rgba;   // or std::unique_ptr<uint8_t[]> + size
    wgpu::Texture texture;        // lazily uploaded on first visible placement
    bool textureDirty;
};
struct Image {
    int width, height;
    std::vector<Frame> frames;
    AnimationState anim;          // current frame index, timing, loop mode
};
struct Placement {
    uint32_t imageId, placementId;
    uint64_t rowId;               // stable, reflow-surviving
    int startCol;
    CropRect crop;
    int zIndex;
    float offX, offY;             // sub-cell pixel offset
};

std::unordered_map<uint32_t, Image> images_;
std::unordered_map<uint64_t /*rowId*/, std::vector<Placement>> placementsByRow_;
```

Indexing placements by `rowId` makes per-frame visibility O(viewport_rows): walk the snapshot's visible row IDs, look each up, collect placements. Evicted/reflow-dropped rows drop their bucket.

### Message Protocol (Parser → Render)

Single SPSC queue, ordering matters across message types:

```cpp
struct MsgImageUpload {
    uint32_t id;
    int w, h, frameCount;
    PixelFormat fmt;
    std::vector<uint8_t> data;    // moved
};
struct MsgFrameLoad {
    uint32_t id;
    int frameIndex;
    std::vector<uint8_t> data;    // moved
    std::optional<ComposeSpec> compose;
};
struct MsgImagePlacement {
    uint32_t imageId, placementId;
    uint64_t rowId;
    int startCol;
    CropRect crop;
    int zIndex;
    float offX, offY;
};
struct MsgImageDelete {
    enum Kind { ById, ByIdRange, ByPosition, ByZ, ByZRange, ByRowId, All };
    Kind kind;
    uint32_t a, b;                // variant args
    bool freeData;                // 'd=i' vs 'd=I'
};
struct MsgFrameCompose {
    uint32_t id;
    int srcFrame, dstFrame;
    Rect region;
    BlendMode mode;
};
struct MsgAnimControl {
    uint32_t id;
    AnimAction action;            // play / pause / stop / set_loop
};
struct MsgRowEvicted {
    uint64_t rowId;               // dropped from scrollback; drop placements on it
};
```

Ordering is required across categories (`upload id=5 → place id=5 → delete id=5` must arrive in that order). Use a single queue, not per-category queues. `moodycamel::ReaderWriterQueue` or a plain mutex+deque at this rate.

### Reflow Semantics

Match kitty's behavior: delete placements on reflow. When `Document::resize()` runs, the parser sends `MsgImageDelete{All}` (or a batch of `MsgRowEvicted` for affected rows). Sidesteps any reasoning about row ID stability across reflow.

### Snapshot Integration

`CellExtra` in the snapshot still carries `imageId` / `imagePlacementId`. The render thread, when rebuilding the visible image set, iterates `snapshot.firstVisibleRowId .. +rows`, queries `placementsByRow_`, and cross-references with the cell grid for sub-cell parameters. No image data appears in the snapshot.

## GPU Frame Pacing (WebGPU / Dawn)

### Default: `PresentMode::Fifo` + `queue.writeBuffer`

Use `PresentMode::Fifo` and let Dawn manage everything else. No per-frame resource rotation, no semaphore, no explicit pacer.

- Vsync is provided by Fifo.
- CPU-GPU backpressure is provided by `GetCurrentTexture()` blocking when all swap chain images are in flight. Depth is driver-chosen: 2 on Vulkan (`SwapChainVk.cpp:73-76`), 3 on Metal (`CAMetalLayer` default drawable count, `SwapChainMTL.mm:87` — "There is no way to control Fifo vs. Mailbox in Metal").
- Per-frame CPU buffers (uniform, cell instance) are uploaded via `queue.writeBuffer()`, which stages internally through Dawn's own ring buffer. You don't need to track which GPU frame is reading which CPU buffer — Dawn does.

Single `wgpu::Buffer` per purpose, reused each frame:

```cpp
wgpu::Buffer uniformBuffer_;        // created once, resized on viewport change
wgpu::Buffer cellInstanceBuffer_;   // created once, grown on grid resize
// No FrameState[N], no FramePacer.
```

### Frame Loop Sketch

```cpp
void RenderThread::run() {
    while (!stop_) {
        wakeup_.wait();                      // parser-side notify, edge-triggered

        {
            std::lock_guard lk(terminal_.mutex());
            if (!snapshot_.update(terminal_)) {
                // sync output active — skip this frame entirely
                continue;
            }
        }

        drainImageMessages(registry_);
        advanceAnimations(registry_);
        syncAtlasIfDirty();                  // atlas counter check

        auto tex = surface_.GetCurrentTexture();  // may block on Fifo backpressure
        if (tex.status != SurfaceGetCurrentTextureStatus::SuccessOptimal) {
            // handle Outdated/Lost/Timeout as appropriate
            continue;
        }

        buildInstanceBuffer(snapshot_, registry_);  // dispatched to WorkerPool
        queue_.WriteBuffer(cellInstanceBuffer_, 0, instances.data(), instances.size());
        queue_.WriteBuffer(uniformBuffer_, 0, &uniforms, sizeof(uniforms));
        auto cb = encode(tex);
        queue_.Submit(1, &cb);
        surface_.Present();
        frameIdx_++;
    }
}
```

Simpler, no Dawn event-loop discipline, works identically on both backends.

### Fallback: Explicit `FramePacer` if profiling demands it

Only adopt if you later find reason to skip the `writeBuffer` staging copy (e.g., persistent-mapped instance buffers to save a memcpy, or the need for speculative frame prep that begins before `GetCurrentTexture` returns). For a terminal at typical grid sizes (400 KB per frame for a 200×60 grid), the staging copy is sub-millisecond and not worth the complexity.

If needed, the explicit approach is `FrameState[N]` rotated per frame, gated by either:

**Future + `WaitAny`** (recommended over `AllowSpontaneous` for portability):

```cpp
struct FrameSlot {
    wgpu::Buffer uniform, instance;
    wgpu::Future done{};   // from previous use of this slot
};
FrameSlot slots_[3];

void renderFrame() {
    auto& slot = slots_[frameIdx_ % 3];
    if (slot.done.id != 0) {
        wgpu::FutureWaitInfo w{slot.done};
        instance_.WaitAny(1, &w, UINT64_MAX);
    }
    // ... write to slot.uniform/instance, build cb ...
    queue_.Submit(1, &cb);
    slot.done = queue_.OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::QueueWorkDoneStatus, wgpu::StringView){});
    frameIdx_++;
}
```

Requires enabling the `TimedWaitAny` instance feature (`webgpu_cpp.h:435`) at instance creation with `InstanceLimits::timedWaitAnyMaxCount >= 1`.

**Why not `AllowSpontaneous` + `std::counting_semaphore`:** works on Metal (Metal's own completion thread fires callbacks spontaneously, `QueueMTL.mm:236`) but on Vulkan Dawn has no background fence poller — the callback only fires when something calls `device.Tick()` / `Instance::ProcessEvents()` / `WaitAny`. Requires explicit tick discipline on the render thread. `WaitAny` side-steps this by being the trigger itself.

### Backend Asymmetry Note

`PresentMode::Fifo` gives 2 images in flight on Vulkan vs. 3 on Metal, and there is no Dawn API to control the depth. In practice this is imperceptible for terminal workloads (sub-ms CPU prep per frame, no GPU bottleneck) but is worth knowing when comparing profiler traces across platforms. If the asymmetry ever becomes load-bearing — e.g., a bug where Linux can only pipeline 2 frames and hits a stall Mac doesn't — the explicit `FramePacer` path gives consistent `N`-deep behavior.

## Atlas Dirty Tracking

`FontData` already has `std::shared_mutex mutex_`. Add an atomic monotonic counter:

```cpp
class FontData {
    std::atomic<uint64_t> modified_{0};
public:
    void addGlyph(...) {
        std::lock_guard lk(mutex_);
        // ... existing atlas update ...
        modified_.fetch_add(1, std::memory_order_release);
    }
    uint64_t version() const { return modified_.load(std::memory_order_acquire); }
};
```

Render thread, per frame: compare `font_.version()` to `fs.atlasVersionSeen`. If higher, take `std::shared_lock` on the font mutex, upload changed regions (or reallocate the GPU texture if the atlas grew), update `fs.atlasVersionSeen`. No per-glyph dirty list.

## Resize Coalescing

Resize events arrive via window-system callbacks on the event loop thread (possibly multiple per frame during drag-resize). Coalesce with a short timer (Ghostty uses 25 ms) before applying to `Terminal` — saves redundant reflow work and avoids triggering a snapshot per pixel of drag.

## Script / IPC Coordination

Scripts (QuickJS) execute on the event loop thread. `Terminal::injectData()` and `paneCommands()` are called from script callbacks. This is safe post-refactor as long as script execution remains on the event loop thread — which it already does (`Platform_EventLoop.cpp:422`).

Rules:
- All mutating calls on `Terminal` happen under the terminal mutex. Script callbacks that mutate (e.g., `injectPaneData`) acquire the mutex internally.
- Script reads of Terminal state (e.g., `paneCommands`) also acquire the mutex briefly.
- IPC (`mb --ctl`) does not directly read Terminal state today. Keep it that way, or route any future reads through the mutex.

## Phasing

Order matters — each phase leaves the codebase shippable.

1. **Snapshot type + mutex, still single-threaded.** Introduce `TerminalSnapshot` and route all renderer reads through it. Add `std::mutex` on `Terminal`; acquire in mutation paths and in `TerminalSnapshot::update()`. Still single-threaded so uncontended. Combining these means the "touch Terminal → either mutate under lock or read via snapshot" discipline is enforced from day one, and the audit for missed access sites completes in one pass. Ship.
2. **Image message channel, still single-threaded.** Move image registry to renderer-owned; parser uses SPSC message queue drained at start of `renderFrame()`. Parser-side metadata-only side index for kitty protocol queries. Ship.
3. **Split render thread.** Actually run `renderFrame()` on its own thread driven by `renderWakeup.notify()` from `flushReadBuffer()`. The mutex now serializes parser vs. render reads; the message queue serializes image mutations. Worker pool calls from render thread read from snapshot. Default GPU path: `PresentMode::Fifo` + `queue.writeBuffer` — no explicit frame pacer. Ship.
4. **Atlas dirty counter.** Atomic monotonic counter on `FontData`; render thread compares per frame and uploads under existing `shared_mutex`. Ship.
5. **Animations to render thread.** Move `tickAnimations()` off parser thread, driven by frame cadence. Ship.
6. **Polish.** Resize coalescing (25 ms), sync-output early-exit, any remaining hot paths.
7. **(Conditional) Explicit frame pacer.** Only if profiling after phase 3 shows the default `Fifo + writeBuffer` path is inadequate. Adopt `FrameState[N]` + Future-based `WaitAny` (see §GPU Frame Pacing fallback).

Each phase should be benchmarked — `vtebench` / `yes`-flood / `cat largefile` against the prior phase. Regressions at any phase indicate a missed coupling or a lock held too long.

## Open Questions

- **Input event threading.** Mouse/keyboard arrive via window-system callbacks on the event loop thread today. Post-refactor, do they still dispatch there and mutate `Terminal` under the mutex, or does rendering need any input state? Current answer: stay on event loop thread, mutate under mutex, renderer sees the result via snapshot. No change needed.
- **Selection updates during drag.** Selection is updated on input events (event loop thread). Renderer sees it via snapshot. Is there any latency concern for "selection visibly lags cursor during fast drag"? Probably not — snapshot cadence is frame-rate — but flag if it surfaces.
- **Multi-pane ownership.** One render thread serving N panes, or N render threads? Start with one (simpler resource sharing, especially for atlas). Revisit if contention shows.
- **Clipboard paths.** `term->selectedText()` is called from input/clipboard handlers on the event loop thread. Safe under mutex, no change.
- **Debug IPC screenshot.** Needs to wait for the next complete frame. Probably a `std::promise`/`std::future` round-trip to the render thread. Minor.

## Estimated Effort

Based on the coupling audit:

- Phase 1 (snapshot + mutex, single-threaded): 4-6 days
- Phase 2 (image message channel): 3-5 days
- Phase 3 (render thread split + Fifo GPU path): 2-3 days, plus 1-2 weeks race debugging
- Phase 4 (atlas counter): 0.5 day
- Phase 5 (animations): 1 day
- Phase 6 (polish): 1-2 days
- Phase 7 (explicit frame pacer, conditional): 1 day if needed, likely not needed

Total: ~2-4 weeks focused effort. The row-dirty tracking and POD cell layout are already in place, and the `flushReadBuffer` → `renderFrame` seam provides a natural mutex boundary — these significantly reduce the scope vs. a greenfield threading refactor. Dropping the explicit frame pacer and relying on Dawn's default `Fifo + writeBuffer` path saves code and removes the Vulkan `Tick()` discipline concern.

## References

- Ghostty's snapshot design: `src/terminal/render.zig` (`RenderState.update`, viewport-only + row-dirty-gated copy with retained-capacity arenas).
- Ghostty's render thread: `src/renderer/Thread.zig` (xev async wakeup, no artificial frame pacing beyond swap chain).
- Ghostty's frame pacing: `src/renderer/Metal.zig` + generic swap chain logic (3-deep swap chain, semaphore on GPU completion).
- Dawn callback modes: `webgpu_cpp.h:158-164`.
- Dawn `OnSubmittedWorkDone`: `webgpu_cpp.h:1948`.
- Dawn `Instance::WaitAny`: `webgpu_cpp.h:1896-1898` (requires `TimedWaitAny` instance feature for non-zero timeouts).

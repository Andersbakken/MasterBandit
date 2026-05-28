#pragma once

// Render-thread synchronization types.
//
// Data flow: main thread accumulates mutations in PendingMutations (no lock),
// then applies them in a single batch into RenderFrameState under
// platformMutex_. The render thread reads RenderFrameState under the same
// mutex during its snapshot phase, copies what it needs, releases the mutex,
// and does all GPU work lockfree using PaneRenderPrivate (render-thread-only).

#include "ComputeTypes.h"
#include "Renderer.h"
#include "Terminal.h"
#include "TerminalSnapshot.h"
#include "TexturePool.h"
#include "Uuid.h"

#include <dawn/webgpu_cpp.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// RenderFrameState — shadow copy of tab/pane structure for the render thread.
// Written by applyPendingMutations() on the main thread under platformMutex_.
// Read by renderFrame() on the render thread under the same mutex.
// ---------------------------------------------------------------------------

struct RenderPanePopupInfo
{
    std::string id;
    int cellX = 0, cellY = 0, cellW = 0, cellH = 0;
    TerminalEmulator *term = nullptr; // raw ptr into popup Terminal child
};

// Embedded terminal shadow-copy entry. Unlike popups (which carry cell
// coordinates captured at createPopup time), embeddeds are anchored to a
// logical line id; their viewport-row position is resolved each frame from
// the parent's Document so the embedded scrolls with the content.
struct RenderPaneEmbeddedInfo
{
    uint64_t lineId        = 0;
    int rows               = 0;       // embedded's row count (cols = parent cols)
    TerminalEmulator *term = nullptr; // raw ptr into embedded Terminal child
    bool focused           = false;
    // Viewport row position is resolved each frame by
    // TerminalSnapshot::update via Terminal::collectEmbeddedAnchors → the
    // segment list — no caching needed on the shadow-copy side.
};

struct RenderPaneInfo
{
    Uuid id;
    Rect rect;
    TerminalEmulator *term = nullptr; // raw ptr — valid while tab/pane exist
    int progressState      = 0;
    int progressPct        = 0;
    std::vector<RenderPanePopupInfo> popups;
    std::string focusedPopupId;
    // Embedded children. The composite only ever sees Embedded segments
    // when the parent is on main screen — TerminalSnapshot::update drops
    // them under alt — but `onAltScreen` is still used to skip the
    // per-embedded RenderTarget construction phase so we don't acquire
    // textures that would never be composited.
    std::vector<RenderPaneEmbeddedInfo> embeddeds;
    uint64_t focusedEmbeddedLineId = 0;
    bool onAltScreen               = false;
};

struct RenderTabInfo
{
    std::string title;
    std::string icon;
    Uuid focusedPaneId;
    int progressState = 0; // from focused pane
    int progressPct   = 0; // from focused pane
};

struct DividerGeom
{
    float x = 0, y = 0, w = 0, h = 0;
    float r = 0, g = 0, b = 0, a = 0;
    bool valid = false;
};

// Pre-computed tab bar cell: one per column in the tab bar row.
struct TabBarCell
{
    std::string ch; // UTF-8 character to render in this column
    uint32_t fgColor = 0;
    uint32_t bgColor = 0;
};

// Per-TabBar-node render data. The frame state holds a vector of these,
// one per TabBar node in the layout that has a non-empty rect this frame
// (i.e. every currently-visible bar — root-level primary plus any
// sub-bars created via wrapInStack inside an active top-level tab).
//
// Each bar carries its own tabs list (synthesized from the bar's bound
// Stack's children for sub-bars, or the global tabs list for the root)
// plus the cell layout that the GPU pass consumes. Texture handles live
// in the render engine, keyed by `id`.
struct TabBarRender
{
    Uuid id;                         // TabBar node uuid (key)
    Rect rect;                       // pixel rect this frame
    std::vector<RenderTabInfo> tabs; // per-tab metadata
    int activeTabIdx  = -1;          // index into `tabs`
    int draggedTabIdx = -1;          // index into `tabs`, -1 = no active drag in this bar
    std::vector<TabBarCell> cells;   // one per column, post-layout
    int cols = 0;
    std::vector<std::pair<int, int>> colRanges; // per-tab [startCol, endCol)
    bool dirty = true;

    // Drag-to-reorder floating overlay. Populated by buildBarCells when
    // draggedTabIdx >= 0: the dragged tab's actual cells (text + colors), the
    // pixel X within the bar where the float should render, and the float's
    // pixel width. The strip's `cells` for that tab's slot are bg-filled
    // (visually empty); these draggedFloat* fields drive a second
    // renderToPane dispatch in RenderEngine::renderTabBar that stamps the
    // float on top at draggedFloatX.
    std::vector<TabBarCell> draggedFloatCells;
    float draggedFloatX  = 0.0f; // bar-relative pixel X for the float's left edge
    int draggedFloatCols = 0;    // number of cells in draggedFloatCells
};

struct RenderFrameState
{
    // Active tab's panes (in iteration order)
    std::vector<RenderPaneInfo> panes;
    Uuid focusedPaneId;

    // One entry per currently-visible TabBar node. The root-level primary
    // bar (if visible) plus any sub-bars inside the active top-level tab.
    // Built by PlatformDawn each frame; consumed by RenderEngine.
    std::vector<TabBarRender> tabBars;

    // Layout geometry
    uint32_t fbWidth = 0, fbHeight = 0;
    std::string tabBarPosition;

    // Font metrics (copied from main thread scalars)
    float charWidth = 0, lineHeight = 0, fontSize = 0;
    float padLeft = 0, padTop = 0, padRight = 0, padBottom = 0;

    // Visual state
    float activeTint[4]          = { 1.0f, 1.0f, 1.0f, 1.0f };
    float inactiveTint[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Window-level default bg color (RGBA8, A in MSB) and opacity scalar.
    // The clear values used by the pane and swapchain passes are derived
    // from these as `premultiplied = (R*a, G*a, B*a, a)` so the surface
    // alpha-channel value matches what the compositor expects.
    uint32_t defaultBgColor      = 0xFF000000u;
    float backgroundOpacity      = 1.0f;
    // Coarse "rebuild every bar in `tabBars` this frame" signal. Per-bar
    // dirty tracking would let us skip unchanged bars, but in the common
    // case (1-2 bars, only the focused/active changing) it's not worth
    // the bookkeeping — the cell build is microseconds per bar.
    bool tabBarsDirty            = true;
    bool dividersDirty           = true;
    bool focusChanged            = false;
    bool surfaceNeedsReconfigure = false;
    bool windowHasFocus          = true;
    float cursorBlinkOpacity     = 1.0f;

    // Tab bar config values needed by renderTabBar
    uint32_t tbBgColor = 0, tbActiveBgColor = 0, tbActiveFgColor = 0;
    uint32_t tbInactiveBgColor = 0, tbInactiveFgColor = 0;
    uint32_t tbDragBgColor = 0, tbDragFgColor = 0;
    float progressColorR = 0, progressColorG = 0.6f, progressColorB = 1.0f;
    float progressBarHeight  = 3.0f;
    bool progressBarEnabled  = false;
    bool progressIconEnabled = false;
    int maxTitleLength       = 0;

    // Per-pane divider geometry (keyed by the divider's owner Terminal Uuid).
    std::unordered_map<Uuid, DividerGeom, UuidHash> dividerGeoms;

    // Font atlas change flags (render thread does the GPU work)
    bool mainFontAtlasChanged   = false;
    bool tabBarFontAtlasChanged = false;
    bool mainFontRemoved        = false;
    bool tabBarFontRemoved      = false;
    bool viewportSizeChanged    = false;

    // Window in live resize — render thread defers SIGWINCH
    bool inLiveResize = false;

    // Texture / cache release requests for the next frame. One-shot:
    // consumed and cleared by snapshotUnderLock so the render thread acts
    // on each request exactly once.
    std::vector<Uuid> releasePaneTextureIds;
    std::vector<std::string> releasePopupTextureKeys;
    std::vector<std::string> releaseEmbeddedTextureKeys;
    bool releaseAllPaneTextures = false;
    // Per-bar texture release. `releaseAllTabBarTextures` covers viewport
    // / framebuffer-size changes that invalidate everything; the vector
    // covers individual TabBar nodes that disappeared from the layout.
    std::vector<Uuid> releaseTabBarTextures;
    bool releaseAllTabBarTextures = false;
    bool invalidateAllRowCaches   = false;

    // Structural destroys accumulated from main-thread pane/popup/embedded
    // destruction. The render thread erases the matching render-private
    // entries and releases their GPU resources. One-shot, cleared by
    // snapshotUnderLock.
    std::vector<Uuid> destroyedPaneIds;
    std::vector<std::string> destroyedPopupKeys;
    std::vector<std::string> destroyedEmbeddedKeys;

    // Divider appearance
    float dividerWidth = 0;
    float dividerR = 0, dividerG = 0, dividerB = 0, dividerA = 0;

    // OSC 133 selected-command outline color, packed RGBA8 (ABGR byte order
    // matching compute shader's unpacking of selection_outline_color).
    uint32_t commandOutlineColor = 0xFFAACCFFu;
    // OSC 133 dim factor for non-selected rows (0 = disabled; 0.4 typical).
    float commandDimFactor       = 0.0f;

    // Font names (for GPU atlas ops and shaping)
    std::string fontName;
    std::string tabBarFontName;
    float tabBarFontSize   = 0;
    float tabBarCharWidth  = 0;
    float tabBarLineHeight = 0;

    // Content scale
    float contentScaleX = 1.0f;

    // Tab bar animation frame counter (snapshot of main-thread counter)
    int tabBarAnimFrame = 0;
};

// ---------------------------------------------------------------------------
// PaneRenderPrivate — render-thread-only state per pane/popup.
// No lock needed — only the render thread touches these.
// ---------------------------------------------------------------------------

struct PaneRenderPrivate
{
    TerminalSnapshot snapshot;
    std::vector<ResolvedCell> resolvedCells;
    std::vector<GlyphEntry> glyphBuffer;
    uint32_t totalGlyphs = 0;

    int lastCursorX = -1, lastCursorY = -1;
    bool lastCursorVisible       = true;
    int lastCursorShape          = -1;
    float lastCursorBlinkOpacity = 1.0f;
    bool lastHasPopupFocus       = false;

    std::unordered_set<uint32_t> lastVisibleImageIds;
    PooledTexture *heldTexture = nullptr;
    std::vector<PooledTexture *> pendingRelease;

    wgpu::Buffer dividerVB;

    struct PopupBorder
    {
        std::string popupId;
        int cellX = 0, cellY = 0, cellW = 0, cellH = 0;
        wgpu::Buffer top, bottom, left, right;
    };

    std::vector<PopupBorder> popupBorders;

    // Content-level viewport-shift detection. Tracked as the line id at
    // viewport row 0: changes iff viewport row 0 now shows different
    // content (user scroll, live-tail roll). Scroll-back pinning (content
    // streams while user is scrolled back) leaves topLineId unchanged
    // because the visible abs rows don't change — so we correctly SKIP
    // invalidating the per-row shape caches in that case, unlike the
    // legacy `viewportOffset`-based heuristic which over-invalidated.
    uint64_t lastTopLineId           = 0;
    // Decoration overlay change-detection — drives selection / command-region
    // / hyperlink / user-decoration repaint. Single hash collapses what used
    // to be lastSelection + lastSelectedCommand comparisons.
    uint64_t lastDecorationsHash     = 0;
    uint32_t lastCommandOutlineColor = 0;

    struct RowGlyphCache
    {
        std::vector<GlyphEntry> glyphs;
        std::vector<std::pair<uint32_t, uint32_t>> cellGlyphRanges;
        std::vector<Renderer::ColrDrawCmd> colrDrawCmds;
        std::vector<Renderer::ColrRasterCmd> colrRasterCmds;
        bool valid         = false;
        // Shape-cache key. Hit iff shapeHash == snap.rowShapeHash[row] AND
        // fontSize / fontEpoch / pane pixel origin match the current frame's.
        // Mismatch forces a re-shape; match lets the worker skip pass 2
        // entirely (no font.mutex traffic, no glyph map lookups, no shaper
        // calls). pixelOriginX/Y are baked into the cached colrDrawCmds'
        // absolute coordinates, so a pane move must invalidate the cache —
        // typically rare and usually accompanied by rs.dirty anyway.
        uint64_t shapeHash = 0;
        float fontSize     = 0.0f;
        uint64_t fontEpoch = 0;
        float pixelOriginX = 0.0f;
        float pixelOriginY = 0.0f;
    };

    std::vector<RowGlyphCache> rowShapingCache;

    // Bumped when something invalidates every cached glyph for this pane
    // (font atlas compaction, full-cache-invalidate path). Compared against
    // RowGlyphCache::fontEpoch on each frame.
    uint64_t fontEpoch = 0;

    bool dirty = true;
};

// ---------------------------------------------------------------------------
// PendingMutations — main-thread-only accumulator.
// Written at scattered call sites without any lock (main-thread-only).
// Consumed by applyPendingMutations() which transfers into RenderFrameState
// under platformMutex_.
// ---------------------------------------------------------------------------

struct PendingMutations
{
    // --- Dirty flags ---
    std::unordered_set<Uuid, UuidHash> dirtyPanes;
    bool tabBarsDirty            = false;
    bool dividersDirty           = false;
    bool focusChanged            = false;
    bool surfaceNeedsReconfigure = false;

    // --- Structural pane/popup operations ---
    struct CreatePaneState
    {
        Uuid paneId;
        int cols;
        int rows;
    };

    struct DestroyPaneState
    {
        Uuid paneId;
    };

    struct ResizePaneState
    {
        Uuid paneId;
        int cols;
        int rows;
    };

    struct CreatePopupState
    {
        Uuid paneId;
        std::string popupId;
        int cols;
        int rows;
    };

    struct DestroyPopupState
    {
        Uuid paneId;
        std::string popupId;
    };

    struct ResizePopupState
    {
        Uuid paneId;
        std::string popupId;
        int cols;
        int rows;
    };

    struct DestroyEmbeddedState
    {
        Uuid paneId;
        uint64_t lineId;
    };

    using StructuralOp = std::variant<
        CreatePaneState, DestroyPaneState, ResizePaneState,
        CreatePopupState, DestroyPopupState, ResizePopupState,
        DestroyEmbeddedState>;
    std::vector<StructuralOp> structuralOps;

    // --- Texture release requests ---
    std::vector<Uuid> releasePaneTextures;            // pane Uuids whose heldTexture should be released
    std::vector<std::string> releasePopupTextures;    // popup keys ("<uuid>/<popupId>")
    std::vector<std::string> releaseEmbeddedTextures; // embedded keys ("<uuid>:<lineId>")
    std::vector<Uuid> releaseTabBarTextures;
    bool releaseAllTabBarTextures = false;
    bool releaseAllPaneTextures   = false; // resize: release everything

    // --- Divider geometry updates ---
    struct DividerUpdate
    {
        Uuid paneId;
        float x, y, w, h;
        float r, g, b, a;
        bool valid;
    };

    std::vector<DividerUpdate> dividerUpdates;
    std::vector<Uuid> clearDividerPanes; // pane Uuids whose divider VB should be cleared

    // --- Framebuffer size ---
    std::optional<std::pair<uint32_t, uint32_t>> newFbSize;

    // --- Font atlas flags ---
    bool mainFontAtlasChanged   = false;
    bool tabBarFontAtlasChanged = false;
    bool mainFontRemoved        = false;
    bool tabBarFontRemoved      = false;
    bool viewportSizeChanged    = false;

    // --- Invalidate all row caches (font change, color change) ---
    bool invalidateAllRowCaches = false;

    void clear()
    {
        dirtyPanes.clear();
        tabBarsDirty            = false;
        dividersDirty           = false;
        focusChanged            = false;
        surfaceNeedsReconfigure = false;
        structuralOps.clear();
        releasePaneTextures.clear();
        releasePopupTextures.clear();
        releaseEmbeddedTextures.clear();
        releaseTabBarTextures.clear();
        releaseAllTabBarTextures = false;
        releaseAllPaneTextures   = false;
        dividerUpdates.clear();
        clearDividerPanes.clear();
        newFbSize.reset();
        mainFontAtlasChanged   = false;
        tabBarFontAtlasChanged = false;
        mainFontRemoved        = false;
        tabBarFontRemoved      = false;
        viewportSizeChanged    = false;
        invalidateAllRowCaches = false;
    }
};

#pragma once

// Render-thread synchronization types.
//
// Data flow: main thread accumulates mutations in PendingMutations (no lock),
// then applies them in a single batch into RenderFrameState under
// platformMutex_. The render thread reads RenderFrameState under the same
// mutex during its snapshot phase, copies what it needs, releases the mutex,
// and does all GPU work lockfree using PaneRenderPrivate (render-thread-only).

#include "ComputeTypes.h"
#include "Terminal.h"
#include "TerminalSnapshot.h"
#include "TexturePool.h"
#include "Renderer.h"
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

struct RenderPanePopupInfo {
    std::string id;
    int cellX = 0, cellY = 0, cellW = 0, cellH = 0;
    TerminalEmulator* term = nullptr;   // raw ptr into popup Terminal child
};

// Embedded terminal shadow-copy entry. Unlike popups (which carry cell
// coordinates captured at createPopup time), embeddeds are anchored to a
// logical line id; their viewport-row position is resolved each frame from
// the parent's Document so the embedded scrolls with the content.
struct RenderPaneEmbeddedInfo {
    uint64_t lineId = 0;
    int rows = 0;                        // embedded's row count (cols = parent cols)
    TerminalEmulator* term = nullptr;   // raw ptr into embedded Terminal child
    bool focused = false;
    // Viewport row position is resolved each frame by
    // TerminalSnapshot::update via Terminal::collectEmbeddedAnchors → the
    // segment list — no caching needed on the shadow-copy side.
};

struct RenderPaneInfo {
    Uuid id;
    Rect rect;
    TerminalEmulator* term = nullptr;   // raw ptr — valid while tab/pane exist
    int progressState = 0;
    int progressPct = 0;
    std::vector<RenderPanePopupInfo> popups;
    std::string focusedPopupId;
    // Embedded children. The composite only ever sees Embedded segments
    // when the parent is on main screen — TerminalSnapshot::update drops
    // them under alt — but `onAltScreen` is still used to skip the
    // per-embedded RenderTarget construction phase so we don't acquire
    // textures that would never be composited.
    std::vector<RenderPaneEmbeddedInfo> embeddeds;
    uint64_t focusedEmbeddedLineId = 0;
    bool onAltScreen = false;
};

struct RenderTabInfo {
    std::string title;
    std::string icon;
    Uuid focusedPaneId;
    int progressState = 0;      // from focused pane
    int progressPct = 0;        // from focused pane
};

struct DividerGeom {
    float x = 0, y = 0, w = 0, h = 0;
    float r = 0, g = 0, b = 0, a = 0;
    bool valid = false;
};

// Pre-computed tab bar cell: one per column in the tab bar row.
struct TabBarCell {
    std::string ch;         // UTF-8 character to render in this column
    uint32_t fgColor = 0;
    uint32_t bgColor = 0;
};

struct RenderFrameState {
    // Active tab's panes (in iteration order)
    std::vector<RenderPaneInfo> panes;
    Uuid focusedPaneId;

    // Tab bar data (all tabs)
    std::vector<RenderTabInfo> tabs;
    int activeTabIdx = -1;

    // Layout geometry
    uint32_t fbWidth = 0, fbHeight = 0;
    Rect tabBarRect;
    std::string tabBarPosition;

    // Font metrics (copied from main thread scalars)
    float charWidth = 0, lineHeight = 0, fontSize = 0;
    float padLeft = 0, padTop = 0, padRight = 0, padBottom = 0;

    // Visual state
    float activeTint[4]   = {1.0f, 1.0f, 1.0f, 1.0f};
    float inactiveTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool tabBarDirty = true;
    bool tabBarVisible = false;
    bool dividersDirty = true;
    bool focusChanged = false;
    bool surfaceNeedsReconfigure = false;
    bool windowHasFocus = true;
    float cursorBlinkOpacity = 1.0f;

    // Tab bar config values needed by renderTabBar
    uint32_t tbBgColor = 0, tbActiveBgColor = 0, tbActiveFgColor = 0;
    uint32_t tbInactiveBgColor = 0, tbInactiveFgColor = 0;
    float progressColorR = 0, progressColorG = 0.6f, progressColorB = 1.0f;
    float progressBarHeight = 3.0f;
    bool progressBarEnabled = false;
    bool progressIconEnabled = false;
    int maxTitleLength = 0;

    // Per-pane divider geometry (keyed by the divider's owner Terminal Uuid).
    std::unordered_map<Uuid, DividerGeom, UuidHash> dividerGeoms;

    // Font atlas change flags (render thread does the GPU work)
    bool mainFontAtlasChanged = false;
    bool tabBarFontAtlasChanged = false;
    bool mainFontRemoved = false;
    bool tabBarFontRemoved = false;
    bool viewportSizeChanged = false;

    // Window in live resize — render thread defers SIGWINCH
    bool inLiveResize = false;

    // Texture / cache release requests for the next frame. One-shot:
    // consumed and cleared by snapshotUnderLock so the render thread acts
    // on each request exactly once.
    std::vector<Uuid> releasePaneTextureIds;
    std::vector<std::string> releasePopupTextureKeys;
    std::vector<std::string> releaseEmbeddedTextureKeys;
    bool releaseAllPaneTextures = false;
    bool releaseTabBarTexture = false;
    bool invalidateAllRowCaches = false;

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
    float commandDimFactor = 0.0f;

    // Font names (for GPU atlas ops and shaping)
    std::string fontName;
    std::string tabBarFontName;
    float tabBarFontSize = 0;
    float tabBarCharWidth = 0;
    float tabBarLineHeight = 0;

    // Content scale
    float contentScaleX = 1.0f;

    // Tab bar animation frame counter (snapshot of main-thread counter)
    int tabBarAnimFrame = 0;

    // Pre-computed tab bar layout (main thread fills, render thread consumes)
    std::vector<TabBarCell> tabBarCells;     // one per column
    int tabBarCols = 0;                      // number of columns
    std::vector<std::pair<int,int>> tabBarColRanges; // per-tab [startCol, endCol)
};


// ---------------------------------------------------------------------------
// PaneRenderPrivate — render-thread-only state per pane/popup.
// No lock needed — only the render thread touches these.
// ---------------------------------------------------------------------------

struct PaneRenderPrivate {
    TerminalSnapshot snapshot;
    std::vector<ResolvedCell> resolvedCells;
    std::vector<GlyphEntry> glyphBuffer;
    uint32_t totalGlyphs = 0;

    int lastCursorX = -1, lastCursorY = -1;
    bool lastCursorVisible = true;
    int lastCursorShape = -1;
    float lastCursorBlinkOpacity = 1.0f;
    bool lastHasPopupFocus = false;

    std::unordered_set<uint32_t> lastVisibleImageIds;
    PooledTexture* heldTexture = nullptr;
    std::vector<PooledTexture*> pendingRelease;

    wgpu::Buffer dividerVB;
    struct PopupBorder {
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
    uint64_t lastTopLineId = 0;
    TerminalEmulator::ResolvedSelection lastSelection{};
    std::optional<TerminalSnapshot::SelectedCommandRegion> lastSelectedCommand;
    uint32_t lastCommandOutlineColor = 0;

    struct RowGlyphCache {
        std::vector<GlyphEntry> glyphs;
        std::vector<std::pair<uint32_t, uint32_t>> cellGlyphRanges;
        std::vector<Renderer::ColrDrawCmd> colrDrawCmds;
        std::vector<Renderer::ColrRasterCmd> colrRasterCmds;
        bool valid = false;
    };
    std::vector<RowGlyphCache> rowShapingCache;

    bool dirty = true;
};


// ---------------------------------------------------------------------------
// PendingMutations — main-thread-only accumulator.
// Written at scattered call sites without any lock (main-thread-only).
// Consumed by applyPendingMutations() which transfers into RenderFrameState
// under platformMutex_.
// ---------------------------------------------------------------------------

struct PendingMutations {
    // --- Dirty flags ---
    std::unordered_set<Uuid, UuidHash> dirtyPanes;
    bool tabBarDirty = false;
    bool dividersDirty = false;
    bool focusChanged = false;
    bool surfaceNeedsReconfigure = false;

    // --- Structural pane/popup operations ---
    struct CreatePaneState  { Uuid paneId; int cols; int rows; };
    struct DestroyPaneState { Uuid paneId; };
    struct ResizePaneState  { Uuid paneId; int cols; int rows; };
    struct CreatePopupState  { Uuid paneId; std::string popupId; int cols; int rows; };
    struct DestroyPopupState { Uuid paneId; std::string popupId; };
    struct ResizePopupState  { Uuid paneId; std::string popupId; int cols; int rows; };
    struct DestroyEmbeddedState { Uuid paneId; uint64_t lineId; };

    using StructuralOp = std::variant<
        CreatePaneState, DestroyPaneState, ResizePaneState,
        CreatePopupState, DestroyPopupState, ResizePopupState,
        DestroyEmbeddedState
    >;
    std::vector<StructuralOp> structuralOps;

    // --- Texture release requests ---
    std::vector<Uuid> releasePaneTextures;      // pane Uuids whose heldTexture should be released
    std::vector<std::string> releasePopupTextures; // popup keys ("<uuid>/<popupId>")
    std::vector<std::string> releaseEmbeddedTextures; // embedded keys ("<uuid>:<lineId>")
    bool releaseTabBarTexture = false;
    bool releaseAllPaneTextures = false;        // resize: release everything

    // --- Divider geometry updates ---
    struct DividerUpdate {
        Uuid paneId;
        float x, y, w, h;
        float r, g, b, a;
        bool valid;
    };
    std::vector<DividerUpdate> dividerUpdates;
    std::vector<Uuid> clearDividerPanes;        // pane Uuids whose divider VB should be cleared

    // --- Framebuffer size ---
    std::optional<std::pair<uint32_t, uint32_t>> newFbSize;

    // --- Font atlas flags ---
    bool mainFontAtlasChanged = false;
    bool tabBarFontAtlasChanged = false;
    bool mainFontRemoved = false;
    bool tabBarFontRemoved = false;
    bool viewportSizeChanged = false;

    // --- Invalidate all row caches (font change, color change) ---
    bool invalidateAllRowCaches = false;

    void clear() {
        dirtyPanes.clear();
        tabBarDirty = false;
        dividersDirty = false;
        focusChanged = false;
        surfaceNeedsReconfigure = false;
        structuralOps.clear();
        releasePaneTextures.clear();
        releasePopupTextures.clear();
        releaseEmbeddedTextures.clear();
        releaseTabBarTexture = false;
        releaseAllPaneTextures = false;
        dividerUpdates.clear();
        clearDividerPanes.clear();
        newFbSize.reset();
        mainFontAtlasChanged = false;
        tabBarFontAtlasChanged = false;
        mainFontRemoved = false;
        tabBarFontRemoved = false;
        viewportSizeChanged = false;
        invalidateAllRowCaches = false;
    }
};

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

struct RenderPaneInfo {
    int id = -1;
    PaneRect rect;
    TerminalEmulator* term = nullptr;   // raw ptr — valid while tab/pane exist
    int progressState = 0;
    int progressPct = 0;
    std::vector<RenderPanePopupInfo> popups;
    std::string focusedPopupId;
};

struct RenderTabInfo {
    std::string title;
    std::string icon;
    int focusedPaneId = -1;
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
    int focusedPaneId = -1;
    bool hasOverlay = false;
    TerminalEmulator* overlay = nullptr;    // null if !hasOverlay

    // Tab bar data (all tabs)
    std::vector<RenderTabInfo> tabs;
    int activeTabIdx = -1;

    // Layout geometry
    uint32_t fbWidth = 0, fbHeight = 0;
    PaneRect tabBarRect;
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

    // Per-pane divider geometry
    std::unordered_map<int, DividerGeom> dividerGeoms;

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
    std::vector<int> releasePaneTextureIds;
    std::vector<std::string> releasePopupTextureKeys;
    bool releaseAllPaneTextures = false;
    bool releaseTabBarTexture = false;
    bool invalidateAllRowCaches = false;

    // Structural destroys accumulated from main-thread pane/popup/overlay
    // destruction. The render thread erases the matching render-private
    // entries and releases their GPU resources. One-shot, cleared by
    // snapshotUnderLock.
    std::vector<int> destroyedPaneIds;
    std::vector<std::string> destroyedPopupKeys;
    bool destroyedOverlay = false;

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
// PaneRenderPrivate — render-thread-only state per pane/popup/overlay.
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

    int lastViewportOffset = 0;
    int lastHistorySize = 0;
    TerminalEmulator::Selection lastSelection{};
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
    std::unordered_set<int> dirtyPanes;
    bool tabBarDirty = false;
    bool dividersDirty = false;
    bool focusChanged = false;
    bool surfaceNeedsReconfigure = false;

    // --- Structural pane/popup/overlay operations ---
    struct CreatePaneState  { int paneId; int cols; int rows; };
    struct DestroyPaneState { int paneId; };
    struct ResizePaneState  { int paneId; int cols; int rows; };
    struct CreatePopupState  { int paneId; std::string popupId; int cols; int rows; };
    struct DestroyPopupState { int paneId; std::string popupId; };
    struct ResizePopupState  { int paneId; std::string popupId; int cols; int rows; };
    struct CreateOverlayState  {};
    struct DestroyOverlayState {};

    using StructuralOp = std::variant<
        CreatePaneState, DestroyPaneState, ResizePaneState,
        CreatePopupState, DestroyPopupState, ResizePopupState,
        CreateOverlayState, DestroyOverlayState
    >;
    std::vector<StructuralOp> structuralOps;

    // --- Texture release requests ---
    std::vector<int> releasePaneTextures;       // pane IDs whose heldTexture should be released
    std::vector<std::string> releasePopupTextures; // popup keys (paneId/popupId)
    bool releaseTabBarTexture = false;
    bool releaseAllPaneTextures = false;        // resize: release everything

    // --- Divider geometry updates ---
    struct DividerUpdate {
        int paneId;
        float x, y, w, h;
        float r, g, b, a;
        bool valid;
    };
    std::vector<DividerUpdate> dividerUpdates;
    std::vector<int> clearDividerPanes;         // pane IDs whose divider VB should be cleared

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

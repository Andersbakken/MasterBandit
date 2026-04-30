#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>
#include <CellGrid.h>
#include <Document.h>
#include <InputTypes.h>

std::string toPrintable(const char *chars, int len);
inline std::string toPrintable(const std::string &string)
{
    return toPrintable(string.c_str(), string.size());
}

class TerminalEmulator;

struct TerminalCallbacks {
    std::function<void(TerminalEmulator*, int /*Event*/, void*)> event;
    std::function<void(const std::string&)>      copyToClipboard;
    std::function<std::string()>                 pasteFromClipboard;
    // OSC 0/2 sets the title; XTWINOPS 22/23 push/pop the stack.
    // Fires with Some(str) when OSC writes the top (even an empty string)
    // or a pop exposes a previously-saved string; fires with nullopt when
    // the stack pops empty (no title left). Downstream tabs treat nullopt
    // as "no pane-driven title — fall back to the tab's JS label or the
    // foreground process name".
    std::function<void(std::optional<std::string>)> onTitleChanged;
    std::function<float()>                       cellPixelWidth;
    std::function<float()>                       cellPixelHeight;
    std::function<void(int, std::string_view)>  onOSC;    // called for unhandled OSC codes
    std::function<void(std::optional<std::string>)> onIconChanged;    // OSC 1; same semantics as onTitleChanged
    std::function<void(int /*state*/, int /*pct*/)> onProgressChanged; // OSC 9;4
    std::function<bool()>                        isDarkMode;         // for mode 2031
    std::function<void(const std::string&)>      onCWDChanged;       // OSC 7
    std::function<void(const std::string&)>      onMouseCursorShape; // OSC 22 (CSS pointer name; "" = default)
    // Desktop notification payload — passed to onDesktopNotification.
    // Aggregates everything the OSC 99 parser accumulated by the time
    // d=1 fired. Other notification protocols (OSC 9 / 777 / 1337) fill
    // a subset and use defaults for the rest.
    struct DesktopNotification {
        std::string title;
        std::string body;
        std::string id;                      // OSC i= (may be empty)
        uint8_t     urgency = 1;             // 0=low, 1=normal, 2=critical
        bool        closeResponseRequested = false;  // c=1
        // Default action set is {focus} per kitty notifications.py:232.
        // OSC 99 a= can add/remove either with +/- prefixes.
        bool        actionFocus  = true;
        bool        actionReport = false;
        // Up to 8 button labels from p=buttons (U+2028-split, max-8 cap
        // in kitty notifications.py:422). Empty for non-OSC-99 sources.
        std::vector<std::string> buttons;
        // OSC 99 o= (only_when) gate. Empty == always-allow.
        // "unfocused" — suppress at send time when our window has focus.
        // "invisible" — suppress when focused or visible-but-unfocused.
        // "always" — allow. Other values treated as empty (kitty parity).
        std::string onlyWhen;
    };
    std::function<void(const DesktopNotification&)> onDesktopNotification;

    // OSC 99 "p=close": ask the platform to programmatically dismiss a
    // previously-shown notification keyed by id. id is the OSC i= value
    // from the close command's metadata; never empty (parser drops the
    // call if i= is missing).
    std::function<void(const std::string& /*id*/)> onCloseNotification;

    // OSC 99 "p=alive": query which of the originating channel's
    // notifications are still active. responderId is the i= from the
    // query command — the platform must reply by writing
    // \e]99;i=<responderId>:p=alive;<csv>\a back into the terminal's
    // input stream (kitty notifications.py:1047-1053). The csv lists the
    // OSC i= values still alive on this channel.
    std::function<void(const std::string& /*responderId*/)> onQueryAliveNotifications;
    std::function<void(const std::string&)>      onForegroundProcessChanged;
    // Called for XTGETTCAP queries not found in the built-in table.
    // Returns the capability value (may be empty for boolean caps), or nullopt if unknown.
    std::function<std::optional<std::string>(const std::string&)> customTcapLookup;
};

class TerminalEmulator
{
public:
    TerminalEmulator(TerminalCallbacks callbacks);
    virtual ~TerminalEmulator();

    // The single mutex protecting all parse-mutated terminal state
    // (grid, document, cursor, mState fields, command ring, selection,
    // hyperlink registry, title/icon stacks, embeddeds, ...).
    // Recursive because script callbacks fired synchronously from
    // inside injectData (OSC handlers, action dispatch) can re-enter
    // mutation APIs on the same thread.
    //
    // Held by:
    //   * The parse worker thread for the entirety of injectData
    //     (TerminalEmulator.cpp:injectData). Worker batches are
    //     typically microseconds but can run into the millisecond
    //     range under a flooding producer.
    //   * The render thread during snapshot capture
    //     (TerminalSnapshot::update), briefly.
    //   * Main-thread one-off readers (mouse/scroll handlers, JS
    //     getters, action dispatch, OSC reply construction).
    //
    // **Hot main-thread paths must not take this mutex.** Per-tick
    // consumers (PlatformDawn::buildRenderFrameState, onBlinkTick)
    // read lock-free atomic snapshots instead:
    //   * usingAltScreen() — atomic<bool>
    //   * currentTitle() / currentIcon() — atomic<shared_ptr<const string>>
    //   * focusedEmbeddedLineId() — atomic<uint64_t>
    //   * embeddedSnapshot() (Terminal) — atomic<shared_ptr<vector<...>>>
    //   * hasEvictedEmbeddeds() — atomic<bool>
    // Mutators publish a fresh snapshot via publishLiveView() at the
    // end of each parse batch / mutation; consumers atomic-load.
    //
    // The only other lock in this subsystem is Terminal::mReadBufferMutex
    // which is leaf-level (never held while taking another lock).
    std::recursive_mutex& mutex() const { return mMutex; }

    // DECSCUSR cursor shapes
    enum CursorShape {
        CursorBlock = 0,         // blinking block (default)
        CursorSteadyBlock = 2,
        CursorUnderline = 3,     // blinking underline
        CursorSteadyUnderline = 4,
        CursorBar = 5,           // blinking bar
        CursorSteadyBar = 6
    };

    // Per-screen terminal state. Main and alt screens each own an instance;
    // mState points at the active one. 1049 h/l swaps mState so app-set modes
    // (mouse, bracketed paste, focus reporting, etc.) don't leak across the
    // alt-screen boundary. mDefaults holds config-seeded + factory defaults
    // used by RIS (and 1049-on-entry for the alt state).
    // Character sets for G0/G1 designation (ESC ( X, ESC ) X). Only the three
    // sets that real-world software actually uses: US ASCII (default), UK
    // (# → £), and DEC Special Graphics (line drawing + misc).
    enum Charset : uint8_t { CharsetASCII, CharsetUK, CharsetDECGraphics };

    struct TerminalState {
        int cursorX { 0 }, cursorY { 0 };
        bool cursorVisible { true };
        CursorShape cursorShape { CursorBlock };
        bool cursorBlinkEnabled { true };       // DEC private mode 12
        bool wrapPending { false };             // deferred autowrap state
        CellAttrs currentAttrs;                 // SGR "pen"
        uint32_t currentUnderlineColor { 0 };   // SGR 58: packed RGBA8, 0 = use fg
        // Character set slots and GL selector. Per-screen so alt-screen apps
        // (ncurses TUIs etc.) can't leak charset state back to the shell.
        Charset charsetG0 { CharsetASCII };
        Charset charsetG1 { CharsetASCII };
        bool shiftOut { false };                // false: GL=G0, true: GL=G1 (SO/SI)
        // DECSC (ESC 7) / DECRC (ESC 8) save slot — per-screen so a DECSC on
        // alt doesn't clobber main's saved cursor. Shape/blink are not saved
        // by DECSC per spec; they're preserved across alt via the state swap
        // itself (main keeps its shape while alt runs).
        int savedCursorX { 0 }, savedCursorY { 0 };
        bool savedWrapPending { false };
        CellAttrs savedAttrs;
        // Charset state is part of DECSC's save set per DEC STD 070.
        Charset savedCharsetG0 { CharsetASCII };
        Charset savedCharsetG1 { CharsetASCII };
        bool savedShiftOut { false };
        bool savedOriginMode { false };
        int scrollTop { 0 }, scrollBottom { 0 };  // scroll region [top, bottom)
        bool cursorKeyMode { false };           // DECCKM
        bool keypadMode { false };              // DECKPAM
        bool autoWrap { true };                 // DECAWM
        bool originMode { false };              // DECOM — CUP/HVP relative to scroll region
        bool insertMode { false };              // IRM
        bool bracketedPaste { false };
        bool focusReporting { false };          // mode 1004
        bool syncOutput { false };              // mode 2026
        bool colorPreferenceReporting { false }; // mode 2031
        bool mouseMode1000 { false };
        bool mouseMode1002 { false };
        bool mouseMode1003 { false };
        bool mouseMode1006 { false };
        bool mouseMode1016 { false };           // SGR-Pixel
    };

    int cursorX() const { return mState->cursorX; }
    int cursorY() const { return mState->cursorY; }
    bool cursorVisible() const { return mState->cursorVisible; }
    CursorShape cursorShape() const { return mState->cursorShape; }
    bool cursorBlinkEnabled() const { return mState->cursorBlinkEnabled; }

    // OSC 22 — current pointer shape (CSS name); empty = platform default.
    std::string currentPointerShape() const {
        const auto& s = mUsingAltScreen ? mPointerShapeStackAlt : mPointerShapeStackMain;
        return s.empty() ? std::string{} : s.back();
    }
    // True if `name` is a CSS pointer name we recognise (or a kitty/X11 alias).
    static bool isKnownPointerShape(std::string_view name);
    // True iff the cursor should currently visibly blink: shape is a blinking
    // variant AND DEC private mode 12 is on.
    bool cursorBlinking() const {
        switch (mState->cursorShape) {
        case CursorBlock:
        case CursorUnderline:
        case CursorBar:
            return true;
        default:
            return mState->cursorBlinkEnabled;
        }
    }
    // Config-applied cursor defaults. Propagates the new default to the
    // config prototype and to BOTH screen states so the user-visible cursor
    // updates live regardless of which screen is active, and so returning
    // from alt doesn't revert to a stale pre-config-reload shape.
    void setDefaultCursorShape(CursorShape s) {
        mDefaults.cursorShape = s;
        mMainState.cursorShape = s;
        mAltState.cursorShape = s;
    }
    void setDefaultCursorBlinkEnabled(bool b) {
        mDefaults.cursorBlinkEnabled = b;
        mMainState.cursorBlinkEnabled = b;
        mAltState.cursorBlinkEnabled = b;
    }
    int width() const { return mWidth; }
    int height() const { return mHeight; }

    // grid() and the bool member mUsingAltScreen are read/written by the
    // parse worker thread inside injectData (mode 1049/47 toggles), and
    // also read on the main thread (e.g. snapshot building, hit-testing,
    // platform layout decisions). Worker uses the plain bool internally
    // for performance; main-thread external readers use usingAltScreen()
    // which goes through mUsingAltScreenAtomic (kept in sync alongside
    // every mutation of mUsingAltScreen).
    const IGrid& grid() const { return mUsingAltScreen ? static_cast<const IGrid&>(mAltGrid) : static_cast<const IGrid&>(mDocument); }
    IGrid& grid() { return mUsingAltScreen ? static_cast<IGrid&>(mAltGrid) : static_cast<IGrid&>(mDocument); }
    bool usingAltScreen() const { return mUsingAltScreenAtomic.load(std::memory_order_acquire); }
    const Document& document() const { return mDocument; }
    Document& document() { return mDocument; }

    virtual void resize(int width, int height);

    // Scrollback viewport.
    //
    // Copies the viewport row at `viewRow` (0 = top) into `dst`, which must
    // be sized to exactly `width()`. Returns true on success, false if the
    // requested row has no backing data (in which case `dst` is untouched).
    // Acquires the terminal mutex internally — safe to call without
    // external locking. The copy-at-the-API-boundary pattern keeps callers
    // from accidentally caching pointers into ring-buffer storage that a
    // later mutation can invalidate.
    bool copyViewportRow(int viewRow, std::span<Cell> dst) const;
    void scrollViewport(int delta);
    void resetViewport();
    // Viewport-offset in rows — the number of history rows between the
    // visual top of the viewport and the first screen row. 0 = live mode
    // (viewport pinned to the screen area's top, auto-follows new content).
    int viewportOffset() const { return mViewportOffset; }

    // direction = -1 (previous) or +1 (next). wrap = whether to cycle past
    // the ends of the command ring (true → Cmd+Up at oldest wraps to newest,
    // Cmd+Down at newest wraps to oldest; false → clamps at ends).
    void scrollToPrompt(int direction, bool wrap = true);
    void selectCommandOutput();         // select output around current viewport position
    std::string serializeScrollback() const; // serialize all content for pager

    enum Event {
        Update,
        ScrollbackChanged,
        VisibleBell,
        CommandComplete,         // payload: const CommandRecord*
        CommandSelectionChanged  // payload: nullptr; read selectedCommandId() for new value
    };

    // Semantic mode transitioned by OSC 133 A/B/C/D; tracks "what is the terminal
    // currently writing?" at the cell level. Inactive = no OSC 133 session active.
    enum class SemanticMode : uint8_t {
        Inactive = 0,
        Prompt,
        Input,
        Output
    };
    SemanticMode semanticMode() const { return mSemanticMode; }

    // One executed command, built up from OSC 133 markers. Coordinates are
    // stored as logical-line ids from Document (see Document::lineIdForAbs).
    // Line ids survive scroll, tier-1/tier-2 migration, AND width-change
    // reflow. They only go stale when the line evicts past the archive cap,
    // in which case Document::firstAbsOfLine returns -1.
    // Cell content mutation (shell redraw) does not affect line ids.
    struct CommandRecord {
        uint64_t id = 0;
        // Logical-line ids from Document — resolve to current abs-row at
        // query time via Document::firstAbsOfLine / lastAbsOfLine.
        uint64_t promptStartLineId = 0;
        uint64_t commandStartLineId = 0;
        uint64_t outputStartLineId = 0;
        uint64_t outputEndLineId = 0;
        int promptStartCol = -1;
        int commandStartCol = -1;
        int outputStartCol = -1;
        int outputEndCol = -1;
        std::string cwd;                        // OSC 7 value at A
        std::optional<int> exitCode;            // from OSC 133;D;<n>
        uint64_t startMs = 0;                   // when C fired
        uint64_t endMs = 0;                     // when D fired
        bool complete = false;
    };
    const std::deque<CommandRecord>& commands() const { return mCommandRing; }
    const CommandRecord* lastCommand() const;   // most recently completed record (skipping any in-flight tail), nullptr if none

    // Hit-test: find the command whose logical-line span contains this id.
    // Lines above the oldest prompt or between complete commands return nullptr.
    // O(log N) via binary search — ring stays sorted by promptStartLineId by
    // construction (startCommand only appends, pruneCommandRing only pops front).
    const CommandRecord* commandForLineId(uint64_t lineId) const;

    // Look up a record by its CommandRecord::id. O(log N) binary search — the
    // ring is sorted by id (monotonic mNextCommandId++ at startCommand; only
    // append + front-pop). Returns nullptr if the id isn't in the ring.
    const CommandRecord* commandForId(uint64_t commandId) const;

    // Select the given command's output region as a text selection and auto-copy
    // to clipboard (same semantics as selectCommandOutput()). Used by mouse paths
    // that already know which command was clicked, avoiding the viewport-center
    // heuristic. No-op if rec is null or its lines have been evicted.
    void selectCommandOutputForRecord(const CommandRecord* rec);

    // Selection of a single command region (OSC 133-scoped). Mutations go
    // through setSelectedCommand so the render thread can observe via snapshot.
    // The id references CommandRecord::id; if the id no longer exists in the
    // ring (command evicted), the selection clears on the next pruneCommandRing.
    std::optional<uint64_t> selectedCommandId() const { return mSelectedCommandId; }
    void setSelectedCommand(std::optional<uint64_t> commandId);

    struct Action
    {
        enum Type {
            Invalid,
            CursorUp,
            CursorDown,
            CursorForward,
            CursorBack,
            CursorNextLine,
            CursorPreviousLine,
            CursorHorizontalAbsolute,
            CursorPosition,
            ClearScreen,
            ClearToBeginningOfScreen,
            ClearToEndOfScreen,
            ClearLine,
            ClearToBeginningOfLine,
            ClearToEndOfLine,
            DeleteChars,
            InsertChars,
            InsertLines,
            DeleteLines,
            EraseChars,
            ScrollUp,
            ScrollDown,
            VerticalPositionAbsolute,
            SelectGraphicRendition,
            AUXPortOn,
            AUXPortOff,
            DeviceStatusReport,
            SaveCursorPosition,
            RestoreCursorPosition,
            SetMode,
            ResetMode
        } type { Invalid };

        static const char *typeName(Type type);

        int count { 0 }, x { 0 }, y { 0 };
    };

    void onAction(const Action *action);

    void keyPressEvent(const KeyEvent *event);
    void mousePressEvent(const MouseEvent *event);
    void mouseReleaseEvent(const MouseEvent *event);
    void mouseMoveEvent(const MouseEvent *event);
    bool mouseReportingActive() const { return mState->mouseMode1000 || mState->mouseMode1002 || mState->mouseMode1003; }

    // Pull-model title/icon: returns the top of the XTWINOPS stack, or
    // nullopt when the stack is empty (no OSC 0/2 has set one, or it's been
    // fully popped away). Push duplicates the current top and is a no-op on
    // an empty stack, so stack-non-empty is equivalent to "app has set a
    // title at some point and hasn't fully revoked it."
    // Title/icon stacks are mutated by the parse worker (OSC 2/0/1,
    // XTWINOPS push/pop) under mMutex. Per-tick tab-bar resolution
    // in buildRenderFrameState reads them on every frame, so we
    // expose a wait-free shadow via mTitleAtomic / mIconAtomic
    // (atomic shared_ptr<const string>; null when the underlying
    // stack is empty) republished by the parser whenever the top
    // changes. Reading under mMutex would block buildRenderFrameState
    // for the entire parse-apply duration of a flooding pane (the
    // apply runs in one shot — see Terminal::queueParse — so it can
    // be hundreds of ms for ~1 MiB of input).
    std::optional<std::string> currentTitle() const {
        if (auto p = mTitleAtomic.load(std::memory_order_acquire)) return *p;
        return std::nullopt;
    }
    std::optional<std::string> currentIcon() const {
        if (auto p = mIconAtomic.load(std::memory_order_acquire)) return *p;
        return std::nullopt;
    }
    bool syncOutputActive() const { return mState->syncOutput; }
    uint8_t kittyFlags() const { return mKittyFlags; }
    bool colorPreferenceReporting() const { return mState->colorPreferenceReporting; }
    void setPaletteColor(int idx, uint8_t r, uint8_t g, uint8_t b) {
        if (idx >= 0 && idx < 16) { m16ColorPalette[idx][0] = r; m16ColorPalette[idx][1] = g; m16ColorPalette[idx][2] = b; }
    }
    void applyColorScheme(const struct ColorScheme& cs);
    void applyCursorConfig(const struct CursorConfig& cc);

    // Default colors (for OSC 10/11/12 and rendering)
    struct DefaultColors {
        uint8_t fgR { 0xDD }, fgG { 0xDD }, fgB { 0xDD };
        uint8_t bgR { 0x00 }, bgG { 0x00 }, bgB { 0x00 };
        uint8_t cursorR { 0xCC }, cursorG { 0xCC }, cursorB { 0xCC };
    };
    const DefaultColors& defaultColors() const { return mDefaultColors; }
    const DefaultColors& configDefaultColors() const { return mConfigDefaultColors; }

    const std::string* hyperlinkURI(uint32_t id) const {
        auto it = mHyperlinkRegistry.find(id);
        return it != mHyperlinkRegistry.end() ? &it->second.uri : nullptr;
    }
    void notifyColorPreference(bool isDark);
    void focusEvent(bool focused);

    // Selection — each anchor stores `(lineId, cellOffset)` where
    // cellOffset is the index of the anchored cell within the logical line,
    // counted in reading order (0 = first cell of the line). This is the
    // same model iTerm2 uses (`LineBufferPosition.absolutePosition`): a
    // logical text position that's independent of the current visual
    // layout. Computed at write as `rowOffsetWithinLine * mWidth + col`
    // (autowrap fills each non-last row of a wrapped logical line to
    // exactly mWidth, so this counts cells in reading order). Resolved at
    // read as `(row = firstAbs + cellOffset / mWidth, col = cellOffset %
    // mWidth)`, clamped to lastAbsOfLine — invariant across column reflow
    // because reflow re-wraps the same cells into new visual rows. A line
    // that evicts past the archive cap becomes unresolvable and the
    // selection self-clears via hasSelection().
    enum class SelectionMode { Normal, Word, Line, Rectangle };
    struct Selection {
        uint64_t startLineId { 0 }; int startCellOffset { 0 };
        uint64_t endLineId   { 0 }; int endCellOffset   { 0 };
        bool active { false };
        bool valid  { false };
        SelectionMode mode { SelectionMode::Normal };
    };

    // Resolved view of a Selection — abs rows looked up at the time of the
    // call. Used by snapshot mirroring and by callers that need rendering
    // coordinates rather than logical-line identity.
    struct ResolvedSelection {
        int startCol { 0 }; int startAbsRow { 0 };
        int endCol   { 0 }; int endAbsRow   { 0 };
        bool active  { false };
        bool valid   { false };
        SelectionMode mode { SelectionMode::Normal };
    };

    // `col` and `xRightHalf` together form a wezterm-style cell-boundary
    // index: boundary = col + (xRightHalf ? 1 : 0), in [0, mWidth]. The
    // boundary is what gets stored in mSelection — resolveSelection then
    // shifts whichever side is "extending" by one cell so the cell under
    // the cursor is excluded until the click crosses the cell midpoint
    // (matches wezterm/iTerm2/Terminal.app).
    void startSelection(int col, int absRow, bool xRightHalf = false);
    void startWordSelection(int col, int absRow);
    void startLineSelection(int absRow);
    void extendSelection(int col, int absRow, bool xRightHalf = false);
    void startRectangleSelection(int col, int absRow, bool xRightHalf = false);
    void updateSelection(int col, int absRow, bool xRightHalf = false);
    void finalizeSelection();
    void clearSelection();
    // True iff a current selection still resolves to live rows. Internally
    // calls resolveSelection(); a stale selection (anchor evicted) reports
    // false here even if the underlying flags are still set — the next
    // snapshot.update() will prune the flags too.
    bool hasSelection() const;
    const Selection& selection() const { return mSelection; }
    // Resolve `mSelection`'s lineIds to current abs rows. Returns empty
    // optional when there's no active/valid selection or when either anchor
    // has evicted past the archive cap.
    std::optional<ResolvedSelection> resolveSelection() const;
    bool isCellSelected(int col, int absRow) const;
    std::string selectedText() const;

    // Image registry
    struct ImageEntry {
        uint32_t id { 0 };
        uint32_t imageNumber { 0 };  // I= (non-unique)
        uint32_t pixelWidth { 0 }, pixelHeight { 0 };
        uint32_t cellWidth { 0 }, cellHeight { 0 };
        // Source rect crop (0 = use full image)
        uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
        std::vector<uint8_t> rgba;  // root frame (frame 0)
        // iTerm OSC 1337 "name=" metadata (base64-decoded filename). Never set
        // by kitty graphics. Purely informational — not displayed.
        std::string name;

        // Per-placement display parameters (one image, multiple positions)
        struct Placement {
            uint32_t cellWidth { 0 }, cellHeight { 0 };
            uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
            uint32_t cellXOffset { 0 }, cellYOffset { 0 }; // X=, Y= sub-cell pixel offsets
            int32_t zIndex { 0 };
        };
        std::unordered_map<uint32_t, Placement> placements; // placementId → params

        // Animation
        struct Frame {
            std::vector<uint8_t> rgba;  // full frame RGBA data (same dimensions as image)
            uint32_t gap { 40 };        // ms before advancing to next frame
        };
        std::vector<Frame> extraFrames;
        uint32_t currentFrameIndex { 0 };  // 0 = root, 1+ = extraFrames[i-1]
        uint32_t frameGeneration { 0 };    // bumped on frame change, for GPU staleness detection
        uint32_t currentLoop { 0 };
        uint32_t maxLoops { 0 };           // 0 = infinite
        uint64_t frameShownAt { 0 };       // monotonic time current frame was first displayed
        enum AnimState : uint8_t { Stopped = 0, Loading = 1, Running = 2 };
        AnimState animationState { Stopped };
        uint32_t rootFrameGap { 40 };

        const std::vector<uint8_t>& currentFrameRGBA() const {
            if (currentFrameIndex == 0 || extraFrames.empty()) return rgba;
            uint32_t idx = currentFrameIndex - 1;
            if (idx < extraFrames.size()) return extraFrames[idx].rgba;
            return rgba;
        }
        uint32_t currentFrameGap() const {
            if (currentFrameIndex == 0 || extraFrames.empty()) return rootFrameGap;
            uint32_t idx = currentFrameIndex - 1;
            if (idx < extraFrames.size()) return extraFrames[idx].gap;
            return rootFrameGap;
        }
        bool hasAnimation() const { return !extraFrames.empty() && animationState == Running; }
    };
    // ImageEntry is owned via shared_ptr so the render thread can hold a
    // reference to an image's data (rgba buffers, placements, animation
    // state) across the Terminal mutex being released. When the parser
    // deletes an image, its map entry is removed; any outstanding
    // shared_ptr — e.g. captured in a TerminalSnapshot — keeps the data
    // alive until the render drops its reference.
    const std::unordered_map<uint32_t, std::shared_ptr<ImageEntry>>& imageRegistry() const { return mImageRegistry; }
    uint32_t findImageByNumber(uint32_t number) const;

    // Test-only: override the monotonic timestamp at which an image's current
    // frame was first displayed. Lets animation tests drive tickAnimations()
    // without wall-clock dependency. Returns false if the image does not exist.
    bool setImageFrameShownAtForTest(uint32_t id, uint64_t t);

    // Advance all running animations based on current time.
    // Advance every running animated image whose gap has elapsed. Returns
    // true iff at least one image's currentFrameIndex actually changed — the
    // renderer uses this to decide whether the pane needs re-rendering.
    bool tickAnimations();

    // Feed bytes into the VT parser. Returns number of bytes actually
    // consumed.
    //
    // `byteBudget`: if > 0, the parser will exit early once it has
    // consumed at least this many bytes AND it's at a safe split
    // boundary (mParserState == Normal, no in-progress UTF-8 sequence).
    // 0 means "consume everything." Used by the async parse worker to
    // bound how long mMutex is held under a flood; the worker calls
    // injectData in a loop, releasing the lock between calls so other
    // threads can acquire it.
    //
    // The safe-split rule means the worker may consume MORE than
    // byteBudget if a long escape sequence (DCS image data, OSC
    // payload) straddles the boundary — that's acceptable since the
    // boundary is just an advisory yield point.
    size_t injectData(const char* data, size_t len, size_t byteBudget = 0);

    void setOSCCallback(std::function<void(int, std::string_view)> cb)
    {
        mCallbacks.onOSC = std::move(cb);
    }

    // Embedded children anchored to logical-line ids. Populated by subclasses
    // that support inline embedded terminals (Terminal); the base emulator
    // has none. Consulted by TerminalSnapshot::update() when building the
    // visual-layout segment list so the snapshot doesn't need to call back
    // into live Terminal state from the render thread. Called under the
    // terminal mutex.
    struct EmbeddedAnchor {
        uint64_t lineId = 0;
        int rows = 0;
    };
    virtual void collectEmbeddedAnchors(std::vector<EmbeddedAnchor>& /*out*/) const {}

    // Called from RIS (full reset) before scrollback / line ids are wiped, so
    // subclasses can hand off document-anchored children (embedded terminals
    // on Terminal) for orderly teardown. Default no-op. Called under the
    // terminal mutex.
    virtual void onFullReset() {}

    // Push enough rows from the top of the document into history so that the
    // cursor sits at or above viewport row `viewportRows - 1 - rowsBelow`,
    // leaving `rowsBelow` viewport rows of room beneath the cursor (plus the
    // cursor's row itself). Cursor screen position is adjusted so it stays on
    // the same logical line. Used by Terminal::createEmbedded to make space
    // for an inline embedded that would otherwise extend past the bottom.
    void scrollCursorUpToFitBelow(int rowsBelow);

    // One visible embedded as (viewport-row, embedded-rows, lineId).
    // Produced by collectVisibleAnchors() for use by both snapshot build
    // (render thread, under terminal mutex) and live hit-test (main
    // thread, where reads are race-free because all mutation is also
    // main-thread). Sorted by viewRow ascending.
    struct ViewAnchor {
        int viewRow = 0;
        int rows = 0;
        uint64_t lineId = 0;
    };
    // Compute the list of embedded anchors currently visible in the
    // viewport, sorted by viewport row ascending. Filters anchors whose
    // backing line has evicted or whose row falls outside the viewport.
    // viewportRows is the number of logical (unshifted) rows in the
    // viewport — i.e. the Terminal's height().
    static std::vector<ViewAnchor> collectVisibleAnchors(
        const TerminalEmulator& term, int viewportOffset, int viewportRows);

    static uint64_t mono();

    // 16-color palette (standard + bright) as RGB — current runtime values
    uint8_t m16ColorPalette[16][3];
    // Config-loaded defaults for OSC 104 reset (indices 0-15)
    uint8_t m16PaletteDefaults[16][3];
    // Runtime overrides for any of the 256 palette entries (set via OSC 4)
    std::unordered_map<int, std::array<uint8_t,3>> m256PaletteOverrides;
    // 256-color palette lookup
    void color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b) const;

protected:
    virtual void writeToOutput(const char* data, size_t len) {}
    bool bracketedPaste() const { return mState->bracketedPaste; }
    void resetScrollback(int scrollbackLines);  // reinitializes document with given scrollback capacity
    TerminalCallbacks& callbacks() { return mCallbacks; }

private:
    TerminalCallbacks mCallbacks;

    mutable std::recursive_mutex mMutex;

    int mWidth { 0 }, mHeight { 0 };

    // Horizontal tab stops — terminal-global (shared between main/alt screens).
    // Sized to mWidth; entry is 1 at a tab stop column, 0 otherwise.
    std::vector<uint8_t> mTabStops;

    // Per-screen state. See TerminalState definition above.
    TerminalState mMainState;
    TerminalState mAltState;
    TerminalState mDefaults;        // seeded from config; source for resetToDefault().
    TerminalState* mState { &mMainState };  // active state — follows 1049 h/l.

    // Reset `s` to current defaults, plus runtime-derived fields (scroll region).
    void resetToDefault(TerminalState& s) {
        s = mDefaults;
        s.scrollBottom = mHeight;
    }

    Document mDocument;
    CellGrid mAltGrid;
    bool mUsingAltScreen { false };
    // Mirror of mUsingAltScreen kept in sync at every write site
    // (TerminalEmulator.cpp:1013, 2020, 2081). Read by main-thread
    // callers via the usingAltScreen() accessor with acquire ordering.
    std::atomic<bool> mUsingAltScreenAtomic { false };

    // Integer row-count viewport anchor. `scrollUpInRegion` compensates by
    // += n when the user is scrolled back (non-zero offset) so they stay
    // pinned to the same content as new rows stream in; when offset == 0
    // (live), the viewport auto-follows. Line-id anchoring was tried
    // briefly but broke on soft-wrap chains where inheritLineIdFromAbove
    // left the same id across many rows and firstAbsOfLine returned the
    // chain's head instead of the intended first-screen-row position.
    int mViewportOffset { 0 };

    char32_t mLastPrintedChar { 0 };       // for REP (CSI b)
    int mLastPrintedX { -1 }, mLastPrintedY { -1 }; // position of last stored cell (for combining codepoints)
    uint_least16_t mGraphemeState { 0 };   // libgrapheme stateful break detection

    enum ParserState {
        Normal,
        InUtf8,
        InEscape,
        InStringSequence
    } mParserState { Normal };
    char mUtf8Buffer[6];
    int mUtf8Index { 0 };

    char mEscapeBuffer[128];
    int mEscapeIndex { 0 };

    // String sequence (OSC/DCS/APC) accumulation
    std::string mStringSequence;
    uint8_t mStringSequenceType { 0 };
    bool mWasInStringSequence { false };
    static constexpr size_t MAX_STRING_SEQUENCE = 16 * 1024 * 1024; // 16 MB

    // Refactored to take parser-state as args so the eventual
    // parseToActions / applyActions split can call them without the
    // helpers reading mStringSequence / mStringSequenceType /
    // mEscapeBuffer / mEscapeIndex member fields directly.
    void processStringSequence(uint8_t kind, std::string_view body);
    void processDCS(std::string_view payload);
    void processOSC_Title(std::string_view text, bool setTitle);

    // Republish mTitleAtomic / mIconAtomic from the current top of
    // mTitleStack / mIconStack. Caller must hold mMutex (parser
    // path always does). Skipped when the new value matches the
    // currently-published one, so the cost is one shared_ptr<string>
    // allocation per *change*, not per write.
    void publishTitleAtomic();
    void publishIconAtomic();
    void processOSC_Color(int oscNum, std::string_view payload);
    void processOSC_Palette(std::string_view payload);
    void processOSC_PaletteReset(std::string_view payload);
    void processOSC_Clipboard(std::string_view payload);
    void processOSC_iTerm(std::string_view payload);
    void processOSC_PointerShape(std::string_view payload);
    void processAPC(std::string_view body);
    void placeImageInGrid(uint32_t imageId, uint32_t placementId, int cellCols, int cellRows, bool moveCursor = true);
    std::string buildCurrentSGR() const;

    // Kitty graphics protocol: chunked transfer accumulation
    struct KittyLoadState {
        std::vector<uint8_t> data;   // accumulated decoded payload
        uint32_t id = 0;             // client image ID (i=)
        uint32_t imageNumber = 0;    // I= (non-unique image number)
        uint32_t placementId = 0;    // placement ID (p=)
        uint32_t format = 32;        // f= (24=RGB, 32=RGBA, 100=PNG)
        uint32_t width = 0, height = 0; // s=, v= (source data dimensions)
        uint32_t cellCols = 0, cellRows = 0; // c=, r=
        uint32_t xOffset = 0, yOffset = 0;   // x=, y= (source rect offset)
        uint32_t cellXOffset = 0, cellYOffset = 0; // X=, Y= (context-dependent: sub-cell offset or a=f compose/bg)
        uint32_t cropWidth = 0, cropHeight = 0; // w=, h= (source rect size)
        uint32_t quiet = 0;          // q=
        int32_t zIndex = 0;          // z=
        uint32_t cursorMovement = 0; // C=
        uint32_t dataSize = 0;       // S=
        uint32_t dataOffset = 0;     // O=
        char action = 'T';           // a=
        char compressed = 0;         // o=
        char transmissionType = 'd'; // t=
        bool active = false;
    };
    KittyLoadState mKittyLoading;
    uint32_t mLastKittyImageId { 0 }; // for a=f/a=a when i=0

    // https://ttssh2.osdn.jp/manual/en/about/ctrlseq.html
    enum EscapeSequence {
        SS2 = 'N',
        SS3 = '0',
        DCS = 'P',
        CSI = '[',
        ST = '\\',
        OSX = ']',
        SOS = 'X',
        PM = '^',
        APC = '_',
        RIS = 'c',
        VB = 'g',
        DECKPAM = '=',
        DECKPNM = '>',
        DECSC = '7',
        DECRC = '8',
        IND = 'D',
        NEL = 'E',
        HTS = 'H',
        RI = 'M'
    };

    void processCSI(const char* buf, int len);
    void processSGR();

    static const char *escapeSequenceName(EscapeSequence seq);

    enum CSISequence {
        CUU = 'A',
        CUD = 'B',
        CUF = 'C',
        CUB = 'D',
        CNL = 'E',
        CPL = 'F',
        CHA = 'G',
        CUP = 'H',
        ED = 'J',
        EL = 'K',
        SU = 'S',
        SD = 'T',
        HVP = 'f',
        SGR = 'm',
        AUX = 'i',
        DSR = 'n',
        SCP = 's',
        RCP = 'u',
        DCH = 'P',
        ICH = '@',
        IL = 'L',
        DL = 'M',
        ECH = 'X',
        REP = 'b',
        VPA = 'd',
        SM = 'h',
        RM = 'l',
        DECSTBM = 'r'  // Set Top and Bottom Margins (scroll region)
    };

    static const char *csiSequenceName(CSISequence seq);

    void scrollUpInRegion(int n);
    void advanceCursorToNewLine();
    void lineFeed();

    // Mouse reporting
    void sendMouseEvent(int button, bool press, bool motion, int cx, int cy, uint32_t modifiers);
    void sendMouseEventPixel(int button, bool press, bool motion, int cx, int cy, int px, int py, uint32_t modifiers);

    int mMouseButtonDown { -1 };
    int mLastMouseX { -1 }, mLastMouseY { -1 };

    // XTSAVE / XTRESTORE: snapshot of DEC private modes saved via CSI ? Pm s,
    // restored via CSI ? Pm r. Empty mode list = all known modes.
    std::unordered_map<int, bool> mSavedPrivateModes;
    void savePrivateModes(const std::vector<int>& modes);
    void restorePrivateModes(const std::vector<int>& modes);

    // OSC 22 mouse pointer shape stacks. Separate stacks for main/alt screen
    // so vim's pointer state in alt screen doesn't bleed back into the main
    // shell on exit. Capped to keep runaway apps from growing unbounded.
    static constexpr size_t MAX_POINTER_SHAPE_STACK = 16;
    std::vector<std::string> mPointerShapeStackMain;
    std::vector<std::string> mPointerShapeStackAlt;
    void notifyPointerShapeChanged();

    // Kitty keyboard protocol
    static constexpr int KITTY_STACK_MAX = 8;
    uint8_t mKittyFlags { 0 };
    uint8_t mKittyStackMain[KITTY_STACK_MAX] {};
    uint8_t mKittyStackAlt[KITTY_STACK_MAX] {};
    int mKittyStackDepthMain { 0 };
    int mKittyStackDepthAlt { 0 };

    void kittyPushFlags(uint8_t flags);
    void kittyPopFlags(int count);
    void kittySetFlags(uint8_t flags, int mode);
    void kittyQueryFlags();
    std::string encodeKittyKey(const KeyEvent& ev) const;

    // Pending selection: button is pressed but mouse hasn't moved yet
    bool mPendingSelection { false };
    int  mPendingSelCol { 0 };
    int  mPendingSelAbsRow { 0 };
    bool mPendingSelXRightHalf { false };

    Selection mSelection;

    // Image registry
    std::unordered_map<uint32_t, std::shared_ptr<ImageEntry>> mImageRegistry;
    uint32_t mNextImageId { 1 };

    // Hyperlink registry (OSC 8)
    struct HyperlinkEntry { std::string uri; std::string id; };
    std::unordered_map<uint32_t, HyperlinkEntry> mHyperlinkRegistry;
    uint32_t mNextHyperlinkId { 1 };
    uint32_t mActiveHyperlinkId { 0 }; // 0 = no active hyperlink

    // Title stack (XTWINOPS CSI 22/23 t + OSC 0/2)
    // Stack top is always the current title. Empty = no title set.
    // Protected by mMutex; lock-free reads via mTitleAtomic.
    std::vector<std::string> mTitleStack;
    static constexpr size_t TITLE_STACK_MAX = 10;

    // Icon stack (XTWINOPS CSI 22/23 t + OSC 1).
    std::vector<std::string> mIconStack;
    static constexpr size_t ICON_STACK_MAX = 10;

    // Lock-free shadow of the current title/icon-stack top (or
    // null shared_ptr if the stack is empty). Read on the per-tick
    // tab-bar resolution path; republished by the parser via
    // publishTitleAtomic() / publishIconAtomic() whenever the top
    // changes. shared_ptr makes the read returnable as a stable
    // optional<string> even if a new title is published mid-read.
    std::atomic<std::shared_ptr<const std::string>> mTitleAtomic;
    std::atomic<std::shared_ptr<const std::string>> mIconAtomic;

    // Desktop notification accumulator (OSC 99)
    std::string mNotifyId;
    std::string mNotifyTitle;
    std::string mNotifyBody;
    uint8_t     mNotifyUrgency { 1 };  // 0=low, 1=normal, 2=critical
    bool        mNotifyCloseResponseRequested { false };  // c=1
    // Action set per kitty notifications.py:160-162. Default {focus} when
    // a= is not specified. +/- prefixes add/remove individual values.
    bool        mNotifyActionFocus  { true };
    bool        mNotifyActionReport { false };
    // Up to 8 button labels (kitty cap, notifications.py:422).
    // U+2028-separated when sent as one p=buttons payload; multiple
    // p=buttons payloads concatenate.
    std::vector<std::string> mNotifyButtons;
    // OSC 99 o= (only_when). Empty == "unset/always". Accepted values per
    // kitty notifications.py:153-157: "always", "unfocused", "invisible".
    // Unknown values silently ignored (no-op assignment). Carries across
    // chunks; resets on dispatch.
    std::string mNotifyOnlyWhen;

    // OSC 133 shell-integration state.
    SemanticMode mSemanticMode { SemanticMode::Inactive };
    std::deque<CommandRecord> mCommandRing;   // all records whose prompt row is still retained
    uint64_t mNextCommandId { 1 };
    bool mCommandInProgress { false };        // true between A and D (or N)
    std::string mCurrentCwd;                  // last OSC 7 value (for command records)
    std::optional<uint64_t> mSelectedCommandId;  // id of command currently highlighted via click or keyboard nav

    int absoluteRowFromScreen(int screenRow) const;
    CommandRecord* inProgressCommandMut();    // nullptr if no in-progress record
    void startCommand(int absRow, int col);
    void markCommandInput(int absRow, int col);
    void markCommandOutput(int absRow, int col);
    void finishCommand(int absRow, int col, std::optional<int> exitCode);
    // Drop front records whose prompt row has fallen past Document's archive cap.
    // Called after operations that may evict archive rows (parse batch, resize).
    void pruneCommandRing();

    // Default colors (OSC 10/11/12)
    DefaultColors mDefaultColors;
    DefaultColors mConfigDefaultColors; // config-loaded originals for OSC 110/111/112 reset
};

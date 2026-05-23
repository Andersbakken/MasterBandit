#pragma once

#include <CellGrid.h>
#include <Decoration.h>
#include <Document.h>
#include <InputTypes.h>
#include <ParserAction.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

std::string toPrintable(const char *chars, int len);

inline std::string toPrintable(const std::string &string)
{
    return toPrintable(string.c_str(), string.size());
}

class TerminalEmulator;
struct TerminalSnapshot;

// X11 distinguishes CLIPBOARD (Ctrl+C/V style) from PRIMARY (drag-select +
// middle-click). Cocoa has only one pasteboard, so Primary downgrades to
// Clipboard there (Window::setPrimarySelection is a no-op by default).
enum class ClipboardTarget
{
    Clipboard,
    Primary
};

struct TerminalCallbacks
{
    std::function<void(TerminalEmulator *, int /*Event*/, void *)> event;
    std::function<void(const std::string &, ClipboardTarget)> copyToClipboard;
    std::function<std::string(ClipboardTarget)> pasteFromClipboard;
    // OSC 0/2 sets the title; XTWINOPS 22/23 push/pop the stack.
    // Fires with Some(str) when OSC writes the top (even an empty string)
    // or a pop exposes a previously-saved string; fires with nullopt when
    // the stack pops empty (no title left). Downstream tabs treat nullopt
    // as "no pane-driven title — fall back to the tab's JS label or the
    // foreground process name".
    std::function<void(std::optional<std::string>)> onTitleChanged;
    std::function<float()> cellPixelWidth;
    std::function<float()> cellPixelHeight;
    std::function<void(int, std::string_view)> onOSC;                  // called for unhandled OSC codes
    std::function<void(std::optional<std::string>)> onIconChanged;     // OSC 1; same semantics as onTitleChanged
    std::function<void(int /*state*/, int /*pct*/)> onProgressChanged; // OSC 9;4
    std::function<bool()> isDarkMode;                                  // for mode 2031
    std::function<void(const std::string &)> onCWDChanged;             // OSC 7
    std::function<void(const std::string &)> onMouseCursorShape;       // OSC 22 (CSS pointer name; "" = default)

    // Desktop notification payload — passed to onDesktopNotification.
    // Aggregates everything the OSC 99 parser accumulated by the time
    // d=1 fired. Other notification protocols (OSC 9 / 777 / 1337) fill
    // a subset and use defaults for the rest.
    struct DesktopNotification
    {
        std::string title;
        std::string body;
        std::string id;                      // OSC i= (may be empty)
        uint8_t urgency             = 1;     // 0=low, 1=normal, 2=critical
        bool closeResponseRequested = false; // c=1
        // Default action set is {focus} per kitty notifications.py:232.
        // OSC 99 a= can add/remove either with +/- prefixes.
        bool actionFocus            = true;
        bool actionReport           = false;
        // Up to 8 button labels from p=buttons (U+2028-split, max-8 cap
        // in kitty notifications.py:422). Empty for non-OSC-99 sources.
        std::vector<std::string> buttons;
        // OSC 99 o= (only_when) gate. Empty == always-allow.
        // "unfocused" — suppress at send time when our window has focus.
        // "invisible" — suppress when focused or visible-but-unfocused.
        // "always" — allow. Other values treated as empty (kitty parity).
        std::string onlyWhen;
    };

    std::function<void(const DesktopNotification &)> onDesktopNotification;

    // OSC 99 "p=close": ask the platform to programmatically dismiss a
    // previously-shown notification keyed by id. id is the OSC i= value
    // from the close command's metadata; never empty (parser drops the
    // call if i= is missing).
    std::function<void(const std::string & /*id*/)> onCloseNotification;

    // OSC 99 "p=alive": query which of the originating channel's
    // notifications are still active. responderId is the i= from the
    // query command — the platform must reply by writing
    // \e]99;i=<responderId>:p=alive;<csv>\a back into the terminal's
    // input stream (kitty notifications.py:1047-1053). The csv lists the
    // OSC i= values still alive on this channel.
    std::function<void(const std::string & /*responderId*/)> onQueryAliveNotifications;
    std::function<void(const std::string &)> onForegroundProcessChanged;
    // Called for XTGETTCAP queries not found in the built-in table.
    // Returns the capability value (may be empty for boolean caps), or nullopt if unknown.
    std::function<std::optional<std::string>(const std::string &)> customTcapLookup;
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
    //   * currentTitle() / currentIcon() — guarded by mTitleIconMutex
    //   * focusedEmbeddedLineId() — atomic<uint64_t>
    //   * hasEvictedEmbeddeds() — atomic<bool>
    // Mutators publish a fresh snapshot via publishLiveView() at the
    // end of each parse batch / mutation; consumers atomic-load.
    //
    // The only other lock in this subsystem is Terminal::mReadBufferMutex
    // which is leaf-level (never held while taking another lock).
    std::recursive_mutex &mutex() const { return mMutex; }

    // DECSCUSR cursor shapes
    enum CursorShape
    {
        CursorBlock           = 0, // blinking block (default)
        CursorSteadyBlock     = 2,
        CursorUnderline       = 3, // blinking underline
        CursorSteadyUnderline = 4,
        CursorBar             = 5, // blinking bar
        CursorSteadyBar       = 6
    };

    // User-level cursor blink override. "Off"/"On" are soft defaults — the
    // running app can still flip blinking via DECSCUSR (blinking vs steady
    // shape) or DEC private mode 12. "Never"/"Always" are hard locks — they
    // shadow the app-controlled state at render time so the cursor is
    // forced unblinking / forced blinking regardless of what the app
    // requests. Modeled after iTerm2's three-way "Blinking cursor" pref,
    // split into four to preserve a config-only initial-default vs locked
    // distinction.
    enum class CursorBlinkMode
    {
        Off,    // initial default: not blinking; app may enable
        On,     // initial default: blinking; app may disable
        Never,  // forced off; app requests ignored at render time
        Always, // forced on;  app requests ignored at render time
    };

    // Per-screen terminal state. Main and alt screens each own an instance;
    // mState points at the active one. 1049 h/l swaps mState so app-set modes
    // (mouse, bracketed paste, focus reporting, etc.) don't leak across the
    // alt-screen boundary. mDefaults holds config-seeded + factory defaults
    // used by RIS (and 1049-on-entry for the alt state).
    // Character sets for G0/G1 designation (ESC ( X, ESC ) X). Only the three
    // sets that real-world software actually uses: US ASCII (default), UK
    // (# → £), and DEC Special Graphics (line drawing + misc).
    enum Charset : uint8_t
    {
        CharsetASCII,
        CharsetUK,
        CharsetDECGraphics
    };

    struct TerminalState
    {
        int cursorX { 0 }, cursorY { 0 };
        bool cursorVisible { true };
        CursorShape cursorShape { CursorBlock };
        bool cursorBlinkEnabled { true };     // DEC private mode 12
        bool wrapPending { false };           // deferred autowrap state
        CellAttrs currentAttrs;               // SGR "pen"
        uint32_t currentUnderlineColor { 0 }; // SGR 58: packed RGBA8, 0 = use fg
        // Character set slots and GL selector. Per-screen so alt-screen apps
        // (ncurses TUIs etc.) can't leak charset state back to the shell.
        Charset charsetG0 { CharsetASCII };
        Charset charsetG1 { CharsetASCII };
        bool shiftOut { false }; // false: GL=G0, true: GL=G1 (SO/SI)
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
        int scrollTop { 0 }, scrollBottom { 0 }; // scroll region [top, bottom)
        bool cursorKeyMode { false };            // DECCKM
        bool keypadMode { false };               // DECKPAM
        bool autoWrap { true };                  // DECAWM
        bool originMode { false };               // DECOM — CUP/HVP relative to scroll region
        bool insertMode { false };               // IRM
        bool bracketedPaste { false };
        bool focusReporting { false };           // mode 1004
        bool syncOutput { false };               // mode 2026
        bool colorPreferenceReporting { false }; // mode 2031
        bool mouseMode1000 { false };
        bool mouseMode1002 { false };
        bool mouseMode1003 { false };
        bool mouseMode1006 { false };
        bool mouseMode1016 { false }; // SGR-Pixel
        // Selective Mouse Reporting (CSI = w). See SELECTIVE-MOUSE-REPORTING.md.
        uint16_t selMouseButtonMask { 0 };
        uint8_t selMouseEventMask { 0 };
    };

    int cursorX() const { return mState->cursorX; }

    int cursorY() const { return mState->cursorY; }

    bool cursorVisible() const { return mState->cursorVisible; }

    CursorShape cursorShape() const { return mState->cursorShape; }

    bool cursorBlinkEnabled() const { return mState->cursorBlinkEnabled; }

    // OSC 22 — current pointer shape (CSS name); empty = platform default.
    std::string currentPointerShape() const
    {
        const auto &s = mUsingAltScreen ? mPointerShapeStackAlt : mPointerShapeStackMain;
        return s.empty() ? std::string {} : s.back();
    }

    // True if `name` is a CSS pointer name we recognise (or a kitty/X11 alias).
    static bool isKnownPointerShape(std::string_view name);

    // True iff the cursor should currently visibly blink. Render-time
    // resolution layered three-deep:
    //   1. CursorBlinkMode::Never / Always (user lock) — short-circuit
    //      both directions, ignoring whatever the app requested.
    //   2. DECSCUSR blinking shape variant (Block/Underline/Bar) — blinks
    //      regardless of mode-12 state, matching xterm semantics.
    //   3. DEC private mode 12 (cursorBlinkEnabled) for steady shapes.
    bool cursorBlinking() const
    {
        if (mBlinkMode == CursorBlinkMode::Never) {
            return false;
        }
        if (mBlinkMode == CursorBlinkMode::Always) {
            return true;
        }
        switch (mState->cursorShape) {
            case CursorBlock:
            case CursorUnderline:
            case CursorBar:
                return true;
            default:
                return mState->cursorBlinkEnabled;
        }
    }

    CursorBlinkMode cursorBlinkMode() const { return mBlinkMode; }

    // Config-applied cursor defaults. Propagates the new default to the
    // config prototype and to BOTH screen states so the user-visible cursor
    // updates live regardless of which screen is active, and so returning
    // from alt doesn't revert to a stale pre-config-reload shape.
    void setDefaultCursorShape(CursorShape s)
    {
        mDefaults.cursorShape  = s;
        mMainState.cursorShape = s;
        mAltState.cursorShape  = s;
    }

    void setDefaultCursorBlinkEnabled(bool b)
    {
        mDefaults.cursorBlinkEnabled  = b;
        mMainState.cursorBlinkEnabled = b;
        mAltState.cursorBlinkEnabled  = b;
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
    const IGrid &grid() const { return mUsingAltScreen ? static_cast<const IGrid &>(mAltGrid) : static_cast<const IGrid &>(mDocument); }

    IGrid &grid() { return mUsingAltScreen ? static_cast<IGrid &>(mAltGrid) : static_cast<IGrid &>(mDocument); }

    bool usingAltScreen() const { return mUsingAltScreenAtomic.load(std::memory_order_acquire); }

    const Document &document() const { return mDocument; }

    Document &document() { return mDocument; }

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

    // Scroll the viewport so the line with the given logical id is visible
    // (best-effort: brings the line's first abs row to the top of the
    // viewport, clamped by the available history). Returns true if the id
    // resolves and the viewport changed; false if the id has been evicted
    // past the archive cap, the id resolves to a row already in the
    // viewport (no-op), or the row is already at the visible top.
    bool scrollToRow(uint64_t lineId);
    void selectCommandOutput();              // select output around current viewport position
    std::string serializeScrollback() const; // serialize all content for pager

    enum Event
    {
        Update,
        ScrollbackChanged,
        VisibleBell,
        CommandComplete,         // payload: const CommandRecord*
        CommandSelectionChanged, // payload: nullptr; read selectedCommandId() for new value
        AltScreenChanged         // payload: nullptr; read usingAltScreen() for new value
    };

    // Semantic mode transitioned by OSC 133 A/B/C/D; tracks "what is the terminal
    // currently writing?" at the cell level. Inactive = no OSC 133 session active.
    enum class SemanticMode : uint8_t
    {
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
    struct CommandRecord
    {
        uint64_t id                 = 0;
        // Logical-line ids from Document — resolve to current abs-row at
        // query time via Document::firstAbsOfLine / lastAbsOfLine.
        uint64_t promptStartLineId  = 0;
        uint64_t commandStartLineId = 0;
        uint64_t outputStartLineId  = 0;
        uint64_t outputEndLineId    = 0;
        int promptStartCol          = -1;
        int commandStartCol         = -1;
        int outputStartCol          = -1;
        int outputEndCol            = -1;
        std::string cwd;             // OSC 7 value at A
        std::optional<int> exitCode; // from OSC 133;D;<n>
        uint64_t startMs = 0;        // when C fired
        uint64_t endMs   = 0;        // when D fired
        bool complete    = false;
    };

    const std::deque<CommandRecord> &commands() const { return mCommandRing; }

    const CommandRecord *lastCommand() const; // most recently completed record (skipping any in-flight tail), nullptr if none

    // Hit-test: find the command whose logical-line span contains this id.
    // Lines above the oldest prompt or between complete commands return nullptr.
    // O(log N) via binary search — ring stays sorted by promptStartLineId by
    // construction (startCommand only appends, pruneCommandRing only pops front).
    const CommandRecord *commandForLineId(uint64_t lineId) const;

    // Look up a record by its CommandRecord::id. O(log N) binary search — the
    // ring is sorted by id (monotonic mNextCommandId++ at startCommand; only
    // append + front-pop). Returns nullptr if the id isn't in the ring.
    const CommandRecord *commandForId(uint64_t commandId) const;

    // Select the given command's output region as a text selection and auto-copy
    // to clipboard (same semantics as selectCommandOutput()). Used by mouse paths
    // that already know which command was clicked, avoiding the viewport-center
    // heuristic. No-op if rec is null or its lines have been evicted.
    void selectCommandOutputForRecord(const CommandRecord *rec);

    // Selection of a single command region (OSC 133-scoped). Mutations go
    // through setSelectedCommand so the render thread can observe via snapshot.
    // The id references CommandRecord::id; if the id no longer exists in the
    // ring (command evicted), the selection clears on the next pruneCommandRing.
    std::optional<uint64_t> selectedCommandId() const { return mSelectedCommandId; }

    void setSelectedCommand(std::optional<uint64_t> commandId);

    struct Action
    {
        enum Type
        {
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
            ClearScrollbackHistory, // xterm CSI 3 J: drop scrollback; live grid untouched
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

    // Reads the shadow atomic so main-thread callers (InputController,
    // PlatformDawn::onScroll) don't race with the parse worker
    // mutating the underlying mode bools under mMutex. Worker-side
    // reads (DECRQM, DECSC, etc.) go through the struct fields
    // directly under mMutex.
    bool mouseReportingActive() const { return mMouseReportingActiveAtomic.load(std::memory_order_acquire); }

    // True when CSI = w state would cause an event to be reported for *some*
    // (button, event) pair. Legacy ?1000/?1002/?1003 take precedence: this
    // returns false while any of them is set, even if the selective masks
    // are non-zero. See SELECTIVE-MOUSE-REPORTING.md.
    bool selectiveMouseActive() const { return mSelectiveMouseActiveAtomic.load(std::memory_order_acquire); }

    // True when a wheel-up or wheel-down notch would be emitted to the PTY.
    // Used by the platform layer's wheel router to decide whether to forward
    // to mousePressEvent (and suppress scrollback) or fall through to native
    // scrollback. Same race shape as mMouseReportingActiveAtomic.
    bool wheelEmissionActive() const { return mWheelEmissionActiveAtomic.load(std::memory_order_acquire); }

    // Pull-model title/icon: returns the top of the XTWINOPS stack, or
    // nullopt when the stack is empty (no OSC 0/2 has set one, or it's been
    // fully popped away). Push duplicates the current top and is a no-op on
    // an empty stack, so stack-non-empty is equivalent to "app has set a
    // title at some point and hasn't fully revoked it."
    // Title/icon stacks are mutated by the parse worker (OSC 2/0/1,
    // XTWINOPS push/pop) under mMutex. Per-tick tab-bar resolution
    // in buildRenderFrameState reads them on every frame, so we
    // expose a shadow of the stack top via mTitleShadow / mIconShadow
    // guarded by mTitleIconMutex (nullopt when the underlying stack
    // is empty), republished by the parser whenever the top changes.
    // Reading under mMutex would block buildRenderFrameState for the
    // entire parse-apply duration of a flooding pane (the apply runs
    // in one shot — see Terminal::queueParse — so it can be hundreds
    // of ms for ~1 MiB of input).
    std::optional<std::string> currentTitle() const
    {
        std::lock_guard<std::mutex> lock(mTitleIconMutex);
        return mTitleShadow;
    }

    std::optional<std::string> currentIcon() const
    {
        std::lock_guard<std::mutex> lock(mTitleIconMutex);
        return mIconShadow;
    }

    bool syncOutputActive() const { return mState->syncOutput; }

    uint8_t kittyFlags() const { return mKittyFlags; }

    bool colorPreferenceReporting() const { return mState->colorPreferenceReporting; }

    void setPaletteColor(int idx, uint8_t r, uint8_t g, uint8_t b)
    {
        if (idx >= 0 && idx < 16) {
            m16ColorPalette[idx][0] = r;
            m16ColorPalette[idx][1] = g;
            m16ColorPalette[idx][2] = b;
        }
    }

    void applyColorScheme(const struct ColorScheme &cs);
    void applyCursorConfig(const struct CursorConfig &cc);

    // Default colors (for OSC 10/11/12 and rendering)
    struct DefaultColors
    {
        uint8_t fgR { 0xDD }, fgG { 0xDD }, fgB { 0xDD };
        uint8_t bgR { 0x00 }, bgG { 0x00 }, bgB { 0x00 };
        uint8_t cursorR { 0xCC }, cursorG { 0xCC }, cursorB { 0xCC };
    };

    const DefaultColors &defaultColors() const { return mDefaultColors; }

    const DefaultColors &configDefaultColors() const { return mConfigDefaultColors; }

    const std::string *hyperlinkURI(uint32_t id) const
    {
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
    enum class SelectionMode
    {
        Normal,
        Word,
        Line,
        Rectangle
    };

    struct Selection
    {
        uint64_t startLineId { 0 };
        int startCellOffset { 0 };
        uint64_t endLineId { 0 };
        int endCellOffset { 0 };
        bool active { false };
        bool valid { false };
        SelectionMode mode { SelectionMode::Normal };

        // Anchor span captured by `startWordSelection` / `startLineSelection`
        // so that a follow-up drag extends from the original word/line rather
        // than collapsing it. Drag-extension in Word / Line modes computes the
        // word/line span at the cursor and merges it with this anchor. Unused
        // in Normal / Rectangle modes (their press point doubles as anchor via
        // start{LineId,CellOffset}).
        uint64_t anchorStartLineId { 0 };
        int anchorStartCellOffset { 0 };
        uint64_t anchorEndLineId { 0 };
        int anchorEndCellOffset { 0 };
    };

    // Resolved view of a Selection — abs rows looked up at the time of the
    // call. Used by snapshot mirroring and by callers that need rendering
    // coordinates rather than logical-line identity.
    struct ResolvedSelection
    {
        int startCol { 0 };
        int startAbsRow { 0 };
        int endCol { 0 };
        int endAbsRow { 0 };
        bool active { false };
        bool valid { false };
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

    const Selection &selection() const { return mSelection; }

    // Resolve `mSelection`'s lineIds to current abs rows. Returns empty
    // optional when there's no active/valid selection or when either anchor
    // has evicted past the archive cap.
    std::optional<ResolvedSelection> resolveSelection() const;
    bool isCellSelected(int col, int absRow) const;
    std::string selectedText() const;

    // Decoration overlay (see Decoration.h). Single source of truth for
    // selection / OSC 133 command region / OSC 8 hyperlink underlines / JS-
    // driven highlights. Live storage anchors are line-id+cellOffset; the
    // snapshot mirror resolves to abs rows. Mutations under `mMutex`.
    //
    // For Selection / CommandRegion / Hyperlink kinds the storage is owned
    // by the relevant lifecycle (selection methods, setSelectedCommand, OSC
    // parser); JS callers must use the User kind. clearDecorations(tag) for
    // a system kind is a no-op (tag is empty for those).
    uint64_t addDecoration(Decoration spec);
    // `cancelledAnimHandlesOut`, if non-null, is appended with the handleIds
    // of any in-flight animations on the removed decoration; the JS layer
    // uses these to resolve the corresponding .onEnd() promises as
    // "cancelled". Pass nullptr if the caller has no observers.
    bool removeDecoration(uint64_t id, std::vector<uint64_t> *cancelledAnimHandlesOut = nullptr);
    // Remove all User decorations matching `tag`. Empty `tag` clears every
    // User decoration (system kinds untouched). Returns count removed.
    // `cancelledAnimHandlesOut` aggregates handleIds across every removed
    // decoration (same semantics as removeDecoration).
    size_t clearUserDecorations(std::string_view tag                           = {},
                                std::vector<uint64_t> *cancelledAnimHandlesOut = nullptr);

    // Apply a queued sequence of Add / Clear ops atomically: one mMutex
    // acquisition, at most one snapshot publish at the end. Add-op ids are
    // returned in the order they appear in `ops` (Clear ops contribute
    // nothing). System-kind decorations are never affected by Clear ops,
    // regardless of tag.
    //
    // Use this when a script mutation would otherwise produce a burst of
    // individual addDecoration / clearDecorations calls — every one of
    // those publishes a snapshot, and the render thread can sample
    // mid-burst and paint with incomplete state (visible as a blank-frame
    // flicker between the clear and the re-add).
    std::vector<uint64_t> applyDecorationBatch(std::vector<DecorationBatchOp> ops,
                                               std::vector<uint64_t> *cancelledAnimHandlesOut = nullptr);

    // wezterm-style screen clear: lift the in-progress prompt span up to
    // row 0, wipe everything else in the live grid, optionally drop
    // scrollback. The prompt-span is `[promptStartLineId..cursorY]` when
    // an OSC 133 in-progress command record is available and its prompt-
    // start resolves into the current viewport; otherwise just the
    // cursor's row is preserved (matches wezterm's behaviour exactly).
    // Cursor is moved up by the same amount the prompt was lifted, so
    // cursorX is preserved and the shell's input position relative to
    // the prompt is unchanged. No-op on alt screen — TUI apps own the
    // screen there.
    //
    // Caveat (carried from wezterm): the shell's line editor still
    // thinks its prompt is at the original row. The first subsequent
    // shell redraw (arrow key, tab completion, RPROMPT update) will
    // paint over that original position, leaving the lifted prompt as
    // an orphaned copy at the top. This is the cost of operating
    // without shell cooperation; it's acceptable because the alternative
    // (sending Ctrl+L to the PTY) breaks for non-shell foreground
    // processes (`top`, `ssh`, `cat > file`, …).
    void clearWithPromptPreserved(bool alsoScrollback);

    const std::vector<Decoration> &decorations() const { return mDecorations; }

    // O(1) id → Decoration lookup. Returns nullptr when the id is unknown.
    // Replaces the linear-scan pattern at every decoration-by-id callsite
    // (animation final-value sampler, JS handle getters, removeDecoration).
    const Decoration *decorationById(uint64_t id) const;

    // Per-line index of single-line User decorations (startLineId ==
    // endLineId). Snapshot construction walks visible lineIds and looks
    // up here, so the per-frame cost is O(visible_rows) rather than
    // O(total_decorations). Multi-line decorations are listed separately
    // in `multiLineDecorationIds()` and always scanned (count is bounded
    // by user actions, not search).
    const std::unordered_map<uint64_t, std::vector<uint64_t>> &
    decorationsByStartLine() const
    {
        return mDecsByStartLine;
    }

    const std::vector<uint64_t> &multiLineDecorationIds() const
    {
        return mMultiLineDecIds;
    }

    // Resolve a single decoration to abs-row coordinates. Returns empty
    // when an anchor has evicted past the archive cap. Used by the snapshot
    // mirror.
    std::optional<ResolvedDecoration> resolveDecoration(const Decoration &dec) const;

    // --- Animation registry ---
    //
    // Flat list of in-flight animations. Each entry points at its target
    // (currently always a decoration; extending the animation system is
    // adding AnimTargetKind variants and per-target sample/finalize
    // routing). The map is keyed by handleId for O(1) cancel/finish; the
    // snapshot copies the values into a vector for cache-friendly
    // iteration in the renderer.
    //
    // Lifecycle (start / arm completion timer / resolve .onEnd promise)
    // is owned by ScriptEngine. The emulator is just the data store —
    // it doesn't tick or advance these descriptors; the renderer samples
    // them at draw time.
    const std::unordered_map<uint64_t, Animation> &animations() const { return mAnimations; }

    // Install a new animation. If another animation already targets the
    // same (kind, targetId, prop) slot, returns its handleId (so the JS
    // layer can resolve that prior animation's onEnd as "cancelled") and
    // erases it from the registry; returns 0 if no replacement happened.
    uint64_t startAnimation(Animation a);

    // Finalize an animation by handleId: writes endValue into the
    // appropriate static field on the targeted object, removes the
    // animation entry. No-op (returns false) if the entry no longer
    // exists.
    bool finishAnimation(uint64_t handleId);

    // Cancel an animation. snapToEnd=true behaves like finishAnimation
    // (writes endValue); false samples at `nowMs` and writes that.
    bool cancelAnimation(uint64_t handleId, bool snapToEnd, uint64_t nowMs);

    // Test-only override for the monotonic clock — when set, mono() returns
    // this value instead of the wall clock. Used by the renderer's sample
    // path and any caller that uses mono() for animation timing. Pass 0 to
    // restore wall-clock behavior. Persistent until cleared.
    static void setMonoForTest(uint64_t t) { sMonoOverride = t; }

private:
    // Internal helper: write a sampled / endValue result into the static
    // field of whatever the animation's target is. Routes by AnimTargetKind.
    void applyAnimResultToTarget(AnimTargetKind kind, uint64_t targetId, AnimProp prop, int64_t value);

public:
    // Image registry
    struct ImageEntry
    {
        uint32_t id { 0 };
        uint32_t imageNumber { 0 }; // I= (non-unique)
        uint32_t pixelWidth { 0 }, pixelHeight { 0 };
        uint32_t cellWidth { 0 }, cellHeight { 0 };
        // Source rect crop (0 = use full image)
        uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
        std::vector<uint8_t> rgba; // root frame (frame 0)
        // iTerm OSC 1337 "name=" metadata (base64-decoded filename). Never set
        // by kitty graphics. Purely informational — not displayed.
        std::string name;

        // Per-placement display parameters (one image, multiple positions)
        struct Placement
        {
            uint32_t cellWidth { 0 }, cellHeight { 0 };
            uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
            uint32_t cellXOffset { 0 }, cellYOffset { 0 }; // X=, Y= sub-cell pixel offsets
            int32_t zIndex { 0 };
        };

        std::unordered_map<uint32_t, Placement> placements; // placementId → params

        // Animation
        struct Frame
        {
            std::vector<uint8_t> rgba; // full frame RGBA data (same dimensions as image)
            uint32_t gap { 40 };       // ms before advancing to next frame
        };

        std::vector<Frame> extraFrames;
        uint32_t currentFrameIndex { 0 }; // 0 = root, 1+ = extraFrames[i-1]
        uint32_t frameGeneration { 0 };   // bumped on frame change, for GPU staleness detection
        uint32_t currentLoop { 0 };
        uint32_t maxLoops { 0 };     // 0 = infinite
        uint64_t frameShownAt { 0 }; // monotonic time current frame was first displayed

        enum AnimState : uint8_t
        {
            Stopped = 0,
            Loading = 1,
            Running = 2
        };

        AnimState animationState { Stopped };
        uint32_t rootFrameGap { 40 };

        const std::vector<uint8_t> &currentFrameRGBA() const
        {
            if (currentFrameIndex == 0 || extraFrames.empty()) {
                return rgba;
            }
            uint32_t idx = currentFrameIndex - 1;
            if (idx < extraFrames.size()) {
                return extraFrames[idx].rgba;
            }
            return rgba;
        }

        uint32_t currentFrameGap() const
        {
            if (currentFrameIndex == 0 || extraFrames.empty()) {
                return rootFrameGap;
            }
            uint32_t idx = currentFrameIndex - 1;
            if (idx < extraFrames.size()) {
                return extraFrames[idx].gap;
            }
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
    const std::unordered_map<uint32_t, std::shared_ptr<ImageEntry>> &imageRegistry() const { return mImageRegistry; }

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

    // Feed bytes into the VT parser. Returns the number of bytes
    // consumed (always == len in the current implementation).
    //
    // Two-phase: parseToActions decodes bytes into a vector of
    // ParserAction::Action under mParseStateMutex (no grid/mState/
    // mDocument access), then applyActions drains the vector under
    // mMutex. Lock ordering is mParseStateMutex first, then mMutex —
    // callers that already hold mMutex must NOT call injectData,
    // because the inner mParseStateMutex acquisition would establish
    // the reverse order and risk deadlock against a concurrent
    // injectData on another thread. Such callers should reach for the
    // protected applyControl helper instead (see Terminal::
    // createEmbedded).
    size_t injectData(const char *data, size_t len);

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
    struct EmbeddedAnchor
    {
        uint64_t lineId = 0;
        int rows        = 0;
    };

    virtual void collectEmbeddedAnchors(std::vector<EmbeddedAnchor> & /*out*/) const {}

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
    struct ViewAnchor
    {
        int viewRow     = 0;
        int rows        = 0;
        uint64_t lineId = 0;
    };

    // Compute the list of embedded anchors currently visible in the
    // viewport, sorted by viewport row ascending. Filters anchors whose
    // backing line has evicted or whose row falls outside the viewport.
    // viewportRows is the number of logical (unshifted) rows in the
    // viewport — i.e. the Terminal's height().
    static std::vector<ViewAnchor> collectVisibleAnchors(
        const TerminalEmulator &term, int viewportOffset, int viewportRows);

    static uint64_t mono();

    // 16-color palette (standard + bright) as RGB — current runtime values
    uint8_t m16ColorPalette[16][3];
    // Config-loaded defaults for OSC 104 reset (indices 0-15)
    uint8_t m16PaletteDefaults[16][3];
    // Runtime overrides for any of the 256 palette entries (set via OSC 4)
    std::unordered_map<int, std::array<uint8_t, 3>> m256PaletteOverrides;
    // 256-color palette lookup
    void color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b) const;

protected:
    virtual void writeToOutput(const char *data, size_t len) {}

    // Reads the shadow atomic so main-thread callers (Terminal::pasteText)
    // don't race with the parse worker mutating mState->bracketedPaste
    // under mMutex. Worker-side reads (DECRQM, DECSC) go through
    // mState->bracketedPaste directly under mMutex.
    bool bracketedPaste() const { return mBracketedPasteAtomic.load(std::memory_order_acquire); }

    void resetScrollback(int scrollbackLines); // reinitializes document with given scrollback capacity

    TerminalCallbacks &callbacks() { return mCallbacks; }

public:
    // Snapshot publish/subscribe channel.
    //
    // The parser builds a fresh TerminalSnapshot at the end of injectData
    // (rate-limited to ~120 Hz so high-throughput input streams don't drown
    // the parser thread in snapshot work) and publishes it via the channel.
    // The render thread / debug IPC / anyone needing a read-only view
    // calls loadSnapshot() to atomically pick up the latest published copy
    // — no mMutex contention with the parser's apply phase.
    //
    // Returns nullptr until the first publish (caller falls back to
    // building one synchronously, or skips the frame).
    std::shared_ptr<const TerminalSnapshot> loadSnapshot() const;

    // Force a synchronous build + publish ignoring the rate limiter.
    // Tests use this to deterministically produce a snapshot reflecting
    // the latest feed() bytes; production should use the per-injectData
    // path instead.
    void publishSnapshotForTest();

private:
    // Build a fresh snapshot from current state and publish it via the
    // channel — skipped only while a 2026 sync block is in progress (mHold).
    // Called from injectData under mMutex. Returns true iff a publish
    // actually happened.
    bool publishSnapshotIfDue();
    // The actual builder: constructs a shared_ptr<TerminalSnapshot>,
    // populates it from `*this`, swaps it into the channel. Caller must
    // hold mMutex.
    void buildAndPublishSnapshotLocked();

    // Decoration-index maintenance. Caller must hold mMutex.
    //
    // indexAddDecorationLocked: append-side incremental maintenance. Called
    // after pushing the Decoration onto mDecorations; classifies it as
    // single- or multi-line and updates the right index.
    //
    // rebuildDecorationIndexesLocked: full rebuild from mDecorations.
    // Called after any erase that shifts vector positions (since the id →
    // idx map keys those positions). Tracking shifts incrementally is
    // complexity for no perf win — erases are user-driven (search-clear
    // tag, JS clearDecorations) and the vector pass already costs O(N).
    void indexAddDecorationLocked(const Decoration &d);
    void rebuildDecorationIndexesLocked();

protected:
    // Publish a fresh snapshot and then fire the given event. Use at any
    // state-change site whose cadence is ~human-paced (resize, viewport
    // scroll, selection mutation, command navigation). Caller must hold
    // mMutex. Centralizes the "publish before notify" invariant so
    // loadSnapshot() returns post-mutation state.
    void publishAndFireEvent(int ev);

private:
    TerminalCallbacks mCallbacks;

    mutable std::recursive_mutex mMutex;

    mutable std::mutex mSnapshotChanMutex;
    std::shared_ptr<const TerminalSnapshot> mSnapshotLatest;

    int mWidth { 0 }, mHeight { 0 };

    // Horizontal tab stops — terminal-global (shared between main/alt screens).
    // Sized to mWidth; entry is 1 at a tab stop column, 0 otherwise.
    std::vector<uint8_t> mTabStops;

    // Per-screen state. See TerminalState definition above.
    TerminalState mMainState;
    TerminalState mAltState;
    TerminalState mDefaults;               // seeded from config; source for resetToDefault().
    TerminalState *mState { &mMainState }; // active state — follows 1049 h/l.

    // User-level blink override. Not part of TerminalState because it's a
    // user preference that must persist across 1049 h/l screen swaps and
    // across RIS — neither the app nor an alt-screen entry/exit can change
    // it. Updated only by applyCursorConfig() (config load + hot reload).
    CursorBlinkMode mBlinkMode { CursorBlinkMode::On };

    // Reset `s` to current defaults, plus runtime-derived fields (scroll region).
    void resetToDefault(TerminalState &s)
    {
        s              = mDefaults;
        s.scrollBottom = mHeight;
    }

    Document mDocument;
    CellGrid mAltGrid;
    bool mUsingAltScreen { false };
    // Mirror of mUsingAltScreen kept in sync at every write site
    // (TerminalEmulator.cpp:1013, 2020, 2081). Read by main-thread
    // callers via the usingAltScreen() accessor with acquire ordering.
    std::atomic<bool> mUsingAltScreenAtomic { false };

    // Mirror of mState->bracketedPaste. The struct field is mutated
    // by the parse worker under mMutex (mode 2004 set/reset/save/
    // restore), but Terminal::pasteText reads it from the main thread
    // without holding mMutex. Plain bool reads are atomic on x86/ARM
    // but lack happens-before vs. the worker's write — so main could
    // see a stale value for one tick. Shadow atomic kept in sync at
    // every write site, read by the bracketedPaste() accessor.
    std::atomic<bool> mBracketedPasteAtomic { false };

    // Mirror of (mouseMode1000 || mouseMode1002 || mouseMode1003).
    // Same race shape as mBracketedPasteAtomic — the worker writes
    // the three bools under mMutex from DEC mode handling; main
    // reads via mouseReportingActive() from InputController and
    // PlatformDawn::onScroll without holding mMutex. Single computed
    // mirror (rather than three separate shadows) makes the read
    // path one atomic load — input is the hot path here.
    std::atomic<bool> mMouseReportingActiveAtomic { false };

    // Shadow of selectiveMouseActive(): true when no legacy mouse mode is
    // set and either selective mask is non-zero. Same race shape as
    // mMouseReportingActiveAtomic.
    std::atomic<bool> mSelectiveMouseActiveAtomic { false };

    // Shadow of wheelEmissionActive(): true when *any* path would emit a
    // wheel event — legacy or selective-with-wheel-bits-press-set.
    std::atomic<bool> mWheelEmissionActiveAtomic { false };

    void syncMouseReportingAtomic()
    {
        const bool legacy = mState->mouseMode1000 || mState->mouseMode1002 || mState->mouseMode1003;
        mMouseReportingActiveAtomic.store(legacy, std::memory_order_release);
        const bool selective = !legacy && (mState->selMouseButtonMask != 0) && (mState->selMouseEventMask != 0);
        mSelectiveMouseActiveAtomic.store(selective, std::memory_order_release);
        // Wheel-up bit 0x0008, wheel-down bit 0x0010; press bit 0x1.
        const bool wheelSel = selective && (mState->selMouseButtonMask & 0x0018) && (mState->selMouseEventMask & 0x1);
        mWheelEmissionActiveAtomic.store(legacy || wheelSel, std::memory_order_release);
    }

    // Integer row-count viewport anchor. `scrollUpInRegion` compensates by
    // += n when the user is scrolled back (non-zero offset) so they stay
    // pinned to the same content as new rows stream in; when offset == 0
    // (live), the viewport auto-follows. Line-id anchoring was tried
    // briefly but broke on soft-wrap chains where inheritLineIdFromAbove
    // left the same id across many rows and firstAbsOfLine returned the
    // chain's head instead of the intended first-screen-row position.
    int mViewportOffset { 0 };

    char32_t mLastPrintedChar { 0 };                // for REP (CSI b)
    int mLastPrintedX { -1 }, mLastPrintedY { -1 }; // position of last stored cell (for combining codepoints)
    uint_least16_t mGraphemeState { 0 };            // libgrapheme stateful break detection

    enum ParserState
    {
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

    // DEC mode 2026 (synchronized output) hold flag, owned by
    // parseToActions. Set on "CSI ?2026 h", cleared on "CSI ?2026 l"
    // or RIS. injectData currently uses it only to suppress the
    // Update render-poke during a sync block; full hold (defer apply
    // across reads) was tried and reverted because it breaks tests /
    // callers that set a mode and immediately query DECRQM (the
    // response gets buffered).
    bool mHold { false };

    // Reused action buffer. parseToActions appends, injectData drains
    // under mMutex on every call. Kept as a member to avoid per-call
    // vector allocation.
    std::vector<ParserAction::Action> mPendingActions;

    // Serializes the decode phase. Normally only the worker thread
    // enters parseToActions, so this is uncontended. DebugIPC's inject /
    // feed commands run on the libwebsockets thread and would otherwise
    // race the worker on parser-state fields (mParserState,
    // mEscapeBuffer, mUtf8Buffer, mStringSequence, mPendingActions).
    // Lock ordering: mParseStateMutex first, then mMutex. Never
    // reverse — anyone holding mMutex must not call injectData.
    std::mutex mParseStateMutex;

    // Refactored to take parser-state as args so the eventual
    // parseToActions / applyActions split can call them without the
    // helpers reading mStringSequence / mStringSequenceType /
    // mEscapeBuffer / mEscapeIndex member fields directly.
    void processStringSequence(uint8_t kind, std::string_view body);
    void processDCS(std::string_view payload);
    void processOSC_Title(std::string_view text, bool setTitle);

    // Decode `len` bytes of pty output, appending Actions to
    // mPendingActions. No grid / mState / mDocument access — purely
    // operates on parser-state member fields (mParserState,
    // mUtf8Buffer, mEscapeBuffer, mStringSequence, mHold,
    // mPendingActions). Caller must hold mParseStateMutex; injectData
    // does this. Returns the number of bytes consumed (always == len
    // in the current implementation).
    size_t parseToActions(const char *buf, size_t len);

    // Apply the actions in `actions` to grid / mDocument / mState.
    // Caller must hold mMutex. Drains the vector by std::visit on each
    // action; helpers below do the per-variant work.
    void applyActions(std::vector<ParserAction::Action> &actions);

    // Per-variant apply helpers — port of the inline mutation logic
    // that lived inside injectData. writePrintable handles charset
    // translation, grapheme cluster combining, wide-char placement,
    // cursor advance and wrap. applyControl handles C0 (CR/LF/BS/HT/
    // VT/FF/BEL/SO/SI). applyEsc handles ESC X (RIS, DECSC, DECRC, IND,
    // NEL, HTS, RI, VB, DECKPAM, DECKPNM). applyDesignateCharset
    // mutates mState->charsetG0 or charsetG1.
    void writePrintable(char32_t cp);

protected:
    // applyControl is exposed to subclasses (Terminal::createEmbedded)
    // so they can synthesize CR/LF directly without re-entering the
    // parser from the main thread. The other helpers stay private.
    void applyControl(ParserAction::ControlCode code);

private:
    void applyEsc(char finalByte);
    void applyDesignateCharset(char slot, char charset);

    void updateWordSelection(int col, int absRow);
    void updateLineSelection(int absRow);

    // Republish mTitleShadow / mIconShadow from the current top of
    // mTitleStack / mIconStack. Caller must hold mMutex (parser
    // path always does); these acquire mTitleIconMutex internally.
    // Skipped when the new value matches the currently-published
    // one, so the cost is one short critical section per *change*,
    // not per write.
    void publishTitle();
    void publishIcon();
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
    struct KittyLoadState
    {
        std::vector<uint8_t> data;                 // accumulated decoded payload
        uint32_t id          = 0;                  // client image ID (i=)
        uint32_t imageNumber = 0;                  // I= (non-unique image number)
        uint32_t placementId = 0;                  // placement ID (p=)
        uint32_t format      = 32;                 // f= (24=RGB, 32=RGBA, 100=PNG)
        uint32_t width = 0, height = 0;            // s=, v= (source data dimensions)
        uint32_t cellCols = 0, cellRows = 0;       // c=, r=
        uint32_t xOffset = 0, yOffset = 0;         // x=, y= (source rect offset)
        uint32_t cellXOffset = 0, cellYOffset = 0; // X=, Y= (context-dependent: sub-cell offset or a=f compose/bg)
        uint32_t cropWidth = 0, cropHeight = 0;    // w=, h= (source rect size)
        uint32_t quiet          = 0;               // q=
        int32_t zIndex          = 0;               // z=
        uint32_t cursorMovement = 0;               // C=
        uint32_t dataSize       = 0;               // S=
        uint32_t dataOffset     = 0;               // O=
        char action             = 'T';             // a=
        char compressed         = 0;               // o=
        char transmissionType   = 'd';             // t=
        bool active             = false;
    };

    KittyLoadState mKittyLoading;
    uint32_t mLastKittyImageId { 0 }; // for a=f/a=a when i=0

    // https://ttssh2.osdn.jp/manual/en/about/ctrlseq.html
    enum EscapeSequence
    {
        SS2     = 'N',
        SS3     = '0',
        DCS     = 'P',
        CSI     = '[',
        ST      = '\\',
        OSX     = ']',
        SOS     = 'X',
        PM      = '^',
        APC     = '_',
        RIS     = 'c',
        VB      = 'g',
        DECKPAM = '=',
        DECKPNM = '>',
        DECSC   = '7',
        DECRC   = '8',
        IND     = 'D',
        NEL     = 'E',
        HTS     = 'H',
        RI      = 'M'
    };

    void processCSI(const char *buf, int len);
    void processSGR(const char *buf, int len);

    // BCE: build a Cell that carries the active state's SGR background and
    // push it to both grids so subsequent erase ops fill with that bg instead
    // of default. fg / styles / inverse / semantic type are all reset on the
    // erase blank — only bg propagates (matches xterm behavior). Cheap (two
    // Cell assignments); call after any path that mutates currentAttrs or
    // swaps mState (SGR, RIS, DECSTR, alt-screen entry/exit, init).
    void pushEraseBlank();

    static const char *escapeSequenceName(EscapeSequence seq);

    enum CSISequence
    {
        CUU     = 'A',
        CUD     = 'B',
        CUF     = 'C',
        CUB     = 'D',
        CNL     = 'E',
        CPL     = 'F',
        CHA     = 'G',
        CUP     = 'H',
        ED      = 'J',
        EL      = 'K',
        SU      = 'S',
        SD      = 'T',
        HVP     = 'f',
        SGR     = 'm',
        AUX     = 'i',
        DSR     = 'n',
        SCP     = 's',
        RCP     = 'u',
        DCH     = 'P',
        ICH     = '@',
        IL      = 'L',
        DL      = 'M',
        ECH     = 'X',
        REP     = 'b',
        VPA     = 'd',
        SM      = 'h',
        RM      = 'l',
        DECSTBM = 'r' // Set Top and Bottom Margins (scroll region)
    };

    static const char *csiSequenceName(CSISequence seq);

    void scrollUpInRegion(int n);
    void advanceCursorToNewLine();
    void lineFeed();

    // Mouse reporting
    void sendMouseEvent(int button, bool press, bool motion, int cx, int cy, uint32_t modifiers);
    // forceSgr=true overrides the X10 fallback when ?1006 is not set. Used by
    // Selective Mouse Reporting, which is defined to emit SGR regardless.
    void sendMouseEventPixel(int button, bool press, bool motion, int cx, int cy, int px, int py, uint32_t modifiers, bool forceSgr = false);

    int mMouseButtonDown { -1 };
    int mLastMouseX { -1 }, mLastMouseY { -1 };

    // XTSAVE / XTRESTORE: snapshot of DEC private modes saved via CSI ? Pm s,
    // restored via CSI ? Pm r. Empty mode list = all known modes.
    std::unordered_map<int, bool> mSavedPrivateModes;
    void savePrivateModes(const std::vector<int> &modes);
    void restorePrivateModes(const std::vector<int> &modes);

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
    std::string encodeKittyKey(const KeyEvent &ev) const;

    // Pending selection: button is pressed but mouse hasn't moved yet
    bool mPendingSelection { false };
    int mPendingSelCol { 0 };
    int mPendingSelAbsRow { 0 };
    bool mPendingSelXRightHalf { false };

    Selection mSelection;

    // Decoration overlay store. Mutated under mMutex; snapshot mirror copies
    // under the same lock. Order is insertion (zPriority is consulted at
    // render-time composition, not in storage).
    std::vector<Decoration> mDecorations;
    uint64_t mNextDecorationId { 1 };

    // Lookup indexes maintained alongside mDecorations to keep the per-frame
    // snapshot cost bounded by visible rows rather than total decoration
    // count. Without these, TerminalSnapshot iterates every Decoration and
    // calls resolveDecoration on each, which is O(N_total) per frame — a
    // search that paints 1000 highlights costs 1000 firstAbsOfLine hash
    // lookups per snapshot, every snapshot, even when nothing's on screen.
    //
    //   mDecIdToIdx       — O(1) id → mDecorations index (replaces linear
    //                       scans in removeDecoration / animation lookups).
    //   mDecsByStartLine  — single-line decorations (startLineId ==
    //                       endLineId), grouped by anchor lineId. Snapshot
    //                       walks visible lineIds and gathers from this map.
    //   mMultiLineDecIds  — decorations whose anchor span crosses logical
    //                       lines (CommandRegion, multi-line Selection, JS-
    //                       owned ranges). Always scanned in the snapshot —
    //                       count is bounded by user actions, not search.
    //
    // Single-line buckets and mMultiLineDecIds keep ids in insertion order
    // (== id order, since ids are monotonic). Erase-shifts in mDecorations
    // trigger a full index rebuild via rebuildDecorationIndexesLocked.
    std::unordered_map<uint64_t, size_t> mDecIdToIdx;
    std::unordered_map<uint64_t, std::vector<uint64_t>> mDecsByStartLine;
    std::vector<uint64_t> mMultiLineDecIds;

    // Animation registry — see animations() and start/finish/cancelAnimation.
    // Cleaned up alongside affected decorations in removeDecoration /
    // clearUserDecorations / applyDecorationBatch.
    std::unordered_map<uint64_t, Animation> mAnimations;

    // Process-wide test override for mono(). 0 = use wall clock. Static
    // because every emulator should see the same simulated time when a
    // test sets it.
    static inline std::atomic<uint64_t> sMonoOverride { 0 };

    // Image registry
    std::unordered_map<uint32_t, std::shared_ptr<ImageEntry>> mImageRegistry;
    uint32_t mNextImageId { 1 };

    // Hyperlink registry — used for both OSC 8 explicit hyperlinks and
    // auto-detected URLs (`UrlDetector` in `scanLogicalLineForUrls`). The
    // entry's `id` is the OSC 8 user-supplied tag (empty for auto-detected
    // URLs). Cells reference entries via `CellExtra::hyperlinkId`. OSC 8
    // wins over auto-detection: `scanLogicalLineForUrls` only stamps cells
    // whose `hyperlinkId` is currently 0.
    struct HyperlinkEntry
    {
        std::string uri;
        std::string id;
    };

    std::unordered_map<uint32_t, HyperlinkEntry> mHyperlinkRegistry;
    uint32_t mNextHyperlinkId { 1 };
    uint32_t mActiveHyperlinkId { 0 }; // 0 = no active hyperlink

    // Scan the logical line that owns `lineId` for URLs (https?://, file://,
    // ssh://) and stamp `CellExtra::hyperlinkId` on matched cells, allocating
    // new entries in `mHyperlinkRegistry`. No-op on alt screen. Skips cells
    // that already have a nonzero hyperlinkId so explicit OSC 8 entries win.
    // Called from `lineFeed()` so the typical newline-terminated tool output
    // gets linkified as it streams. Caller must hold `mMutex`.
    void scanLogicalLineForUrls(uint64_t lineId);

    // Title stack (XTWINOPS CSI 22/23 t + OSC 0/2)
    // Stack top is always the current title. Empty = no title set.
    // Protected by mMutex; reads via currentTitle() use mTitleIconMutex.
    std::vector<std::string> mTitleStack;
    static constexpr size_t TITLE_STACK_MAX = 10;

    // Icon stack (XTWINOPS CSI 22/23 t + OSC 1).
    std::vector<std::string> mIconStack;
    static constexpr size_t ICON_STACK_MAX = 10;

    // Shadow of the current title/icon-stack top (nullopt when the
    // stack is empty). Read on the per-tick tab-bar resolution path
    // through currentTitle() / currentIcon(); republished by the
    // parser via publishTitle() / publishIcon() whenever the top
    // changes. Guarded by mTitleIconMutex (separate from mMutex so
    // reads don't block on a long parse-apply).
    mutable std::mutex mTitleIconMutex;
    std::optional<std::string> mTitleShadow;
    std::optional<std::string> mIconShadow;

    // Desktop notification accumulator (OSC 99)
    std::string mNotifyId;
    std::string mNotifyTitle;
    std::string mNotifyBody;
    uint8_t mNotifyUrgency { 1 };                 // 0=low, 1=normal, 2=critical
    bool mNotifyCloseResponseRequested { false }; // c=1
    // Action set per kitty notifications.py:160-162. Default {focus} when
    // a= is not specified. +/- prefixes add/remove individual values.
    bool mNotifyActionFocus { true };
    bool mNotifyActionReport { false };
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
    std::deque<CommandRecord> mCommandRing; // all records whose prompt row is still retained
    uint64_t mNextCommandId { 1 };
    bool mCommandInProgress { false };          // true between A and D (or N)
    std::string mCurrentCwd;                    // last OSC 7 value (for command records)
    std::optional<uint64_t> mSelectedCommandId; // id of command currently highlighted via click or keyboard nav

    int absoluteRowFromScreen(int screenRow) const;
    CommandRecord *inProgressCommandMut(); // nullptr if no in-progress record
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

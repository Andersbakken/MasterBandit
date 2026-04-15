#pragma once

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
    std::function<void(const std::string&)>      onTitleChanged;
    std::function<float()>                       cellPixelWidth;
    std::function<float()>                       cellPixelHeight;
    std::function<void(int, std::string_view)>  onOSC;    // called for unhandled OSC codes
    std::function<void(const std::string&)>      onIconChanged;    // OSC 1
    std::function<void(int /*state*/, int /*pct*/)> onProgressChanged; // OSC 9;4
    std::function<bool()>                        isDarkMode;         // for mode 2031
    std::function<void(const std::string&)>      onCWDChanged;       // OSC 7
    std::function<void(const std::string&)>      onMouseCursorShape; // OSC 22 (CSS pointer name; "" = default)
    std::function<void(const std::string&, const std::string&, const std::string&)> onDesktopNotification; // OSC 99
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

    // Serializes mutation vs. render-thread snapshot capture. Recursive because
    // script callbacks fired synchronously from inside injectData (OSC
    // handlers, action dispatch) can re-enter mutation APIs on the same
    // thread. Using std::recursive_mutex avoids deadlock in that path.
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
    struct TerminalState {
        int cursorX { 0 }, cursorY { 0 };
        bool cursorVisible { true };
        CursorShape cursorShape { CursorBlock };
        bool cursorBlinkEnabled { true };       // DEC private mode 12
        bool wrapPending { false };             // deferred autowrap state
        CellAttrs currentAttrs;                 // SGR "pen"
        uint32_t currentUnderlineColor { 0 };   // SGR 58: packed RGBA8, 0 = use fg
        // DECSC (ESC 7) / DECRC (ESC 8) save slot — per-screen so a DECSC on
        // alt doesn't clobber main's saved cursor. Shape/blink are not saved
        // by DECSC per spec; they're preserved across alt via the state swap
        // itself (main keeps its shape while alt runs).
        int savedCursorX { 0 }, savedCursorY { 0 };
        bool savedWrapPending { false };
        CellAttrs savedAttrs;
        int scrollTop { 0 }, scrollBottom { 0 };  // scroll region [top, bottom)
        bool cursorKeyMode { false };           // DECCKM
        bool keypadMode { false };              // DECKPAM
        bool autoWrap { true };                 // DECAWM
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

    const IGrid& grid() const { return mUsingAltScreen ? static_cast<const IGrid&>(mAltGrid) : static_cast<const IGrid&>(mDocument); }
    IGrid& grid() { return mUsingAltScreen ? static_cast<IGrid&>(mAltGrid) : static_cast<IGrid&>(mDocument); }
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
    int viewportOffset() const { return mViewportOffset; }
    void scrollToPrompt(int direction); // -1 = previous, +1 = next
    void selectCommandOutput();         // select output around current viewport position
    std::string serializeScrollback() const; // serialize all content for pager

    enum Event {
        Update,
        ScrollbackChanged,
        VisibleBell,
        CommandComplete          // payload: const CommandRecord*
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
    // stored as stable row ids (assigned by Document); convert to current
    // abs-rows via `document().absForRowId(id)` when you need to navigate.
    // Archive-head eviction past a row makes its id "stale" — lookup returns -1.
    // Cell content mutation (shell redraw) does not affect row ids.
    struct CommandRecord {
        uint64_t id = 0;
        // Stable row ids from Document — resolve to current abs-row at query time.
        // Text is extracted lazily via Document::getTextFromRows rather than captured here.
        uint64_t promptStartRowId = 0;
        uint64_t commandStartRowId = 0;
        uint64_t outputStartRowId = 0;
        uint64_t outputEndRowId = 0;
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

    // Selection
    enum class SelectionMode { Normal, Word, Line, Rectangle };
    struct Selection {
        int startCol { 0 }, startAbsRow { 0 };
        int endCol { 0 }, endAbsRow { 0 };
        bool active { false };
        bool valid { false };
        SelectionMode mode { SelectionMode::Normal };
    };
    void startSelection(int col, int absRow);
    void startWordSelection(int col, int absRow);
    void startLineSelection(int absRow);
    void extendSelection(int col, int absRow);
    void startRectangleSelection(int col, int absRow);
    void updateSelection(int col, int absRow);
    void finalizeSelection();
    void clearSelection();
    bool hasSelection() const { return mSelection.valid || mSelection.active; }
    const Selection& selection() const { return mSelection; }
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

    void injectData(const char* data, size_t len);

    void setOSCCallback(std::function<void(int, std::string_view)> cb)
    {
        mCallbacks.onOSC = std::move(cb);
    }

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

    void processStringSequence();
    void processDCS();
    void processOSC_Title(std::string_view text, bool setTitle);
    void processOSC_Color(int oscNum, std::string_view payload);
    void processOSC_Palette(std::string_view payload);
    void processOSC_PaletteReset(std::string_view payload);
    void processOSC_Clipboard(std::string_view payload);
    void processOSC_iTerm(std::string_view payload);
    void processOSC_PointerShape(std::string_view payload);
    void processAPC();
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
        RI = 'M'
    };

    void processCSI();
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
    std::vector<std::string> mTitleStack;
    static constexpr size_t TITLE_STACK_MAX = 10;

    // Desktop notification accumulator (OSC 99)
    std::string mNotifyId;
    std::string mNotifyTitle;
    std::string mNotifyBody;

    // OSC 133 shell-integration state.
    SemanticMode mSemanticMode { SemanticMode::Inactive };
    std::deque<CommandRecord> mCommandRing;   // last N completed + in-progress
    static constexpr size_t COMMAND_RING_MAX = 256;
    uint64_t mNextCommandId { 1 };
    bool mCommandInProgress { false };        // true between A and D (or N)
    std::string mCurrentCwd;                  // last OSC 7 value (for command records)

    int absoluteRowFromScreen(int screenRow) const;
    CommandRecord* inProgressCommandMut();    // nullptr if no in-progress record
    void startCommand(int absRow, int col);
    void markCommandInput(int absRow, int col);
    void markCommandOutput(int absRow, int col);
    void finishCommand(int absRow, int col, std::optional<int> exitCode);

    // Default colors (OSC 10/11/12)
    DefaultColors mDefaultColors;
    DefaultColors mConfigDefaultColors; // config-loaded originals for OSC 110/111/112 reset
};

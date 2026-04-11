#pragma once

#include <cstdint>
#include <optional>
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

    int cursorX() const { return mCursorX; }
    int cursorY() const { return mCursorY; }
    bool cursorVisible() const { return mCursorVisible; }

    // DECSCUSR cursor shapes
    enum CursorShape {
        CursorBlock = 0,         // blinking block (default)
        CursorSteadyBlock = 2,
        CursorUnderline = 3,     // blinking underline
        CursorSteadyUnderline = 4,
        CursorBar = 5,           // blinking bar
        CursorSteadyBar = 6
    };
    CursorShape cursorShape() const { return mCursorShape; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }

    const IGrid& grid() const { return mUsingAltScreen ? static_cast<const IGrid&>(mAltGrid) : static_cast<const IGrid&>(mDocument); }
    IGrid& grid() { return mUsingAltScreen ? static_cast<IGrid&>(mAltGrid) : static_cast<IGrid&>(mDocument); }
    const Document& document() const { return mDocument; }

    virtual void resize(int width, int height);

    // Scrollback viewport
    const Cell* viewportRow(int viewRow) const;
    void scrollViewport(int delta);
    void resetViewport();
    int viewportOffset() const { return mViewportOffset; }
    void scrollToPrompt(int direction); // -1 = previous, +1 = next
    void selectCommandOutput();         // select output around current viewport position
    std::string serializeScrollback() const; // serialize all content for pager

    enum Event {
        Update,
        ScrollbackChanged,
        VisibleBell
    };

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
    bool mouseReportingActive() const { return mMouseMode1000 || mMouseMode1002 || mMouseMode1003; }
    bool syncOutputActive() const { return mSyncOutput; }
    uint8_t kittyFlags() const { return mKittyFlags; }
    bool colorPreferenceReporting() const { return mColorPreferenceReporting; }
    void setPaletteColor(int idx, uint8_t r, uint8_t g, uint8_t b) {
        if (idx >= 0 && idx < 16) { m16ColorPalette[idx][0] = r; m16ColorPalette[idx][1] = g; m16ColorPalette[idx][2] = b; }
    }
    void applyColorScheme(const struct ColorScheme& cs);

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

        // Per-placement display parameters (one image, multiple positions)
        struct Placement {
            uint32_t cellWidth { 0 }, cellHeight { 0 };
            uint32_t cropX { 0 }, cropY { 0 }, cropW { 0 }, cropH { 0 };
            uint32_t cellXOffset { 0 }, cellYOffset { 0 }; // X=, Y= sub-cell pixel offsets
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
    const std::unordered_map<uint32_t, ImageEntry>& imageRegistry() const { return mImageRegistry; }
    std::unordered_map<uint32_t, ImageEntry>& imageRegistryMut() { return mImageRegistry; }
    uint32_t findImageByNumber(uint32_t number) const;

    // Advance all running animations based on current time.
    void tickAnimations();

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
    bool bracketedPaste() const { return mBracketedPaste; }
    void resetScrollback(int scrollbackLines);  // reinitializes document with given scrollback capacity
    TerminalCallbacks& callbacks() { return mCallbacks; }

private:
    TerminalCallbacks mCallbacks;

    int mWidth { 0 }, mHeight { 0 };
    int mCursorX { 0 }, mCursorY { 0 };
    bool mCursorVisible { true };
    CursorShape mCursorShape { CursorBlock };
    bool mWrapPending { false };    // deferred autowrap state

    Document mDocument;
    CellGrid mAltGrid;
    bool mUsingAltScreen { false };

    int mViewportOffset { 0 };

    CellAttrs mCurrentAttrs;       // SGR "pen"
    uint32_t mCurrentUnderlineColor { 0 }; // SGR 58: packed RGBA8, 0 = use fg
    char32_t mLastPrintedChar { 0 };       // for REP (CSI b)
    int mLastPrintedX { -1 }, mLastPrintedY { -1 }; // position of last stored cell (for combining codepoints)
    int mSavedCursorX { 0 }, mSavedCursorY { 0 };
    bool mSavedWrapPending { false };
    CellAttrs mSavedAttrs;
    int mScrollTop { 0 }, mScrollBottom { 0 }; // scroll region [top, bottom)

    enum State {
        Normal,
        InUtf8,
        InEscape,
        InStringSequence
    } mState { Normal };
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
    void processAPC();
    void placeImageInGrid(uint32_t imageId, uint32_t placementId, int cellCols, int cellRows, bool moveCursor = true);
    std::string buildCurrentSGR() const;

    // Kitty graphics protocol: chunked transfer accumulation
    struct KittyLoadState {
        std::vector<uint8_t> data;   // accumulated decoded payload
        uint32_t id = 0;             // client image ID (i=)
        uint32_t placementId = 0;    // placement ID (p=)
        uint32_t format = 32;        // f= (24=RGB, 32=RGBA, 100=PNG)
        uint32_t width = 0, height = 0; // s=, v= (source data dimensions)
        uint32_t cellCols = 0, cellRows = 0; // c=, r=
        uint32_t xOffset = 0, yOffset = 0;   // x=, y= (source rect offset)
        uint32_t cropWidth = 0, cropHeight = 0; // w=, h= (source rect size)
        uint32_t quiet = 0;          // q=
        int32_t zIndex = 0;          // z=
        uint32_t cursorMovement = 0; // C=
        char action = 'T';           // a=
        char compressed = 0;         // o=
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

    bool mCursorKeyMode { false };  // DECCKM: true = application mode
    bool mKeypadMode { false };     // DECKPAM: true = application mode

    bool mMouseMode1000 { false };
    bool mMouseMode1002 { false };
    bool mMouseMode1003 { false };
    bool mMouseMode1006 { false };
    int mMouseButtonDown { -1 };
    int mLastMouseX { -1 }, mLastMouseY { -1 };
    bool mAutoWrap { true };         // DECAWM (private mode 7): autowrap at right margin
    bool mInsertMode { false };      // IRM (mode 4): insert mode
    bool mBracketedPaste { false };
    bool mFocusReporting { false };  // Mode 1004
    bool mSyncOutput { false };      // Mode 2026: synchronized output
    bool mColorPreferenceReporting { false }; // Mode 2031

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
    std::unordered_map<uint32_t, ImageEntry> mImageRegistry;
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

    // Default colors (OSC 10/11/12)
    DefaultColors mDefaultColors;
    DefaultColors mConfigDefaultColors; // config-loaded originals for OSC 110/111/112 reset
};

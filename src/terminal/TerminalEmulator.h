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
        uint32_t pixelWidth { 0 }, pixelHeight { 0 };
        uint32_t cellWidth { 0 }, cellHeight { 0 };
        std::vector<uint8_t> rgba;
    };
    const std::unordered_map<uint32_t, ImageEntry>& imageRegistry() const { return mImageRegistry; }

    void injectData(const char* data, size_t len);

    void setOSCCallback(std::function<void(int, std::string_view)> cb)
    {
        mCallbacks.onOSC = std::move(cb);
    }

    static unsigned long long mono();

    // 16-color palette (standard + bright) as RGB
    uint8_t m16ColorPalette[16][3];
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
    void processOSC_Clipboard(std::string_view payload);
    void processOSC_iTerm(std::string_view payload);
    void placeImageInGrid(uint32_t imageId, int cellCols, int cellRows);

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
    void sendMouseEvent(int button, bool press, bool motion, int cx, int cy, unsigned int modifiers);

    bool mCursorKeyMode { false };  // DECCKM: true = application mode
    bool mKeypadMode { false };     // DECKPAM: true = application mode

    bool mMouseMode1000 { false };
    bool mMouseMode1002 { false };
    bool mMouseMode1003 { false };
    bool mMouseMode1006 { false };
    int mMouseButtonDown { -1 };
    int mLastMouseX { -1 }, mLastMouseY { -1 };
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

    // Desktop notification accumulator (OSC 99)
    std::string mNotifyId;
    std::string mNotifyTitle;
    std::string mNotifyBody;

    // Default colors (OSC 10/11/12)
    DefaultColors mDefaultColors;
};

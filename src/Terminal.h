#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>
#include <Platform.h>
#include <TerminalOptions.h>
#include <CellGrid.h>

std::string toPrintable(const std::string &str);
std::string toPrintable(const char *chars, int len);
inline std::string toPrintable(const std::string &string)
{
    return toPrintable(string.c_str(), string.size());
}

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Platform;
class Terminal
{
public:
    Terminal(Platform *platform);
    ~Terminal();

    virtual bool init(const TerminalOptions &options);
    enum Flag {
        None = 0,
        LineWrap = (1ull << 1)
    };

    int cursorX() const { return mCursorX; }
    int cursorY() const { return mCursorY; }
    bool cursorVisible() const { return mCursorVisible; }
    int x() const { return mX; }
    int y() const { return mY; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }

    const CellGrid& grid() const { return mUsingAltScreen ? mAltGrid : mGrid; }
    CellGrid& grid() { return mUsingAltScreen ? mAltGrid : mGrid; }

    void scroll(int left, int top);
    void resize(int width, int height);

    enum Event {
        Update,
        ScrollbackChanged,
        VisibleBell
    };

    static const char *eventName(Event event);

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
            ScrollUp,
            ScrollDown,
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

    virtual void event(Event, void *data = nullptr) = 0;

    void keyPressEvent(const KeyEvent *event);
    void keyReleaseEvent(const KeyEvent *event);
    void mousePressEvent(const MouseEvent *event);
    void mouseReleaseEvent(const MouseEvent *event);
    void mouseMoveEvent(const MouseEvent *event);
    int masterFD() const { return mMasterFD; }
    void readFromFD();
    int exitCode() const { return mExitCode; }

    static unsigned long long mono();
private:
    Platform *mPlatform;
    TerminalOptions mOptions;
    int mX { 0 }, mY { 0 }, mWidth { 0 }, mHeight { 0 };
    int mCursorX { 0 }, mCursorY { 0 };
    bool mCursorVisible { true };
    unsigned int mFlags { 0 };
    int mMasterFD { -1 }, mSlaveFD { -1 };
    int mExitCode { 0 };

    CellGrid mGrid;
    CellGrid mAltGrid;
    bool mUsingAltScreen { false };

    CellAttrs mCurrentAttrs;       // SGR "pen"
    int mSavedCursorX { 0 }, mSavedCursorY { 0 };
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
        DECKPNM = '>'
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
        SM = 'h',
        RM = 'l',
        DECSTBM = 'r'  // Set Top and Bottom Margins (scroll region)
    };

    static const char *csiSequenceName(CSISequence seq);

    void scrollUpInRegion(int n);
    void advanceCursorToNewLine();

    // 16-color palette (standard + bright) as RGB
    static const uint8_t s16ColorPalette[16][3];
    // 256-color palette lookup
    static void color256ToRGB(int idx, uint8_t &r, uint8_t &g, uint8_t &b);
};

#endif /* TERMINAL_H */

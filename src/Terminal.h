#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>
#include "KeyEvent.h"
#include "MouseEvent.h"

std::string toUtf8(const std::u16string &string);
std::string toPrintable(const char *chars, size_t len);
inline std::string toPrintable(const std::string &str) { return toPrintable(str.c_str(), str.size()); }


#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Window;
class Terminal
{
public:
    Terminal();
    ~Terminal();

    struct Options {
        std::string shell;
        std::string user;
    };

    virtual bool init(const Options &options);
    enum Flag {
        None = 0,
        LineWrap = (1ull << 1)
    };

    size_t cursorX() const { return mCursorX; }
    size_t cursorY() const { return mCursorY; }
    size_t x() const { return mX; }
    size_t y() const { return mY; }
    size_t width() const { return mWidth; }
    size_t height() const { return mHeight; }
    size_t scrollBackLength() const { return mScrollbackLength; }

    void scroll(size_t left, size_t top);
    void resize(size_t width, size_t height);
    void render();

    enum Event {
        Update,
        ScrollbackChanged,
        LineChanged
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
            ScrollUp,
            ScrollDown,
            SelectGraphicRendition,
            AUXPortOn,
            AUXPortOff,
            DeviceStatusReport,
            SaveCursorPosition,
            RestoreCursorPosition
        } type { Invalid };

        static const char *typeName(Type type);

        size_t count { 0 },  x { 0 }, y { 0 };
    };

    void onAction(const Action *action);

    virtual void event(Event, void *data = nullptr) = 0;
    enum RenderFlag {
        Render_None = 0,
        Render_Selected = (1ull << 1)
    };
    virtual void render(size_t y, size_t x, const char16_t *ch, size_t len, size_t cursor, unsigned int flags) = 0;
    virtual void quit() = 0;

    void addText(const char *str, size_t len);

    void keyPressEvent(const KeyEvent &event);
    void keyReleaseEvent(const KeyEvent &event);
    void mousePressEvent(const MouseEvent &event);
    void mouseReleaseEvent(const MouseEvent &event);
    void mouseMoveEvent(const MouseEvent &event);
    int masterFD() const { return mMasterFD; }
    void readFromFD();
    int exitCode() const { return mExitCode; }

    static unsigned long long mono();
private:
    bool isSelected(size_t y, size_t *start, size_t *length) const;
private:
    void processChunks();
    Options mOptions;
    size_t mX { 0 }, mY { 0 }, mWidth { 0 }, mHeight { 0 };
    size_t mCursorX { std::u16string::npos }, mCursorY { std::u16string::npos };
    unsigned int mFlags { 0 };
    bool mHasSelection { false };
    size_t mSelectionStartX { 0 }, mSelectionStartY { 0 }, mSelectionEndX { 0 }, mSelectionEndY { 0 };
    int mMasterFD { -1 }, mSlaveFD { -1 };
    int mExitCode { 0 };

    struct Command {
        enum Type {
            None,
            ResetToInitialState,
            Color
        } type { None };
        std::string data;
        size_t idx { std::u16string::npos };
    };
    struct Line {
        std::u16string data;
        std::vector<size_t> lineBreaks;
        std::vector<Command> commands;
    };
    std::vector<Line> mLines;
    size_t mScrollbackLength { 0 };
    enum State {
        Normal,
        InUtf8,
        InEscape
    } mState { Normal };
    char mUtf8Buffer[6];
    size_t mUtf8Index { 0 };

    char mEscapeBuffer[128];
    size_t mEscapeIndex { 0 };

    enum EscapeSequence {
        SS2 = 'N', // Single Shift Two
        SS3 = '0', // Single Shift Three
        DCS = 'P', // Device Control String
        CSI = '[', // Control Sequence Introducer
        ST = '\\', // String Terminator
        OSX = ']', // Operating System Command
        SOS = 'X', // Start of String
        PM = '^', // Privacy Message
        APC = '_', // Application Program Command
        RIS = 'c' // Reset to Initial State
    };

    void processCSI();
    void processSGR(Action *action);

    static const char *escapeSequenceName(EscapeSequence seq);

    enum CSISequence {
        CUU = 'A',
        CUD = 'B',
        CUF = 'C',
        CUB = 'D',
        CNL	= 'E',
        CPL = 'F',
        CHA = 'G',
        CUP = 'H',
        ED ='J',
        EL = 'K',
        SU = 'S',
        SD = 'T',
        HVP	= 'f',
        SGR	= 'm',
        AUX	= 'i', // Port On or Port Off
        DSR	= 'n',
        SCP = 's',
        RCP	= 'u',

        // private sequences
        // DECTCEM = 'h',

        // CSI ? 25 h	DECTCEM Shows the cursor, from the VT320.
        // CSI ? 25 l	DECTCEM Hides the cursor.
        // CSI ? 1049 h	Enable alternative screen buffer
        // CSI ? 1049 l	Disable alternative screen buffer
        // CSI ? 2004 h	Turn on bracketed paste mode. Text pasted into the terminal will be surrounded by ESC [200~ and ESC [201~, and characters in it should not be treated as commands (for example in Vim).[20] From Unix terminal emulators.
        //                                                                                                                      CSI ? 2004 l	Turn off bracketed paste mode.
    };

    static const char *csiSequenceName(CSISequence seq);
};

#endif /* TERMINAL_H */

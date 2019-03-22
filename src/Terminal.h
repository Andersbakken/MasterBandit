#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>
#include <QtCore/QString>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>

QString toPrintable(const char *chars, int len);
QString toPrintable(const QString &str);

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
        QString shell;
        QString user;
    };

    virtual bool init(const Options &options);
    enum Flag {
        None = 0,
        LineWrap = (1ull << 1)
    };

    int cursorX() const { return mCursorX; }
    int cursorY() const { return mCursorY; }
    int x() const { return mX; }
    int y() const { return mY; }
    int width() const { return mWidth; }
    int height() const { return mHeight; }
    int scrollBackLength() const { return mScrollbackLength; }

    void scroll(int left, int top);
    void resize(int width, int height);
    void render();

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

        int count { 0 },  x { 0 }, y { 0 };
    };

    void onAction(const Action *action);

    virtual void event(Event, void *data = nullptr) = 0;
    enum RenderFlag {
        Render_None = 0,
        Render_Selected = (1ull << 1)
    };
    virtual void render(int y, int x, const QString &str, int idx, int len, int cursor, unsigned int flags) = 0;
    virtual void quit() = 0;

    void addText(const char *str, int len);

    void keyPressEvent(const QKeyEvent *event);
    void keyReleaseEvent(const QKeyEvent *event);
    void mousePressEvent(const QMouseEvent *event);
    void mouseReleaseEvent(const QMouseEvent *event);
    void mouseMoveEvent(const QMouseEvent *event);
    int masterFD() const { return mMasterFD; }
    void readFromFD();
    int exitCode() const { return mExitCode; }

    static unsigned long long mono();
private:
    bool isSelected(int y, int *start, int *length) const;
private:
    void processChunks();
    Options mOptions;
    int mX { 0 }, mY { 0 }, mWidth { 0 }, mHeight { 0 };
    int mCursorX { -1 }, mCursorY { -1 };
    unsigned int mFlags { 0 };
    bool mHasSelection { false };
    int mSelectionStartX { 0 }, mSelectionStartY { 0 }, mSelectionEndX { 0 }, mSelectionEndY { 0 };
    int mMasterFD { -1 }, mSlaveFD { -1 };
    int mExitCode { 0 };

    struct Command {
        enum Type {
            None,
            ResetToInitialState,
            Color
        } type { None };
        std::string data;
        int idx { -1 };
    };
    struct Line {
        QString data;
        std::vector<int> lineBreaks;
        std::vector<Command> commands;
    };
    std::vector<Line> mLines;
    int mScrollbackLength { 0 };
    enum State {
        Normal,
        InUtf8,
        InEscape
    } mState { Normal };
    char mUtf8Buffer[6];
    int mUtf8Index { 0 };

    char mEscapeBuffer[128];
    int mEscapeIndex { 0 };

    // https://ttssh2.osdn.jp/manual/en/about/ctrlseq.html
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
        RIS = 'c', // Reset to Initial State
        VB = 'g' // Visible bell
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
        ED = 'J',
        EL = 'K',
        SU = 'S',
        SD = 'T',
        HVP	= 'f',
        SGR	= 'm',
        AUX	= 'i', // Port On or Port Off
        DSR	= 'n',
        SCP = 's',
        RCP	= 'u',
        DCH = 'P'

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

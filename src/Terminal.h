#ifndef TERMINAL_H
#define TERMINAL_H

#include <vector>
#include <string>
#include <utf8.h>
#include <stdlib.h>
#include <string.h>
#include "KeyEvent.h"
#include "MouseEvent.h"

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
    size_t scrollBackLength() const { return mScrollback.size(); }
    size_t widestLine() const { return mWidestLine; }

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

        size_t count { 0 },  row { 0 }, column { 0 };
    };

    virtual void event(Event, void *data = nullptr) = 0;
    enum RenderFlag {
        Render_None = 0,
        Render_Selected = (1ull << 1)
    };
    virtual void render(size_t y, size_t x, const char *ch, size_t len, unsigned int flags) = 0;
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
    size_t mCursorX { 0 }, mCursorY { 0 };
    unsigned int mFlags { 0 };
    bool mHasSelection { false };
    size_t mSelectionStartX { 0 }, mSelectionStartY { 0 }, mSelectionEndX { 0 }, mSelectionEndY { 0 };
    size_t mWidestLine { 0 };
    int mMasterFD { -1 }, mSlaveFD { -1 };
    int mExitCode { 0 };

    struct LineBreak {
        size_t idx { std::u16string::npos };
        bool wrap { false };
    };
    struct Command {
        enum Type {
            None,
            ResetToInitialState,
            Color
        } type { None };
        std::string data;
        size_t idx { std::u16string::npos };
    };
    std::vector<Command> mCommands;
    std::vector<LineBreak> mLineBreaks;
    std::u16string mScrollback;
    std::string mRawBuffer;
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

    static const char *escapeSequenceName(EscapeSequence seq)
    {
        switch (seq) {
        case SS2: return "SS2";
        case SS3: return "SS3";
        case DCS: return "DCS";
        case CSI: return "CSI";
        case ST: return "ST";
        case OSX: return "OSX";
        case SOS: return "SOS";
        case PM: return "PM";
        case APC: return "APC";
        case RIS: return "RIS";
        }
        abort();
        return nullptr;
    }

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
        RCP	= 'u'
    };

    static const char *csiSequenceName(CSISequence seq)
    {
        switch (seq) {
        case CUU: return "CUU";
        case CUD: return "CUD";
        case CUF: return "CUF";
        case CUB: return "CUB";
        case CNL: return "CNL";
        case CPL: return "CPL";
        case CHA: return "CHA";
        case CUP: return "CUP";
        case ED: return "ED";
        case EL: return "EL";
        case SU: return "SU";
        case SD: return "SD";
        case HVP: return "HVP";
        case SGR: return "SGR";
        case AUX: return "AUX";
        case DSR: return "DSR";
        case SCP: return "SCP";
        case RCP: return "RCP";
        }
        return nullptr;
    };
};

#endif /* TERMINAL_H */

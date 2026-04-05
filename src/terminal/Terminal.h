#pragma once
#include "TerminalEmulator.h"
#include "TerminalOptions.h"

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Terminal; // forward declare for PlatformCallbacks

struct PlatformCallbacks {
    std::function<void(Terminal*)> onTerminalExited;
    std::function<void()> quit;
};

class Terminal : public TerminalEmulator {
public:
    Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks);
    ~Terminal() override;

    bool init(const TerminalOptions& options);
    int masterFD() const { return mMasterFD; }
    void readFromFD();
    void pasteText(const std::string& text);
    void resize(int width, int height) override;

    // Deferred TIOCSWINSZ: set by resize(), consumed by flushPendingResize()
    bool hasResizePending() const { return mResizePending; }
    void flushPendingResize();

protected:
    void writeToOutput(const char* data, size_t len) override;

private:
    void writeToPTY(const char* data, size_t len);

    PlatformCallbacks mPlatformCbs;
    TerminalOptions mOptions;
    int mMasterFD { -1 };
    bool mResizePending { false };
};

#pragma once
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
#include <uv.h>
#include <vector>

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
    void setLoop(uv_loop_t* loop) { mLoop = loop; }
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
    void flushWriteQueue();
    static void onWritePollReady(uv_poll_t* handle, int status, int events);

    PlatformCallbacks mPlatformCbs;
    TerminalOptions mOptions;
    int mMasterFD { -1 };
    bool mResizePending { false };
    uv_loop_t* mLoop { nullptr };
    uv_poll_t mWritePoll {};
    bool mWritePollActive { false };
    std::vector<char> mWriteQueue;
};

#pragma once
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
#include <eventloop/EventLoop.h>
#include <vector>

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Terminal; // forward declare for PlatformCallbacks

struct PlatformCallbacks {
    std::function<void(Terminal*)> onTerminalExited;
    std::function<void()> quit;
    // Output filter: check shouldFilterOutput first (cheap, no copy),
    // then call filterOutput only if it returns true.
    std::function<bool()> shouldFilterOutput;
    std::function<void(std::string& data)> filterOutput;
    // Input filter: same pattern.
    std::function<bool()> shouldFilterInput;
    std::function<void(std::string& data)> filterInput;
    // Headless input sink: receives keystrokes when no PTY exists (applet mode).
    std::function<void(const char* data, size_t len)> onInput;
};

class Terminal : public TerminalEmulator {
public:
    Terminal(PlatformCallbacks platformCbs, TerminalCallbacks callbacks);
    ~Terminal() override;

    bool init(const TerminalOptions& options);
    // Initialize without a PTY or child process. For script-driven applets.
    bool initHeadless(const TerminalOptions& options);
    void setLoop(EventLoop* loop) { mLoop = loop; }
    int masterFD() const { return mMasterFD; }
    bool isHeadless() const { return mHeadless; }
    void readFromFD();
    void flushReadBuffer();
    void flushWriteQueue();
    void pasteText(const std::string& text);

    // Query the foreground process name via tcgetpgrp + platform process lookup
    std::string foregroundProcess() const;
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
    bool mHeadless { false };
    bool mResizePending { false };
    pid_t mLastFgPgid { -1 };
    EventLoop* mLoop { nullptr };
    EventLoop::TimerId mWritePollId { 0 };
    bool mWritePollActive { false };
    std::vector<char> mWriteQueue;
    std::vector<char> mReadCoalesceBuffer;
};

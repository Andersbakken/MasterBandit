#pragma once
#include "TerminalEmulator.h"
#include "TerminalOptions.h"
// Platform.h and toPrintable already provided by TerminalEmulator.h

#define EINTRWRAP(ret, op) \
    do {                   \
        ret = (op);        \
    } while (ret == -1 && errno == EINTR)

class Terminal : public TerminalEmulator {
public:
    Terminal(Platform* platform, TerminalCallbacks callbacks);
    ~Terminal() override;

    bool init(const TerminalOptions& options);
    int masterFD() const { return mMasterFD; }
    void readFromFD();
    void pasteText(const std::string& text);
    void resize(int width, int height) override; // calls base + ioctl

protected:
    void writeToOutput(const char* data, size_t len) override;

private:
    void writeToPTY(const char* data, size_t len);

    Platform* mPlatform;
    TerminalOptions mOptions;
    int mMasterFD { -1 };
};

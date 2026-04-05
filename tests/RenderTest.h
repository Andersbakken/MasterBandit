#pragma once

#include <string>
#include <vector>
#include <functional>

#include <uv.h>
#include <libwebsockets.h>

class RenderTest {
public:
    struct Options {
        std::string fontPath;   // defaults to MB_TEST_FONT
        int cols = 80;
        int rows = 24;
        float fontSize = 16.0f;
    };

    RenderTest();
    explicit RenderTest(const Options& opts);
    ~RenderTest();

    RenderTest(const RenderTest&) = delete;
    RenderTest& operator=(const RenderTest&) = delete;

    // Connect to the mb --test child's IPC socket. Blocks with timeout.
    bool connect(int timeoutMs = 5000);

    // Inject text into the terminal
    bool sendText(const std::string& text, int timeoutMs = 2000);

    // Inject a named key with optional modifiers
    bool sendKey(const std::string& key, const std::vector<std::string>& mods = {}, int timeoutMs = 2000);

    // Take a PNG screenshot of the full composite
    std::vector<uint8_t> screenshotPng(int timeoutMs = 5000);

    // Take a PNG screenshot of a specific pane
    std::vector<uint8_t> screenshotPane(int paneId, int timeoutMs = 5000);

    // Take a cropped PNG screenshot (cell coordinates)
    std::vector<uint8_t> screenshotRect(int x, int y, int w, int h, int timeoutMs = 5000);

    // Take a cropped PNG screenshot of a specific pane (cell coordinates)
    std::vector<uint8_t> screenshotPaneRect(int paneId, int x, int y, int w, int h, int timeoutMs = 5000);

    // Compare two PNG byte buffers. Returns max absolute channel difference.
    static int comparePng(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

    // Compare against a reference file. If MB_UPDATE_REFS=1, saves actual as new reference.
    bool matchesReference(const std::vector<uint8_t>& png, const std::string& refName, int tolerance = 2);

    // Save raw PNG bytes to a file
    static void savePng(const std::vector<uint8_t>& png, const std::string& path);

    // Load raw PNG bytes from a file
    static std::vector<uint8_t> loadPng(const std::string& path);

    // Wait for the given duration while keeping the WebSocket alive
    void wait(int ms);

    // Reset terminal state (sends RIS) for reuse between tests
    bool reset(int timeoutMs = 2000);

    const std::string& socketPath() const { return socketPath_; }
    pid_t childPid() const { return pid_; }

    // Shared instance for tests with default options (lazily created)
    static RenderTest& shared();

    // Public because it's referenced from the static protocol table
    static int wsCallback(struct lws* wsi, enum lws_callback_reasons reason,
                          void* user, void* in, size_t len);

private:
    // Send a JSON request and block until response. Returns response JSON string.
    std::string sendRequest(const std::string& json, int timeoutMs);

    // Screenshot helper that builds the request JSON
    std::vector<uint8_t> doScreenshot(const std::string& target, int x, int y, int w, int h, bool hasRect, int timeoutMs);

    pid_t pid_ = -1;
    std::string socketPath_;

    // WebSocket client state
    uv_loop_t loop_ = {};
    struct lws_context* ctx_ = nullptr;
    struct lws* wsi_ = nullptr;
    bool connected_ = false;

    std::string pendingTx_;
    bool txPending_ = false;
    std::string rxBuffer_;
    std::string lastResponse_;
    bool responseReady_ = false;

    int nextId_ = 1;
};
